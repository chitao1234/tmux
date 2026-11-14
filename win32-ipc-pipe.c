/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

/*
 * Win32 IPC transport: named pipes.
 *
 * A pipe is not a socket, so libevent cannot poll it. Data travels over the
 * pipe via the Win32 I/O reactor; a small socketpair is used only as a
 * readiness signal so the existing libevent-driven imsg code keeps working.
 *
 * Peer identity is recorded for diagnostics only. Admission is still decided
 * by the existing MSG_WIN32_AUTH_* flow.
 */

#include <sys/types.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#ifdef TMUX_WIN32

#include <windows.h>

#include "win32-platform.h"

#define WIN32_IPC_PIPE_MAX_INSTANCES 64
#define WIN32_IPC_PIPE_BUFFER (64 * 1024)

struct win32_ipc_pipe_listener {
	char	*name;
	wchar_t	*wname;
	int	 first_done;
};

struct win32_ipc_pipe {
	HANDLE				 handle;
	OVERLAPPED			 connect_ov;
	int				 connect_pending;
	DWORD				 peer_pid;
	int				 connected;
	int				 eof;

	struct win32_io_endpoint	*reader;
	struct evbuffer			*input;

	SOCKET				 notify_read;
	SOCKET				 notify_write;
	int				 notify_armed;
};

static int
win32_ipc_pipe_name(const char *path, wchar_t **namep, char **name8p,
    char **cause)
{
	unsigned int	 hash = 2166136261u;
	char		 buf[64];
	size_t		 i;

	*namep = NULL;
	if (name8p != NULL)
		*name8p = NULL;
	if (path == NULL || *path == '\0') {
		if (cause != NULL)
			xasprintf(cause, "invalid transport path");
		errno = EINVAL;
		return (-1);
	}
	for (i = 0; path[i] != '\0'; i++) {
		unsigned char c = (unsigned char)path[i];

		if (c >= 'A' && c <= 'Z')
			c = c - 'A' + 'a';
		else if (c == '\\')
			c = '/';
		hash ^= c;
		hash *= 16777619u;
	}
	xsnprintf(buf, sizeof buf, "\\\\.\\pipe\\tmux-%08x", hash);
	*namep = win32_utf8_to_wide(buf);
	if (*namep == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't build pipe name");
		errno = EINVAL;
		return (-1);
	}
	if (name8p != NULL)
		*name8p = xstrdup(buf);
	return (0);
}

/*
 * Create a pipe instance with the same protected DACL as the endpoint
 * directory, rather than the token default DACL. The listener sets
 * FILE_FLAG_FIRST_PIPE_INSTANCE on its first instance so a process that
 * guesses the name (it is a hash of the socket path) cannot create the pipe
 * first and impersonate the server; subsequent instances omit the flag
 * because only one may hold it.
 */
static HANDLE
win32_ipc_pipe_create_instance(const wchar_t *wname, int first, DWORD *errorp)
{
	SECURITY_ATTRIBUTES	 sa;
	PSECURITY_DESCRIPTOR	 sd = NULL;
	HANDLE			 handle;
	DWORD			 flags;
	char			*cause = NULL;

	sd = win32_ipc_build_endpoint_sd(&cause);
	if (sd == NULL) {
		log_debug("%s: couldn't build pipe security descriptor: %s",
		    __func__, cause != NULL ? cause : strerror(errno));
		free(cause);
		if (errorp != NULL)
			*errorp = ERROR_ACCESS_DENIED;
		return (INVALID_HANDLE_VALUE);
	}

	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = sd;
	sa.bInheritHandle = FALSE;

	flags = PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED;
	if (first)
		flags |= FILE_FLAG_FIRST_PIPE_INSTANCE;
	handle = CreateNamedPipeW(wname, flags,
	    PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|
	    PIPE_REJECT_REMOTE_CLIENTS,
	    WIN32_IPC_PIPE_MAX_INSTANCES, WIN32_IPC_PIPE_BUFFER,
	    WIN32_IPC_PIPE_BUFFER, 0, &sa);
	if (handle == INVALID_HANDLE_VALUE && errorp != NULL)
		*errorp = GetLastError();
	LocalFree(sd);
	return (handle);
}

/* mark a pipe ready for I/O and start the reactor reader */
static int
win32_ipc_pipe_activate(struct win32_ipc_pipe *p, char **cause)
{
	p->connected = 1;
	p->input = evbuffer_new();
	if (p->input == NULL) {
		if (cause != NULL)
			xasprintf(cause, "out of memory");
		errno = ENOMEM;
		return (-1);
	}
	return (0);
}

/* Attach the reactor reader; the callback is needed to see readiness. */
static void	win32_ipc_pipe_event_cb(void *, uint32_t);

static int
win32_ipc_pipe_start_reader(struct win32_ipc_pipe *p, char **cause)
{
	if (p->reader != NULL)
		return (0);
	p->reader = win32_io_reader_new_overlapped(p->handle,
	    win32_ipc_pipe_event_cb, p);
	if (p->reader == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't start transport reader");
		errno = EIO;
		return (-1);
	}
	return (0);
}

static void
win32_ipc_pipe_notify(struct win32_ipc_pipe *p)
{
	char	c = 1;

	if (p->notify_write == INVALID_SOCKET || p->notify_armed)
		return;
	if (send(p->notify_write, &c, 1, 0) == 1)
		p->notify_armed = 1;
}

static void
win32_ipc_pipe_event_cb(void *arg, uint32_t events)
{
	struct win32_ipc_pipe	*p = arg;

	if (p == NULL)
		return;
	if (events & WIN32_IO_EVENT_READ)
		win32_io_reader_drain(p->reader, p->input);
	if (events & (WIN32_IO_EVENT_READ_EOF|WIN32_IO_EVENT_ERROR|
	    WIN32_IO_EVENT_CANCELED))
		p->eof = 1;
	if (EVBUFFER_LENGTH(p->input) != 0 || p->eof)
		win32_ipc_pipe_notify(p);
}

static int
win32_ipc_pipe_make_notify(struct win32_ipc_pipe *p, char **cause)
{
	SOCKET	pair[2];
	u_long	one = 1;

	if (win32_socketpair(pair) != 0) {
		if (cause != NULL)
			xasprintf(cause, "couldn't create transport wakeup");
		errno = EIO;
		return (-1);
	}
	if (ioctlsocket(pair[0], FIONBIO, &one) == SOCKET_ERROR ||
	    ioctlsocket(pair[1], FIONBIO, &one) == SOCKET_ERROR) {
		closesocket(pair[0]);
		closesocket(pair[1]);
		if (cause != NULL)
			xasprintf(cause, "couldn't make transport wakeup "
			    "non-blocking");
		errno = EIO;
		return (-1);
	}
	p->notify_read = pair[0];
	p->notify_write = pair[1];
	p->notify_armed = 0;
	return (0);
}

/*
 * A listener is just the pipe name; each accept creates a fresh instance.
 */
struct win32_ipc_pipe_listener *
win32_ipc_pipe_listen(const char *path, char **cause)
{
	struct win32_ipc_pipe_listener	*l;
	wchar_t				*wname = NULL;
	char				*name = NULL;

	if (cause != NULL)
		*cause = NULL;
	if (win32_ipc_pipe_name(path, &wname, &name, cause) != 0)
		return (NULL);
	log_debug("%s: path=%s pipe=%s", __func__, path, name);
	l = xcalloc(1, sizeof *l);
	l->name = name;
	l->wname = wname;
	return (l);
}

/*
 * Poll for a client. Creates a pending instance and checks whether it has
 * connected. Returns a connected pipe, or NULL with errno EAGAIN.
 */
struct win32_ipc_pipe *
win32_ipc_pipe_accept(struct win32_ipc_pipe_listener *l,
    struct win32_ipc_pipe **pendingp, char **cause)
{
	struct win32_ipc_pipe	*p = *pendingp;
	DWORD			 error, bytes;
	BOOL			 ok;

	if (cause != NULL)
		*cause = NULL;
	if (l == NULL) {
		errno = EINVAL;
		return (NULL);
	}

	if (p == NULL) {
		p = xcalloc(1, sizeof *p);
		p->notify_read = INVALID_SOCKET;
		p->notify_write = INVALID_SOCKET;
		p->handle = win32_ipc_pipe_create_instance(l->wname,
		    !l->first_done, &error);
		if (p->handle == INVALID_HANDLE_VALUE) {
			if (cause != NULL) {
				xasprintf(cause, "couldn't create transport "
				    "pipe %s: %s", l->name,
				    win32_strerror(error));
			}
			if (error == ERROR_ACCESS_DENIED)
				errno = EACCES;
			else if (error == ERROR_PIPE_BUSY)
				errno = EAGAIN;
			else
				errno = EIO;
			free(p);
			return (NULL);
		}
		l->first_done = 1;
		memset(&p->connect_ov, 0, sizeof p->connect_ov);
		if (!ConnectNamedPipe(p->handle, &p->connect_ov)) {
			error = GetLastError();
			if (error == ERROR_IO_PENDING)
				p->connect_pending = 1;
			else if (error != ERROR_PIPE_CONNECTED) {
				if (cause != NULL) {
					xasprintf(cause, "transport accept "
					    "failed: %s",
					    win32_strerror(error));
				}
				errno = EIO;
				CloseHandle(p->handle);
				free(p);
				return (NULL);
			}
		}
		/*
		 * The instance exists but no client is connected yet. Keep it
		 * in *pendingp so the next poll reuses it instead of creating
		 * another instance with the same name.
		 */
		*pendingp = p;
		errno = EAGAIN;
		return (NULL);
	}

	if (p->connect_pending) {
		ok = GetOverlappedResult(p->handle, &p->connect_ov, &bytes,
		    FALSE);
		if (!ok && GetLastError() == ERROR_IO_INCOMPLETE) {
			errno = EAGAIN;
			return (NULL);
		}
		p->connect_pending = 0;
	}

	(void)GetNamedPipeClientProcessId(p->handle, &p->peer_pid);
	if (win32_ipc_pipe_activate(p, cause) != 0) {
		CloseHandle(p->handle);
		free(p);
		*pendingp = NULL;
		return (NULL);
	}
	*pendingp = NULL;
	return (p);
}

struct win32_ipc_pipe *
win32_ipc_pipe_connect(const char *path, char **cause)
{
	struct win32_ipc_pipe	*p;
	wchar_t			*wname = NULL;
	HANDLE			 handle;
	DWORD			 error;

	if (cause != NULL)
		*cause = NULL;
	if (win32_ipc_pipe_name(path, &wname, NULL, cause) != 0)
		return (NULL);
	log_debug("%s: path=%s pipe=%ls", __func__, path, wname);

	handle = CreateFileW(wname, GENERIC_READ|GENERIC_WRITE, 0, NULL,
	    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		error = GetLastError();
		if (cause != NULL) {
			xasprintf(cause, "couldn't connect to transport: %s",
			    win32_strerror(error));
		}
		if (error == ERROR_FILE_NOT_FOUND ||
		    error == ERROR_PATH_NOT_FOUND)
			errno = ENOENT;
		else if (error == ERROR_ACCESS_DENIED)
			errno = EACCES;
		else if (error == ERROR_PIPE_BUSY)
			errno = EBUSY;
		else
			errno = EIO;
		free(wname);
		return (NULL);
	}
	free(wname);

	p = xcalloc(1, sizeof *p);
	p->handle = handle;
	p->notify_read = INVALID_SOCKET;
	p->notify_write = INVALID_SOCKET;
	if (win32_ipc_pipe_activate(p, cause) != 0) {
		CloseHandle(handle);
		free(p);
		return (NULL);
	}
	(void)GetNamedPipeServerProcessId(handle, &p->peer_pid);
	return (p);
}

int
win32_ipc_pipe_attach_notify(struct win32_ipc_pipe *p, char **cause)
{
	if (p == NULL) {
		errno = EINVAL;
		return (-1);
	}
	if (p->reader != NULL) {
		win32_io_endpoint_free(p->reader);
		p->reader = NULL;
	}
	if (win32_ipc_pipe_start_reader(p, cause) != 0)
		return (-1);
	return (win32_ipc_pipe_make_notify(p, cause));
}

SOCKET
win32_ipc_pipe_notify_socket(struct win32_ipc_pipe *p)
{
	if (p == NULL)
		return (INVALID_SOCKET);
	return (p->notify_read);
}

void
win32_ipc_pipe_consume_notify(struct win32_ipc_pipe *p)
{
	char	buf[64];

	if (p == NULL || p->notify_read == INVALID_SOCKET)
		return;
	while (recv(p->notify_read, buf, sizeof buf, 0) > 0)
		;
	p->notify_armed = 0;
}

ssize_t
win32_ipc_pipe_read(struct win32_ipc_pipe *p, void *data, size_t size)
{
	size_t	take;

	if (p == NULL || p->input == NULL) {
		errno = EBADF;
		return (-1);
	}
	take = EVBUFFER_LENGTH(p->input);
	if (take > size)
		take = size;
	if (take == 0) {
		if (p->eof)
			return (0);
		errno = EAGAIN;
		return (-1);
	}
	if (evbuffer_remove(p->input, data, take) != (int)take) {
		errno = EIO;
		return (-1);
	}
	/*
	 * If bytes remain buffered, post another readiness byte: the reactor
	 * notifies once per read completion, and the imsg reader may consume
	 * only part of what arrived.
	 */
	if (EVBUFFER_LENGTH(p->input) != 0)
		win32_ipc_pipe_notify(p);
	return ((ssize_t)take);
}

ssize_t
win32_ipc_pipe_write(struct win32_ipc_pipe *p, const void *data, size_t size)
{
	size_t		 left = size, nwrite;
	ssize_t		 total = 0;
	DWORD		 written, error;

	if (p == NULL || p->handle == NULL) {
		errno = EBADF;
		return (-1);
	}
	while (left != 0) {
		nwrite = left;
		if (nwrite > INT_MAX)
			nwrite = INT_MAX;
		if (!WriteFile(p->handle, (const char *)data + total,
		    (DWORD)nwrite, &written, NULL)) {
			error = GetLastError();
			if (error == ERROR_NO_DATA || error == ERROR_BROKEN_PIPE)
				errno = EPIPE;
			else
				errno = EIO;
			return (total == 0 ? -1 : total);
		}
		if (written == 0) {
			errno = EPIPE;
			return (total == 0 ? -1 : total);
		}
		total += written;
		left -= written;
	}
	return (total);
}

int
win32_ipc_pipe_eof(struct win32_ipc_pipe *p)
{
	if (p == NULL)
		return (1);
	return (p->eof && EVBUFFER_LENGTH(p->input) == 0);
}

void
win32_ipc_pipe_close(struct win32_ipc_pipe *p, int graceful)
{
	if (p == NULL)
		return;
	/*
	 * Detach the reader first. Its completion callback carries this pipe as
	 * its argument, and a completion may already be queued on the reactor;
	 * freeing the pipe while that still points here would be a
	 * use-after-free.
	 */
	if (p->reader != NULL) {
		win32_io_endpoint_detach(p->reader);
		win32_io_endpoint_free(p->reader);
		p->reader = NULL;
	}
	if (p->handle != NULL) {
		if (p->connected && graceful)
			(void)FlushFileBuffers(p->handle);
		CloseHandle(p->handle);
		p->handle = NULL;
	}
	if (p->notify_read != INVALID_SOCKET)
		closesocket(p->notify_read);
	if (p->notify_write != INVALID_SOCKET)
		closesocket(p->notify_write);
	if (p->input != NULL)
		evbuffer_free(p->input);
	free(p);
}

void
win32_ipc_pipe_listener_free(struct win32_ipc_pipe_listener *l)
{
	if (l == NULL)
		return;
	free(l->name);
	free(l->wname);
	free(l);
}

#endif /* TMUX_WIN32 */
