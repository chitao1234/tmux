/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include <sys/types.h>
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

struct ipc_endpoint {
	char				*path;
	const struct ipc_backend	*backend;
};

struct ipc_listener {
	int				 fd;
	char				*path;
	const struct ipc_backend	*backend;
};

struct ipc_coordination {
	char				*name;
	const struct ipc_backend	*backend;
#ifdef TMUX_WIN32
	HANDLE				 handle;
#else
	int				 fd;
#endif
};

struct ipc_backend {
	char	*(*canonicalize)(const char *, char **);
	int	 (*connect)(struct ipc_endpoint *, uint64_t, char **);
	int	 (*connect_dead)(int);
	int	 (*coordination_acquire)(struct ipc_endpoint *,
		    struct ipc_coordination **, char **);
	void	 (*coordination_finish)(struct ipc_coordination *);
	void	 (*coordination_release)(struct ipc_coordination *);
	int	 (*listener_create)(struct ipc_endpoint *, uint64_t,
		    struct ipc_listener **, char **);
	int	 (*remove_stale)(struct ipc_endpoint *, char **);
	void	 (*listener_destroy)(struct ipc_listener *);
	void	 (*closefd)(int);
};

#ifndef TMUX_WIN32
static char	*ipc_endpoint_canonicalize_unix(const char *, char **);
static int	 ipc_endpoint_connect_unix(struct ipc_endpoint *, uint64_t,
		    char **);
static int	 ipc_endpoint_connect_dead_unix(int);
static int	 ipc_coordination_acquire_unix(struct ipc_endpoint *,
		    struct ipc_coordination **, char **);
static void	 ipc_coordination_finish_unix(struct ipc_coordination *);
static void	 ipc_coordination_release_unix(struct ipc_coordination *);
static int	 ipc_listener_create_unix(struct ipc_endpoint *, uint64_t,
		    struct ipc_listener **, char **);
static int	 ipc_remove_stale_unix(struct ipc_endpoint *, char **);
static void	 ipc_listener_destroy_unix(struct ipc_listener *);
static void	 ipc_closefd_unix(int);
#else
static char	*ipc_endpoint_canonicalize_win32(const char *, char **);
static int	 ipc_endpoint_connect_win32(struct ipc_endpoint *, uint64_t,
		    char **);
static int	 ipc_endpoint_connect_dead_win32(int);
static int	 ipc_coordination_acquire_win32(struct ipc_endpoint *,
		    struct ipc_coordination **, char **);
static void	 ipc_coordination_finish_win32(struct ipc_coordination *);
static void	 ipc_coordination_release_win32(struct ipc_coordination *);
static int	 ipc_listener_create_win32(struct ipc_endpoint *, uint64_t,
		    struct ipc_listener **, char **);
static int	 ipc_remove_stale_win32(struct ipc_endpoint *, char **);
static void	 ipc_listener_destroy_win32(struct ipc_listener *);
static void	 ipc_closefd_win32(int);
#endif

#ifdef TMUX_WIN32
static const struct ipc_backend ipc_backend = {
	.canonicalize = ipc_endpoint_canonicalize_win32,
	.connect = ipc_endpoint_connect_win32,
	.connect_dead = ipc_endpoint_connect_dead_win32,
	.coordination_acquire = ipc_coordination_acquire_win32,
	.coordination_finish = ipc_coordination_finish_win32,
	.coordination_release = ipc_coordination_release_win32,
	.listener_create = ipc_listener_create_win32,
	.remove_stale = ipc_remove_stale_win32,
	.listener_destroy = ipc_listener_destroy_win32,
	.closefd = ipc_closefd_win32,
};
#else
static const struct ipc_backend ipc_backend = {
	.canonicalize = ipc_endpoint_canonicalize_unix,
	.connect = ipc_endpoint_connect_unix,
	.connect_dead = ipc_endpoint_connect_dead_unix,
	.coordination_acquire = ipc_coordination_acquire_unix,
	.coordination_finish = ipc_coordination_finish_unix,
	.coordination_release = ipc_coordination_release_unix,
	.listener_create = ipc_listener_create_unix,
	.remove_stale = ipc_remove_stale_unix,
	.listener_destroy = ipc_listener_destroy_unix,
	.closefd = ipc_closefd_unix,
};
#endif

struct ipc_endpoint *
ipc_endpoint_create(const char *path, char **cause)
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
	endpoint->path = endpoint->backend->canonicalize(path, cause);
	if (endpoint->path == NULL) {
		free(endpoint);
		return (NULL);
	}
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
	free(endpoint->path);
	free(endpoint);
}

int
ipc_endpoint_connect(struct ipc_endpoint *endpoint, uint64_t flags, char **cause)
{
	return (endpoint->backend->connect(endpoint, flags, cause));
}

int
ipc_endpoint_connect_dead(struct ipc_endpoint *endpoint, int error)
{
	return (endpoint->backend->connect_dead(error));
}

int
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
	int			 fd, probe_fd;

	if (listenerp != NULL)
		*listenerp = NULL;

	fd = endpoint->backend->listener_create(endpoint, flags, &listener, cause);
	if (fd != -1 || errno != EADDRINUSE)
		goto success;

	probe_fd = endpoint->backend->connect(endpoint, 0, &probe_cause);
	if (probe_fd != -1) {
		endpoint->backend->closefd(probe_fd);
		errno = EADDRINUSE;
		free(probe_cause);
		return (-1);
	}
	if (!endpoint->backend->connect_dead(errno)) {
		free(probe_cause);
		errno = EADDRINUSE;
		return (-1);
	}
	free(probe_cause);

	if (endpoint->backend->remove_stale(endpoint, cause) != 0)
		return (-1);

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

#ifndef TMUX_WIN32
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
ipc_coordination_acquire_unix(struct ipc_endpoint *endpoint,
    struct ipc_coordination **coordinationp, char **cause)
{
	struct ipc_coordination	*coordination;

	*coordinationp = NULL;

	coordination = xcalloc(1, sizeof *coordination);
	coordination->backend = &ipc_backend;
	xasprintf(&coordination->name, "%s.lock", ipc_endpoint_path(endpoint));
	log_debug("lock file is %s", coordination->name);

	coordination->fd = open(coordination->name, O_WRONLY|O_CREAT, 0600);
	if (coordination->fd == -1) {
		if (cause != NULL) {
			xasprintf(cause, "open(%s) failed: %s", coordination->name,
			    strerror(errno));
		}
		free(coordination->name);
		free(coordination);
		return (-1);
	}

	while (flock(coordination->fd, LOCK_EX) == -1) {
		if (errno != EINTR) {
			if (cause != NULL) {
				xasprintf(cause, "flock(%s) failed: %s",
				    coordination->name, strerror(errno));
			}
			close(coordination->fd);
			free(coordination->name);
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
	if (coordination->fd >= 0) {
		(void)unlink(coordination->name);
		close(coordination->fd);
	}
	free(coordination->name);
	free(coordination);
}

static void
ipc_coordination_release_unix(struct ipc_coordination *coordination)
{
	if (coordination->fd >= 0)
		close(coordination->fd);
	free(coordination->name);
	free(coordination);
}

static int
ipc_listener_create_unix(struct ipc_endpoint *endpoint, uint64_t flags,
    struct ipc_listener **listenerp, char **cause)
{
	const char		*path = ipc_endpoint_path(endpoint);
	struct sockaddr_un	 sa;
	struct ipc_listener	*listener;
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
	listener->fd = fd;
	listener->path = xstrdup(path);
	listener->backend = &ipc_backend;
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
	if (listener->fd != -1)
		(void)close(listener->fd);
	if (listener->path != NULL)
		(void)unlink(listener->path);
	free(listener->path);
	free(listener);
}

static void
ipc_closefd_unix(int fd)
{
	(void)close(fd);
}
#endif

#ifdef TMUX_WIN32
static char *
ipc_endpoint_canonicalize_win32(const char *path, char **cause)
{
	wchar_t	*wpath, *wfull = NULL;
	char	*full;
	DWORD	 n;
	u_int	 i;

	wpath = win32_utf8_to_wide(path);
	if (wpath == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't convert socket path: %s", path);
		errno = EINVAL;
		return (NULL);
	}

	n = GetFullPathNameW(wpath, 0, NULL, NULL);
	free(wpath);
	if (n == 0)
		goto fail;
	wfull = xcalloc(n, sizeof *wfull);
	wpath = win32_utf8_to_wide(path);
	if (wpath == NULL)
		goto fail;
	if (GetFullPathNameW(wpath, n, wfull, NULL) == 0) {
		free(wpath);
		goto fail;
	}
	free(wpath);

	full = win32_wide_to_utf8(wfull);
	free(wfull);
	if (full == NULL)
		goto fail;
	for (i = 0; full[i] != '\0'; i++) {
		if (full[i] == '\\')
			full[i] = '/';
	}
	while (strlen(full) > 3 && full[strlen(full) - 1] == '/')
		full[strlen(full) - 1] = '\0';
	return (full);

fail:
	free(wfull);
	if (cause != NULL && *cause == NULL) {
		xasprintf(cause, "couldn't canonicalize socket path %s: %s",
		    path, win32_strerror(GetLastError()));
	}
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
ipc_coordination_acquire_win32(struct ipc_endpoint *endpoint,
    struct ipc_coordination **coordinationp, char **cause)
{
	struct ipc_coordination	*coordination;
	wchar_t			*wname;

	*coordinationp = NULL;

	coordination = xcalloc(1, sizeof *coordination);
	coordination->backend = &ipc_backend;
	xasprintf(&coordination->name, "%s.lock", ipc_endpoint_path(endpoint));

	wname = win32_utf8_to_wide(coordination->name);
	if (wname == NULL) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't convert startup lock path: %s",
			    coordination->name);
		}
		free(coordination->name);
		free(coordination);
		errno = EINVAL;
		return (-1);
	}
	coordination->handle = CreateFileW(wname, GENERIC_READ|GENERIC_WRITE,
	    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL,
	    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	free(wname);
	if (coordination->handle == INVALID_HANDLE_VALUE) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't open startup lock %s: %s",
			    coordination->name, win32_strerror(GetLastError()));
		}
		free(coordination->name);
		free(coordination);
		errno = EACCES;
		return (-1);
	}
	for (;;) {
		OVERLAPPED	ov = { 0 };

		if (LockFileEx(coordination->handle, LOCKFILE_EXCLUSIVE_LOCK, 0,
		    1, 0, &ov)) {
			*coordinationp = coordination;
			return (0);
		}
		if (GetLastError() != ERROR_LOCK_VIOLATION) {
			if (cause != NULL) {
				xasprintf(cause, "couldn't lock startup guard %s:"
				    " %s", coordination->name,
				    win32_strerror(GetLastError()));
			}
			CloseHandle(coordination->handle);
			free(coordination->name);
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
	OVERLAPPED	ov = { 0 };

	if (coordination->handle != NULL &&
	    coordination->handle != INVALID_HANDLE_VALUE) {
		UnlockFileEx(coordination->handle, 0, 1, 0, &ov);
		CloseHandle(coordination->handle);
	}
	if (coordination->name != NULL)
		(void)win32_unlink_utf8(coordination->name);
	free(coordination->name);
	free(coordination);
}

static void
ipc_coordination_release_win32(struct ipc_coordination *coordination)
{
	OVERLAPPED	ov = { 0 };

	if (coordination->handle != NULL &&
	    coordination->handle != INVALID_HANDLE_VALUE) {
		UnlockFileEx(coordination->handle, 0, 1, 0, &ov);
		CloseHandle(coordination->handle);
	}
	free(coordination->name);
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
	free(listener->path);
	free(listener);
}

static void
ipc_closefd_win32(int fd)
{
	(void)win32_ipc_close(fd);
}
#endif
