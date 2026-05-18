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
#include <sys/stat.h>
#include <sys/utsname.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"

struct options	*global_options;	/* server options */
struct options	*global_s_options;	/* session options */
struct options	*global_w_options;	/* window options */
struct environ	*global_environ;

struct timeval	 start_time;
struct ipc_endpoint *socket_endpoint;
const char	*socket_path;
int		 ptm_fd = -1;
const char	*shell_command;

static __dead void	 usage(int);
#ifdef TMUX_WIN32
static int		 win32_get_argv(int *, char ***);
#endif

static int		 areshell(const char *);
static const char	*getenv_canonical(const char *);
static const char	*getshell(void);
static size_t		 path_root_length(const char *);
#ifdef TMUX_WIN32
static char		*path_list_next(char **);
#endif

static __dead void
usage(int status)
{
	fprintf(status ? stderr : stdout,
	    "usage: %s [-2CDhlNuVv] [-c shell-command] [-f file] [-L socket-name]\n"
	    "            [-S socket-path] [-T features] [command [flags]]\n",
	    getprogname());
	exit(status);
}

#ifdef TMUX_WIN32
static int
win32_get_argv(int *argcp, char ***argvp)
{
	return (win32_wide_to_argv(GetCommandLineW(), argcp, argvp));
}
#endif

static const char *
getenv_canonical(const char *name)
{
	struct environ_entry	*envent;

#ifdef TMUX_WIN32
	if (global_environ != NULL) {
		envent = environ_find(global_environ, name);
		if (envent != NULL)
			return (envent->value);
	}
#endif
	return (getenv(name));
}

static const char *
getshell(void)
{
	struct passwd	*pw;
	const char	*shell;

	shell = getenv_canonical("SHELL");
	if (checkshell(shell))
		return (shell);

#ifdef TMUX_WIN32
	shell = win32_default_shell();
	if (checkshell(shell))
		return (shell);
#endif

	pw = getpwuid(getuid());
	if (pw != NULL && checkshell(pw->pw_shell))
		return (pw->pw_shell);

	return (_PATH_BSHELL);
}

int
checkshell(const char *shell)
{
	if (!path_is_absolute(shell))
		return (0);
	if (areshell(shell))
		return (0);
#ifdef TMUX_WIN32
	if (win32_access_utf8(shell, X_OK) != 0)
		return (0);
#else
	if (access(shell, X_OK) != 0)
		return (0);
#endif
	return (1);
}

static int
areshell(const char *shell)
{
	const char	*progname, *ptr;

	ptr = path_basename(shell);
	progname = getprogname();
	if (*progname == '-')
		progname++;
	if (strcmp(ptr, progname) == 0)
		return (1);
	return (0);
}

int
path_is_absolute(const char *path)
{
#ifdef TMUX_WIN32
	u_char	drive;
#endif

	if (path == NULL || *path == '\0')
		return (0);
	if (*path == '/')
		return (1);
#ifdef TMUX_WIN32
	if (*path == '\\')
		return (1);
	drive = (u_char)path[0];
	if (((drive >= 'A' && drive <= 'Z') ||
	    (drive >= 'a' && drive <= 'z')) &&
	    path[1] == ':' &&
	    (path[2] == '/' || path[2] == '\\'))
		return (1);
	if (path[0] == '\\' && path[1] == '\\')
		return (1);
#endif
	return (0);
}

int
path_is_drive_relative(const char *path)
{
#ifdef TMUX_WIN32
	u_char	drive;

	if (path == NULL || *path == '\0')
		return (0);
	drive = (u_char)path[0];
	if (((drive >= 'A' && drive <= 'Z') ||
	    (drive >= 'a' && drive <= 'z')) && path[1] == ':' &&
	    path[2] != '/' && path[2] != '\\')
		return (1);
#else
	(void)path;
#endif
	return (0);
}

char *
expand_path(const char *path, const char *home)
{
	char		*expanded, *name;
	const char	*end, *slash, *backslash, *value;

	if (path == NULL)
		return (NULL);
#ifdef TMUX_WIN32
	if (path[0] == '~' && (path[1] == '/' || path[1] == '\\')) {
#else
	if (strncmp(path, "~/", 2) == 0) {
#endif
		if (home == NULL)
			return (NULL);
		xasprintf(&expanded, "%s%s", home, path + 1);
		return (expanded);
	}

	if (*path == '$') {
#ifdef TMUX_WIN32
		slash = strchr(path, '/');
		backslash = strchr(path, '\\');
		if (slash == NULL || (backslash != NULL && backslash < slash))
			end = backslash;
		else
			end = slash;
#else
		end = strchr(path, '/');
#endif
		if (end == NULL)
			name = xstrdup(path + 1);
		else
			name = xstrndup(path + 1, end - path - 1);
		value = getenv_canonical(name);
		free(name);
		if (value == NULL)
			return (NULL);
		if (end == NULL)
			end = "";
		xasprintf(&expanded, "%s%s", value, end);
		return (expanded);
	}

	return (xstrdup(path));
}

void
expand_paths(const char *s, char ***paths, u_int *n, int no_realpath)
{
	const char	*home = find_home();
	char		*copy, *next, *tmp, *expanded;
	char		*path;
#ifndef TMUX_WIN32
	char		 resolved[PATH_MAX];
#endif
	u_int		 i;

	*paths = NULL;
	*n = 0;

	copy = tmp = xstrdup(s);
#ifdef TMUX_WIN32
	while ((next = path_list_next(&tmp)) != NULL) {
#else
	while ((next = strsep(&tmp, ":")) != NULL) {
#endif
		expanded = expand_path(next, home);
		if (expanded == NULL
#ifdef TMUX_WIN32
		    || path_is_drive_relative(expanded)
#endif
		    ) {
			log_debug("%s: invalid path: %s", __func__, next);
			free(expanded);
			continue;
		}
		if (no_realpath)
			path = expanded;
#ifdef TMUX_WIN32
		else
			path = expanded;
#else
		else {
			if (realpath(expanded, resolved) == NULL) {
				log_debug("%s: realpath(\"%s\") failed: %s", __func__,
			  expanded, strerror(errno));
				free(expanded);
				continue;
			}
			path = xstrdup(resolved);
			free(expanded);
		}
#endif
		for (i = 0; i < *n; i++) {
			if (strcmp(path, (*paths)[i]) == 0)
				break;
		}
		if (i != *n) {
			log_debug("%s: duplicate path: %s", __func__, path);
			free(path);
			continue;
		}
		*paths = xreallocarray(*paths, (*n) + 1, sizeof *paths);
		(*paths)[(*n)++] = path;
	}
	free(copy);
}

char *
path_join(const char *base, const char *path)
{
	char	*joined;
	size_t	 len;

	if (path == NULL)
		return (base != NULL ? xstrdup(base) : NULL);
	if (base == NULL || *base == '\0' || path_is_absolute(path))
		return (xstrdup(path));
#ifdef TMUX_WIN32
	if (path_is_drive_relative(path))
		return (NULL);
#endif

	len = strlen(base);
	if (len != 0 && (base[len - 1] == '/'
#ifdef TMUX_WIN32
	    || base[len - 1] == '\\'
#endif
	    ))
		xasprintf(&joined, "%s%s", base, path);
	else
		xasprintf(&joined, "%s/%s", base, path);
	return (joined);
}

const char *
path_basename(const char *path)
{
	const char	*ptr, *end;
	size_t		 rootlen;

	if (path == NULL || *path == '\0')
		return ("");

	rootlen = path_root_length(path);
	end = path + strlen(path);
	while ((size_t)(end - path) > rootlen &&
	    (end[-1] == '/'
#ifdef TMUX_WIN32
	    || end[-1] == '\\'
#endif
	    ))
		end--;
	if ((size_t)(end - path) == rootlen)
		return (path);

	ptr = end;
	while (ptr > path && ptr[-1] != '/'
#ifdef TMUX_WIN32
	    && ptr[-1] != '\\'
#endif
	    )
		ptr--;
	return (ptr);
}

char *
path_basename_copy(const char *path)
{
	const char	*ptr, *end;
	size_t		 rootlen;

	if (path == NULL || *path == '\0')
		return (xstrdup(""));

	rootlen = path_root_length(path);
	end = path + strlen(path);
	while ((size_t)(end - path) > rootlen &&
	    (end[-1] == '/'
#ifdef TMUX_WIN32
	    || end[-1] == '\\'
#endif
	    ))
		end--;
	if ((size_t)(end - path) == rootlen)
		return (xstrndup(path, end - path));

	ptr = end;
	while (ptr > path && ptr[-1] != '/'
#ifdef TMUX_WIN32
	    && ptr[-1] != '\\'
#endif
	    )
		ptr--;
	return (xstrndup(ptr, end - ptr));
}

char *
path_dirname(const char *path)
{
	size_t	len, rootlen;

	if (path == NULL || *path == '\0')
		return (xstrdup("."));

	rootlen = path_root_length(path);
	len = strlen(path);
	while (len > rootlen && (path[len - 1] == '/'
#ifdef TMUX_WIN32
	    || path[len - 1] == '\\'
#endif
	    ))
		len--;
	if (len == 0)
		return (xstrdup("."));
	if (rootlen != 0 && len <= rootlen)
		return (xstrndup(path, rootlen));
	while (len > rootlen && path[len - 1] != '/'
#ifdef TMUX_WIN32
	    && path[len - 1] != '\\'
#endif
	    )
		len--;
	while (len > rootlen && (path[len - 1] == '/'
#ifdef TMUX_WIN32
	    || path[len - 1] == '\\'
#endif
	    ))
		len--;
	if (len == 0) {
		if (rootlen != 0)
			return (xstrndup(path, rootlen));
		return (xstrdup("."));
	}
	if (rootlen != 0 && len < rootlen)
		len = rootlen;
	return (xstrndup(path, len));
}

char *
shell_argv0(const char *shell, int is_login)
{
	const char	*name;
	char		*argv0;

	name = path_basename(shell);
	if (is_login)
		xasprintf(&argv0, "-%s", name);
	else
		xasprintf(&argv0, "%s", name);
	return (argv0);
}

void
setblocking(int fd, int state)
{
#ifdef TMUX_WIN32
	u_long mode = state ? 0 : 1;

	ioctlsocket(win32_ipc_socket(fd), FIONBIO, &mode);
#else
	int mode;

	if ((mode = fcntl(fd, F_GETFL)) != -1) {
		if (!state)
			mode |= O_NONBLOCK;
		else
			mode &= ~O_NONBLOCK;
		fcntl(fd, F_SETFL, mode);
	}
#endif
}

uint64_t
get_timer(void)
{
	struct timespec	ts;

	/*
	 * We want a timestamp in milliseconds suitable for time measurement,
	 * so prefer the monotonic clock.
	 */
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		clock_gettime(CLOCK_REALTIME, &ts);
	return ((ts.tv_sec * 1000ULL) + (ts.tv_nsec / 1000000ULL));
}

char *
clean_name(const char *name, const char* forbid)
{
	char	*copy, *cp, *new_name;

	if (*name == '\0' || !utf8_isvalid(name))
		return (NULL);
	copy = xstrdup(name);
	for (cp = copy; *cp != '\0'; cp++) {
		if (strchr(forbid, *cp) != NULL)
			*cp = '_';
	}
	utf8_stravis(&new_name, copy, VIS_OCTAL|VIS_CSTYLE|VIS_TAB|VIS_NL);
	free(copy);
	return (new_name);
}

const char *
sig2name(int signo)
{
     static char	s[11];

#ifdef HAVE_SYS_SIGNAME
     if (signo > 0 && signo < NSIG)
	     return (sys_signame[signo]);
#endif
     xsnprintf(s, sizeof s, "%d", signo);
     return (s);
}

const char *
find_cwd(void)
{
#ifdef TMUX_WIN32
	static char	*cwd;
#else
	static char	 cwd[PATH_MAX];
#endif
	const char	*pwd;
#ifndef TMUX_WIN32
	char		 resolved1[PATH_MAX], resolved2[PATH_MAX];
#endif

#ifdef TMUX_WIN32
	free(cwd);
	cwd = win32_getcwd_utf8();
	if (cwd == NULL)
		return (NULL);
#else
	if (getcwd(cwd, sizeof cwd) == NULL)
		return (NULL);
#endif
	if ((pwd = getenv_canonical("PWD")) == NULL || *pwd == '\0')
		return (cwd);

#ifdef TMUX_WIN32
	return (cwd);
#else
	/*
	 * We want to use PWD so that symbolic links are maintained,
	 * but only if it matches the actual working directory.
	 */
	if (realpath(pwd, resolved1) == NULL)
		return (cwd);
	if (realpath(cwd, resolved2) == NULL)
		return (cwd);
	if (strcmp(resolved1, resolved2) != 0)
		return (cwd);
	return (pwd);
#endif
}

const char *
find_home(void)
{
	struct passwd		*pw;
#ifdef TMUX_WIN32
	static char		*home;
	const char		*value;
#else
	static const char	*home;
#endif

	if (home != NULL)
		return (home);

#ifdef TMUX_WIN32
	value = getenv_canonical("HOME");
	if (value != NULL && *value != '\0') {
		home = xstrdup(value);
		return (home);
	}
#else
	home = getenv_canonical("HOME");
#endif
	if (home == NULL || *home == '\0') {
		pw = getpwuid(getuid());
		if (pw != NULL) {
#ifdef TMUX_WIN32
			home = xstrdup(pw->pw_dir);
#else
			home = pw->pw_dir;
#endif
		}
		else
			home = NULL;
	}

	return (home);
}

static size_t
path_root_length(const char *path)
{
#ifdef TMUX_WIN32
	const char	*p;

	if (path == NULL || *path == '\0')
		return (0);
	if (((path[0] >= 'A' && path[0] <= 'Z') ||
	    (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') {
		if (path[2] == '/' || path[2] == '\\')
			return (3);
		return (2);
	}
	if ((path[0] == '/' || path[0] == '\\') &&
	    (path[1] == '/' || path[1] == '\\')) {
		p = path + 2;
		while (*p != '\0' && *p != '/' && *p != '\\')
			p++;
		if (*p == '\0')
			return (2);
		p++;
		while (*p != '\0' && *p != '/' && *p != '\\')
			p++;
		if (*p == '\0')
			return ((size_t)(p - path));
		return ((size_t)(p - path + 1));
	}
	if (path[0] == '/' || path[0] == '\\')
		return (1);
	return (0);
#else
	if (path != NULL && path[0] == '/')
		return (1);
	return (0);
#endif
}

#ifdef TMUX_WIN32
static char *
path_list_next(char **listp)
{
	char	*start, *p;

	if (listp == NULL || *listp == NULL)
		return (NULL);
	start = *listp;
	p = start;
	while (*p != '\0') {
		if (*p == ';' ||
		    (*p == ':' &&
		    !(p == start + 1 && isalpha((u_char)start[0])))) {
			*p++ = '\0';
			*listp = p;
			return (start);
		}
		p++;
	}
	*listp = NULL;
	return (start);
}
#endif

const char *
getversion(void)
{
	return (TMUX_VERSION);
}

int
main(int argc, char **argv)
{
	char					*path = NULL, *label = NULL;
	char					*cause;
	enum ipc_endpoint_source		 endpoint_source;
#ifndef TMUX_WIN32
	char					**var;
#endif
	const char				*s, *cwd;
	int					 opt, keys, feat = 0, fflag = 0;
	uint64_t				 flags = 0;
	const struct options_table_entry	*oe;
	u_int					 i;

	if (setlocale(LC_CTYPE, "en_US.UTF-8") == NULL &&
	    setlocale(LC_CTYPE, "C.UTF-8") == NULL) {
		if (setlocale(LC_CTYPE, "") == NULL)
			errx(1, "invalid LC_ALL, LC_CTYPE or LANG");
		s = nl_langinfo(CODESET);
		if (strcasecmp(s, "UTF-8") != 0 && strcasecmp(s, "UTF8") != 0)
			errx(1, "need UTF-8 locale (LC_CTYPE) but have %s", s);
	}

	setlocale(LC_TIME, "");
	tzset();

#ifdef TMUX_WIN32
	if (win32_get_argv(&argc, &argv) != 0)
		errx(1, "couldn't decode Windows command line");
	win32_refresh_environ();
#endif

	if (**argv == '-')
		flags = CLIENT_LOGIN;

	global_environ = environ_create();
#ifdef TMUX_WIN32
	win32_copy_environ(global_environ);
#else
	for (var = environ; *var != NULL; var++)
		environ_put(global_environ, *var, 0);
#endif
	if ((cwd = find_cwd()) != NULL)
		environ_set(global_environ, "PWD", 0, "%s", cwd);
	expand_paths(TMUX_CONF, &cfg_files, &cfg_nfiles, 1);

	while ((opt = getopt(argc, argv, "2c:CDdf:hlL:NqS:T:uUvVwW")) != -1) {
		switch (opt) {
		case '2':
			tty_add_features(&feat, "256", ":,");
			break;
		case 'c':
			shell_command = optarg;
			break;
		case 'D':
			flags |= CLIENT_NOFORK;
			break;
		case 'C':
			if (flags & CLIENT_CONTROL)
				flags |= CLIENT_CONTROLCONTROL;
			else
				flags |= CLIENT_CONTROL;
			break;
		case 'f':
			if (!fflag) {
				fflag = 1;
				for (i = 0; i < cfg_nfiles; i++)
					free(cfg_files[i]);
				cfg_nfiles = 0;
			}
			cfg_files = xreallocarray(cfg_files, cfg_nfiles + 1,
			    sizeof *cfg_files);
			cfg_files[cfg_nfiles++] = xstrdup(optarg);
			cfg_quiet = 0;
			break;
		case 'h':
			usage(0);
		case 'V':
			printf("tmux %s\n", getversion());
			exit(0);
		case 'l':
			flags |= CLIENT_LOGIN;
			break;
		case 'L':
			free(label);
			label = xstrdup(optarg);
			break;
		case 'N':
			flags |= CLIENT_NOSTARTSERVER;
			break;
		case 'q':
			break;
		case 'S':
			free(path);
			path = xstrdup(optarg);
			break;
		case 'T':
			tty_add_features(&feat, optarg, ":,");
			break;
		case 'u':
			flags |= CLIENT_UTF8;
			break;
		case 'v':
			log_add_level();
			break;
#ifdef TMUX_WIN32
		case 'w':
			flags |= CLIENT_WIN32_HELPER;
			break;
		case 'W':
			flags |= CLIENT_SPAWNEDSERVER;
			break;
#endif
		default:
			usage(1);
		}
	}
	argc -= optind;
	argv += optind;

	if (shell_command != NULL && argc != 0)
		usage(1);
	if ((flags & CLIENT_NOFORK) && argc != 0)
		usage(1);

#ifndef TMUX_WIN32
	if ((ptm_fd = getptmfd()) == -1)
		err(1, "getptmfd");
#endif
	if (pledge("stdio rpath wpath cpath flock fattr unix getpw sendfd "
	    "recvfd proc exec tty ps", NULL) != 0)
		err(1, "pledge");

	/*
	 * tmux is a UTF-8 terminal, so if TMUX is set, assume UTF-8.
	 * Otherwise, if the user has set LC_ALL, LC_CTYPE or LANG to contain
	 * UTF-8, it is a safe assumption that either they are using a UTF-8
	 * terminal, or if not they know that output from UTF-8-capable
	 * programs may be wrong.
	 */
	if (getenv_canonical("TMUX") != NULL)
		flags |= CLIENT_UTF8;
	else {
		s = getenv_canonical("LC_ALL");
		if (s == NULL || *s == '\0')
			s = getenv_canonical("LC_CTYPE");
		if (s == NULL || *s == '\0')
			s = getenv_canonical("LANG");
		if (s == NULL || *s == '\0')
			s = "";
		if (strcasestr(s, "UTF-8") != NULL ||
		    strcasestr(s, "UTF8") != NULL)
			flags |= CLIENT_UTF8;
	}
#ifdef TMUX_WIN32
	flags |= CLIENT_UTF8;
#endif

	global_options = options_create(NULL);
	global_s_options = options_create(NULL);
	global_w_options = options_create(NULL);
	for (oe = options_table; oe->name != NULL; oe++) {
		if (oe->scope & OPTIONS_TABLE_SERVER)
			options_default(global_options, oe);
		if (oe->scope & OPTIONS_TABLE_SESSION)
			options_default(global_s_options, oe);
		if (oe->scope & OPTIONS_TABLE_WINDOW)
			options_default(global_w_options, oe);
	}

	/*
	 * The default shell comes from SHELL or from the user's passwd entry
	 * if available.
	 */
	options_set_string(global_s_options, "default-shell", 0, "%s",
	    getshell());

	/* Override keys to vi if VISUAL or EDITOR are set. */
	if ((s = getenv_canonical("VISUAL")) != NULL ||
	    (s = getenv_canonical("EDITOR")) != NULL) {
		options_set_string(global_options, "editor", 0, "%s", s);
		s = path_basename(s);
		if (strstr(s, "vi") != NULL)
			keys = MODEKEY_VI;
		else
			keys = MODEKEY_EMACS;
		options_set_number(global_s_options, "status-keys", keys);
		options_set_number(global_w_options, "mode-keys", keys);
	}

	/*
	 * If socket is specified on the command-line with -S or -L, it is
	 * used. Otherwise, $TMUX is checked and if that fails "default" is
	 * used.
	 */
	endpoint_source = IPC_ENDPOINT_SOURCE_DEFAULT;
	if (path == NULL && label == NULL) {
		s = getenv_canonical("TMUX");
		if (s != NULL && *s != '\0' && *s != ',') {
			path = xstrdup(s);
			path[strcspn(path, ",")] = '\0';
			endpoint_source = IPC_ENDPOINT_SOURCE_ENV;
		}
	} else if (path != NULL)
		endpoint_source = IPC_ENDPOINT_SOURCE_CLI;
	socket_endpoint = ipc_endpoint_resolve(path, label, &flags,
	    endpoint_source, &cause);
	free(path);
	if (socket_endpoint == NULL) {
		if (cause != NULL) {
			fprintf(stderr, "%s\n", cause);
			free(cause);
		}
		exit(1);
	}
	socket_path = ipc_endpoint_path(socket_endpoint);
	free(label);

	/* Pass control to the client. */
	exit(client_main(osdep_event_init(), argc, argv, flags, feat));
}
