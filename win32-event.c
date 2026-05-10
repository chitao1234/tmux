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

ssize_t
readv(int fd, const struct iovec *iov, int iovcnt)
{
	char	*base;
	size_t	 len;

	if (iovcnt != 1) {
		errno = EINVAL;
		return (-1);
	}
	base = iov[0].iov_base;
	len = iov[0].iov_len;
	if (len > INT_MAX)
		len = INT_MAX;
	return (read(fd, base, len));
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
			n = write(fd, base, len > INT_MAX ? INT_MAX : len);
			if (n == -1)
				return (total == 0 ? -1 : total);
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
	whe->readcb = readcb;
	whe->errorcb = errorcb;
	whe->arg = arg;
	whe->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
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
    __unused const void *data, __unused size_t size)
{
	errno = ENOSYS;
	return (-1);
}

#endif /* TMUX_WIN32 */
