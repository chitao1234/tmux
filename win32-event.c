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

enum win32_handle_event_state {
	WIN32_HANDLE_EVENT_RUNNING,
	WIN32_HANDLE_EVENT_EOF,
	WIN32_HANDLE_EVENT_ERROR
};

enum win32_handle_writer_state {
	WIN32_HANDLE_WRITER_RUNNING,
	WIN32_HANDLE_WRITER_CLOSING,
	WIN32_HANDLE_WRITER_CLOSED,
	WIN32_HANDLE_WRITER_ERROR
};

static int
win32_socket_errno(int error)
{
	switch (error) {
	case WSAEWOULDBLOCK:
		return (EAGAIN);
	case WSAEINTR:
		return (EINTR);
	case WSAECONNABORTED:
		return (ECONNABORTED);
	case WSAECONNREFUSED:
		return (ECONNREFUSED);
	case WSAECONNRESET:
	case WSAENETRESET:
		return (ECONNRESET);
	case WSAENOTCONN:
		return (ENOTCONN);
	case WSAETIMEDOUT:
		return (ETIMEDOUT);
	case WSAEMFILE:
		return (EMFILE);
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
	TAILQ_ENTRY(win32_handle_event) entry;
	HANDLE		 handle;
	HANDLE		 thread;
	HANDLE		 stop;
	HANDLE		 ready;
	struct evbuffer	*input;
	CRITICAL_SECTION lock;
	void		(*readcb)(void *);
	void		(*errorcb)(void *);
	void		 *arg;
	int		 paused;
	int		 throttled;
	enum win32_handle_event_state state;
	DWORD		 error;
	int		 pending;
	int		 active;
};

struct win32_handle_writer {
	TAILQ_ENTRY(win32_handle_writer) entry;
	HANDLE		 handle;
	HANDLE		 thread;
	HANDLE		 ready;
	struct evbuffer	*output;
	CRITICAL_SECTION lock;
	void		(*writecb)(void *);
	void		(*errorcb)(void *);
	void		 *arg;
	enum win32_handle_writer_state state;
	int		 borrowed;
	int		 stop;
	int		 pending;
	int		 active;
};

struct win32_process_event {
	TAILQ_ENTRY(win32_process_event) entry;
	HANDLE		 wait;
	void		(*exitcb)(void *);
	void		 *arg;
	int		 pending;
	int		 active;
};

struct win32_io_service {
	SOCKET		 notify_read;
	SOCKET		 notify_write;
	struct event	 event;
	CRITICAL_SECTION lock;
	TAILQ_HEAD(, win32_handle_event) pending;
	TAILQ_HEAD(, win32_handle_writer) write_pending;
	TAILQ_HEAD(, win32_process_event) process_pending;
	int		 initialized;
	int		 event_added;
};

static struct win32_io_service win32_io;

#define WIN32_HANDLE_EVENT_HIGH (1024 * 1024)
#define WIN32_HANDLE_EVENT_LOW (512 * 1024)
#define WIN32_HANDLE_WRITER_CHUNK (256 * 1024)

static int	win32_io_service_init(void);
static void	win32_io_service_enqueue_reader(struct win32_handle_event *);
static void	win32_io_service_enqueue_writer(struct win32_handle_writer *);
static void	win32_io_service_enqueue_process(struct win32_process_event *);
static void	win32_io_service_cb(evutil_socket_t, short, void *);
static void	win32_handle_event_update_ready(
		     struct win32_handle_event *);
static int	win32_handle_event_error_is_eof(DWORD);
static int	win32_handle_write(HANDLE, const void *, size_t);

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

static int
win32_io_service_init(void)
{
	SOCKET	pair[2];
	u_long	nonblock = 1;

	if (win32_io.initialized)
		return (0);

	if (win32_socketpair(pair) != 0)
		return (-1);
	ioctlsocket(pair[0], FIONBIO, &nonblock);
	ioctlsocket(pair[1], FIONBIO, &nonblock);

	win32_io.notify_read = pair[0];
	win32_io.notify_write = pair[1];
	TAILQ_INIT(&win32_io.pending);
	TAILQ_INIT(&win32_io.write_pending);
	TAILQ_INIT(&win32_io.process_pending);
	InitializeCriticalSection(&win32_io.lock);

	event_set(&win32_io.event, (evutil_socket_t)win32_io.notify_read,
	    EV_READ|EV_PERSIST, win32_io_service_cb, NULL);
	if (event_add(&win32_io.event, NULL) != 0) {
		DeleteCriticalSection(&win32_io.lock);
		closesocket(pair[0]);
		closesocket(pair[1]);
		memset(&win32_io, 0, sizeof win32_io);
		return (-1);
	}
	win32_io.event_added = 1;
	win32_io.initialized = 1;
	return (0);
}

void
win32_io_service_fini(void)
{
	if (!win32_io.initialized)
		return;
	if (win32_io.event_added)
		event_del(&win32_io.event);
	if (win32_io.notify_read != INVALID_SOCKET)
		closesocket(win32_io.notify_read);
	if (win32_io.notify_write != INVALID_SOCKET)
		closesocket(win32_io.notify_write);
	DeleteCriticalSection(&win32_io.lock);
	memset(&win32_io, 0, sizeof win32_io);
}

static void
win32_io_service_enqueue_reader(struct win32_handle_event *whe)
{
	char	one = 1;

	EnterCriticalSection(&win32_io.lock);
	if (whe->active && !whe->pending) {
		TAILQ_INSERT_TAIL(&win32_io.pending, whe, entry);
		whe->pending = 1;
		send(win32_io.notify_write, &one, 1, 0);
	}
	LeaveCriticalSection(&win32_io.lock);
}

static void
win32_io_service_enqueue_writer(struct win32_handle_writer *whw)
{
	char	one = 1;

	EnterCriticalSection(&win32_io.lock);
	if (whw->active && !whw->pending) {
		TAILQ_INSERT_TAIL(&win32_io.write_pending, whw, entry);
		whw->pending = 1;
		send(win32_io.notify_write, &one, 1, 0);
	}
	LeaveCriticalSection(&win32_io.lock);
}

static void
win32_io_service_enqueue_process(struct win32_process_event *wpe)
{
	char	one = 1;

	EnterCriticalSection(&win32_io.lock);
	if (wpe->active && !wpe->pending) {
		TAILQ_INSERT_TAIL(&win32_io.process_pending, wpe, entry);
		wpe->pending = 1;
		send(win32_io.notify_write, &one, 1, 0);
	}
	LeaveCriticalSection(&win32_io.lock);
}

static void
win32_io_service_dispatch_readers(void)
{
	struct win32_handle_event	*whe;
	size_t				 buffered;
	enum win32_handle_event_state	 state;

	for (;;) {
		EnterCriticalSection(&win32_io.lock);
		whe = TAILQ_FIRST(&win32_io.pending);
		if (whe != NULL) {
			TAILQ_REMOVE(&win32_io.pending, whe, entry);
			whe->pending = 0;
		}
		LeaveCriticalSection(&win32_io.lock);
		if (whe == NULL)
			break;

		EnterCriticalSection(&whe->lock);
		state = whe->state;
		buffered = EVBUFFER_LENGTH(whe->input);
		LeaveCriticalSection(&whe->lock);

		if (state != WIN32_HANDLE_EVENT_RUNNING) {
			if (whe->errorcb != NULL)
				whe->errorcb(whe->arg);
		} else if (buffered != 0 && whe->readcb != NULL)
			whe->readcb(whe->arg);
	}
}

static void
win32_io_service_dispatch_writers(void)
{
	struct win32_handle_writer	*whw;
	enum win32_handle_writer_state	 state;
	size_t				 buffered;

	for (;;) {
		EnterCriticalSection(&win32_io.lock);
		whw = TAILQ_FIRST(&win32_io.write_pending);
		if (whw != NULL) {
			TAILQ_REMOVE(&win32_io.write_pending, whw, entry);
			whw->pending = 0;
		}
		LeaveCriticalSection(&win32_io.lock);
		if (whw == NULL)
			break;

		EnterCriticalSection(&whw->lock);
		state = whw->state;
		buffered = EVBUFFER_LENGTH(whw->output);
		LeaveCriticalSection(&whw->lock);

		if (state == WIN32_HANDLE_WRITER_ERROR) {
			if (whw->errorcb != NULL)
				whw->errorcb(whw->arg);
		} else if (buffered == 0 && whw->writecb != NULL)
			whw->writecb(whw->arg);
	}
}

static void
win32_io_service_dispatch_processes(void)
{
	struct win32_process_event	*wpe;

	for (;;) {
		EnterCriticalSection(&win32_io.lock);
		wpe = TAILQ_FIRST(&win32_io.process_pending);
		if (wpe != NULL) {
			TAILQ_REMOVE(&win32_io.process_pending, wpe, entry);
			wpe->pending = 0;
		}
		LeaveCriticalSection(&win32_io.lock);
		if (wpe == NULL)
			break;

		if (wpe->exitcb != NULL)
			wpe->exitcb(wpe->arg);
	}
}

static void
win32_io_service_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *arg)
{
	char	buf[64];

	while (recv(win32_io.notify_read, buf, sizeof buf, 0) > 0)
		;

	win32_io_service_dispatch_readers();
	win32_io_service_dispatch_writers();
	win32_io_service_dispatch_processes();
}

static VOID CALLBACK
win32_process_event_wait_cb(PVOID arg, __unused BOOLEAN timed_out)
{
	struct win32_process_event	*wpe = arg;

	win32_io_service_enqueue_process(wpe);
}

struct win32_process_event *
win32_process_event_new(HANDLE process, void (*exitcb)(void *), void *arg)
{
	struct win32_process_event	*wpe;

	if (process == NULL || process == INVALID_HANDLE_VALUE)
		return (NULL);
	if (win32_io_service_init() != 0)
		return (NULL);

	wpe = xcalloc(1, sizeof *wpe);
	wpe->exitcb = exitcb;
	wpe->arg = arg;
	wpe->active = 1;
	if (!RegisterWaitForSingleObject(&wpe->wait, process,
	    win32_process_event_wait_cb, wpe, INFINITE,
	    WT_EXECUTEONLYONCE)) {
		free(wpe);
		return (NULL);
	}
	return (wpe);
}

void
win32_process_event_free(struct win32_process_event *wpe)
{
	if (wpe == NULL)
		return;
	if (win32_io.initialized) {
		EnterCriticalSection(&win32_io.lock);
		wpe->active = 0;
		if (wpe->pending) {
			TAILQ_REMOVE(&win32_io.process_pending, wpe, entry);
			wpe->pending = 0;
		}
		LeaveCriticalSection(&win32_io.lock);
	}
	if (wpe->wait != NULL &&
	    !UnregisterWaitEx(wpe->wait, INVALID_HANDLE_VALUE)) {
		log_debug("%s: UnregisterWaitEx failed: %s", __func__,
		    win32_strerror(GetLastError()));
	}
	free(wpe);
}

void
win32_process_event_notify(struct win32_process_event *wpe)
{
	if (wpe != NULL)
		win32_io_service_enqueue_process(wpe);
}

static void
win32_handle_event_update_ready(struct win32_handle_event *whe)
{
	size_t	buffered;

	buffered = EVBUFFER_LENGTH(whe->input);
	if (whe->throttled && buffered <= WIN32_HANDLE_EVENT_LOW)
		whe->throttled = 0;
	if (whe->ready == NULL)
		return;
	if (!whe->paused && !whe->throttled &&
	    whe->state == WIN32_HANDLE_EVENT_RUNNING)
		SetEvent(whe->ready);
	else
		ResetEvent(whe->ready);
}

static int
win32_handle_event_error_is_eof(DWORD error)
{
	return (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF ||
	    error == ERROR_NO_DATA);
}

static DWORD WINAPI
win32_handle_event_thread(void *arg)
{
	struct win32_handle_event	*whe = arg;
	char				 buf[8192];
	HANDLE				 events[2];
	DWORD				 wait;
	DWORD				 nread;
	enum win32_handle_event_state	 state;
	u_int				 i;

	events[0] = whe->stop;
	events[1] = whe->ready;
	for (;;) {
		wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
		if (wait != WAIT_OBJECT_0 + 1)
			break;
		if (!ReadFile(whe->handle, buf, sizeof buf, &nread, NULL)) {
			DWORD error = GetLastError();

			EnterCriticalSection(&whe->lock);
			if (win32_handle_event_error_is_eof(error))
				whe->state = WIN32_HANDLE_EVENT_EOF;
			else
				whe->state = WIN32_HANDLE_EVENT_ERROR;
			whe->error = error;
			win32_handle_event_update_ready(whe);
			LeaveCriticalSection(&whe->lock);
			win32_io_service_enqueue_reader(whe);
			break;
		}
		if (nread == 0) {
			EnterCriticalSection(&whe->lock);
			whe->state = WIN32_HANDLE_EVENT_EOF;
			whe->error = ERROR_SUCCESS;
			win32_handle_event_update_ready(whe);
			LeaveCriticalSection(&whe->lock);
			win32_io_service_enqueue_reader(whe);
			break;
		}
		if (log_get_level() > 1) {
			for (i = 0; i < nread; i++) {
				if (buf[i] == '\003') {
					log_debug("%s: read Ctrl-C byte",
					    __func__);
					break;
				}
			}
		}
		EnterCriticalSection(&whe->lock);
		if (evbuffer_add(whe->input, buf, nread) != 0) {
			whe->state = WIN32_HANDLE_EVENT_ERROR;
			whe->error = ERROR_NOT_ENOUGH_MEMORY;
		}
		if (EVBUFFER_LENGTH(whe->input) >= WIN32_HANDLE_EVENT_HIGH)
			whe->throttled = 1;
		win32_handle_event_update_ready(whe);
		state = whe->state;
		LeaveCriticalSection(&whe->lock);
		win32_io_service_enqueue_reader(whe);
		if (state != WIN32_HANDLE_EVENT_RUNNING)
			break;
	}
	return (0);
}

struct win32_handle_event *
win32_handle_event_new(HANDLE handle, void (*readcb)(void *),
    void (*errorcb)(void *), void *arg)
{
	struct win32_handle_event	*whe;

	if (win32_io_service_init() != 0)
		return (NULL);

	whe = xcalloc(1, sizeof *whe);
	whe->handle = handle;
	whe->input = evbuffer_new();
	if (whe->input == NULL) {
		free(whe);
		return (NULL);
	}
	whe->readcb = readcb;
	whe->errorcb = errorcb;
	whe->arg = arg;
	whe->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (whe->stop == NULL) {
		evbuffer_free(whe->input);
		free(whe);
		return (NULL);
	}
	whe->ready = CreateEventW(NULL, TRUE, TRUE, NULL);
	if (whe->ready == NULL) {
		CloseHandle(whe->stop);
		evbuffer_free(whe->input);
		free(whe);
		return (NULL);
	}
	InitializeCriticalSection(&whe->lock);
	whe->active = 1;
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
	if (win32_io.initialized) {
		EnterCriticalSection(&win32_io.lock);
		whe->active = 0;
		if (whe->pending) {
			TAILQ_REMOVE(&win32_io.pending, whe, entry);
			whe->pending = 0;
		}
		LeaveCriticalSection(&win32_io.lock);
	}
	if (whe->input != NULL)
		evbuffer_free(whe->input);
	if (whe->thread != NULL)
		CloseHandle(whe->thread);
	if (whe->stop != NULL)
		CloseHandle(whe->stop);
	if (whe->ready != NULL)
		CloseHandle(whe->ready);
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
	win32_handle_event_update_ready(whe);
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
	win32_handle_event_update_ready(whe);
	LeaveCriticalSection(&whe->lock);
	evbuffer_freeze(dst, 0);
}

void
win32_handle_event_set_reading(struct win32_handle_event *whe, int enabled)
{
	if (whe == NULL)
		return;
	EnterCriticalSection(&whe->lock);
	whe->paused = !enabled;
	win32_handle_event_update_ready(whe);
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
win32_handle_event_done(struct win32_handle_event *whe)
{
	int	done;

	EnterCriticalSection(&whe->lock);
	done = (whe->state != WIN32_HANDLE_EVENT_RUNNING);
	LeaveCriticalSection(&whe->lock);
	return (done);
}

int
win32_handle_event_eof(struct win32_handle_event *whe)
{
	int	eof;

	EnterCriticalSection(&whe->lock);
	eof = (whe->state == WIN32_HANDLE_EVENT_EOF);
	LeaveCriticalSection(&whe->lock);
	return (eof);
}

int
win32_handle_event_error(struct win32_handle_event *whe)
{
	int	error;

	EnterCriticalSection(&whe->lock);
	error = (whe->state == WIN32_HANDLE_EVENT_ERROR);
	LeaveCriticalSection(&whe->lock);
	return (error);
}

static DWORD WINAPI
win32_handle_writer_thread(void *arg)
{
	struct win32_handle_writer	*whw = arg;
	char				*buf;
	HANDLE				 handle;
	size_t				 size;
	int				 written;
	int				 notify;

	buf = xmalloc(WIN32_HANDLE_WRITER_CHUNK);
	for (;;) {
		WaitForSingleObject(whw->ready, INFINITE);

		for (;;) {
			EnterCriticalSection(&whw->lock);
			if (whw->stop)
				goto stop;
			if (whw->state == WIN32_HANDLE_WRITER_ERROR ||
			    whw->handle == NULL)
				goto stop;
			size = EVBUFFER_LENGTH(whw->output);
			if (size == 0) {
				if (whw->state == WIN32_HANDLE_WRITER_CLOSING)
					goto stop;
				ResetEvent(whw->ready);
				LeaveCriticalSection(&whw->lock);
				break;
			}
			if (size > WIN32_HANDLE_WRITER_CHUNK)
				size = WIN32_HANDLE_WRITER_CHUNK;
			memcpy(buf, EVBUFFER_DATA(whw->output), size);
			handle = whw->handle;
			LeaveCriticalSection(&whw->lock);

			written = win32_handle_write(handle, buf, size);
			if (written == -1 || written == 0) {
				EnterCriticalSection(&whw->lock);
				whw->state = WIN32_HANDLE_WRITER_ERROR;
				evbuffer_drain(whw->output,
				    EVBUFFER_LENGTH(whw->output));
				handle = whw->handle;
				whw->handle = NULL;
				LeaveCriticalSection(&whw->lock);
				if (handle != NULL && !whw->borrowed)
					CloseHandle(handle);
				win32_io_service_enqueue_writer(whw);
				free(buf);
				return (0);
			}

			EnterCriticalSection(&whw->lock);
			evbuffer_drain(whw->output, written);
			notify = EVBUFFER_LENGTH(whw->output) == 0;
			LeaveCriticalSection(&whw->lock);
			if (notify)
				win32_io_service_enqueue_writer(whw);
		}
	}

stop:
	evbuffer_drain(whw->output, EVBUFFER_LENGTH(whw->output));
	handle = whw->handle;
	whw->handle = NULL;
	if (whw->state != WIN32_HANDLE_WRITER_ERROR)
		whw->state = WIN32_HANDLE_WRITER_CLOSED;
	LeaveCriticalSection(&whw->lock);
	if (handle != NULL && !whw->borrowed)
		CloseHandle(handle);
	win32_io_service_enqueue_writer(whw);
	free(buf);
	return (0);
}

static struct win32_handle_writer *
win32_handle_writer_new1(HANDLE *handle, void (*writecb)(void *),
    void (*errorcb)(void *), void *arg, int borrowed)
{
	struct win32_handle_writer	*whw;

	if (win32_io_service_init() != 0)
		return (NULL);
	if (handle == NULL || *handle == NULL || *handle == INVALID_HANDLE_VALUE)
		return (NULL);

	whw = xcalloc(1, sizeof *whw);
	whw->handle = *handle;
	whw->writecb = writecb;
	whw->errorcb = errorcb;
	whw->arg = arg;
	whw->borrowed = borrowed;
	whw->output = evbuffer_new();
	if (whw->output == NULL) {
		free(whw);
		return (NULL);
	}
	whw->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (whw->ready == NULL) {
		evbuffer_free(whw->output);
		free(whw);
		return (NULL);
	}
	InitializeCriticalSection(&whw->lock);
	whw->active = 1;
	whw->thread = CreateThread(NULL, 0, win32_handle_writer_thread, whw, 0,
	    NULL);
	if (whw->thread == NULL) {
		DeleteCriticalSection(&whw->lock);
		CloseHandle(whw->ready);
		evbuffer_free(whw->output);
		free(whw);
		return (NULL);
	}
	if (!borrowed)
		*handle = NULL;
	if (whw->writecb != NULL)
		win32_io_service_enqueue_writer(whw);
	return (whw);
}

struct win32_handle_writer *
win32_handle_writer_new_cb(HANDLE *handle, void (*writecb)(void *),
    void (*errorcb)(void *), void *arg)
{
	return (win32_handle_writer_new1(handle, writecb, errorcb, arg, 0));
}

struct win32_handle_writer *
win32_handle_writer_new_borrowed(HANDLE *handle, void (*writecb)(void *),
    void (*errorcb)(void *), void *arg)
{
	return (win32_handle_writer_new1(handle, writecb, errorcb, arg, 1));
}

struct win32_handle_writer *
win32_handle_writer_new(HANDLE *handle)
{
	return (win32_handle_writer_new_cb(handle, NULL, NULL, NULL));
}

void
win32_handle_writer_free(struct win32_handle_writer *whw)
{
	if (whw == NULL)
		return;
	EnterCriticalSection(&whw->lock);
	whw->stop = 1;
	SetEvent(whw->ready);
	LeaveCriticalSection(&whw->lock);
	CancelSynchronousIo(whw->thread);
	WaitForSingleObject(whw->thread, INFINITE);
	if (win32_io.initialized) {
		EnterCriticalSection(&win32_io.lock);
		whw->active = 0;
		if (whw->pending) {
			TAILQ_REMOVE(&win32_io.write_pending, whw, entry);
			whw->pending = 0;
		}
		LeaveCriticalSection(&win32_io.lock);
	}
	CloseHandle(whw->thread);
	CloseHandle(whw->ready);
	evbuffer_free(whw->output);
	DeleteCriticalSection(&whw->lock);
	free(whw);
}

int
win32_handle_writer_write(struct win32_handle_writer *whw, const void *data,
    size_t size)
{
	size_t	nwrite;

	if (whw == NULL) {
		errno = EPIPE;
		return (-1);
	}
	nwrite = size;
	if (nwrite > INT_MAX)
		nwrite = INT_MAX;
	if (nwrite == 0)
		return (0);

	EnterCriticalSection(&whw->lock);
	if (whw->state != WIN32_HANDLE_WRITER_RUNNING || whw->stop ||
	    whw->handle == NULL) {
		LeaveCriticalSection(&whw->lock);
		errno = EPIPE;
		return (-1);
	}
	if (evbuffer_add(whw->output, data, nwrite) != 0) {
		LeaveCriticalSection(&whw->lock);
		errno = ENOMEM;
		return (-1);
	}
	SetEvent(whw->ready);
	LeaveCriticalSection(&whw->lock);
	return ((int)nwrite);
}

void
win32_handle_writer_close(struct win32_handle_writer *whw)
{
	if (whw == NULL)
		return;
	EnterCriticalSection(&whw->lock);
	if (whw->state != WIN32_HANDLE_WRITER_RUNNING) {
		LeaveCriticalSection(&whw->lock);
		return;
	}
	whw->state = WIN32_HANDLE_WRITER_CLOSING;
	SetEvent(whw->ready);
	LeaveCriticalSection(&whw->lock);
}

size_t
win32_handle_writer_buffered(struct win32_handle_writer *whw)
{
	size_t	size;

	if (whw == NULL)
		return (0);
	EnterCriticalSection(&whw->lock);
	size = EVBUFFER_LENGTH(whw->output);
	LeaveCriticalSection(&whw->lock);
	return (size);
}

int
win32_handle_writer_drained(struct win32_handle_writer *whw)
{
	int	drained;

	if (whw == NULL)
		return (1);
	EnterCriticalSection(&whw->lock);
	drained = (EVBUFFER_LENGTH(whw->output) == 0);
	LeaveCriticalSection(&whw->lock);
	return (drained);
}

int
win32_handle_writer_writable(struct win32_handle_writer *whw)
{
	int	writable;

	if (whw == NULL)
		return (0);
	EnterCriticalSection(&whw->lock);
	writable = (whw->state == WIN32_HANDLE_WRITER_RUNNING &&
	    !whw->stop && whw->handle != NULL);
	LeaveCriticalSection(&whw->lock);
	return (writable);
}

int
win32_handle_writer_closed(struct win32_handle_writer *whw)
{
	int	closed;

	if (whw == NULL)
		return (1);
	EnterCriticalSection(&whw->lock);
	closed = (whw->state == WIN32_HANDLE_WRITER_CLOSED ||
	    whw->handle == NULL);
	LeaveCriticalSection(&whw->lock);
	return (closed);
}

int
win32_handle_writer_error(struct win32_handle_writer *whw)
{
	int	error;

	if (whw == NULL)
		return (0);
	EnterCriticalSection(&whw->lock);
	error = (whw->state == WIN32_HANDLE_WRITER_ERROR);
	LeaveCriticalSection(&whw->lock);
	return (error);
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
