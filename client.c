/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#ifndef TMUX_WIN32
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/file.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

static struct tmuxproc	*client_proc;
static struct tmuxpeer	*client_peer;
static uint64_t		 client_flags;
#ifdef TMUX_WIN32
static int		 client_is_console;
static int		 client_console_ready;
static struct win32_handle_event *client_win32_input;
static struct win32_handle_writer *client_win32_output;
static size_t		 client_win32_output_pending;
static struct event	 client_win32_resize_timer;
static u_int		 client_win32_resize_sx;
static u_int		 client_win32_resize_sy;
static int		 client_win32_resize_timer_set;
#else
static int		 client_suspended;
#endif
static enum {
	CLIENT_EXIT_NONE,
	CLIENT_EXIT_DETACHED,
	CLIENT_EXIT_DETACHED_HUP,
	CLIENT_EXIT_LOST_TTY,
	CLIENT_EXIT_TERMINATED,
	CLIENT_EXIT_LOST_SERVER,
	CLIENT_EXIT_EXITED,
	CLIENT_EXIT_SERVER_EXITED,
	CLIENT_EXIT_MESSAGE_PROVIDED
} client_exitreason = CLIENT_EXIT_NONE;
static int		 client_exitflag;
static int		 client_exitval;
static enum msgtype	 client_exittype;
static const char	*client_exitsession;
static char		*client_exitmessage;
static const char	*client_execshell;
static const char	*client_execcmd;
static int		 client_attached;
static struct client_files client_files = RB_INITIALIZER(&client_files);

static __dead void	 client_exec(const char *,const char *);
#ifndef TMUX_WIN32
static int		 client_get_lock(char *);
#endif
static int		 client_connect(struct event_base *, const char *,
			     uint64_t);
#ifdef TMUX_WIN32
static void		 client_win32_resize_timer_callback(tmux_event_fd,
			     short, void *);
static void		 client_win32_resize_timer_start(void);
static void		 client_win32_resize_timer_stop(void);
static void		 client_win32_input_start(void);
static void		 client_win32_input_stop(void);
static void		 client_win32_output_callback(void *);
static void		 client_win32_output_error_callback(void *);
static int		 client_win32_output_start(void);
static void		 client_win32_output_stop(void);
static void		 client_win32_tty_output(char *, ssize_t);
static void		 client_restore_terminal(void);
#endif
static void		 client_send_identify(const char *, const char *,
			     char **, u_int, const char *, int);
static void		 client_signal(int);
static void		 client_dispatch(struct imsg *, void *);
static void		 client_dispatch_attached(struct imsg *);
static void		 client_dispatch_wait(struct imsg *);
static const char	*client_exit_message(void);

#ifndef TMUX_WIN32
/*
 * Get server create lock. If already held then server start is happening in
 * another client, so block until the lock is released and return -2 to
 * retry. Return -1 on failure to continue and start the server anyway.
 */
static int
client_get_lock(char *lockfile)
{
	int lockfd;

	log_debug("lock file is %s", lockfile);

	if ((lockfd = open(lockfile, O_WRONLY|O_CREAT, 0600)) == -1) {
		log_debug("open failed: %s", strerror(errno));
		return (-1);
	}

	if (flock(lockfd, LOCK_EX|LOCK_NB) == -1) {
		log_debug("flock failed: %s", strerror(errno));
		if (errno != EAGAIN)
			return (lockfd);
		while (flock(lockfd, LOCK_EX) == -1 && errno == EINTR)
			/* nothing */;
		close(lockfd);
		return (-2);
	}
	log_debug("flock succeeded");

	return (lockfd);
}
#endif

/* Connect client to server. */
static int
client_connect(struct event_base *base, const char *path, uint64_t flags)
{
#ifdef TMUX_WIN32
	char	*cause = NULL;
	int	 fd, i, saved_errno;
	HANDLE	 startup_lock;

	fd = win32_ipc_client_connect(path, flags, &cause);
	if (fd != -1) {
		setblocking(fd, 0);
		return (fd);
	}
	if (cause != NULL) {
		log_debug("%s", cause);
		free(cause);
		cause = NULL;
	}
	if (flags & CLIENT_NOSTARTSERVER)
		return (-1);
	if (~flags & CLIENT_STARTSERVER)
		return (-1);
	if (flags & CLIENT_NOFORK)
		return (server_start(client_proc, flags, base, -1, NULL));
	startup_lock = win32_ipc_startup_lock(path, &cause);
	if (startup_lock == NULL) {
		if (cause != NULL) {
			log_debug("%s", cause);
			free(cause);
		}
		return (-1);
	}

	/*
	 * Another client may have started the server while this client was
	 * waiting for the startup lock.
	 */
	fd = win32_ipc_client_connect(path, flags, &cause);
	if (fd != -1) {
		win32_ipc_startup_unlock(startup_lock);
		setblocking(fd, 0);
		return (fd);
	}
	if (cause != NULL) {
		log_debug("%s", cause);
		free(cause);
		cause = NULL;
	}
	saved_errno = errno;
	if (saved_errno != ENOENT && saved_errno != ECONNREFUSED) {
		win32_ipc_startup_unlock(startup_lock);
		errno = saved_errno;
		return (-1);
	}

	if (win32_server_spawn(path, flags, &cause) != 0) {
		if (cause != NULL) {
			log_debug("%s", cause);
			free(cause);
		}
		win32_ipc_startup_unlock(startup_lock);
		return (-1);
	}
	for (i = 0; i < 100; i++) {
		fd = win32_ipc_client_connect(path, flags, &cause);
		if (fd != -1) {
			win32_ipc_startup_unlock(startup_lock);
			setblocking(fd, 0);
			return (fd);
		}
		if (cause != NULL) {
			log_debug("%s", cause);
			free(cause);
			cause = NULL;
		}
		saved_errno = errno;
		if (saved_errno != ENOENT && saved_errno != ECONNREFUSED) {
			win32_ipc_startup_unlock(startup_lock);
			errno = saved_errno;
			return (-1);
		}
		Sleep(50);
	}
	win32_ipc_startup_unlock(startup_lock);
	errno = ETIMEDOUT;
	return (-1);
#else
	struct sockaddr_un	sa;
	size_t			size;
	int			fd, lockfd = -1, locked = 0;
	char		       *lockfile = NULL;

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	size = strlcpy(sa.sun_path, path, sizeof sa.sun_path);
	if (size >= sizeof sa.sun_path) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	log_debug("socket is %s", path);

retry:
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return (-1);

	log_debug("trying connect");
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == -1) {
		log_debug("connect failed: %s", strerror(errno));
		if (errno != ECONNREFUSED && errno != ENOENT)
			goto failed;
		if (flags & CLIENT_NOSTARTSERVER)
			goto failed;
		if (~flags & CLIENT_STARTSERVER)
			goto failed;
		close(fd);

		if (!locked) {
			xasprintf(&lockfile, "%s.lock", path);
			if ((lockfd = client_get_lock(lockfile)) < 0) {
				log_debug("didn't get lock (%d)", lockfd);

				free(lockfile);
				lockfile = NULL;

				if (lockfd == -2)
					goto retry;
			}
			log_debug("got lock (%d)", lockfd);

			/*
			 * Always retry at least once, even if we got the lock,
			 * because another client could have taken the lock,
			 * started the server and released the lock between our
			 * connect() and flock().
			 */
			locked = 1;
			goto retry;
		}

		if (lockfd >= 0 && unlink(path) != 0 && errno != ENOENT) {
			free(lockfile);
			close(lockfd);
			return (-1);
		}
		fd = server_start(client_proc, flags, base, lockfd, lockfile);
	}

	if (locked && lockfd >= 0) {
		free(lockfile);
		close(lockfd);
	}
	setblocking(fd, 0);
	return (fd);

failed:
	if (locked) {
		free(lockfile);
		close(lockfd);
	}
	close(fd);
	return (-1);
#endif
}

/* Get exit string from reason number. */
const char *
client_exit_message(void)
{
	static char msg[256];

	switch (client_exitreason) {
	case CLIENT_EXIT_NONE:
		break;
	case CLIENT_EXIT_DETACHED:
		if (client_exitsession != NULL) {
			xsnprintf(msg, sizeof msg, "detached "
			    "(from session %s)", client_exitsession);
			return (msg);
		}
		return ("detached");
	case CLIENT_EXIT_DETACHED_HUP:
		if (client_exitsession != NULL) {
			xsnprintf(msg, sizeof msg, "detached and SIGHUP "
			    "(from session %s)", client_exitsession);
			return (msg);
		}
		return ("detached and SIGHUP");
	case CLIENT_EXIT_LOST_TTY:
		return ("lost tty");
	case CLIENT_EXIT_TERMINATED:
		return ("terminated");
	case CLIENT_EXIT_LOST_SERVER:
		return ("server exited unexpectedly");
	case CLIENT_EXIT_EXITED:
		return ("exited");
	case CLIENT_EXIT_SERVER_EXITED:
		return ("server exited");
	case CLIENT_EXIT_MESSAGE_PROVIDED:
		return (client_exitmessage);
	}
	return ("unknown reason");
}

/* Exit if all streams flushed. */
static void
client_exit(void)
{
#ifdef TMUX_WIN32
	if (client_win32_output_pending != 0)
		return;
#endif
	if (!file_write_left(&client_files))
		proc_exit(client_proc);
}

#ifdef TMUX_WIN32
static void
client_win32_resize_timer_callback(__unused tmux_event_fd fd,
    __unused short events, __unused void *arg)
{
	struct timeval	tv = { .tv_usec = 250000 };
	struct msg_win32_terminal_size size;
	u_int		sx, sy, xpixel, ypixel;
	long		ctrl_c;

	ctrl_c = win32_console_ctrl_c_events();
	if (ctrl_c != 0) {
		log_debug("%s: ignored %ld CTRL_C_EVENT%s", __func__, ctrl_c,
		    ctrl_c == 1 ? "" : "s");
	}

	if (client_peer != NULL &&
	    client_attached &&
	    !client_exitflag &&
	    win32_terminal_get_size(NULL, &sx, &sy, &xpixel, &ypixel) == 0 &&
	    (sx != client_win32_resize_sx || sy != client_win32_resize_sy)) {
		log_debug("%s: console size is now %ux%u", __func__, sx, sy);
		client_win32_resize_sx = sx;
		client_win32_resize_sy = sy;
		size.sx = sx;
		size.sy = sy;
		size.xpixel = xpixel;
		size.ypixel = ypixel;
		proc_send(client_peer, MSG_WIN32_TTY_RESIZE, -1, &size,
		    sizeof size);
	}

	if (!client_exitflag)
		evtimer_add(&client_win32_resize_timer, &tv);
}

static void
client_win32_resize_timer_start(void)
{
	struct timeval	tv = { .tv_usec = 250000 };
	u_int		sx, sy, xpixel, ypixel;

	if (client_win32_resize_timer_set)
		return;
	if (!client_is_console || (client_flags & CLIENT_CONTROL))
		return;

	if (win32_terminal_get_size(NULL, &sx, &sy, &xpixel, &ypixel) == 0) {
		client_win32_resize_sx = sx;
		client_win32_resize_sy = sy;
	}
	evtimer_set(&client_win32_resize_timer,
	    client_win32_resize_timer_callback, NULL);
	evtimer_add(&client_win32_resize_timer, &tv);
	client_win32_resize_timer_set = 1;
}

static void
client_win32_resize_timer_stop(void)
{
	if (!client_win32_resize_timer_set)
		return;
	evtimer_del(&client_win32_resize_timer);
	client_win32_resize_timer_set = 0;
}

static void
client_win32_input_callback(__unused void *arg)
{
	struct evbuffer	*input = NULL;
	size_t		 size, left, nsend;
	u_char		*data;

	if (client_peer == NULL || client_win32_input == NULL)
		return;

	input = evbuffer_new();
	if (input == NULL)
		fatalx("out of memory");
	win32_handle_event_drain(client_win32_input, input);
	size = EVBUFFER_LENGTH(input);
	data = EVBUFFER_DATA(input);
	log_debug("%s: forwarding %zu bytes", __func__, size);
	left = size;
	while (left != 0) {
		nsend = left;
		if (nsend > MAX_IMSGSIZE - IMSG_HEADER_SIZE)
			nsend = MAX_IMSGSIZE - IMSG_HEADER_SIZE;
		if (proc_send(client_peer, MSG_WIN32_TTY_INPUT, -1, data,
		    nsend) != 0)
			break;
		data += nsend;
		left -= nsend;
	}
	evbuffer_free(input);
}

static void
client_win32_input_error_callback(__unused void *arg)
{
	log_debug("%s: console input closed", __func__);
	if (client_attached && client_peer != NULL) {
		client_exitreason = CLIENT_EXIT_LOST_TTY;
		client_exitval = 1;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
	}
}

static void
client_win32_input_start(void)
{
	HANDLE	hin;

	if (client_win32_input != NULL)
		return;
	if (!client_is_console || (client_flags & CLIENT_CONTROL))
		return;

	hin = GetStdHandle(STD_INPUT_HANDLE);
	client_win32_input = win32_handle_event_new(hin,
	    client_win32_input_callback, client_win32_input_error_callback,
	    NULL);
	if (client_win32_input == NULL)
		log_debug("%s: couldn't create console input event", __func__);
}

static void
client_win32_input_stop(void)
{
	if (client_win32_input == NULL)
		return;
	win32_handle_event_free(client_win32_input);
	client_win32_input = NULL;
}

static void
client_win32_output_callback(__unused void *arg)
{
	struct msg_win32_tty_output_ack	ack;

	if (client_win32_output_pending == 0)
		return;
	ack.size = client_win32_output_pending;
	client_win32_output_pending = 0;
	if (client_peer != NULL) {
		proc_send(client_peer, MSG_WIN32_TTY_OUTPUT_ACK, -1, &ack,
		    sizeof ack);
	}
	if (client_exitflag)
		client_exit();
}

static void
client_win32_output_error_callback(__unused void *arg)
{
	log_debug("%s: console output failed", __func__);
	client_win32_output_pending = 0;
	client_exitreason = CLIENT_EXIT_LOST_TTY;
	client_exitval = 1;
	if (client_peer != NULL)
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
	if (client_exitflag)
		client_exit();
}

static int
client_win32_output_start(void)
{
	HANDLE	hout;

	if (client_win32_output != NULL)
		return (0);
	if (!client_console_ready)
		return (-1);

	hout = GetStdHandle(STD_OUTPUT_HANDLE);
	client_win32_output = win32_handle_writer_new_borrowed(&hout,
	    client_win32_output_callback, client_win32_output_error_callback,
	    NULL);
	if (client_win32_output == NULL) {
		log_debug("%s: couldn't create console output writer",
		    __func__);
		return (-1);
	}
	return (0);
}

static void
client_win32_output_stop(void)
{
	if (client_win32_output == NULL)
		return;
	win32_handle_writer_free(client_win32_output);
	client_win32_output = NULL;
	client_win32_output_pending = 0;
}

static void
client_win32_tty_output(char *data, ssize_t datalen)
{
	struct msg_win32_tty_output_ack ack;

	if (datalen < 0 || datalen > UINT32_MAX) {
		client_exitreason = CLIENT_EXIT_LOST_TTY;
		client_exitval = 1;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		return;
	}
	ack.size = datalen;

	if (!client_console_ready) {
		proc_send(client_peer, MSG_WIN32_TTY_OUTPUT_ACK, -1, &ack,
		    sizeof ack);
		return;
	}
	if (datalen == 0) {
		proc_send(client_peer, MSG_WIN32_TTY_OUTPUT_ACK, -1, &ack,
		    sizeof ack);
		return;
	}
	if (client_win32_output_start() != 0 ||
	    win32_handle_writer_write(client_win32_output, data,
	    datalen) == -1) {
		client_exitreason = CLIENT_EXIT_LOST_TTY;
		client_exitval = 1;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		return;
	}
	client_win32_output_pending += datalen;
}

static void
client_restore_terminal(void)
{
	client_win32_input_stop();
	client_win32_output_stop();
	client_win32_resize_timer_stop();
	if (client_console_ready) {
		win32_terminal_restore_client();
		client_console_ready = 0;
	}
}
#endif

/* Client main loop. */
int
client_main(struct event_base *base, int argc, char **argv, uint64_t flags,
    int feat)
{
	struct cmd_parse_result	*pr;
	struct msg_command	*data;
	int			 fd, i;
	const char		*ttynam, *termname, *cwd;
#ifndef TMUX_WIN32
	pid_t			 ppid;
	enum msgtype		 msg;
	struct termios		 tio, saved_tio;
#else
	enum msgtype		 msg;
#endif
	size_t			 size, linesize = 0;
	ssize_t			 linelen;
	char			*line = NULL, **caps = NULL, *cause;
	u_int			 ncaps = 0;
	struct args_value	*values;

	/* Set up the initial command. */
	if (shell_command != NULL) {
		msg = MSG_SHELL;
		flags |= CLIENT_STARTSERVER;
	} else if (argc == 0) {
		msg = MSG_COMMAND;
		flags |= CLIENT_STARTSERVER;
	} else {
		msg = MSG_COMMAND;

		/*
		 * It's annoying parsing the command string twice (in client
		 * and later in server) but it is necessary to get the start
		 * server flag.
		 */
		values = args_from_vector(argc, argv);
		pr = cmd_parse_from_arguments(values, argc, NULL);
		if (pr->status == CMD_PARSE_SUCCESS) {
			if (cmd_list_any_have(pr->cmdlist, CMD_STARTSERVER))
				flags |= CLIENT_STARTSERVER;
			cmd_list_free(pr->cmdlist);
		} else
			free(pr->error);
		args_free_values(values, argc);
		free(values);
	}

	/* Create client process structure (starts logging). */
	client_proc = proc_start("client");
	proc_set_signals(client_proc, client_signal);

	/* Save the flags. */
	client_flags = flags;
	log_debug("flags are %#llx", (unsigned long long)client_flags);

	/* Initialize the client socket and start the server. */
#ifdef HAVE_SYSTEMD
	if (systemd_activated()) {
		/* socket-based activation, do not even try to be a client. */
		fd = server_start(client_proc, flags, base, 0, NULL);
	} else
#endif
	fd = client_connect(base, socket_path, client_flags);
	if (fd == -1) {
		if (errno == ECONNREFUSED) {
			fprintf(stderr, "no server running on %s\n",
			    socket_path);
		} else {
			fprintf(stderr, "error connecting to %s (%s)\n",
			    socket_path, strerror(errno));
		}
		return (1);
	}
	client_peer = proc_add_peer(client_proc, fd, client_dispatch, NULL);

	/* Save these before pledge(). */
	if ((cwd = find_cwd()) == NULL)
#ifdef TMUX_WIN32
		cwd = win32_default_cwd();
#else
	if ((cwd = find_home()) == NULL)
		cwd = "/";
#endif
#ifdef TMUX_WIN32
	ttynam = "";
#else
	if ((ttynam = ttyname(STDIN_FILENO)) == NULL)
		ttynam = "";
#endif
	if ((termname = getenv("TERM")) == NULL)
		termname = "";
#ifdef TMUX_WIN32
	(void)win32_terminal_prepare_terminfo();
	client_is_console = win32_terminal_is_client_console();
	if (client_is_console &&
	    (*termname == '\0' || strcmp(termname, "dumb") == 0))
		termname = "xterm-256color";
#endif

	/*
	 * Drop privileges for client. "proc exec" is needed for -c and for
	 * locking (which uses system(3)).
	 *
	 * "tty" is needed to restore termios(4) and also for some reason -CC
	 * does not work properly without it (input is not recognised).
	 *
	 * "sendfd" is dropped later in client_dispatch_wait().
	 */
	if (pledge(
	    "stdio rpath wpath cpath unix sendfd proc exec tty",
	    NULL) != 0)
		fatal("pledge failed");

	/* Load terminfo entry if any. */
#ifdef TMUX_WIN32
	if (*termname != '\0' &&
	    tty_term_read_list(termname, -1, &caps, &ncaps, &cause) != 0) {
#else
	if (isatty(STDIN_FILENO) &&
	    *termname != '\0' &&
	    tty_term_read_list(termname, STDIN_FILENO, &caps, &ncaps,
	    &cause) != 0) {
#endif
		fprintf(stderr, "%s\n", cause);
		free(cause);
		return (1);
	}

	/* Free stuff that is not used in the client. */
	if (ptm_fd != -1)
		close(ptm_fd);
	options_free(global_options);
	options_free(global_s_options);
	options_free(global_w_options);
	environ_free(global_environ);

	/* Set up control mode. */
#ifndef TMUX_WIN32
	if (client_flags & CLIENT_CONTROLCONTROL) {
		if (tcgetattr(STDIN_FILENO, &saved_tio) != 0) {
			fprintf(stderr, "tcgetattr failed: %s\n",
			    strerror(errno));
			return (1);
		}
		cfmakeraw(&tio);
		tio.c_iflag = ICRNL|IXANY;
		tio.c_oflag = OPOST|ONLCR;
#ifdef NOKERNINFO
		tio.c_lflag = NOKERNINFO;
#endif
		tio.c_cflag = CREAD|CS8|HUPCL;
		tio.c_cc[VMIN] = 1;
		tio.c_cc[VTIME] = 0;
		cfsetispeed(&tio, cfgetispeed(&saved_tio));
		cfsetospeed(&tio, cfgetospeed(&saved_tio));
		tcsetattr(STDIN_FILENO, TCSANOW, &tio);
	}
#else
	if (client_is_console && !(client_flags & CLIENT_CONTROL)) {
		if (win32_terminal_init_client(&cause) != 0) {
			fprintf(stderr, "%s\n", cause);
			free(cause);
			return (1);
		}
		client_console_ready = 1;
	}
	client_win32_resize_timer_start();
#endif

	/* Send identify messages. */
	client_send_identify(ttynam, termname, caps, ncaps, cwd, feat);
	tty_term_free_list(caps, ncaps);
	proc_flush_peer(client_peer);

	/* Send first command. */
	if (msg == MSG_COMMAND) {
		/* How big is the command? */
		size = 0;
		for (i = 0; i < argc; i++)
			size += strlen(argv[i]) + 1;
		if (size > MAX_IMSGSIZE - (sizeof *data)) {
#ifdef TMUX_WIN32
			client_restore_terminal();
#endif
			fprintf(stderr, "command too long\n");
			return (1);
		}
		data = xmalloc((sizeof *data) + size);

		/* Prepare command for server. */
		data->argc = argc;
		if (cmd_pack_argv(argc, argv, (char *)(data + 1), size) != 0) {
#ifdef TMUX_WIN32
			client_restore_terminal();
#endif
			fprintf(stderr, "command too long\n");
			free(data);
			return (1);
		}
		size += sizeof *data;

		/* Send the command. */
		if (proc_send(client_peer, msg, -1, data, size) != 0) {
#ifdef TMUX_WIN32
			client_restore_terminal();
#endif
			fprintf(stderr, "failed to send command\n");
			free(data);
			return (1);
		}
		free(data);
	} else if (msg == MSG_SHELL)
		proc_send(client_peer, msg, -1, NULL, 0);

	/* Start main loop. */
	proc_loop(client_proc, NULL);

	/* Run command if user requested exec, instead of exiting. */
	if (client_exittype == MSG_EXEC) {
#ifndef TMUX_WIN32
		if (client_flags & CLIENT_CONTROLCONTROL)
			tcsetattr(STDOUT_FILENO, TCSAFLUSH, &saved_tio);
#endif
		client_exec(client_execshell, client_execcmd);
	}

#ifdef TMUX_WIN32
	client_restore_terminal();
#endif

	/* Restore streams to blocking. */
	setblocking(STDIN_FILENO, 1);
	setblocking(STDOUT_FILENO, 1);
	setblocking(STDERR_FILENO, 1);

	/* Print the exit message, if any, and exit. */
	if (client_attached) {
		if (client_exitreason != CLIENT_EXIT_NONE)
			printf("[%s]\n", client_exit_message());

#ifndef TMUX_WIN32
		ppid = getppid();
		if (client_exittype == MSG_DETACHKILL && ppid > 1)
			kill(ppid, SIGHUP);
#endif
	} else if (client_flags & CLIENT_CONTROL) {
		if (client_exitreason != CLIENT_EXIT_NONE)
			printf("%%exit %s\n", client_exit_message());
		else
			printf("%%exit\n");
		fflush(stdout);
		if (client_flags & CLIENT_CONTROL_WAITEXIT) {
			setvbuf(stdin, NULL, _IOLBF, 0);
			for (;;) {
				linelen = getline(&line, &linesize, stdin);
				if (linelen <= 1)
					break;
			}
			free(line);
		}
		if (client_flags & CLIENT_CONTROLCONTROL) {
			printf("\033\\");
			fflush(stdout);
#ifndef TMUX_WIN32
			tcsetattr(STDOUT_FILENO, TCSAFLUSH, &saved_tio);
#endif
		}
	} else if (client_exitreason != CLIENT_EXIT_NONE)
		fprintf(stderr, "%s\n", client_exit_message());
	return (client_exitval);
}

/* Send identify messages to server. */
static void
client_send_identify(const char *ttynam, const char *termname, char **caps,
    u_int ncaps, const char *cwd, int feat)
{
	char	**ss;
	size_t	  sslen;
#ifdef TMUX_WIN32
	struct msg_win32_terminal_size size;
#else
	int	  fd;
#endif
	uint64_t  flags = client_flags;
	pid_t	  pid;
	u_int	  i;

	proc_send(client_peer, MSG_IDENTIFY_LONGFLAGS, -1, &flags, sizeof flags);
	proc_send(client_peer, MSG_IDENTIFY_LONGFLAGS, -1, &client_flags,
	    sizeof client_flags);

	proc_send(client_peer, MSG_IDENTIFY_TERM, -1, termname,
	    strlen(termname) + 1);
	proc_send(client_peer, MSG_IDENTIFY_FEATURES, -1, &feat, sizeof feat);

	proc_send(client_peer, MSG_IDENTIFY_TTYNAME, -1, ttynam,
	    strlen(ttynam) + 1);
	proc_send(client_peer, MSG_IDENTIFY_CWD, -1, cwd, strlen(cwd) + 1);

	for (i = 0; i < ncaps; i++) {
		proc_send(client_peer, MSG_IDENTIFY_TERMINFO, -1,
		    caps[i], strlen(caps[i]) + 1);
	}

#ifdef TMUX_WIN32
	if (client_is_console && !(client_flags & CLIENT_CONTROL)) {
		if (win32_terminal_get_size(NULL, &size.sx, &size.sy,
		    &size.xpixel, &size.ypixel) != 0) {
			size.sx = 80;
			size.sy = 24;
			size.xpixel = 0;
			size.ypixel = 0;
		}
		proc_send(client_peer, MSG_IDENTIFY_WIN32_TERMINAL, -1,
		    &size, sizeof size);
	}
#else
	if ((fd = dup(STDIN_FILENO)) == -1)
		fatal("dup failed");
	proc_send(client_peer, MSG_IDENTIFY_STDIN, fd, NULL, 0);
	if ((fd = dup(STDOUT_FILENO)) == -1)
		fatal("dup failed");
	proc_send(client_peer, MSG_IDENTIFY_STDOUT, fd, NULL, 0);
#endif

	pid = getpid();
	proc_send(client_peer, MSG_IDENTIFY_CLIENTPID, -1, &pid, sizeof pid);

	for (ss = environ; *ss != NULL; ss++) {
		sslen = strlen(*ss) + 1;
		if (sslen > MAX_IMSGSIZE - IMSG_HEADER_SIZE)
			continue;
		proc_send(client_peer, MSG_IDENTIFY_ENVIRON, -1, *ss, sslen);
	}

	proc_send(client_peer, MSG_IDENTIFY_DONE, -1, NULL, 0);
}

/* Run command in shell; used for -c. */
static __dead void
client_exec(const char *shell, const char *shellcmd)
{
	char	*argv0;

	log_debug("shell %s, command %s", shell, shellcmd);
	argv0 = shell_argv0(shell, !!(client_flags & CLIENT_LOGIN));
	setenv("SHELL", shell, 1);

	proc_clear_signals(client_proc, 1);

#ifdef TMUX_WIN32
	client_restore_terminal();
#endif

	setblocking(STDIN_FILENO, 1);
	setblocking(STDOUT_FILENO, 1);
	setblocking(STDERR_FILENO, 1);
	closefrom(STDERR_FILENO + 1);

	execl(shell, argv0, "-c", shellcmd, (char *) NULL);
	fatal("execl failed");
}

/* Callback to handle signals in the client. */
static void
client_signal(int sig)
{
#ifdef TMUX_WIN32
	(void)sig;
#else
	struct sigaction sigact;
	int		 status;
	pid_t		 pid;

	log_debug("%s: %s", __func__, strsignal(sig));
	if (sig == SIGCHLD) {
		for (;;) {
			pid = waitpid(WAIT_ANY, &status, WNOHANG);
			if (pid == 0)
				break;
			if (pid == -1) {
				if (errno == ECHILD)
					break;
				log_debug("waitpid failed: %s",
				    strerror(errno));
			}
		}
	} else if (!client_attached) {
		if (sig == SIGTERM || sig == SIGHUP)
			proc_exit(client_proc);
	} else {
		switch (sig) {
		case SIGHUP:
			client_exitreason = CLIENT_EXIT_LOST_TTY;
			client_exitval = 1;
			proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
			break;
		case SIGTERM:
			if (!client_suspended)
				client_exitreason = CLIENT_EXIT_TERMINATED;
			client_exitval = 1;
			proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
			break;
		case SIGWINCH:
			proc_send(client_peer, MSG_RESIZE, -1, NULL, 0);
			break;
		case SIGCONT:
			memset(&sigact, 0, sizeof sigact);
			sigemptyset(&sigact.sa_mask);
			sigact.sa_flags = SA_RESTART;
			sigact.sa_handler = SIG_IGN;
			if (sigaction(SIGTSTP, &sigact, NULL) != 0)
				fatal("sigaction failed");
			proc_send(client_peer, MSG_WAKEUP, -1, NULL, 0);
			client_suspended = 0;
			break;
		}
	}
#endif
}

/* Callback for file write error or close. */
static void
client_file_check_cb(__unused struct client *c, __unused const char *path,
    __unused int error, __unused int closed, __unused struct evbuffer *buffer,
    __unused void *data)
{
	if (client_exitflag)
		client_exit();
}

/* Callback for client read events. */
static void
client_dispatch(struct imsg *imsg, __unused void *arg)
{
	if (imsg == NULL) {
		if (!client_exitflag) {
			client_exitreason = CLIENT_EXIT_LOST_SERVER;
			client_exitval = 1;
		}
#ifdef TMUX_WIN32
		client_exitflag = 1;
		client_exit();
#else
		proc_exit(client_proc);
#endif
		return;
	}

	if (client_attached)
		client_dispatch_attached(imsg);
	else
		client_dispatch_wait(imsg);
}

/* Process an exit message. */
static void
client_dispatch_exit_message(char *data, size_t datalen)
{
	int	retval;

	if (datalen < sizeof retval && datalen != 0)
		fatalx("bad MSG_EXIT size");

	if (datalen >= sizeof retval) {
		memcpy(&retval, data, sizeof retval);
		client_exitval = retval;
	}

	if (datalen > sizeof retval) {
		datalen -= sizeof retval;
		data += sizeof retval;

		client_exitmessage = xmalloc(datalen);
		memcpy(client_exitmessage, data, datalen);
		client_exitmessage[datalen - 1] = '\0';

		client_exitreason = CLIENT_EXIT_MESSAGE_PROVIDED;
	}
}

/* Dispatch imsgs when in wait state (before MSG_READY). */
static void
client_dispatch_wait(struct imsg *imsg)
{
	char		*data;
	ssize_t		 datalen;
	static int	 pledge_applied;

	/*
	 * "sendfd" is no longer required once all of the identify messages
	 * have been sent. We know the server won't send us anything until that
	 * point (because we don't ask it to), so we can drop "sendfd" once we
	 * get the first message from the server.
	 */
	if (!pledge_applied) {
		if (pledge(
		    "stdio rpath wpath cpath unix proc exec tty",
		    NULL) != 0)
			fatal("pledge failed");
		pledge_applied = 1;
	}

	data = imsg->data;
	datalen = imsg->hdr.len - IMSG_HEADER_SIZE;

	switch (imsg->hdr.type) {
	case MSG_EXIT:
	case MSG_SHUTDOWN:
		client_dispatch_exit_message(data, datalen);
		client_exitflag = 1;
		client_exit();
		break;
	case MSG_READY:
		if (datalen != 0)
			fatalx("bad MSG_READY size");

		client_attached = 1;
#ifdef TMUX_WIN32
		client_win32_input_start();
#else
		proc_send(client_peer, MSG_RESIZE, -1, NULL, 0);
#endif
		break;
	case MSG_VERSION:
		if (datalen != 0)
			fatalx("bad MSG_VERSION size");

		fprintf(stderr, "protocol version mismatch "
		    "(client %d, server %u)\n", PROTOCOL_VERSION,
		    imsg->hdr.peerid & 0xff);
		client_exitval = 1;
		proc_exit(client_proc);
		break;
	case MSG_FLAGS:
		if (datalen != sizeof client_flags)
			fatalx("bad MSG_FLAGS string");

		memcpy(&client_flags, data, sizeof client_flags);
		log_debug("new flags are %#llx",
		    (unsigned long long)client_flags);
		break;
	case MSG_SHELL:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_SHELL string");

		client_exec(data, shell_command);
		/* NOTREACHED */
	case MSG_DETACH:
	case MSG_DETACHKILL:
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXITED:
#ifdef TMUX_WIN32
		client_exitflag = 1;
		client_exit();
#else
		proc_exit(client_proc);
#endif
		break;
	case MSG_READ_OPEN:
		file_read_open(&client_files, client_peer, imsg, 1,
		    !(client_flags & CLIENT_CONTROL), client_file_check_cb,
		    NULL);
		break;
	case MSG_READ_CANCEL:
		file_read_cancel(&client_files, imsg);
		break;
	case MSG_WRITE_OPEN:
		file_write_open(&client_files, client_peer, imsg, 1,
		    !(client_flags & CLIENT_CONTROL), client_file_check_cb,
		    NULL);
		break;
	case MSG_WRITE:
		file_write_data(&client_files, imsg);
		break;
	case MSG_WRITE_CLOSE:
		file_write_close(&client_files, imsg);
		break;
#ifdef TMUX_WIN32
	case MSG_WIN32_TTY_OUTPUT:
		client_win32_tty_output(data, datalen);
		break;
#endif
	case MSG_OLDSTDERR:
	case MSG_OLDSTDIN:
	case MSG_OLDSTDOUT:
		fprintf(stderr, "server version is too old for client\n");
		proc_exit(client_proc);
		break;
	}
}

/* Dispatch imsgs in attached state (after MSG_READY). */
static void
client_dispatch_attached(struct imsg *imsg)
{
#ifndef TMUX_WIN32
	struct sigaction	 sigact;
#endif
	char			*data;
	ssize_t			 datalen;

	data = imsg->data;
	datalen = imsg->hdr.len - IMSG_HEADER_SIZE;

	switch (imsg->hdr.type) {
	case MSG_FLAGS:
		if (datalen != sizeof client_flags)
			fatalx("bad MSG_FLAGS string");

		memcpy(&client_flags, data, sizeof client_flags);
		log_debug("new flags are %#llx",
		    (unsigned long long)client_flags);
		break;
	case MSG_DETACH:
	case MSG_DETACHKILL:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_DETACH string");

		client_exitsession = xstrdup(data);
		client_exittype = imsg->hdr.type;
		if (imsg->hdr.type == MSG_DETACHKILL)
			client_exitreason = CLIENT_EXIT_DETACHED_HUP;
		else
			client_exitreason = CLIENT_EXIT_DETACHED;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXEC:
		if (datalen == 0 || data[datalen - 1] != '\0' ||
		    strlen(data) + 1 == (size_t)datalen)
			fatalx("bad MSG_EXEC string");
		client_execcmd = xstrdup(data);
		client_execshell = xstrdup(data + strlen(data) + 1);

		client_exittype = imsg->hdr.type;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXIT:
		client_dispatch_exit_message(data, datalen);
		if (client_exitreason == CLIENT_EXIT_NONE)
			client_exitreason = CLIENT_EXIT_EXITED;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXITED:
		if (datalen != 0)
			fatalx("bad MSG_EXITED size");

#ifdef TMUX_WIN32
		client_exitflag = 1;
		client_exit();
#else
		proc_exit(client_proc);
#endif
		break;
	case MSG_SHUTDOWN:
		if (datalen != 0)
			fatalx("bad MSG_SHUTDOWN size");

		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		client_exitreason = CLIENT_EXIT_SERVER_EXITED;
		client_exitval = 1;
		break;
	case MSG_SUSPEND:
		if (datalen != 0)
			fatalx("bad MSG_SUSPEND size");

#ifdef TMUX_WIN32
		client_exitreason = CLIENT_EXIT_DETACHED;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
#else
		memset(&sigact, 0, sizeof sigact);
		sigemptyset(&sigact.sa_mask);
		sigact.sa_flags = SA_RESTART;
		sigact.sa_handler = SIG_DFL;
		if (sigaction(SIGTSTP, &sigact, NULL) != 0)
			fatal("sigaction failed");
		client_suspended = 1;
		kill(getpid(), SIGTSTP);
#endif
		break;
	case MSG_LOCK:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_LOCK string");

		system(data);
		proc_send(client_peer, MSG_UNLOCK, -1, NULL, 0);
		break;
#ifdef TMUX_WIN32
	case MSG_WIN32_TTY_OUTPUT:
		client_win32_tty_output(data, datalen);
		break;
#endif
	}
}
