/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include <sys/types.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#ifdef TMUX_WIN32

static int
win32_socket_errno(int error)
{
	switch (error) {
	case WSAEWOULDBLOCK:
		return (EAGAIN);
	case WSAEINTR:
		return (EINTR);
	case WSAECONNREFUSED:
		return (ECONNREFUSED);
	default:
		return (error);
	}
}

ssize_t
readv(int fd, const struct iovec *iov, int iovcnt)
{
	char	*base;
	size_t	 len;
	int	 n;

	if (iovcnt != 1) {
		errno = EINVAL;
		return (-1);
	}
	base = iov[0].iov_base;
	len = iov[0].iov_len;
	if (len > INT_MAX)
		len = INT_MAX;
	n = recv(win32_ipc_socket(fd), base, len, 0);
	if (n == SOCKET_ERROR) {
		errno = win32_socket_errno(WSAGetLastError());
		return (-1);
	}
	return (n);
}

ssize_t
writev(int fd, const struct iovec *iov, int iovcnt)
{
	const char	*base;
	size_t		 len;
	ssize_t		 total = 0, n;
	int		 i;

	for (i = 0; i < iovcnt; i++) {
		base = iov[i].iov_base;
		len = iov[i].iov_len;
		while (len != 0) {
			n = send(win32_ipc_socket(fd), base,
			    len > INT_MAX ? INT_MAX : len, 0);
			if (n == SOCKET_ERROR) {
				errno = win32_socket_errno(WSAGetLastError());
				return (total == 0 ? -1 : total);
			}
			if (n == 0)
				return (total);
			total += n;
			base += n;
			len -= n;
		}
	}
	return (total);
}

struct win32_handle_event {
	HANDLE		 handle;
	HANDLE		 thread;
	HANDLE		 stop;
	SOCKET		 notify_read;
	SOCKET		 notify_write;
	struct event	 event;
	struct evbuffer	*input;
	CRITICAL_SECTION lock;
	void		(*readcb)(void *);
	void		(*errorcb)(void *);
	void		 *arg;
	int		 error;
};

static int
win32_socketpair(SOCKET pair[2])
{
	SOCKET			 listener = INVALID_SOCKET;
	struct sockaddr_in	 addr;
	int			 len = sizeof addr;

	pair[0] = INVALID_SOCKET;
	pair[1] = INVALID_SOCKET;

	listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_SOCKET)
		return (-1);
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0 ||
	    listen(listener, 1) != 0 ||
	    getsockname(listener, (struct sockaddr *)&addr, &len) != 0)
		goto fail;
	pair[0] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (pair[0] == INVALID_SOCKET)
		goto fail;
	if (connect(pair[0], (struct sockaddr *)&addr, sizeof addr) != 0)
		goto fail0;
	pair[1] = accept(listener, NULL, NULL);
	if (pair[1] == INVALID_SOCKET)
		goto fail0;
	closesocket(listener);
	return (0);

fail0:
	closesocket(pair[0]);
	pair[0] = INVALID_SOCKET;
fail:
	closesocket(listener);
	return (-1);
}

static DWORD WINAPI
win32_handle_event_thread(void *arg)
{
	struct win32_handle_event	*whe = arg;
	char				 buf[8192], one = 1;
	DWORD				 nread;

	for (;;) {
		if (WaitForSingleObject(whe->stop, 0) == WAIT_OBJECT_0)
			break;
		if (!ReadFile(whe->handle, buf, sizeof buf, &nread, NULL) ||
		    nread == 0) {
			EnterCriticalSection(&whe->lock);
			whe->error = 1;
			LeaveCriticalSection(&whe->lock);
			send(whe->notify_write, &one, 1, 0);
			break;
		}
		EnterCriticalSection(&whe->lock);
		evbuffer_add(whe->input, buf, nread);
		LeaveCriticalSection(&whe->lock);
		send(whe->notify_write, &one, 1, 0);
	}
	return (0);
}

static void
win32_handle_event_cb(__unused evutil_socket_t fd, __unused short events,
    void *arg)
{
	struct win32_handle_event	*whe = arg;
	char				 buf[64];
	int				 error;

	while (recv(whe->notify_read, buf, sizeof buf, 0) > 0)
		;
	EnterCriticalSection(&whe->lock);
	error = whe->error;
	LeaveCriticalSection(&whe->lock);

	if (error) {
		if (whe->errorcb != NULL)
			whe->errorcb(whe->arg);
		return;
	}
	if (whe->readcb != NULL)
		whe->readcb(whe->arg);
}

struct win32_handle_event *
win32_handle_event_new(HANDLE handle, void (*readcb)(void *),
    void (*errorcb)(void *), void *arg)
{
	struct win32_handle_event	*whe;
	SOCKET				 pair[2];
	u_long				 nonblock = 1;

	if (win32_socketpair(pair) != 0)
		return (NULL);
	ioctlsocket(pair[0], FIONBIO, &nonblock);
	ioctlsocket(pair[1], FIONBIO, &nonblock);

	whe = xcalloc(1, sizeof *whe);
	whe->handle = handle;
	whe->notify_read = pair[0];
	whe->notify_write = pair[1];
	whe->input = evbuffer_new();
	if (whe->input == NULL) {
		closesocket(pair[0]);
		closesocket(pair[1]);
		free(whe);
		return (NULL);
	}
	whe->readcb = readcb;
	whe->errorcb = errorcb;
	whe->arg = arg;
	whe->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (whe->stop == NULL) {
		evbuffer_free(whe->input);
		closesocket(pair[0]);
		closesocket(pair[1]);
		free(whe);
		return (NULL);
	}
	InitializeCriticalSection(&whe->lock);

	event_set(&whe->event, (evutil_socket_t)whe->notify_read,
	    EV_READ|EV_PERSIST,
	    win32_handle_event_cb, whe);
	event_add(&whe->event, NULL);
	whe->thread = CreateThread(NULL, 0, win32_handle_event_thread, whe, 0,
	    NULL);
	if (whe->thread == NULL) {
		win32_handle_event_free(whe);
		return (NULL);
	}
	return (whe);
}

void
win32_handle_event_free(struct win32_handle_event *whe)
{
	if (whe == NULL)
		return;
	if (whe->stop != NULL)
		SetEvent(whe->stop);
	if (whe->thread != NULL) {
		CancelSynchronousIo(whe->thread);
		WaitForSingleObject(whe->thread, INFINITE);
	}
	event_del(&whe->event);
	if (whe->input != NULL)
		evbuffer_free(whe->input);
	if (whe->notify_read != INVALID_SOCKET)
		closesocket(whe->notify_read);
	if (whe->notify_write != INVALID_SOCKET)
		closesocket(whe->notify_write);
	if (whe->thread != NULL)
		CloseHandle(whe->thread);
	if (whe->stop != NULL)
		CloseHandle(whe->stop);
	DeleteCriticalSection(&whe->lock);
	free(whe);
}

struct evbuffer *
win32_handle_event_input(struct win32_handle_event *whe)
{
	return (whe->input);
}

void
win32_handle_event_drain(struct win32_handle_event *whe, struct evbuffer *dst)
{
	EnterCriticalSection(&whe->lock);
	evbuffer_add_buffer(dst, whe->input);
	LeaveCriticalSection(&whe->lock);
}

void
win32_handle_event_drain_bev(struct win32_handle_event *whe,
    struct bufferevent *bev)
{
	struct evbuffer	*dst = bev->input;

	evbuffer_unfreeze(dst, 0);
	EnterCriticalSection(&whe->lock);
	evbuffer_add_buffer(dst, whe->input);
	LeaveCriticalSection(&whe->lock);
	evbuffer_freeze(dst, 0);
}

size_t
win32_handle_event_buffered(struct win32_handle_event *whe)
{
	size_t	size;

	EnterCriticalSection(&whe->lock);
	size = EVBUFFER_LENGTH(whe->input);
	LeaveCriticalSection(&whe->lock);
	return (size);
}

int
win32_handle_event_write(__unused struct win32_handle_event *whe,
    const void *data, size_t size)
{
	return (win32_handle_write(whe->handle, data, size));
}

static int
win32_handle_write_file(HANDLE handle, const void *data, size_t size)
{
	DWORD	written, nwrite;

	nwrite = size > INT_MAX ? INT_MAX : (DWORD)size;
	if (nwrite == 0)
		return (0);
	if (!WriteFile(handle, data, nwrite, &written, NULL)) {
		log_debug("%s: WriteFile failed: %s", __func__,
		    win32_strerror(GetLastError()));
		errno = EIO;
		return (-1);
	}
	return ((int)written);
}

int
win32_handle_write(HANDLE handle, const void *data, size_t size)
{
	DWORD	 written, mode, total;
	wchar_t	*wdata;
	int	 n, nbytes;

	nbytes = size > INT_MAX ? INT_MAX : (int)size;
	if (nbytes == 0)
		return (0);

	if (GetConsoleMode(handle, &mode)) {
		n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data,
		    nbytes, NULL, 0);
		if (n == 0) {
			log_debug("%s: MultiByteToWideChar failed: %s", __func__,
			    win32_strerror(GetLastError()));
			return (win32_handle_write_file(handle, data, size));
		}
		wdata = xcalloc(n, sizeof *wdata);
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data,
		    nbytes, wdata, n) == 0) {
			log_debug("%s: MultiByteToWideChar failed: %s", __func__,
			    win32_strerror(GetLastError()));
			free(wdata);
			return (win32_handle_write_file(handle, data, size));
		}
		total = 0;
		while (total < (DWORD)n) {
			if (!WriteConsoleW(handle, wdata + total, n - total,
			    &written, NULL)) {
				log_debug("%s: WriteConsoleW failed: %s",
				    __func__, win32_strerror(GetLastError()));
				free(wdata);
				errno = EIO;
				return (-1);
			}
			if (written == 0) {
				log_debug("%s: WriteConsoleW wrote nothing",
				    __func__);
				free(wdata);
				errno = EIO;
				return (-1);
			}
			total += written;
		}
		free(wdata);
		if (log_get_level() > 1) {
			log_debug("%s: WriteConsoleW wrote %lu UTF-16 units "
			    "from %d UTF-8 bytes", __func__,
			    (unsigned long)total, nbytes);
		}
		return (nbytes);
	}

	return (win32_handle_write_file(handle, data, size));
}

void
win32_log_handle(const char *name, HANDLE handle)
{
	DWORD	mode = 0, type, error = ERROR_SUCCESS;

	if (handle == INVALID_HANDLE_VALUE || handle == NULL) {
		log_debug("%s: invalid handle %p", name, handle);
		return;
	}

	type = GetFileType(handle);
	if (!GetConsoleMode(handle, &mode))
		error = GetLastError();
	log_debug("%s: handle %p type %#lx console %s mode %#lx error %lu "
	    "cp in/out %u/%u", name, handle, (unsigned long)type,
	    error == ERROR_SUCCESS ? "yes" : "no", (unsigned long)mode,
	    (unsigned long)error, GetConsoleCP(), GetConsoleOutputCP());
}

#endif /* TMUX_WIN32 */
