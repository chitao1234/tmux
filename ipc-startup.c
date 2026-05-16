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
#include <sys/un.h>
#include <sys/file.h>
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

struct ipc_endpoint {
	char	*path;
};

struct ipc_startup_guard {
#ifdef TMUX_WIN32
	HANDLE	 handle;
	char	*lockfile;
#else
	int	 fd;
	char	*lockfile;
#endif
};

static char	*ipc_endpoint_canonicalize(const char *, char **);
static int	 ipc_server_create_backend(struct ipc_endpoint *, uint64_t,
		     char **);
static int	 ipc_server_connect_probe(struct ipc_endpoint *, char **);
static int	 ipc_server_remove_stale(struct ipc_endpoint *, char **);
static int	 ipc_server_connect_dead_errno(int);
static char	*ipc_startup_lock_path(struct ipc_endpoint *);

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
	endpoint->path = ipc_endpoint_canonicalize(path, cause);
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

#ifdef TMUX_WIN32
static char *
ipc_endpoint_canonicalize(const char *path, char **cause)
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
#else
static char *
ipc_endpoint_canonicalize(const char *path, char **cause)
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
#endif

static char *
ipc_startup_lock_path(struct ipc_endpoint *endpoint)
{
	char	*lockfile;

	xasprintf(&lockfile, "%s.lock", ipc_endpoint_path(endpoint));
	return (lockfile);
}

#ifndef TMUX_WIN32
static int
ipc_startup_guard_acquire_unix(struct ipc_endpoint *endpoint,
    struct ipc_startup_guard **guardp, char **cause)
{
	struct ipc_startup_guard	*guard;

	*guardp = NULL;
	guard = xcalloc(1, sizeof *guard);
	guard->lockfile = ipc_startup_lock_path(endpoint);
	log_debug("lock file is %s", guard->lockfile);

	guard->fd = open(guard->lockfile, O_WRONLY|O_CREAT, 0600);
	if (guard->fd == -1) {
		if (cause != NULL) {
			xasprintf(cause, "open(%s) failed: %s", guard->lockfile,
			    strerror(errno));
		}
		free(guard->lockfile);
		free(guard);
		return (-1);
	}

	if (flock(guard->fd, LOCK_EX|LOCK_NB) == -1) {
		if (errno != EAGAIN) {
			if (cause != NULL) {
				xasprintf(cause, "flock(%s) failed: %s",
				    guard->lockfile, strerror(errno));
			}
			close(guard->fd);
			free(guard->lockfile);
			free(guard);
			return (-1);
		}
		while (flock(guard->fd, LOCK_EX) == -1 && errno == EINTR)
			/* nothing */;
		close(guard->fd);
		free(guard->lockfile);
		free(guard);
		errno = EAGAIN;
		return (1);
	}
	log_debug("flock succeeded");

	*guardp = guard;
	return (0);
}
#endif

static int
ipc_server_connect_dead_errno(int error)
{
	if (error == ENOENT || error == ECONNREFUSED)
		return (1);
#ifdef TMUX_WIN32
	if (error == ETIMEDOUT)
		return (1);
#endif
	return (0);
}

#ifndef TMUX_WIN32
static int
ipc_server_connect_probe(struct ipc_endpoint *endpoint, char **cause)
{
	const char		*path = ipc_endpoint_path(endpoint);
	struct sockaddr_un	sa;
	size_t			size;
	int			fd, saved_errno;

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
		close(fd);
		errno = saved_errno;
		return (-1);
	}
	close(fd);
	return (0);
}

static int
ipc_server_create_backend(struct ipc_endpoint *endpoint, uint64_t flags,
    char **cause)
{
	const char		*path = ipc_endpoint_path(endpoint);
	struct sockaddr_un	sa;
	size_t			size;
	mode_t			mask;
	int			fd, saved_errno;

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

	return (fd);

fail:
	if (cause != NULL) {
		xasprintf(cause, "error creating %s (%s)", path, strerror(errno));
	}
	return (-1);
}

static int
ipc_server_remove_stale(struct ipc_endpoint *endpoint, char **cause)
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
#else
static int
ipc_server_connect_probe(struct ipc_endpoint *endpoint, char **cause)
{
	return (win32_ipc_client_connect(ipc_endpoint_path(endpoint), 0, cause));
}

static int
ipc_server_create_backend(struct ipc_endpoint *endpoint, uint64_t flags,
    char **cause)
{
	(void)flags;
	return (win32_ipc_server_create(ipc_endpoint_path(endpoint), cause));
}

static int
ipc_server_remove_stale(struct ipc_endpoint *endpoint, char **cause)
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
#endif

int
ipc_startup_guard_acquire(struct ipc_endpoint *endpoint,
    struct ipc_startup_guard **guardp, char **cause)
{
#ifdef TMUX_WIN32
	struct ipc_startup_guard	*guard;
	wchar_t				*wlockfile;

	*guardp = NULL;
	guard = xcalloc(1, sizeof *guard);
	guard->lockfile = ipc_startup_lock_path(endpoint);
	wlockfile = win32_utf8_to_wide(guard->lockfile);
	if (wlockfile == NULL) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't convert startup lock path: %s",
			    guard->lockfile);
		}
		free(guard->lockfile);
		free(guard);
		errno = EINVAL;
		return (-1);
	}
	guard->handle = CreateFileW(wlockfile, GENERIC_READ|GENERIC_WRITE,
	    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL,
	    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	free(wlockfile);
	if (guard->handle == INVALID_HANDLE_VALUE) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't open startup lock %s: %s",
			    guard->lockfile, win32_strerror(GetLastError()));
		}
		free(guard->lockfile);
		free(guard);
		errno = EACCES;
		return (-1);
	}
	for (;;) {
		OVERLAPPED ov = { 0 };

		if (LockFileEx(guard->handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0,
		    &ov)) {
			*guardp = guard;
			return (0);
		}
		if (GetLastError() != ERROR_LOCK_VIOLATION) {
			if (cause != NULL) {
				xasprintf(cause, "couldn't lock startup guard %s:"
				    " %s", guard->lockfile,
				    win32_strerror(GetLastError()));
			}
			CloseHandle(guard->handle);
			free(guard->lockfile);
			free(guard);
			errno = EACCES;
			return (-1);
		}
		Sleep(50);
	}
#else
	return (ipc_startup_guard_acquire_unix(endpoint, guardp, cause));
#endif
}

int
ipc_server_create(struct ipc_endpoint *endpoint, uint64_t flags, char **cause)
{
	char	*probe_cause = NULL;
	int	 fd, probe_fd;

	fd = ipc_server_create_backend(endpoint, flags, cause);
	if (fd != -1 || errno != EADDRINUSE)
		return (fd);

	probe_fd = ipc_server_connect_probe(endpoint, &probe_cause);
	if (probe_fd != -1) {
#ifdef TMUX_WIN32
		win32_ipc_close(probe_fd);
#else
		close(probe_fd);
#endif
		errno = EADDRINUSE;
		free(probe_cause);
		return (-1);
	}
	if (!ipc_server_connect_dead_errno(errno)) {
		free(probe_cause);
		errno = EADDRINUSE;
		return (-1);
	}
	free(probe_cause);

	if (ipc_server_remove_stale(endpoint, cause) != 0)
		return (-1);
	return (ipc_server_create_backend(endpoint, flags, cause));
}

void
ipc_startup_guard_finish(struct ipc_startup_guard *guard)
{
	if (guard == NULL)
		return;
#ifdef TMUX_WIN32
	if (guard->handle != NULL && guard->handle != INVALID_HANDLE_VALUE) {
		OVERLAPPED ov = { 0 };

		UnlockFileEx(guard->handle, 0, 1, 0, &ov);
		CloseHandle(guard->handle);
	}
	if (guard->lockfile != NULL)
		(void)win32_unlink_utf8(guard->lockfile);
#else
	if (guard->fd >= 0) {
		(void)unlink(guard->lockfile);
		close(guard->fd);
	}
	free(guard->lockfile);
#endif
	free(guard);
}

void
ipc_startup_guard_release(struct ipc_startup_guard *guard)
{
	if (guard == NULL)
		return;
#ifdef TMUX_WIN32
	if (guard->handle != NULL && guard->handle != INVALID_HANDLE_VALUE) {
		OVERLAPPED ov = { 0 };

		UnlockFileEx(guard->handle, 0, 1, 0, &ov);
		CloseHandle(guard->handle);
	}
	free(guard->lockfile);
#else
	if (guard->fd >= 0)
		close(guard->fd);
	free(guard->lockfile);
#endif
	free(guard);
}
