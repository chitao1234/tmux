/* $OpenBSD$ */

/*
 * Copyright (c) 2009 Nicholas Marriott <nicholas.marriott@gmail.com>
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
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Open pipe to redirect pane output. If already open, close first.
 */

static enum cmd_retval	cmd_pipe_pane_exec(struct cmd *, struct cmdq_item *);

#ifdef TMUX_WIN32
struct cmd_pipe_pane_data {
	u_int	wp_id;
	struct job *job;
	struct event retry;
	int	in;
};

static void cmd_pipe_pane_job_update(struct job *);
static int cmd_pipe_pane_job_update1(struct job *, int);
static void cmd_pipe_pane_job_retry(__unused evutil_socket_t,
	     __unused short, void *);
static void cmd_pipe_pane_job_complete(struct job *);
static void cmd_pipe_pane_job_free(void *);
#else
static void cmd_pipe_pane_read_callback(struct bufferevent *, void *);
static void cmd_pipe_pane_write_callback(struct bufferevent *, void *);
static void cmd_pipe_pane_error_callback(struct bufferevent *, short, void *);
#endif

const struct cmd_entry cmd_pipe_pane_entry = {
	.name = "pipe-pane",
	.alias = "pipep",

	.args = { "IOot:", 0, 1, NULL },
	.usage = "[-IOo] " CMD_TARGET_PANE_USAGE " [shell-command]",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_pipe_pane_exec
};

static enum cmd_retval
cmd_pipe_pane_exec(struct cmd *self, struct cmdq_item *item)
{
#ifdef TMUX_WIN32
	struct args			*args = cmd_get_args(self);
	struct cmd_find_state		*target = cmdq_get_target(item);
	struct client			*tc = cmdq_get_target_client(item);
	struct window_pane		*wp = target->wp;
	struct session			*s = target->s;
	struct winlink			*wl = target->wl;
	struct window_pane_offset	*wpo = &wp->pipe_offset;
	struct format_tree		*ft;
	struct job			*job;
	struct cmd_pipe_pane_data	*cdata;
	char				*cmd;
	int				 old_pipe, in, out;

	/* Do nothing if pane is dead. */
	if (window_pane_exited(wp)) {
		cmdq_error(item, "target pane has exited");
		return (CMD_RETURN_ERROR);
	}

	/* Destroy the old pipe. */
	old_pipe = window_pane_pipe_active(wp);
	if (wp->pipe_job != NULL) {
		window_pane_close_pipe(wp);

		if (window_pane_destroy_ready(wp)) {
			server_destroy_pane(wp, 1);
			return (CMD_RETURN_NORMAL);
		}
	}

	/* If no pipe command, that is enough. */
	if (args_count(args) == 0 || *args_string(args, 0) == '\0')
		return (CMD_RETURN_NORMAL);

	/*
	 * With -o, only open the new pipe if there was no previous one. This
	 * allows a pipe to be toggled with a single key, for example:
	 *
	 *	bind ^p pipep -o 'cat >>~/output'
	 */
	if (args_has(args, 'o') && old_pipe)
		return (CMD_RETURN_NORMAL);

	/* What do we want to do? Neither -I or -O is -O. */
	if (args_has(args, 'I')) {
		in = 1;
		out = args_has(args, 'O');
	} else {
		in = 0;
		out = 1;
	}

	/* Expand the command. */
	ft = format_create(cmdq_get_client(item), item, FORMAT_NONE, 0);
	format_defaults(ft, tc, s, wl, wp);
	cmd = format_expand_time(ft, args_string(args, 0));
	format_free(ft);

	cdata = xcalloc(1, sizeof *cdata);
	cdata->wp_id = wp->id;
	cdata->in = in;

	job = job_run(cmd, 0, NULL, NULL, s, NULL, cmd_pipe_pane_job_update,
	    cmd_pipe_pane_job_complete, cmd_pipe_pane_job_free, cdata,
	    JOB_NOWAIT|JOB_KEEPWRITE, -1, -1);
	free(cmd);
	if (job == NULL) {
		cmdq_error(item, "failed to run pipe command");
		cmd_pipe_pane_job_free(cdata);
		return (CMD_RETURN_ERROR);
	}
	cdata->job = job;
	evtimer_set(&cdata->retry, cmd_pipe_pane_job_retry, cdata);

	wp->pipe_job = job;
	job_get_pid(job, &wp->pipe_pid);
	memcpy(wpo, &wp->offset, sizeof *wpo);
	if (!out)
		job_close_stdin(job);
	return (CMD_RETURN_NORMAL);
#else
	struct args			*args = cmd_get_args(self);
	struct cmd_find_state		*target = cmdq_get_target(item);
	struct client			*tc = cmdq_get_target_client(item);
	struct window_pane		*wp = target->wp;
	struct session			*s = target->s;
	struct winlink			*wl = target->wl;
	struct window_pane_offset	*wpo = &wp->pipe_offset;
	char				*cmd;
	int				 old_fd, pipe_fd[2], null_fd, in, out;
	struct format_tree		*ft;
	sigset_t			 set, oldset;

	/* Do nothing if pane is dead. */
	if (window_pane_exited(wp)) {
		cmdq_error(item, "target pane has exited");
		return (CMD_RETURN_ERROR);
	}

	/* Destroy the old pipe. */
	old_fd = wp->pipe_fd;
	if (wp->pipe_fd != -1) {
		bufferevent_free(wp->pipe_event);
		close(wp->pipe_fd);
		wp->pipe_fd = -1;

		if (window_pane_destroy_ready(wp)) {
			server_destroy_pane(wp, 1);
			return (CMD_RETURN_NORMAL);
		}
	}

	/* If no pipe command, that is enough. */
	if (args_count(args) == 0 || *args_string(args, 0) == '\0')
		return (CMD_RETURN_NORMAL);

	/*
	 * With -o, only open the new pipe if there was no previous one. This
	 * allows a pipe to be toggled with a single key, for example:
	 *
	 *	bind ^p pipep -o 'cat >>~/output'
	 */
	if (args_has(args, 'o') && old_fd != -1)
		return (CMD_RETURN_NORMAL);

	/* What do we want to do? Neither -I or -O is -O. */
	if (args_has(args, 'I')) {
		in = 1;
		out = args_has(args, 'O');
	} else {
		in = 0;
		out = 1;
	}

	/* Open the new pipe. */
	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, pipe_fd) != 0) {
		cmdq_error(item, "socketpair error: %s", strerror(errno));
		return (CMD_RETURN_ERROR);
	}

	/* Expand the command. */
	ft = format_create(cmdq_get_client(item), item, FORMAT_NONE, 0);
	format_defaults(ft, tc, s, wl, wp);
	cmd = format_expand_time(ft, args_string(args, 0));
	format_free(ft);

	/* Fork the child. */
	sigfillset(&set);
	sigprocmask(SIG_BLOCK, &set, &oldset);
	switch ((wp->pipe_pid = fork())) {
	case -1:
		sigprocmask(SIG_SETMASK, &oldset, NULL);
		cmdq_error(item, "fork error: %s", strerror(errno));

		close(pipe_fd[0]);
		close(pipe_fd[1]);
		free(cmd);
		return (CMD_RETURN_ERROR);
	case 0:
		/* Child process. */
		proc_clear_signals(server_proc, 1);
		sigprocmask(SIG_SETMASK, &oldset, NULL);
		close(pipe_fd[0]);

		if (setpgid(0, 0) == -1)
			_exit(1);

		null_fd = open(_PATH_DEVNULL, O_WRONLY);
		if (out) {
			if (dup2(pipe_fd[1], STDIN_FILENO) == -1)
				_exit(1);
		} else {
			if (dup2(null_fd, STDIN_FILENO) == -1)
				_exit(1);
		}
		if (in) {
			if (dup2(pipe_fd[1], STDOUT_FILENO) == -1)
				_exit(1);
			if (pipe_fd[1] != STDOUT_FILENO)
				close(pipe_fd[1]);
		} else {
			if (dup2(null_fd, STDOUT_FILENO) == -1)
				_exit(1);
		}
		if (dup2(null_fd, STDERR_FILENO) == -1)
			_exit(1);
		closefrom(STDERR_FILENO + 1);

		execl(_PATH_BSHELL, "sh", "-c", cmd, (char *) NULL);
		_exit(1);
	default:
		/* Parent process. */
		sigprocmask(SIG_SETMASK, &oldset, NULL);
		close(pipe_fd[1]);

		wp->pipe_fd = pipe_fd[0];
		memcpy(wpo, &wp->offset, sizeof *wpo);

		setblocking(wp->pipe_fd, 0);
		wp->pipe_event = bufferevent_new(wp->pipe_fd,
		    cmd_pipe_pane_read_callback,
		    cmd_pipe_pane_write_callback,
		    cmd_pipe_pane_error_callback,
		    wp);
		if (wp->pipe_event == NULL)
			fatalx("out of memory");
		if (out)
			bufferevent_enable(wp->pipe_event, EV_WRITE);
		if (in)
			bufferevent_enable(wp->pipe_event, EV_READ);

		free(cmd);
		return (CMD_RETURN_NORMAL);
	}
#endif
}

#ifdef TMUX_WIN32
static void
cmd_pipe_pane_job_schedule_retry(struct cmd_pipe_pane_data *cdata)
{
	struct timeval	tv = { .tv_usec = 10000 };

	if (!evtimer_initialized(&cdata->retry))
		return;
	evtimer_add(&cdata->retry, &tv);
}

static int
cmd_pipe_pane_job_update1(struct job *job, int allow_pause)
{
	struct cmd_pipe_pane_data	*cdata = job_get_data(job);
	struct window_pane		*wp;
	struct evbuffer			*evb = job_get_event(job)->input;
	size_t				 available;

	available = EVBUFFER_LENGTH(evb);
	wp = window_pane_find_by_id(cdata->wp_id);
	if (wp == NULL || wp->pipe_job != job || !cdata->in) {
		if (available != 0)
			evbuffer_drain(evb, available);
		return (0);
	}

	log_debug("%%%u pipe job read %zu", wp->id, available);
	if (available != 0) {
		if (!win32_pane_input_ready(wp) ||
		    win32_pane_write(wp, EVBUFFER_DATA(evb), available) != 0) {
			log_debug("%%%u pipe job input backpressure", wp->id);
			if (allow_pause) {
				job_set_reading(job, 0);
				cmd_pipe_pane_job_schedule_retry(cdata);
			}
			return (-1);
		}
		evbuffer_drain(evb, available);
	}
	job_set_reading(job, 1);

	if (window_pane_destroy_ready(wp))
		server_destroy_pane(wp, 1);
	return (0);
}

static void
cmd_pipe_pane_job_update(struct job *job)
{
	(void)cmd_pipe_pane_job_update1(job, 1);
}

static void
cmd_pipe_pane_job_retry(__unused evutil_socket_t fd, __unused short events,
    void *arg)
{
	struct cmd_pipe_pane_data	*cdata = arg;

	if (cdata->job == NULL)
		return;
	cmd_pipe_pane_job_update(cdata->job);
}

static void
cmd_pipe_pane_job_complete(struct job *job)
{
	struct cmd_pipe_pane_data	*cdata = job_get_data(job);
	struct window_pane		*wp;

	if (cmd_pipe_pane_job_update1(job, 0) != 0)
		log_debug("pipe job completed with undelivered pane input");

	wp = window_pane_find_by_id(cdata->wp_id);
	if (wp == NULL || wp->pipe_job != job)
		return;
	log_debug("%%%u pipe job complete", wp->id);

	wp->pipe_job = NULL;
	wp->pipe_pid = -1;
	if (window_pane_destroy_ready(wp))
		server_destroy_pane(wp, 1);
}

static void
cmd_pipe_pane_job_free(void *data)
{
	struct cmd_pipe_pane_data	*cdata = data;

	if (evtimer_initialized(&cdata->retry))
		evtimer_del(&cdata->retry);
	free(data);
}
#else
static void
cmd_pipe_pane_read_callback(__unused struct bufferevent *bufev, void *data)
{
	struct window_pane	*wp = data;
	struct evbuffer		*evb = wp->pipe_event->input;
	size_t			 available;

	available = EVBUFFER_LENGTH(evb);
	log_debug("%%%u pipe read %zu", wp->id, available);

	bufferevent_write(wp->event, EVBUFFER_DATA(evb), available);
	evbuffer_drain(evb, available);

	if (window_pane_destroy_ready(wp))
		server_destroy_pane(wp, 1);
}

static void
cmd_pipe_pane_write_callback(__unused struct bufferevent *bufev, void *data)
{
	struct window_pane	*wp = data;

	log_debug("%%%u pipe empty", wp->id);

	if (window_pane_destroy_ready(wp))
		server_destroy_pane(wp, 1);
}

static void
cmd_pipe_pane_error_callback(__unused struct bufferevent *bufev,
    __unused short what, void *data)
{
	struct window_pane	*wp = data;

	log_debug("%%%u pipe error", wp->id);

	bufferevent_free(wp->pipe_event);
	close(wp->pipe_fd);
	wp->pipe_fd = -1;

	if (window_pane_destroy_ready(wp))
		server_destroy_pane(wp, 1);
}
#endif
