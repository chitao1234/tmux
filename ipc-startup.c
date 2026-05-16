/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#ifndef TMUX_WIN32
#include <sys/file.h>
#include <sys/un.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

#ifdef TMUX_WIN32
#include <windows.h>
#endif

struct ipc_backend;

enum ipc_endpoint_probe_result {
	IPC_ENDPOINT_PROBE_LIVE,
	IPC_ENDPOINT_PROBE_DEAD,
	IPC_ENDPOINT_PROBE_UNKNOWN
};

enum ipc_endpoint_class {
	IPC_ENDPOINT_CLASS_MANAGED,
	IPC_ENDPOINT_CLASS_EXPLICIT
};

struct ipc_endpoint {
	char				*path;
	char				*compare_path;
	const struct ipc_backend	*backend;
	enum ipc_endpoint_source	 source;
	enum ipc_endpoint_class		 class;
};

struct ipc_listener {
	int				 fd;
	const struct ipc_backend	*backend;
	void				*data;
};

struct ipc_coordination {
	const struct ipc_backend	*backend;
	void				*data;
};

struct ipc_backend {
	char	*(*default_path)(const char *, char **);
	char	*(*canonicalize)(const char *, char **);
	int	 (*connect)(struct ipc_endpoint *, uint64_t, char **);
	int	 (*probe)(struct ipc_endpoint *, uint64_t, char **);
	int	 (*coordination_acquire)(struct ipc_endpoint *,
		    struct ipc_coordination **, char **);
	void	 (*coordination_finish)(struct ipc_coordination *);
	void	 (*coordination_release)(struct ipc_coordination *);
	int	 (*listener_create)(struct ipc_endpoint *, uint64_t,
		    struct ipc_listener **, char **);
	int	 (*remove_stale)(struct ipc_endpoint *, char **);
	void	 (*listener_destroy)(struct ipc_listener *);
	int	 (*start)(struct event_base *, struct tmuxproc *,
		    struct ipc_endpoint *, struct ipc_coordination *,
		    uint64_t, char **);
};

static void	 ipc_log_and_free_cause(char **);
static struct ipc_endpoint *ipc_endpoint_create(const char *,
		    enum ipc_endpoint_source, enum ipc_endpoint_class, char **);
static int	 ipc_endpoint_connect(struct ipc_endpoint *, uint64_t, char **);
static int	 ipc_coordination_acquire(struct ipc_endpoint *,
		    struct ipc_coordination **, char **);
static int	 ipc_endpoint_probe(struct ipc_endpoint *, uint64_t, char **);

#ifndef TMUX_WIN32
struct ipc_coordination_unix {
	char	*path;
	int	 fd;
};

struct ipc_listener_unix {
	char	*path;
};

static char	*ipc_default_path_unix(const char *, char **);
static char	*ipc_endpoint_canonicalize_unix(const char *, char **);
static int	 ipc_endpoint_connect_unix(struct ipc_endpoint *, uint64_t,
		    char **);
static int	 ipc_endpoint_connect_dead_unix(int);
static int	 ipc_endpoint_probe_unix(struct ipc_endpoint *, uint64_t,
		    char **);
static int	 ipc_coordination_acquire_unix(struct ipc_endpoint *,
		    struct ipc_coordination **, char **);
static void	 ipc_coordination_finish_unix(struct ipc_coordination *);
static void	 ipc_coordination_release_unix(struct ipc_coordination *);
static int	 ipc_listener_create_unix(struct ipc_endpoint *, uint64_t,
		    struct ipc_listener **, char **);
static int	 ipc_remove_stale_unix(struct ipc_endpoint *, char **);
static void	 ipc_listener_destroy_unix(struct ipc_listener *);
static int	 ipc_server_start_unix(struct event_base *, struct tmuxproc *,
		    struct ipc_endpoint *, struct ipc_coordination *,
		    uint64_t, char **);
#else
struct ipc_coordination_win32 {
	char	*path;
	HANDLE	 handle;
};

static char	*ipc_default_path_win32(const char *, char **);
static char	*ipc_endpoint_canonicalize_win32(const char *, char **);
static int	 ipc_endpoint_connect_win32(struct ipc_endpoint *, uint64_t,
		    char **);
static int	 ipc_endpoint_connect_dead_win32(int);
static int	 ipc_endpoint_probe_win32(struct ipc_endpoint *, uint64_t,
		    char **);
static int	 ipc_coordination_acquire_win32(struct ipc_endpoint *,
		    struct ipc_coordination **, char **);
static void	 ipc_coordination_finish_win32(struct ipc_coordination *);
static void	 ipc_coordination_release_win32(struct ipc_coordination *);
static int	 ipc_listener_create_win32(struct ipc_endpoint *, uint64_t,
		    struct ipc_listener **, char **);
static int	 ipc_remove_stale_win32(struct ipc_endpoint *, char **);
static void	 ipc_listener_destroy_win32(struct ipc_listener *);
static int	 ipc_server_start_win32(struct event_base *, struct tmuxproc *,
		    struct ipc_endpoint *, struct ipc_coordination *,
		    uint64_t, char **);
#endif

#ifdef TMUX_WIN32
static const struct ipc_backend ipc_backend = {
	.default_path = ipc_default_path_win32,
	.canonicalize = ipc_endpoint_canonicalize_win32,
	.connect = ipc_endpoint_connect_win32,
	.probe = ipc_endpoint_probe_win32,
	.coordination_acquire = ipc_coordination_acquire_win32,
	.coordination_finish = ipc_coordination_finish_win32,
	.coordination_release = ipc_coordination_release_win32,
	.listener_create = ipc_listener_create_win32,
	.remove_stale = ipc_remove_stale_win32,
	.listener_destroy = ipc_listener_destroy_win32,
	.start = ipc_server_start_win32,
};
#else
static const struct ipc_backend ipc_backend = {
	.default_path = ipc_default_path_unix,
	.canonicalize = ipc_endpoint_canonicalize_unix,
	.connect = ipc_endpoint_connect_unix,
	.probe = ipc_endpoint_probe_unix,
	.coordination_acquire = ipc_coordination_acquire_unix,
	.coordination_finish = ipc_coordination_finish_unix,
	.coordination_release = ipc_coordination_release_unix,
	.listener_create = ipc_listener_create_unix,
	.remove_stale = ipc_remove_stale_unix,
	.listener_destroy = ipc_listener_destroy_unix,
	.start = ipc_server_start_unix,
};
#endif

struct ipc_endpoint *
ipc_endpoint_resolve(const char *path, const char *label, uint64_t *flags,
    enum ipc_endpoint_source source, char **cause)
{
	struct ipc_endpoint	*endpoint;
	char			*default_path = NULL;
	enum ipc_endpoint_class	 endpoint_class;

	if (cause != NULL)
		*cause = NULL;

	if (path == NULL) {
		default_path = ipc_backend.default_path(label, cause);
		if (default_path == NULL)
			return (NULL);
		path = default_path;
		if (flags != NULL)
			*flags |= CLIENT_DEFAULTSOCKET;
		source = IPC_ENDPOINT_SOURCE_DEFAULT;
		endpoint_class = IPC_ENDPOINT_CLASS_MANAGED;
	} else
		endpoint_class = IPC_ENDPOINT_CLASS_EXPLICIT;

	endpoint = ipc_endpoint_create(path, source, endpoint_class, cause);
	free(default_path);
	return (endpoint);
}

static struct ipc_endpoint *
ipc_endpoint_create(const char *path, enum ipc_endpoint_source source,
    enum ipc_endpoint_class endpoint_class, char **cause)
{
	struct ipc_endpoint	*endpoint;

	if (cause != NULL)
		*cause = NULL;
	if (path == NULL) {
		errno = EINVAL;
		return (NULL);
	}

	endpoint = xcalloc(1, sizeof *endpoint);
	endpoint->backend = &ipc_backend;
	endpoint->source = source;
	endpoint->class = endpoint_class;
	endpoint->path = endpoint->backend->canonicalize(path, cause);
	if (endpoint->path == NULL) {
		free(endpoint);
		return (NULL);
	}
	endpoint->compare_path = xstrdup(endpoint->path);
	return (endpoint);
}

const char *
ipc_endpoint_path(struct ipc_endpoint *endpoint)
{
	if (endpoint == NULL)
		return (NULL);
	return (endpoint->path);
}

void
ipc_endpoint_free(struct ipc_endpoint *endpoint)
{
	if (endpoint == NULL)
		return;
	free(endpoint->compare_path);
	free(endpoint->path);
	free(endpoint);
}

int
ipc_client_connect_or_start(struct event_base *base, struct tmuxproc *client,
    struct ipc_endpoint *endpoint, uint64_t flags)
{
	struct ipc_coordination	*coordination = NULL;
	char			*cause = NULL;
	int			 fd;

	fd = ipc_endpoint_connect(endpoint, flags, &cause);
	if (fd != -1) {
		setblocking(fd, 0);
		return (fd);
	}
	ipc_log_and_free_cause(&cause);
	if (flags & CLIENT_NOSTARTSERVER)
		return (-1);
	if (~flags & CLIENT_STARTSERVER)
		return (-1);
	if (ipc_coordination_acquire(endpoint, &coordination, &cause) != 0) {
		ipc_log_and_free_cause(&cause);
		return (-1);
	}

	fd = ipc_endpoint_connect(endpoint, flags, &cause);
	if (fd != -1) {
		ipc_coordination_release(coordination);
		setblocking(fd, 0);
		return (fd);
	}
	ipc_log_and_free_cause(&cause);
	if (ipc_endpoint_probe(endpoint, flags, &cause) !=
	    IPC_ENDPOINT_PROBE_DEAD) {
		ipc_log_and_free_cause(&cause);
		ipc_coordination_release(coordination);
		return (-1);
	}

	fd = endpoint->backend->start(base, client, endpoint, coordination, flags,
	    &cause);
	if (fd == -1) {
		ipc_log_and_free_cause(&cause);
		return (-1);
	}
	if (fd != -1)
		setblocking(fd, 0);
	return (fd);
}

static int
ipc_endpoint_connect(struct ipc_endpoint *endpoint, uint64_t flags, char **cause)
{
	return (endpoint->backend->connect(endpoint, flags, cause));
}

static int
ipc_coordination_acquire(struct ipc_endpoint *endpoint,
    struct ipc_coordination **coordination, char **cause)
{
	return (endpoint->backend->coordination_acquire(endpoint, coordination,
	    cause));
}

int
ipc_server_create(struct ipc_endpoint *endpoint, uint64_t flags,
    struct ipc_listener **listenerp, char **cause)
{
	struct ipc_listener	*listener = NULL;
	char			*probe_cause = NULL;
	int			 fd, state;

	if (listenerp != NULL)
		*listenerp = NULL;

	fd = endpoint->backend->listener_create(endpoint, flags, &listener, cause);
	if (fd != -1 || errno != EADDRINUSE)
		goto success;

	state = ipc_endpoint_probe(endpoint, 0, &probe_cause);
	if (state == IPC_ENDPOINT_PROBE_LIVE) {
		ipc_log_and_free_cause(&probe_cause);
		errno = EADDRINUSE;
		return (-1);
	}
	if (state != IPC_ENDPOINT_PROBE_DEAD) {
		ipc_log_and_free_cause(&probe_cause);
		errno = EADDRINUSE;
		return (-1);
	}
	ipc_log_and_free_cause(&probe_cause);

	if (endpoint->backend->remove_stale(endpoint, cause) != 0)
		return (-1);
	if (cause != NULL) {
		free(*cause);
		*cause = NULL;
	}

	fd = endpoint->backend->listener_create(endpoint, flags, &listener, cause);
	if (fd == -1)
		return (-1);

success:
	if (listenerp != NULL)
		*listenerp = listener;
	else
		ipc_listener_destroy(listener);
	return (fd);
}

void
ipc_listener_destroy(struct ipc_listener *listener)
{
	if (listener == NULL)
		return;
	listener->backend->listener_destroy(listener);
}

void
ipc_coordination_finish(struct ipc_coordination *coordination)
{
	if (coordination == NULL)
		return;
	coordination->backend->coordination_finish(coordination);
}

void
ipc_coordination_release(struct ipc_coordination *coordination)
{
	if (coordination == NULL)
		return;
	coordination->backend->coordination_release(coordination);
}

static void
ipc_log_and_free_cause(char **cause)
{
	if (cause == NULL || *cause == NULL)
		return;
	log_debug("%s", *cause);
	free(*cause);
	*cause = NULL;
}

static int
ipc_endpoint_probe(struct ipc_endpoint *endpoint, uint64_t flags, char **cause)
{
	return (endpoint->backend->probe(endpoint, flags, cause));
}

#ifndef TMUX_WIN32
static char *
ipc_default_path_unix(const char *label, char **cause)
{
	char		**paths, *path, *base;
	struct stat	  sb;
	uid_t		  uid;
	u_int		  i, n;

	if (cause != NULL)
		*cause = NULL;
	if (label == NULL)
		label = "default";
	uid = getuid();

	expand_paths(TMUX_SOCK, &paths, &n, 0);
	if (n == 0) {
		if (cause != NULL)
			xasprintf(cause, "no suitable socket path");
		return (NULL);
	}
	path = paths[0];
	for (i = 1; i < n; i++)
		free(paths[i]);
	free(paths);

	xasprintf(&base, "%s/tmux-%ld", path, (long)uid);
	free(path);
	if (mkdir(base, S_IRWXU) != 0 && errno != EEXIST) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't create directory %s (%s)", base,
			    strerror(errno));
		}
		goto fail;
	}
	if (lstat(base, &sb) != 0) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't read directory %s (%s)", base,
			    strerror(errno));
		}
		goto fail;
	}
	if (!S_ISDIR(sb.st_mode)) {
		if (cause != NULL)
			xasprintf(cause, "%s is not a directory", base);
		goto fail;
	}
	if (sb.st_uid != uid || (sb.st_mode & TMUX_SOCK_PERM) != 0) {
		if (cause != NULL) {
			xasprintf(cause, "directory %s has unsafe permissions",
			    base);
		}
		goto fail;
	}
	xasprintf(&path, "%s/%s", base, label);
	free(base);
	return (path);

fail:
	free(base);
	return (NULL);
}

static char *
ipc_endpoint_canonicalize_unix(const char *path, char **cause)
{
	char		*full;
	const char	*cwd;

	if (path_is_absolute(path))
		return (xstrdup(path));

	cwd = find_cwd();
	if (cwd == NULL) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't determine cwd for socket path %s",
			    path);
		}
		errno = ENOENT;
		return (NULL);
	}
	xasprintf(&full, "%s/%s", cwd, path);
	return (full);
}

static int
ipc_endpoint_connect_unix(struct ipc_endpoint *endpoint, __unused uint64_t flags,
    char **cause)
{
	const char		*path = ipc_endpoint_path(endpoint);
	struct sockaddr_un	 sa;
	size_t			 size;
	int			 fd, saved_errno;

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	size = strlcpy(sa.sun_path, path, sizeof sa.sun_path);
	if (size >= sizeof sa.sun_path) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return (-1);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == -1) {
		saved_errno = errno;
		if (cause != NULL) {
			xasprintf(cause, "couldn't connect to %s: %s", path,
			    strerror(saved_errno));
		}
		close(fd);
		errno = saved_errno;
		return (-1);
	}
	return (fd);
}

static int
ipc_endpoint_connect_dead_unix(int error)
{
	if (error == ENOENT || error == ECONNREFUSED)
		return (1);
	return (0);
}

static int
ipc_endpoint_probe_unix(struct ipc_endpoint *endpoint, uint64_t flags,
    char **cause)
{
	char	*probe_cause = NULL;
	int	 fd, saved_errno;

	fd = ipc_endpoint_connect_unix(endpoint, flags, &probe_cause);
	if (fd != -1) {
		close(fd);
		free(probe_cause);
		return (IPC_ENDPOINT_PROBE_LIVE);
	}
	saved_errno = errno;
	if (ipc_endpoint_connect_dead_unix(saved_errno)) {
		free(probe_cause);
		return (IPC_ENDPOINT_PROBE_DEAD);
	}
	if (cause != NULL)
		*cause = probe_cause;
	else
		free(probe_cause);
	errno = saved_errno;
	return (IPC_ENDPOINT_PROBE_UNKNOWN);
}

static int
ipc_coordination_acquire_unix(struct ipc_endpoint *endpoint,
    struct ipc_coordination **coordinationp, char **cause)
{
	struct ipc_coordination		*coordination;
	struct ipc_coordination_unix	*data;

	*coordinationp = NULL;

	coordination = xcalloc(1, sizeof *coordination);
	data = xcalloc(1, sizeof *data);
	coordination->backend = &ipc_backend;
	coordination->data = data;
	data->fd = -1;
	xasprintf(&data->path, "%s.lock", ipc_endpoint_path(endpoint));
	log_debug("lock file is %s", data->path);

	data->fd = open(data->path, O_WRONLY|O_CREAT, 0600);
	if (data->fd == -1) {
		if (cause != NULL) {
			xasprintf(cause, "open(%s) failed: %s", data->path,
			    strerror(errno));
		}
		free(data->path);
		free(data);
		free(coordination);
		return (-1);
	}

	while (flock(data->fd, LOCK_EX) == -1) {
		if (errno != EINTR) {
			if (cause != NULL) {
				xasprintf(cause, "flock(%s) failed: %s",
				    data->path, strerror(errno));
			}
			close(data->fd);
			free(data->path);
			free(data);
			free(coordination);
			return (-1);
		}
	}
	log_debug("flock succeeded");

	*coordinationp = coordination;
	return (0);
}

static void
ipc_coordination_finish_unix(struct ipc_coordination *coordination)
{
	struct ipc_coordination_unix	*data = coordination->data;

	if (data->fd >= 0) {
		(void)unlink(data->path);
		close(data->fd);
	}
	free(data->path);
	free(data);
	free(coordination);
}

static void
ipc_coordination_release_unix(struct ipc_coordination *coordination)
{
	struct ipc_coordination_unix	*data = coordination->data;

	if (data->fd >= 0)
		close(data->fd);
	free(data->path);
	free(data);
	free(coordination);
}

static int
ipc_listener_create_unix(struct ipc_endpoint *endpoint, uint64_t flags,
    struct ipc_listener **listenerp, char **cause)
{
	const char		*path = ipc_endpoint_path(endpoint);
	struct sockaddr_un	 sa;
	struct ipc_listener	*listener;
	struct ipc_listener_unix *data;
	size_t			 size;
	mode_t			 mask;
	int			 fd, saved_errno;

	if (listenerp != NULL)
		*listenerp = NULL;

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	size = strlcpy(sa.sun_path, path, sizeof sa.sun_path);
	if (size >= sizeof sa.sun_path) {
		errno = ENAMETOOLONG;
		goto fail;
	}

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		goto fail;

	if (flags & CLIENT_DEFAULTSOCKET)
		mask = umask(S_IXUSR|S_IXGRP|S_IRWXO);
	else
		mask = umask(S_IXUSR|S_IRWXG|S_IRWXO);
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) == -1) {
		saved_errno = errno;
		umask(mask);
		close(fd);
		errno = saved_errno;
		goto fail;
	}
	umask(mask);

	if (listen(fd, 128) == -1) {
		saved_errno = errno;
		close(fd);
		errno = saved_errno;
		goto fail;
	}
	setblocking(fd, 0);

	listener = xcalloc(1, sizeof *listener);
	data = xcalloc(1, sizeof *data);
	listener->fd = fd;
	listener->backend = &ipc_backend;
	listener->data = data;
	data->path = xstrdup(path);
	if (listenerp != NULL)
		*listenerp = listener;
	return (fd);

fail:
	if (cause != NULL) {
		xasprintf(cause, "error creating %s (%s)", path, strerror(errno));
	}
	return (-1);
}

static int
ipc_remove_stale_unix(struct ipc_endpoint *endpoint, char **cause)
{
	const char	*path = ipc_endpoint_path(endpoint);

	if (unlink(path) == 0 || errno == ENOENT)
		return (0);
	if (cause != NULL) {
		xasprintf(cause, "couldn't remove stale endpoint %s (%s)", path,
		    strerror(errno));
	}
	return (-1);
}

static void
ipc_listener_destroy_unix(struct ipc_listener *listener)
{
	struct ipc_listener_unix	*data = listener->data;

	if (listener->fd != -1)
		(void)close(listener->fd);
	if (data->path != NULL)
		(void)unlink(data->path);
	free(data->path);
	free(data);
	free(listener);
}

static int
ipc_server_start_unix(struct event_base *base, struct tmuxproc *client,
    struct ipc_endpoint *endpoint, struct ipc_coordination *coordination,
    uint64_t flags, char **cause)
{
	(void)cause;

	return (server_start(client, flags, base, endpoint, coordination));
}
#endif

#ifdef TMUX_WIN32
static char *
ipc_default_path_win32(const char *label, char **cause)
{
	const char	*base;
	char		*path;

	if (cause != NULL)
		*cause = NULL;
	if (label == NULL)
		label = "default";

	base = win32_default_socket_dir();
	if (base == NULL) {
		if (cause != NULL)
			xasprintf(cause, "no suitable socket path");
		errno = ENOENT;
		return (NULL);
	}
	if (win32_ipc_ensure_socket_dir(base, cause) != 0)
		return (NULL);

	xasprintf(&path, "%s/%s", base, label);
	if (strlen(path) >= 100) {
		if (cause != NULL)
			xasprintf(cause, "socket path too long: %s", path);
		free(path);
		errno = ENAMETOOLONG;
		return (NULL);
	}
	return (path);
}

static char *
ipc_endpoint_canonicalize_win32(const char *path, char **cause)
{
	char		*expanded, *resolved;
	const char	*cwd;

	expanded = expand_path(path, find_home());
	if (expanded == NULL) {
		if (cause != NULL)
			xasprintf(cause, "invalid socket path: %s", path);
		errno = EINVAL;
		return (NULL);
	}
	if (path_is_drive_relative(expanded)) {
		if (cause != NULL) {
			xasprintf(cause, "socket path must not be drive-relative: "
			    "%s", expanded);
		}
		free(expanded);
		errno = EINVAL;
		return (NULL);
	}
	cwd = find_cwd();
	resolved = win32_resolve_cwd(expanded, cwd, cause);
	free(expanded);
	if (resolved != NULL)
		return (resolved);
	if (cause != NULL && *cause == NULL)
		xasprintf(cause, "couldn't canonicalize socket path: %s", path);
	errno = EINVAL;
	return (NULL);
}

static int
ipc_endpoint_connect_win32(struct ipc_endpoint *endpoint, uint64_t flags,
    char **cause)
{
	return (win32_ipc_client_connect(ipc_endpoint_path(endpoint), flags,
	    cause));
}

static int
ipc_endpoint_connect_dead_win32(int error)
{
	if (error == ENOENT || error == ECONNREFUSED || error == ETIMEDOUT)
		return (1);
	return (0);
}

static int
ipc_endpoint_probe_win32(struct ipc_endpoint *endpoint, uint64_t flags,
    char **cause)
{
	char	*probe_cause = NULL;
	int	 fd, saved_errno;

	fd = ipc_endpoint_connect_win32(endpoint, flags, &probe_cause);
	if (fd != -1) {
		win32_ipc_close(fd);
		free(probe_cause);
		return (IPC_ENDPOINT_PROBE_LIVE);
	}
	saved_errno = errno;
	if (ipc_endpoint_connect_dead_win32(saved_errno)) {
		free(probe_cause);
		return (IPC_ENDPOINT_PROBE_DEAD);
	}
	if (cause != NULL)
		*cause = probe_cause;
	else
		free(probe_cause);
	errno = saved_errno;
	return (IPC_ENDPOINT_PROBE_UNKNOWN);
}

static int
ipc_coordination_acquire_win32(struct ipc_endpoint *endpoint,
    struct ipc_coordination **coordinationp, char **cause)
{
	struct ipc_coordination		*coordination;
	struct ipc_coordination_win32	*data;
	wchar_t				*wname;

	*coordinationp = NULL;

	coordination = xcalloc(1, sizeof *coordination);
	data = xcalloc(1, sizeof *data);
	coordination->backend = &ipc_backend;
	coordination->data = data;
	data->handle = INVALID_HANDLE_VALUE;
	xasprintf(&data->path, "%s.lock", ipc_endpoint_path(endpoint));

	wname = win32_utf8_to_wide(data->path);
	if (wname == NULL) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't convert startup lock path: %s",
			    data->path);
		}
		free(data->path);
		free(data);
		free(coordination);
		errno = EINVAL;
		return (-1);
	}
	data->handle = CreateFileW(wname, GENERIC_READ|GENERIC_WRITE,
	    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL,
	    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	free(wname);
	if (data->handle == INVALID_HANDLE_VALUE) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't open startup lock %s: %s",
			    data->path, win32_strerror(GetLastError()));
		}
		free(data->path);
		free(data);
		free(coordination);
		errno = EACCES;
		return (-1);
	}
	for (;;) {
		OVERLAPPED	ov = { 0 };

		if (LockFileEx(data->handle, LOCKFILE_EXCLUSIVE_LOCK, 0,
		    1, 0, &ov)) {
			*coordinationp = coordination;
			return (0);
		}
		if (GetLastError() != ERROR_LOCK_VIOLATION) {
			if (cause != NULL) {
				xasprintf(cause, "couldn't lock startup guard %s:"
				    " %s", data->path,
				    win32_strerror(GetLastError()));
			}
			CloseHandle(data->handle);
			free(data->path);
			free(data);
			free(coordination);
			errno = EACCES;
			return (-1);
		}
		Sleep(50);
	}
}

static void
ipc_coordination_finish_win32(struct ipc_coordination *coordination)
{
	struct ipc_coordination_win32	*data = coordination->data;
	OVERLAPPED	ov = { 0 };

	if (data->handle != NULL && data->handle != INVALID_HANDLE_VALUE) {
		UnlockFileEx(data->handle, 0, 1, 0, &ov);
		CloseHandle(data->handle);
	}
	if (data->path != NULL)
		(void)win32_unlink_utf8(data->path);
	free(data->path);
	free(data);
	free(coordination);
}

static void
ipc_coordination_release_win32(struct ipc_coordination *coordination)
{
	struct ipc_coordination_win32	*data = coordination->data;
	OVERLAPPED	ov = { 0 };

	if (data->handle != NULL && data->handle != INVALID_HANDLE_VALUE) {
		UnlockFileEx(data->handle, 0, 1, 0, &ov);
		CloseHandle(data->handle);
	}
	free(data->path);
	free(data);
	free(coordination);
}

static int
ipc_listener_create_win32(struct ipc_endpoint *endpoint, __unused uint64_t flags,
    struct ipc_listener **listenerp, char **cause)
{
	struct ipc_listener	*listener;
	int			 fd;

	if (listenerp != NULL)
		*listenerp = NULL;

	fd = win32_ipc_server_create(ipc_endpoint_path(endpoint), cause);
	if (fd == -1)
		return (-1);

	listener = xcalloc(1, sizeof *listener);
	listener->fd = fd;
	listener->backend = &ipc_backend;
	if (listenerp != NULL)
		*listenerp = listener;
	return (fd);
}

static int
ipc_remove_stale_win32(struct ipc_endpoint *endpoint, char **cause)
{
	const char	*path = ipc_endpoint_path(endpoint);

	if (win32_unlink_utf8(path) == 0 || errno == ENOENT)
		return (0);
	if (cause != NULL) {
		xasprintf(cause, "couldn't remove stale endpoint %s (%s)", path,
		    strerror(errno));
	}
	return (-1);
}

static void
ipc_listener_destroy_win32(struct ipc_listener *listener)
{
	if (listener->fd != -1)
		(void)win32_ipc_close(listener->fd);
	free(listener);
}

static int
ipc_server_start_win32(struct event_base *base, struct tmuxproc *client,
    struct ipc_endpoint *endpoint, struct ipc_coordination *coordination,
    uint64_t flags, char **cause)
{
	int	fd, i, saved_errno;

	if (flags & CLIENT_NOFORK)
		return (server_start(client, flags, base, endpoint, coordination));

	if (win32_server_spawn(ipc_endpoint_path(endpoint), flags, cause) != 0) {
		ipc_coordination_finish(coordination);
		return (-1);
	}
	for (i = 0; i < 100; i++) {
		fd = ipc_endpoint_connect_win32(endpoint, flags, NULL);
		if (fd != -1) {
			ipc_coordination_finish(coordination);
			return (fd);
		}
		saved_errno = errno;
		if (!ipc_endpoint_connect_dead_win32(saved_errno)) {
			if (cause != NULL) {
				xasprintf(cause, "couldn't connect to %s: %s",
				    ipc_endpoint_path(endpoint),
				    strerror(saved_errno));
			}
			ipc_coordination_finish(coordination);
			errno = saved_errno;
			return (-1);
		}
		Sleep(50);
	}
	if (cause != NULL) {
		xasprintf(cause, "timed out waiting for server startup at %s",
		    ipc_endpoint_path(endpoint));
	}
	ipc_coordination_finish(coordination);
	errno = ETIMEDOUT;
	return (-1);
}
#endif
