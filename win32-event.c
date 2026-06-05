/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include <sys/types.h>
#include <sys/un.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

enum win32_process_event_state {
	WIN32_PROCESS_EVENT_RUNNING,
	WIN32_PROCESS_EVENT_EXITED,
	WIN32_PROCESS_EVENT_CANCELED
};

enum win32_handle_event_backend {
	WIN32_HANDLE_EVENT_WORKER,
	WIN32_HANDLE_EVENT_CONSOLE,
	WIN32_HANDLE_EVENT_STDIO,
	WIN32_HANDLE_EVENT_TERMINAL,
	WIN32_HANDLE_EVENT_IOCP
};

enum win32_console_input_action {
	WIN32_CONSOLE_INPUT_WAIT,
	WIN32_CONSOLE_INPUT_READ,
	WIN32_CONSOLE_INPUT_APPENDED,
	WIN32_CONSOLE_INPUT_ERROR
};

enum win32_handle_writer_backend {
	WIN32_HANDLE_WRITER_WORKER,
	WIN32_HANDLE_WRITER_CONSOLE,
	WIN32_HANDLE_WRITER_IOCP
};

enum win32_io_endpoint_type {
	WIN32_IO_ENDPOINT_READER,
	WIN32_IO_ENDPOINT_WRITER,
	WIN32_IO_ENDPOINT_PROCESS
};

struct win32_io_endpoint {
	TAILQ_ENTRY(win32_io_endpoint) entry;
	enum win32_io_endpoint_type type;
	void		*owner;
	void		(*eventcb)(void *, uint32_t);
	void		*arg;
	uint32_t	 events;
	int		 pending;
	int		 active;
};

static int
win32_socket_errno(int error)
{
	switch (error) {
	case WSAEACCES:
		return (EACCES);
	case WSAEADDRINUSE:
		return (EADDRINUSE);
	case WSAEADDRNOTAVAIL:
		return (EADDRNOTAVAIL);
	case WSAEAFNOSUPPORT:
		return (EAFNOSUPPORT);
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
	case WSAEINVAL:
		return (EINVAL);
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
			if (n == 0) {
				errno = ECONNRESET;
				return (total == 0 ? -1 : total);
			}
			total += n;
			base += n;
			len -= n;
		}
	}
	return (total);
}

struct win32_handle_event {
	struct win32_io_endpoint endpoint;
	TAILQ_ENTRY(win32_handle_event) console_entry;
	HANDLE		 handle;
	HANDLE		 thread;
	HANDLE		 stop;
	HANDLE		 ready;
	HANDLE		 changed;
	HANDLE		 complete;
	OVERLAPPED	 overlapped;
	char		 iocp_buf[8192];
	struct evbuffer	*input;
	CRITICAL_SECTION lock;
	uint64_t	 offset;
	enum win32_handle_event_backend backend;
	int		 use_offset;
	int		 pending;
	int		 paused;
	int		 stopping;
	int		 throttled;
	int		 console_queued;
	int		 console_active;
	DWORD		 console_key_state;
	enum win32_handle_event_state state;
	DWORD		 error;
};

struct win32_handle_writer {
	struct win32_io_endpoint endpoint;
	TAILQ_ENTRY(win32_handle_writer) console_entry;
	HANDLE		 handle;
	HANDLE		 thread;
	HANDLE		 ready;
	HANDLE		 complete;
	OVERLAPPED	 overlapped;
	char		*iocp_buf;
	size_t		 iocp_size;
	struct evbuffer	*output;
	CRITICAL_SECTION lock;
	uint64_t	 offset;
	enum win32_handle_writer_backend backend;
	enum win32_handle_writer_state state;
	int		 use_offset;
	int		 append;
	int		 pending;
	int		 borrowed;
	int		 stop;
	int		 console_queued;
	int		 console_active;
	size_t		 write_progress;
	u_char		 utf8_partial[4];
	size_t		 utf8_partial_len;
	int		 test_utf8_split_fired;
	int		 test_invalid_utf8_fired;
};

struct win32_process_event {
	struct win32_io_endpoint endpoint;
	HANDLE		 wait;
	CRITICAL_SECTION lock;
	enum win32_process_event_state state;
};

struct win32_io_service {
	SOCKET		 notify_read;
	SOCKET		 notify_write;
	struct event	 event;
	CRITICAL_SECTION lock;
	HANDLE		 iocp;
	HANDLE		 iocp_thread;
	TAILQ_HEAD(, win32_io_endpoint) pending;
	int		 initialized;
	int		 event_added;
	int		 notify_pending;
};

struct win32_console_writer_service {
	HANDLE		 wake;
	HANDLE		 thread;
	CRITICAL_SECTION lock;
	TAILQ_HEAD(, win32_handle_writer) pending;
	int		 initialized;
	int		 stopping;
};

struct win32_console_reader_service {
	HANDLE		 wake;
	HANDLE		 stop;
	HANDLE		 thread;
	CRITICAL_SECTION lock;
	TAILQ_HEAD(, win32_handle_event) pending;
	int		 initialized;
	int		 stopping;
};

static struct win32_io_service win32_io;
static struct win32_console_writer_service win32_console_writer;
static struct win32_console_reader_service win32_console_reader;

#define WIN32_HANDLE_EVENT_HIGH (1024 * 1024)
#define WIN32_HANDLE_EVENT_LOW (512 * 1024)
#define WIN32_HANDLE_WRITER_HIGH (1024 * 1024)
#define WIN32_HANDLE_WRITER_CHUNK (256 * 1024)
#define WIN32_WORKER_STOP_TIMEOUT 1000
#define WIN32_IOCP_STOP_TIMEOUT 1000
#define WIN32_IOCP_SERVICE_STOP_TIMEOUT 1000
#define WIN32_CONSOLE_WRITER_STOP_TIMEOUT 1000
#define WIN32_CONSOLE_READER_STOP_TIMEOUT 1000
#define WIN32_PROCESS_WAIT_STOP_TIMEOUT 1000
#define WIN32_IOCP_STOP_KEY ((ULONG_PTR)-1)

static int	win32_io_service_init(void);
static int	win32_io_service_init_iocp(void);
static DWORD WINAPI win32_io_service_iocp_thread(void *);
static void	win32_io_endpoint_init(struct win32_io_endpoint *,
		     enum win32_io_endpoint_type, void *,
		     void (*)(void *, uint32_t), void *);
static void	win32_io_service_enqueue_endpoint(struct win32_io_endpoint *,
		     uint32_t);
static void	win32_io_service_deactivate_endpoint(
		     struct win32_io_endpoint *);
static void	win32_io_service_wakeup_locked(void);
static void	win32_io_service_enqueue_reader(struct win32_handle_event *);
static void	win32_io_service_enqueue_writer(struct win32_handle_writer *,
		     uint32_t);
static void	win32_io_service_enqueue_process(struct win32_process_event *,
		     uint32_t);
static void	win32_io_service_cb(evutil_socket_t, short, void *);
static void	win32_io_service_dispatch_endpoint(
		     struct win32_io_endpoint *, uint32_t);
static int	win32_wait_worker_thread(HANDLE, const char *);
static int	win32_wait_iocp_endpoint(HANDLE, const char *);
static int	win32_wait_iocp_thread(HANDLE, const char *);
static int	win32_wait_console_endpoint(HANDLE, const char *);
static int	win32_wait_console_thread(HANDLE, const char *);
static int	win32_wait_console_reader_endpoint(HANDLE, const char *);
static int	win32_wait_console_reader_thread(HANDLE, const char *);
static int	win32_unregister_process_wait(struct win32_process_event *,
		     const char *);
static void	win32_handle_event_update_ready(
		     struct win32_handle_event *);
static int	win32_handle_event_reading_enabled(
		     struct win32_handle_event *);
static int	win32_handle_event_prepare_console_wait(
		     struct win32_handle_event *);
static uint32_t	win32_handle_event_iocp_start(struct win32_handle_event *);
static void	win32_handle_event_iocp_complete(
		     struct win32_handle_event *, DWORD, DWORD);
static int	win32_handle_event_append_input(
		     struct win32_handle_event *, const void *, size_t);
static int	win32_handle_event_read_once(struct win32_handle_event *);
static void	win32_console_key_record_update_state(
		     struct win32_handle_event *, const KEY_EVENT_RECORD *);
static int	win32_console_key_record_ignore(const KEY_EVENT_RECORD *);
static int	win32_console_key_record_ctrl_j(const KEY_EVENT_RECORD *,
		     DWORD);
static int	win32_console_read_one_record(HANDLE, INPUT_RECORD *);
static int	win32_console_peek_one_record(HANDLE, INPUT_RECORD *);
static int	win32_console_emit_ctrl_j(struct win32_handle_event *,
		     const KEY_EVENT_RECORD *);
static enum win32_console_input_action
		win32_console_reader_prepare_key_input(
		     struct win32_handle_event *);
static DWORD WINAPI win32_handle_event_thread(void *);
static int	win32_console_reader_service_init(void);
static int	win32_console_reader_service_fini(const char *);
static DWORD WINAPI win32_console_reader_thread(void *);
static void	win32_console_reader_enqueue_locked(
		     struct win32_handle_event *);
static void	win32_console_reader_enqueue(struct win32_handle_event *);
static void	win32_console_reader_run(struct win32_handle_event *);
static int	win32_console_writer_service_init(void);
static int	win32_console_writer_service_fini(const char *);
static DWORD WINAPI win32_console_writer_thread(void *);
static void	win32_console_writer_enqueue_locked(
		     struct win32_handle_writer *);
static void	win32_console_writer_enqueue(struct win32_handle_writer *);
static void	win32_console_writer_run(struct win32_handle_writer *);
static uint32_t	win32_handle_writer_iocp_start(
		     struct win32_handle_writer *);
static void	win32_handle_writer_iocp_complete(
		     struct win32_handle_writer *, DWORD, DWORD);
static int	win32_handle_event_error_is_eof(DWORD);
static void	win32_handle_event_free(struct win32_handle_event *);
static void	win32_handle_writer_free(struct win32_handle_writer *);
static void	win32_process_event_free(struct win32_process_event *);
static int	win32_console_handle(HANDLE);
static int	win32_handle_write_file(HANDLE, const void *, size_t, DWORD *);
static int	win32_handle_write_console(HANDLE, const u_char *, size_t);
static int	win32_handle_write_console_sanitized(HANDLE, const u_char *,
		     size_t);
static int	win32_handle_write_console_utf8(struct win32_handle_writer *,
		     HANDLE, const void *, size_t);
static int	win32_console_test_env_enabled(const char *);
static size_t	win32_console_test_utf8_split_at(const u_char *, size_t);
static u_char	*win32_console_test_invalid_utf8(const u_char *, size_t);
static enum win32_handle_writer_backend
		win32_handle_writer_backend_for_handle(HANDLE *);
static void	win32_handle_writer_record_progress(
		     struct win32_handle_writer *, size_t);

static int
win32_socketpair(SOCKET pair[2])
{
	static LONG		 serial;
	struct sockaddr_un	 sun;
	SOCKET			 listener = INVALID_SOCKET;
	SOCKET			 client = INVALID_SOCKET;
	SOCKET			 server = INVALID_SOCKET;
	const char		*dir;
	char			*path = NULL, *cause = NULL;
	size_t			 i, size;
	ULONGLONG		 tick;
	int			 error, saved_errno;

	pair[0] = INVALID_SOCKET;
	pair[1] = INVALID_SOCKET;

	dir = win32_default_socket_dir();
	if (dir == NULL) {
		errno = ENOENT;
		return (-1);
	}
	if (win32_ipc_ensure_socket_dir(dir, &cause) != 0) {
		log_debug("%s: couldn't prepare AF_UNIX wakeup dir: %s",
		    __func__, cause);
		free(cause);
		return (-1);
	}

	tick = GetTickCount64();
	xasprintf(&path, "%s/io-%lx-%lx-%llx.sock", dir,
	    (u_long)GetCurrentProcessId(),
	    (u_long)InterlockedIncrement(&serial),
	    (unsigned long long)tick);
	size = strlen(path);
	if (size >= sizeof sun.sun_path) {
		free(path);
		errno = ENAMETOOLONG;
		return (-1);
	}

	memset(&sun, 0, sizeof sun);
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, path, sizeof sun.sun_path);
	for (i = 0; sun.sun_path[i] != '\0'; i++) {
		if (sun.sun_path[i] == '/')
			sun.sun_path[i] = '\\';
	}

	(void)win32_unlink_utf8(path);

	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener == INVALID_SOCKET)
		goto fail;
	if (bind(listener, (struct sockaddr *)&sun, sizeof sun) != 0 ||
	    listen(listener, 1) != 0)
		goto fail;

	client = socket(AF_UNIX, SOCK_STREAM, 0);
	if (client == INVALID_SOCKET)
		goto fail;
	if (connect(client, (struct sockaddr *)&sun, sizeof sun) != 0)
		goto fail;

	server = accept(listener, NULL, NULL);
	if (server == INVALID_SOCKET)
		goto fail;

	closesocket(listener);
	(void)win32_unlink_utf8(path);
	free(path);
	pair[0] = client;
	pair[1] = server;
	return (0);

fail:
	error = WSAGetLastError();
	saved_errno = win32_socket_errno(error);
	if (client != INVALID_SOCKET)
		closesocket(client);
	if (server != INVALID_SOCKET)
		closesocket(server);
	if (listener != INVALID_SOCKET)
		closesocket(listener);
	if (path != NULL) {
		(void)win32_unlink_utf8(path);
		free(path);
	}
	errno = saved_errno;
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
	win32_io.iocp = NULL;
	win32_io.iocp_thread = NULL;
	TAILQ_INIT(&win32_io.pending);
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

static int
win32_io_service_init_iocp(void)
{
	if (win32_io.iocp != NULL)
		return (0);
	if (win32_io_service_init() != 0)
		return (-1);
	win32_io.iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0,
	    0);
	if (win32_io.iocp == NULL)
		return (-1);
	win32_io.iocp_thread = CreateThread(NULL, 0,
	    win32_io_service_iocp_thread, NULL, 0, NULL);
	if (win32_io.iocp_thread == NULL) {
		CloseHandle(win32_io.iocp);
		win32_io.iocp = NULL;
		return (-1);
	}
	return (0);
}

void
win32_io_service_fini(void)
{
	if (!win32_io.initialized)
		return;
	if (win32_io.iocp != NULL)
		PostQueuedCompletionStatus(win32_io.iocp, 0,
		    WIN32_IOCP_STOP_KEY, NULL);
	if (win32_wait_iocp_thread(win32_io.iocp_thread, __func__) != 0)
		return;
	if (win32_console_reader_service_fini(__func__) != 0)
		return;
	if (win32_console_writer_service_fini(__func__) != 0)
		return;
	if (win32_io.event_added)
		event_del(&win32_io.event);
	if (win32_io.iocp_thread != NULL)
		CloseHandle(win32_io.iocp_thread);
	if (win32_io.iocp != NULL)
		CloseHandle(win32_io.iocp);
	if (win32_io.notify_read != INVALID_SOCKET)
		closesocket(win32_io.notify_read);
	if (win32_io.notify_write != INVALID_SOCKET)
		closesocket(win32_io.notify_write);
	DeleteCriticalSection(&win32_io.lock);
	memset(&win32_io, 0, sizeof win32_io);
}

static void
win32_io_endpoint_init(struct win32_io_endpoint *endpoint,
    enum win32_io_endpoint_type type, void *owner,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	endpoint->type = type;
	endpoint->owner = owner;
	endpoint->eventcb = eventcb;
	endpoint->arg = arg;
	endpoint->active = 1;
}

static void
win32_io_service_wakeup_locked(void)
{
	char	one = 1;
	int	error;

	if (win32_io.notify_pending)
		return;
	if (send(win32_io.notify_write, &one, 1, 0) == 1) {
		win32_io.notify_pending = 1;
		return;
	}
	error = WSAGetLastError();
	if (error == WSAEWOULDBLOCK) {
		win32_io.notify_pending = 1;
		return;
	}
	log_debug("%s: wakeup send failed: %s", __func__,
	    win32_strerror(error));
}

static void
win32_io_service_enqueue_endpoint(struct win32_io_endpoint *endpoint,
    uint32_t events)
{
	EnterCriticalSection(&win32_io.lock);
	endpoint->events |= events;
	if (endpoint->active && !endpoint->pending) {
		TAILQ_INSERT_TAIL(&win32_io.pending, endpoint, entry);
		endpoint->pending = 1;
		win32_io_service_wakeup_locked();
	}
	LeaveCriticalSection(&win32_io.lock);
}

static void
win32_io_service_deactivate_endpoint(struct win32_io_endpoint *endpoint)
{
	if (!win32_io.initialized)
		return;
	EnterCriticalSection(&win32_io.lock);
	endpoint->active = 0;
	if (endpoint->pending) {
		TAILQ_REMOVE(&win32_io.pending, endpoint, entry);
		endpoint->pending = 0;
	}
	LeaveCriticalSection(&win32_io.lock);
}

static void
win32_io_service_enqueue_reader(struct win32_handle_event *whe)
{
	uint32_t	events;

	EnterCriticalSection(&whe->lock);
	if (whe->state == WIN32_HANDLE_EVENT_RUNNING)
		events = WIN32_IO_EVENT_READ;
	else if (whe->state == WIN32_HANDLE_EVENT_EOF)
		events = WIN32_IO_EVENT_READ_EOF;
	else
		events = WIN32_IO_EVENT_ERROR;
	LeaveCriticalSection(&whe->lock);
	win32_io_service_enqueue_endpoint(&whe->endpoint, events);
}

static void
win32_io_service_enqueue_writer(struct win32_handle_writer *whw,
    uint32_t events)
{
	win32_io_service_enqueue_endpoint(&whw->endpoint, events);
}

static void
win32_io_service_enqueue_process(struct win32_process_event *wpe,
    uint32_t events)
{
	win32_io_service_enqueue_endpoint(&wpe->endpoint, events);
}

static DWORD WINAPI
win32_io_service_iocp_thread(__unused void *arg)
{
	struct win32_io_endpoint		*endpoint;
	OVERLAPPED			*overlapped;
	DWORD				 transferred, error;
	ULONG_PTR			 key;

	for (;;) {
		error = ERROR_SUCCESS;
		if (!GetQueuedCompletionStatus(win32_io.iocp, &transferred,
		    &key, &overlapped, INFINITE))
			error = GetLastError();
		if (overlapped == NULL && key == WIN32_IOCP_STOP_KEY)
			break;
		if (overlapped == NULL)
			continue;
		endpoint = (struct win32_io_endpoint *)key;
		if (endpoint->type == WIN32_IO_ENDPOINT_READER) {
			win32_handle_event_iocp_complete(endpoint->owner,
			    error, transferred);
		} else if (endpoint->type == WIN32_IO_ENDPOINT_WRITER) {
			win32_handle_writer_iocp_complete(endpoint->owner,
			    error, transferred);
		}
	}
	return (0);
}

static void
win32_io_service_dispatch_reader(struct win32_handle_event *whe,
    uint32_t events)
{
	size_t				 buffered;
	enum win32_handle_event_state	 state;

	EnterCriticalSection(&whe->lock);
	state = whe->state;
	buffered = EVBUFFER_LENGTH(whe->input);
	LeaveCriticalSection(&whe->lock);

	if (state == WIN32_HANDLE_EVENT_EOF)
		events |= WIN32_IO_EVENT_READ_EOF;
	else if (state == WIN32_HANDLE_EVENT_ERROR)
		events |= WIN32_IO_EVENT_ERROR;
	if (buffered != 0)
		events |= WIN32_IO_EVENT_READ;

	whe->endpoint.eventcb(whe->endpoint.arg, events);
}

static void
win32_io_service_dispatch_writer(struct win32_handle_writer *whw,
    uint32_t events)
{
	enum win32_handle_writer_state	 state;
	size_t				 buffered;
	int				 pending;
	uint32_t			 out;

	EnterCriticalSection(&whw->lock);
	state = whw->state;
	buffered = EVBUFFER_LENGTH(whw->output);
	pending = whw->pending;
	LeaveCriticalSection(&whw->lock);

	out = events & ~(WIN32_IO_EVENT_WRITE_DRAINED|
	    WIN32_IO_EVENT_WRITE_CLOSED|WIN32_IO_EVENT_ERROR);
	if (state == WIN32_HANDLE_WRITER_ERROR)
		out |= WIN32_IO_EVENT_ERROR;
	else if (state == WIN32_HANDLE_WRITER_CLOSED)
		out |= WIN32_IO_EVENT_WRITE_CLOSED;
	else if ((events & WIN32_IO_EVENT_WRITE_DRAINED) && buffered == 0 &&
	    !pending)
		out |= WIN32_IO_EVENT_WRITE_DRAINED;
	if (out != 0)
		whw->endpoint.eventcb(whw->endpoint.arg, out);
}

static void
win32_handle_writer_record_progress(struct win32_handle_writer *whw, size_t size)
{
	if (size == 0)
		return;
	if (size > SIZE_MAX - whw->write_progress)
		fatalx("Win32 writer progress overflow");
	whw->write_progress += size;
}

static void
win32_io_service_dispatch_process(struct win32_process_event *wpe,
    uint32_t events)
{
	enum win32_process_event_state state;

	EnterCriticalSection(&wpe->lock);
	state = wpe->state;
	LeaveCriticalSection(&wpe->lock);

	if (state == WIN32_PROCESS_EVENT_EXITED &&
	    (events & WIN32_IO_EVENT_PROCESS_EXIT))
		wpe->endpoint.eventcb(wpe->endpoint.arg, events);
}

static void
win32_io_service_dispatch_endpoint(struct win32_io_endpoint *endpoint,
    uint32_t events)
{
	switch (endpoint->type) {
	case WIN32_IO_ENDPOINT_READER:
		win32_io_service_dispatch_reader(endpoint->owner, events);
		break;
	case WIN32_IO_ENDPOINT_WRITER:
		win32_io_service_dispatch_writer(endpoint->owner, events);
		break;
	case WIN32_IO_ENDPOINT_PROCESS:
		win32_io_service_dispatch_process(endpoint->owner, events);
		break;
	}
}

void
win32_io_endpoint_free(struct win32_io_endpoint *endpoint)
{
	if (endpoint == NULL)
		return;
	switch (endpoint->type) {
	case WIN32_IO_ENDPOINT_READER:
		win32_handle_event_free(endpoint->owner);
		break;
	case WIN32_IO_ENDPOINT_WRITER:
		win32_handle_writer_free(endpoint->owner);
		break;
	case WIN32_IO_ENDPOINT_PROCESS:
		win32_process_event_free(endpoint->owner);
		break;
	}
}

static int
win32_wait_worker_thread(HANDLE thread, const char *name)
{
	DWORD	wait;

	if (thread == NULL)
		return (0);
	wait = WaitForSingleObject(thread, WIN32_WORKER_STOP_TIMEOUT);
	if (wait == WAIT_OBJECT_0)
		return (0);
	log_debug("%s: worker thread did not stop within %u ms",
	    name, WIN32_WORKER_STOP_TIMEOUT);
	return (-1);
}

static int
win32_wait_iocp_endpoint(HANDLE complete, const char *name)
{
	DWORD	wait;

	if (complete == NULL)
		return (0);
	wait = WaitForSingleObject(complete, WIN32_IOCP_STOP_TIMEOUT);
	if (wait == WAIT_OBJECT_0)
		return (0);
	log_debug("%s: IOCP endpoint did not stop within %u ms",
	    name, WIN32_IOCP_STOP_TIMEOUT);
	return (-1);
}

static int
win32_wait_iocp_thread(HANDLE thread, const char *name)
{
	DWORD	wait;

	if (thread == NULL)
		return (0);
	wait = WaitForSingleObject(thread, WIN32_IOCP_SERVICE_STOP_TIMEOUT);
	if (wait == WAIT_OBJECT_0)
		return (0);
	log_debug("%s: IOCP service thread did not stop within %u ms",
	    name, WIN32_IOCP_SERVICE_STOP_TIMEOUT);
	return (-1);
}

static int
win32_wait_console_endpoint(HANDLE complete, const char *name)
{
	DWORD	wait;

	if (complete == NULL)
		return (0);
	wait = WaitForSingleObject(complete, WIN32_CONSOLE_WRITER_STOP_TIMEOUT);
	if (wait == WAIT_OBJECT_0)
		return (0);
	log_debug("%s: console writer endpoint did not stop within %u ms",
	    name, WIN32_CONSOLE_WRITER_STOP_TIMEOUT);
	return (-1);
}

static int
win32_wait_console_thread(HANDLE thread, const char *name)
{
	DWORD	wait;

	if (thread == NULL)
		return (0);
	wait = WaitForSingleObject(thread, WIN32_CONSOLE_WRITER_STOP_TIMEOUT);
	if (wait == WAIT_OBJECT_0)
		return (0);
	log_debug("%s: console writer thread did not stop within %u ms",
	    name, WIN32_CONSOLE_WRITER_STOP_TIMEOUT);
	return (-1);
}

static int
win32_wait_console_reader_endpoint(HANDLE complete, const char *name)
{
	DWORD	wait;

	if (complete == NULL)
		return (0);
	wait = WaitForSingleObject(complete, WIN32_CONSOLE_READER_STOP_TIMEOUT);
	if (wait == WAIT_OBJECT_0)
		return (0);
	log_debug("%s: console reader endpoint did not stop within %u ms",
	    name, WIN32_CONSOLE_READER_STOP_TIMEOUT);
	return (-1);
}

static int
win32_wait_console_reader_thread(HANDLE thread, const char *name)
{
	DWORD	wait;

	if (thread == NULL)
		return (0);
	wait = WaitForSingleObject(thread, WIN32_CONSOLE_READER_STOP_TIMEOUT);
	if (wait == WAIT_OBJECT_0)
		return (0);
	log_debug("%s: console reader thread did not stop within %u ms",
	    name, WIN32_CONSOLE_READER_STOP_TIMEOUT);
	return (-1);
}

static int
win32_unregister_process_wait(struct win32_process_event *wpe, const char *name)
{
	HANDLE	complete;
	DWORD	error = ERROR_SUCCESS, wait;
	int	pending = 0;

	if (wpe->wait == NULL)
		return (0);
	complete = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (complete == NULL) {
		log_debug("%s: CreateEventW failed: %s", name,
		    win32_strerror(GetLastError()));
		return (-1);
	}
	if (!UnregisterWaitEx(wpe->wait, complete)) {
		error = GetLastError();
		if (error == ERROR_IO_PENDING)
			pending = 1;
		else {
			CloseHandle(complete);
			log_debug("%s: UnregisterWaitEx failed: %s", name,
			    win32_strerror(error));
			return (-1);
		}
	}
	wait = WaitForSingleObject(complete, WIN32_PROCESS_WAIT_STOP_TIMEOUT);
	if (wait == WAIT_OBJECT_0) {
		CloseHandle(complete);
		return (0);
	}
	if (pending) {
		log_debug("%s: process wait callback did not finish within %u ms",
		    name, WIN32_PROCESS_WAIT_STOP_TIMEOUT);
	} else {
		log_debug("%s: process wait did not unregister within %u ms",
		    name, WIN32_PROCESS_WAIT_STOP_TIMEOUT);
	}
	/*
	 * The completion event must remain valid until it is signaled. Leak it
	 * with the process endpoint rather than risking a callback use-after-free.
	 */
	return (-1);
}

static void
win32_io_service_dispatch_endpoints(void)
{
	struct win32_io_endpoint	*endpoint;
	uint32_t		 events;

	for (;;) {
		EnterCriticalSection(&win32_io.lock);
		endpoint = TAILQ_FIRST(&win32_io.pending);
		if (endpoint != NULL) {
			TAILQ_REMOVE(&win32_io.pending, endpoint, entry);
			endpoint->pending = 0;
			events = endpoint->events;
			endpoint->events = 0;
		}
		LeaveCriticalSection(&win32_io.lock);
		if (endpoint == NULL)
			break;

		win32_io_service_dispatch_endpoint(endpoint, events);
	}
}

static void
win32_io_service_cb(__unused evutil_socket_t fd, __unused short events,
    __unused void *arg)
{
	char	buf[64];

	while (recv(win32_io.notify_read, buf, sizeof buf, 0) > 0)
		;
	EnterCriticalSection(&win32_io.lock);
	win32_io.notify_pending = 0;
	LeaveCriticalSection(&win32_io.lock);

	win32_io_service_dispatch_endpoints();
}

static VOID CALLBACK
win32_process_event_wait_cb(PVOID arg, __unused BOOLEAN timed_out)
{
	struct win32_process_event	*wpe = arg;
	int				 notify = 0;

	EnterCriticalSection(&wpe->lock);
	if (wpe->state == WIN32_PROCESS_EVENT_RUNNING) {
		wpe->state = WIN32_PROCESS_EVENT_EXITED;
		notify = 1;
	}
	LeaveCriticalSection(&wpe->lock);

	if (notify)
		win32_io_service_enqueue_process(wpe,
		    WIN32_IO_EVENT_PROCESS_EXIT);
}

struct win32_io_endpoint *
win32_io_process_new(HANDLE process,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	struct win32_process_event	*wpe;

	if (process == NULL || process == INVALID_HANDLE_VALUE)
		return (NULL);
	if (eventcb == NULL)
		return (NULL);
	if (win32_io_service_init() != 0)
		return (NULL);

	wpe = xcalloc(1, sizeof *wpe);
	InitializeCriticalSection(&wpe->lock);
	wpe->state = WIN32_PROCESS_EVENT_RUNNING;
	win32_io_endpoint_init(&wpe->endpoint, WIN32_IO_ENDPOINT_PROCESS, wpe,
	    eventcb, arg);
	/*
	 * Process handles are wait-only objects rather than IOCP byte streams.
	 * The threadpool wait is the final backend; it only posts a typed
	 * completion back through the tmux-owned service queue.
	 */
	if (!RegisterWaitForSingleObject(&wpe->wait, process,
	    win32_process_event_wait_cb, wpe, INFINITE,
	    WT_EXECUTEONLYONCE)) {
		DeleteCriticalSection(&wpe->lock);
		free(wpe);
		return (NULL);
	}
	return (&wpe->endpoint);
}

static void
win32_process_event_free(struct win32_process_event *wpe)
{
	if (wpe == NULL)
		return;
	EnterCriticalSection(&wpe->lock);
	wpe->state = WIN32_PROCESS_EVENT_CANCELED;
	LeaveCriticalSection(&wpe->lock);
	win32_io_service_deactivate_endpoint(&wpe->endpoint);
	if (win32_unregister_process_wait(wpe, __func__) != 0)
		return;
	DeleteCriticalSection(&wpe->lock);
	free(wpe);
}

void
win32_io_process_notify(struct win32_io_endpoint *endpoint)
{
	struct win32_process_event	*wpe;
	int				 notify = 0;

	if (endpoint == NULL)
		return;
	wpe = endpoint->owner;
	if (wpe == NULL)
		return;
	EnterCriticalSection(&wpe->lock);
	if (wpe->state != WIN32_PROCESS_EVENT_CANCELED) {
		wpe->state = WIN32_PROCESS_EVENT_EXITED;
		notify = 1;
	}
	LeaveCriticalSection(&wpe->lock);
	if (notify)
		win32_io_service_enqueue_process(wpe,
		    WIN32_IO_EVENT_PROCESS_EXIT);
}

static void
win32_handle_event_update_ready(struct win32_handle_event *whe)
{
	size_t	buffered;
	int	ready;

	if (whe->backend == WIN32_HANDLE_EVENT_IOCP) {
		uint32_t events = win32_handle_event_iocp_start(whe);

		if (events != 0) {
			LeaveCriticalSection(&whe->lock);
			win32_io_service_enqueue_endpoint(&whe->endpoint,
			    events);
			EnterCriticalSection(&whe->lock);
		}
		return;
	}
	if (whe->backend == WIN32_HANDLE_EVENT_CONSOLE) {
		buffered = EVBUFFER_LENGTH(whe->input);
		if (whe->throttled && buffered <= WIN32_HANDLE_EVENT_LOW)
			whe->throttled = 0;
		if (whe->changed != NULL)
			SetEvent(whe->changed);
		if (whe->stopping || whe->paused || whe->throttled ||
		    whe->state != WIN32_HANDLE_EVENT_RUNNING)
			return;
		LeaveCriticalSection(&whe->lock);
		win32_console_reader_enqueue(whe);
		EnterCriticalSection(&whe->lock);
		return;
	}

	buffered = EVBUFFER_LENGTH(whe->input);
	if (whe->throttled && buffered <= WIN32_HANDLE_EVENT_LOW)
		whe->throttled = 0;
	if (whe->ready == NULL)
		return;
	ready = (!whe->paused && !whe->throttled &&
	    whe->state == WIN32_HANDLE_EVENT_RUNNING);
	if (ready)
		SetEvent(whe->ready);
	else
		ResetEvent(whe->ready);
	if (whe->changed != NULL)
		SetEvent(whe->changed);
}

static int
win32_handle_event_reading_enabled(struct win32_handle_event *whe)
{
	int	ready;

	EnterCriticalSection(&whe->lock);
	ready = (!whe->paused && !whe->throttled &&
	    whe->state == WIN32_HANDLE_EVENT_RUNNING);
	LeaveCriticalSection(&whe->lock);
	return (ready);
}

static int
win32_handle_event_prepare_console_wait(struct win32_handle_event *whe)
{
	int	ready;

	EnterCriticalSection(&whe->lock);
	ready = (!whe->paused && !whe->throttled &&
	    whe->state == WIN32_HANDLE_EVENT_RUNNING);
	if (ready && whe->changed != NULL)
		ResetEvent(whe->changed);
	LeaveCriticalSection(&whe->lock);
	return (ready);
}

static uint32_t
win32_handle_event_iocp_start(struct win32_handle_event *whe)
{
	size_t	buffered;
	DWORD	error, nread;
	uint32_t events;

	if (whe->backend != WIN32_HANDLE_EVENT_IOCP)
		return (0);
	buffered = EVBUFFER_LENGTH(whe->input);
	if (whe->throttled && buffered <= WIN32_HANDLE_EVENT_LOW)
		whe->throttled = 0;
	if (whe->stopping || whe->pending || whe->paused || whe->throttled ||
	    whe->state != WIN32_HANDLE_EVENT_RUNNING)
		return (0);

	memset(&whe->overlapped, 0, sizeof whe->overlapped);
	if (whe->use_offset) {
		whe->overlapped.Offset = (DWORD)(whe->offset & 0xffffffff);
		whe->overlapped.OffsetHigh = (DWORD)(whe->offset >> 32);
	}
	ResetEvent(whe->complete);
	whe->pending = 1;
	if (ReadFile(whe->handle, whe->iocp_buf, sizeof whe->iocp_buf, &nread,
	    &whe->overlapped))
		return (0);
	error = GetLastError();
	if (error == ERROR_IO_PENDING)
		return (0);

	whe->pending = 0;
	if (win32_handle_event_error_is_eof(error)) {
		whe->state = WIN32_HANDLE_EVENT_EOF;
		events = WIN32_IO_EVENT_READ_EOF;
	} else {
		whe->state = WIN32_HANDLE_EVENT_ERROR;
		events = WIN32_IO_EVENT_ERROR;
	}
	whe->error = error;
	SetEvent(whe->complete);
	return (events);
}

static void
win32_handle_event_iocp_complete(struct win32_handle_event *whe, DWORD error,
    DWORD nread)
{
	enum win32_handle_event_state	 state;
	uint32_t			 events = 0;
	int				 complete = 0;

	EnterCriticalSection(&whe->lock);
	if (whe->pending)
		whe->pending = 0;
	if (whe->stopping)
		goto out;
	if (whe->state != WIN32_HANDLE_EVENT_RUNNING) {
		events = WIN32_IO_EVENT_ERROR;
		goto out;
	}
	if (error != ERROR_SUCCESS) {
		if (win32_handle_event_error_is_eof(error))
			whe->state = WIN32_HANDLE_EVENT_EOF;
		else
			whe->state = WIN32_HANDLE_EVENT_ERROR;
		whe->error = error;
		events = (whe->state == WIN32_HANDLE_EVENT_EOF) ?
		    WIN32_IO_EVENT_READ_EOF : WIN32_IO_EVENT_ERROR;
		goto out;
	}
	if (nread == 0) {
		whe->state = WIN32_HANDLE_EVENT_EOF;
		whe->error = ERROR_SUCCESS;
		events = WIN32_IO_EVENT_READ_EOF;
		goto out;
	}
	if (evbuffer_add(whe->input, whe->iocp_buf, nread) != 0) {
		whe->state = WIN32_HANDLE_EVENT_ERROR;
		whe->error = ERROR_NOT_ENOUGH_MEMORY;
		events = WIN32_IO_EVENT_ERROR;
		goto out;
	}
	if (whe->use_offset)
		whe->offset += nread;
	if (EVBUFFER_LENGTH(whe->input) >= WIN32_HANDLE_EVENT_HIGH)
		whe->throttled = 1;
	events = WIN32_IO_EVENT_READ;

out:
	state = whe->state;
	if (state == WIN32_HANDLE_EVENT_RUNNING)
		events |= win32_handle_event_iocp_start(whe);
	if (events != 0)
		win32_io_service_enqueue_endpoint(&whe->endpoint, events);
	if (!whe->pending)
		complete = 1;
	if (complete)
		SetEvent(whe->complete);
	LeaveCriticalSection(&whe->lock);
}

static int
win32_handle_event_append_input(struct win32_handle_event *whe,
    const void *data, size_t size)
{
	enum win32_handle_event_state	state;

	if (size == 0)
		return (0);
	EnterCriticalSection(&whe->lock);
	if (evbuffer_add(whe->input, data, size) != 0) {
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
		return (-1);
	return (0);
}

static int
win32_console_key_record_ctrl_mask(WORD virtual_key)
{
	switch (virtual_key) {
	case VK_CONTROL:
		return (LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED);
	case VK_LCONTROL:
		return (LEFT_CTRL_PRESSED);
	case VK_RCONTROL:
		return (RIGHT_CTRL_PRESSED);
	}
	return (0);
}

static int
win32_console_key_record_alt_mask(WORD virtual_key)
{
	switch (virtual_key) {
	case VK_MENU:
		return (LEFT_ALT_PRESSED|RIGHT_ALT_PRESSED);
	case VK_LMENU:
		return (LEFT_ALT_PRESSED);
	case VK_RMENU:
		return (RIGHT_ALT_PRESSED);
	}
	return (0);
}

static void
win32_console_key_record_update_state(struct win32_handle_event *whe,
    const KEY_EVENT_RECORD *key)
{
	DWORD	mask;

	mask = win32_console_key_record_ctrl_mask(key->wVirtualKeyCode);
	if (mask == 0)
		mask = win32_console_key_record_alt_mask(key->wVirtualKeyCode);
	if (mask == 0)
		return;

	if (key->bKeyDown)
		whe->console_key_state |= mask;
	else
		whe->console_key_state &= ~mask;
}

static int
win32_console_key_record_ignore(const KEY_EVENT_RECORD *key)
{
	if (!key->bKeyDown)
		return (1);
	switch (key->wVirtualKeyCode) {
	case VK_SHIFT:
	case VK_LSHIFT:
	case VK_RSHIFT:
	case VK_CONTROL:
	case VK_LCONTROL:
	case VK_RCONTROL:
	case VK_MENU:
	case VK_LMENU:
	case VK_RMENU:
	case VK_CAPITAL:
	case VK_NUMLOCK:
	case VK_SCROLL:
		return (1);
	}
	return (0);
}

static int
win32_console_key_record_ctrl_j(const KEY_EVENT_RECORD *key, DWORD state)
{
	DWORD	ctrl, alt;

	if (!key->bKeyDown)
		return (0);

	ctrl = LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED;
	alt = LEFT_ALT_PRESSED|RIGHT_ALT_PRESSED;
	state |= key->dwControlKeyState;
	if ((state & ctrl) == 0 || (state & alt) != 0)
		return (0);

	if (key->wVirtualKeyCode == 'J' || key->uChar.UnicodeChar == L'\n')
		return (1);
	return (0);
}

static int
win32_console_read_one_record(HANDLE handle, INPUT_RECORD *record)
{
	DWORD	nread;

	if (!ReadConsoleInputW(handle, record, 1, &nread)) {
		log_debug("%s: ReadConsoleInputW failed: %s", __func__,
		    win32_strerror(GetLastError()));
		return (-1);
	}
	if (nread != 1)
		return (-1);
	return (0);
}

static int
win32_console_peek_one_record(HANDLE handle, INPUT_RECORD *record)
{
	DWORD	nread;

	if (!PeekConsoleInputW(handle, record, 1, &nread)) {
		log_debug("%s: PeekConsoleInputW failed: %s", __func__,
		    win32_strerror(GetLastError()));
		return (-1);
	}
	if (nread != 1)
		return (1);
	return (0);
}

static int
win32_console_emit_ctrl_j(struct win32_handle_event *whe,
    const KEY_EVENT_RECORD *key)
{
	char	buf[64];
	WORD	repeat, chunk;

	repeat = key->wRepeatCount;
	if (repeat == 0)
		repeat = 1;
	memset(buf, '\n', sizeof buf);

	while (repeat != 0) {
		chunk = repeat;
		if (chunk > (WORD)sizeof buf)
			chunk = (WORD)sizeof buf;
		if (win32_handle_event_append_input(whe, buf, chunk) != 0)
			return (-1);
		repeat -= chunk;
	}
	if (log_get_level() > 1)
		log_debug("%s: translated Ctrl-J key record to LF", __func__);
	return (0);
}

static enum win32_console_input_action
win32_console_reader_prepare_key_input(struct win32_handle_event *whe)
{
	INPUT_RECORD	record;
	int		rc, appended = 0;

	/*
	 * Keep mouse and ordinary key input on the ReadFile byte path, but
	 * translate Ctrl-J key records that some console hosts do not expose as
	 * bytes. Modifier state is tracked across ignored modifier records so a
	 * physical Ctrl-down, J-down sequence still produces LF.
	 */
	for (;;) {
		rc = win32_console_peek_one_record(whe->handle, &record);
		if (rc < 0)
			return (appended ? WIN32_CONSOLE_INPUT_APPENDED :
			    WIN32_CONSOLE_INPUT_READ);
		if (rc > 0)
			return (appended ? WIN32_CONSOLE_INPUT_APPENDED :
			    WIN32_CONSOLE_INPUT_WAIT);

		switch (record.EventType) {
		case KEY_EVENT:
			win32_console_key_record_update_state(whe,
			    &record.Event.KeyEvent);
			if (win32_console_key_record_ignore(
			    &record.Event.KeyEvent)) {
				if (win32_console_read_one_record(whe->handle,
				    &record) != 0)
					return (appended ?
					    WIN32_CONSOLE_INPUT_APPENDED :
					    WIN32_CONSOLE_INPUT_READ);
				continue;
			}
			if (!win32_console_key_record_ctrl_j(
			    &record.Event.KeyEvent, whe->console_key_state))
				return (appended ? WIN32_CONSOLE_INPUT_APPENDED :
				    WIN32_CONSOLE_INPUT_READ);
			if (win32_console_read_one_record(whe->handle,
			    &record) != 0)
				return (appended ? WIN32_CONSOLE_INPUT_APPENDED :
				    WIN32_CONSOLE_INPUT_READ);
			if (win32_console_emit_ctrl_j(whe,
			    &record.Event.KeyEvent) != 0)
				return (WIN32_CONSOLE_INPUT_ERROR);
			appended = 1;
			continue;
		case MOUSE_EVENT:
			return (appended ? WIN32_CONSOLE_INPUT_APPENDED :
			    WIN32_CONSOLE_INPUT_READ);
		case WINDOW_BUFFER_SIZE_EVENT:
		case MENU_EVENT:
		case FOCUS_EVENT:
		default:
			if (win32_console_read_one_record(whe->handle,
			    &record) != 0)
				return (appended ?
				    WIN32_CONSOLE_INPUT_APPENDED :
				    WIN32_CONSOLE_INPUT_READ);
			continue;
		}
	}
}

static int
win32_console_test_env_enabled(const char *name)
{
	char	*value;
	int	 enabled;

	value = win32_getenv_utf8(name);
	enabled = value != NULL && strcmp(value, "0") != 0;
	free(value);
	return (enabled);
}

static size_t
win32_console_test_utf8_split_at(const u_char *data, size_t size)
{
	static const u_char marker[] = {
		0xe6, 0x96, 0x87,
		'U', 'T', 'F', '8', '-', 'S', 'P', 'L', 'I', 'T', '-', 'O',
		'K'
	};
	const u_char	*found;

	found = memmem(data, size, marker, sizeof marker);
	if (found == NULL)
		return ((size_t)-1);
	return ((size_t)(found - data) + 1);
}

static u_char *
win32_console_test_invalid_utf8(const u_char *data, size_t size)
{
	static const u_char marker[] = {
		'U', 'T', 'F', '8', '-', 'I', 'N', 'V', 'A', 'L', 'I', 'D',
		'-', 'P', 'R', 'E', 'F', 'I', 'X', '-'
	};
	u_char		*copy;
	const u_char	*found;
	size_t		 offset;

	found = memmem(data, size, marker, sizeof marker);
	if (found == NULL)
		return (NULL);
	offset = (size_t)(found - data) + sizeof marker;
	if (offset + 1 >= size)
		return (NULL);
	copy = xmalloc(size);
	memcpy(copy, data, size);
	copy[offset] = 0xe6;
	copy[offset + 1] = 0x41;
	return (copy);
}

static int
win32_handle_event_error_is_eof(DWORD error)
{
	return (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF ||
	    error == ERROR_NO_DATA);
}

static int
win32_handle_event_read_once(struct win32_handle_event *whe)
{
	DWORD				 nread;
	char				 buf[8192];
	u_int				 i;

	if (!ReadFile(whe->handle, buf, sizeof buf, &nread, NULL)) {
		DWORD error = GetLastError();
		int eof = win32_handle_event_error_is_eof(error);

		EnterCriticalSection(&whe->lock);
		if (whe->stopping) {
			if (eof)
				whe->state = WIN32_HANDLE_EVENT_EOF;
			else
				whe->state = WIN32_HANDLE_EVENT_ERROR;
			whe->error = error;
			LeaveCriticalSection(&whe->lock);
			return (-1);
		}
		if (eof)
			whe->state = WIN32_HANDLE_EVENT_EOF;
		else
			whe->state = WIN32_HANDLE_EVENT_ERROR;
		whe->error = error;
		win32_handle_event_update_ready(whe);
		LeaveCriticalSection(&whe->lock);
		if (!eof) {
			log_debug("%s: ReadFile failed: %s", __func__,
			    win32_strerror(error));
		}
		win32_io_service_enqueue_reader(whe);
		return (-1);
	}
	if (nread == 0) {
		EnterCriticalSection(&whe->lock);
		whe->state = WIN32_HANDLE_EVENT_EOF;
		whe->error = ERROR_SUCCESS;
		win32_handle_event_update_ready(whe);
		LeaveCriticalSection(&whe->lock);
		win32_io_service_enqueue_reader(whe);
		return (-1);
	}
	if (log_get_level() > 1) {
		for (i = 0; i < nread; i++) {
			if (buf[i] == '\003') {
				log_debug("%s: read Ctrl-C byte", __func__);
				break;
			}
		}
	}
	if (win32_handle_event_append_input(whe, buf, nread) != 0)
		return (-1);
	return (0);
}

static DWORD WINAPI
win32_handle_event_thread(void *arg)
{
	struct win32_handle_event	*whe = arg;
	HANDLE				 events[2];
	DWORD				 wait;

	events[0] = whe->stop;
	events[1] = whe->ready;
	for (;;) {
		wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
		if (wait != WAIT_OBJECT_0 + 1)
			break;
		if (win32_handle_event_read_once(whe) != 0)
			break;
	}
	return (0);
}

static int
win32_console_reader_service_init(void)
{
	if (win32_console_reader.initialized)
		return (0);

	win32_console_reader.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (win32_console_reader.wake == NULL)
		return (-1);
	win32_console_reader.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (win32_console_reader.stop == NULL) {
		CloseHandle(win32_console_reader.wake);
		memset(&win32_console_reader, 0, sizeof win32_console_reader);
		return (-1);
	}
	InitializeCriticalSection(&win32_console_reader.lock);
	TAILQ_INIT(&win32_console_reader.pending);
	win32_console_reader.thread = CreateThread(NULL, 0,
	    win32_console_reader_thread, NULL, 0, NULL);
	if (win32_console_reader.thread == NULL) {
		DeleteCriticalSection(&win32_console_reader.lock);
		CloseHandle(win32_console_reader.stop);
		CloseHandle(win32_console_reader.wake);
		memset(&win32_console_reader, 0, sizeof win32_console_reader);
		return (-1);
	}
	win32_console_reader.initialized = 1;
	return (0);
}

static int
win32_console_reader_service_fini(const char *name)
{
	struct win32_handle_event	*whe;

	if (!win32_console_reader.initialized)
		return (0);

	EnterCriticalSection(&win32_console_reader.lock);
	win32_console_reader.stopping = 1;
	TAILQ_FOREACH(whe, &win32_console_reader.pending, console_entry) {
		if (whe->changed != NULL)
			SetEvent(whe->changed);
	}
	SetEvent(win32_console_reader.stop);
	SetEvent(win32_console_reader.wake);
	LeaveCriticalSection(&win32_console_reader.lock);
	CancelSynchronousIo(win32_console_reader.thread);

	if (win32_wait_console_reader_thread(win32_console_reader.thread,
	    name) != 0)
		return (-1);

	CloseHandle(win32_console_reader.thread);
	CloseHandle(win32_console_reader.stop);
	CloseHandle(win32_console_reader.wake);
	DeleteCriticalSection(&win32_console_reader.lock);
	memset(&win32_console_reader, 0, sizeof win32_console_reader);
	return (0);
}

static void
win32_console_reader_enqueue_locked(struct win32_handle_event *whe)
{
	if (whe->console_queued || whe->console_active)
		return;
	TAILQ_INSERT_TAIL(&win32_console_reader.pending, whe, console_entry);
	whe->console_queued = 1;
	SetEvent(win32_console_reader.wake);
}

static void
win32_console_reader_enqueue(struct win32_handle_event *whe)
{
	EnterCriticalSection(&win32_console_reader.lock);
	win32_console_reader_enqueue_locked(whe);
	LeaveCriticalSection(&win32_console_reader.lock);
}

static DWORD WINAPI
win32_console_reader_thread(__unused void *arg)
{
	struct win32_handle_event	*whe;
	int				 stopping;

	for (;;) {
		WaitForSingleObject(win32_console_reader.wake, INFINITE);
		for (;;) {
			EnterCriticalSection(&win32_console_reader.lock);
			whe = TAILQ_FIRST(&win32_console_reader.pending);
			if (whe != NULL) {
				TAILQ_REMOVE(&win32_console_reader.pending, whe,
				    console_entry);
				whe->console_queued = 0;
				whe->console_active = 1;
			}
			stopping = win32_console_reader.stopping;
			LeaveCriticalSection(&win32_console_reader.lock);
			if (whe == NULL)
				break;

			if (!stopping)
				win32_console_reader_run(whe);

			EnterCriticalSection(&win32_console_reader.lock);
			EnterCriticalSection(&whe->lock);
			whe->console_active = 0;
			if (stopping || whe->stopping ||
			    whe->state != WIN32_HANDLE_EVENT_RUNNING) {
				SetEvent(whe->complete);
			} else if (!whe->paused && !whe->throttled) {
				win32_console_reader_enqueue_locked(whe);
			}
			LeaveCriticalSection(&whe->lock);
			LeaveCriticalSection(&win32_console_reader.lock);
		}
		if (stopping)
			break;
	}
	return (0);
}

static void
win32_console_reader_run(struct win32_handle_event *whe)
{
	HANDLE				events[4];
	DWORD				wait;
	enum win32_console_input_action	action;

	events[0] = win32_console_reader.stop;
	events[1] = whe->stop;
	events[2] = whe->changed;
	events[3] = whe->handle;

	if (!win32_handle_event_prepare_console_wait(whe))
		return;
	wait = WaitForMultipleObjects(4, events, FALSE, INFINITE);
	if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1 ||
	    wait == WAIT_OBJECT_0 + 2)
		return;
	if (wait != WAIT_OBJECT_0 + 3)
		return;
	if (!win32_handle_event_reading_enabled(whe))
		return;

	action = win32_console_reader_prepare_key_input(whe);
	switch (action) {
	case WIN32_CONSOLE_INPUT_READ:
		break;
	case WIN32_CONSOLE_INPUT_WAIT:
	case WIN32_CONSOLE_INPUT_APPENDED:
	case WIN32_CONSOLE_INPUT_ERROR:
		return;
	}
	(void)win32_handle_event_read_once(whe);
}

static int
win32_console_writer_service_init(void)
{
	if (win32_console_writer.initialized)
		return (0);

	win32_console_writer.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (win32_console_writer.wake == NULL)
		return (-1);
	InitializeCriticalSection(&win32_console_writer.lock);
	TAILQ_INIT(&win32_console_writer.pending);
	win32_console_writer.thread = CreateThread(NULL, 0,
	    win32_console_writer_thread, NULL, 0, NULL);
	if (win32_console_writer.thread == NULL) {
		DeleteCriticalSection(&win32_console_writer.lock);
		CloseHandle(win32_console_writer.wake);
		memset(&win32_console_writer, 0, sizeof win32_console_writer);
		return (-1);
	}
	win32_console_writer.initialized = 1;
	return (0);
}

static int
win32_console_writer_service_fini(const char *name)
{
	if (!win32_console_writer.initialized)
		return (0);

	EnterCriticalSection(&win32_console_writer.lock);
	win32_console_writer.stopping = 1;
	SetEvent(win32_console_writer.wake);
	LeaveCriticalSection(&win32_console_writer.lock);

	if (win32_wait_console_thread(win32_console_writer.thread, name) != 0)
		return (-1);

	CloseHandle(win32_console_writer.thread);
	CloseHandle(win32_console_writer.wake);
	DeleteCriticalSection(&win32_console_writer.lock);
	memset(&win32_console_writer, 0, sizeof win32_console_writer);
	return (0);
}

static void
win32_console_writer_enqueue_locked(struct win32_handle_writer *whw)
{
	if (whw->console_queued || whw->console_active)
		return;
	TAILQ_INSERT_TAIL(&win32_console_writer.pending, whw, console_entry);
	whw->console_queued = 1;
	SetEvent(win32_console_writer.wake);
}

static void
win32_console_writer_enqueue(struct win32_handle_writer *whw)
{
	EnterCriticalSection(&win32_console_writer.lock);
	win32_console_writer_enqueue_locked(whw);
	LeaveCriticalSection(&win32_console_writer.lock);
}

static DWORD WINAPI
win32_console_writer_thread(__unused void *arg)
{
	struct win32_handle_writer	*whw;
	int				 stopping;

	for (;;) {
		WaitForSingleObject(win32_console_writer.wake, INFINITE);
		for (;;) {
			EnterCriticalSection(&win32_console_writer.lock);
			whw = TAILQ_FIRST(&win32_console_writer.pending);
			if (whw != NULL) {
				TAILQ_REMOVE(&win32_console_writer.pending, whw,
				    console_entry);
				whw->console_queued = 0;
				whw->console_active = 1;
			}
			stopping = win32_console_writer.stopping;
			LeaveCriticalSection(&win32_console_writer.lock);
			if (whw == NULL)
				break;

			win32_console_writer_run(whw);

			EnterCriticalSection(&win32_console_writer.lock);
			EnterCriticalSection(&whw->lock);
			whw->console_active = 0;
			if (stopping) {
				if (whw->stop ||
				    whw->state == WIN32_HANDLE_WRITER_CLOSED ||
				    whw->state == WIN32_HANDLE_WRITER_ERROR)
					SetEvent(whw->complete);
			} else if (whw->state == WIN32_HANDLE_WRITER_RUNNING &&
			    !whw->stop && whw->handle != NULL &&
			    EVBUFFER_LENGTH(whw->output) != 0) {
				win32_console_writer_enqueue_locked(whw);
			} else if (whw->state == WIN32_HANDLE_WRITER_CLOSING &&
			    !whw->stop && whw->handle != NULL) {
				win32_console_writer_enqueue_locked(whw);
			} else if ((whw->stop ||
			    whw->state == WIN32_HANDLE_WRITER_CLOSED ||
			    whw->state == WIN32_HANDLE_WRITER_ERROR) &&
			    !whw->console_queued)
				SetEvent(whw->complete);
			LeaveCriticalSection(&whw->lock);
			LeaveCriticalSection(&win32_console_writer.lock);
		}
		if (stopping)
			break;
	}
	return (0);
}

static void
win32_console_writer_run(struct win32_handle_writer *whw)
{
	char		*buf;
	HANDLE		 handle;
	u_char		*testbuf;
	size_t		 size;
	size_t		 split;
	int		 part, written, notify;
	uint32_t	 events;

	buf = xmalloc(WIN32_HANDLE_WRITER_CHUNK);
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
			LeaveCriticalSection(&whw->lock);
			break;
		}
		if (size > WIN32_HANDLE_WRITER_CHUNK)
			size = WIN32_HANDLE_WRITER_CHUNK;
		memcpy(buf, EVBUFFER_DATA(whw->output), size);
		handle = whw->handle;
		LeaveCriticalSection(&whw->lock);

		testbuf = NULL;
		if (win32_console_test_env_enabled(
		    "TMUX_WIN32_CONSOLE_RELAY_TEST_UTF8_SPLIT") &&
		    !whw->test_utf8_split_fired) {
			split = win32_console_test_utf8_split_at((u_char *)buf,
			    size);
			if (split != (size_t)-1 && split < size) {
				whw->test_utf8_split_fired = 1;
				log_debug("%s: forcing console UTF-8 split at "
				    "%zu bytes", __func__, split);
				part = win32_handle_write_console_utf8(whw,
				    handle, buf, split);
				if (part == (int)split) {
					part = win32_handle_write_console_utf8(
					    whw, handle, buf + split,
					    size - split);
					if (part == (int)(size - split))
						written = (int)size;
					else
						written = part;
				} else
					written = part;
				goto have_written;
			}
		}
		if (win32_console_test_env_enabled(
		    "TMUX_WIN32_CONSOLE_RELAY_TEST_INVALID_UTF8") &&
		    !whw->test_invalid_utf8_fired) {
			testbuf = win32_console_test_invalid_utf8(
			    (u_char *)buf, size);
			if (testbuf != NULL) {
				whw->test_invalid_utf8_fired = 1;
				log_debug("%s: forcing console invalid UTF-8 "
				    "near marker", __func__);
				written = win32_handle_write_console_utf8(whw,
				    handle, testbuf, size);
				free(testbuf);
				goto have_written;
			}
		}
		written = win32_handle_write_console_utf8(whw, handle, buf,
		    size);
have_written:
		if (written == -1 || written == 0) {
			EnterCriticalSection(&whw->lock);
			whw->state = WIN32_HANDLE_WRITER_ERROR;
			evbuffer_drain(whw->output, EVBUFFER_LENGTH(whw->output));
			handle = whw->handle;
			whw->handle = NULL;
			LeaveCriticalSection(&whw->lock);
			if (handle != NULL && !whw->borrowed)
				CloseHandle(handle);
			win32_io_service_enqueue_writer(whw,
			    WIN32_IO_EVENT_ERROR);
			free(buf);
			return;
		}

		EnterCriticalSection(&whw->lock);
		evbuffer_drain(whw->output, written);
		win32_handle_writer_record_progress(whw, written);
		notify = EVBUFFER_LENGTH(whw->output) == 0;
		LeaveCriticalSection(&whw->lock);
		win32_io_service_enqueue_writer(whw,
		    WIN32_IO_EVENT_WRITE_PROGRESS |
		    (notify ? WIN32_IO_EVENT_WRITE_DRAINED : 0));
	}
	free(buf);
	return;

stop:
	if (whw->state != WIN32_HANDLE_WRITER_ERROR && whw->handle != NULL &&
	    whw->utf8_partial_len != 0) {
		handle = whw->handle;
		size = whw->utf8_partial_len;
		memcpy(buf, whw->utf8_partial, size);
		LeaveCriticalSection(&whw->lock);
		written = win32_handle_write_console(handle, (u_char *)buf,
		    size);
		EnterCriticalSection(&whw->lock);
		if (written != (int)size) {
			whw->state = WIN32_HANDLE_WRITER_ERROR;
			whw->utf8_partial_len = 0;
		} else
			whw->utf8_partial_len = 0;
	}
	evbuffer_drain(whw->output, EVBUFFER_LENGTH(whw->output));
	handle = whw->handle;
	whw->handle = NULL;
	if (whw->state == WIN32_HANDLE_WRITER_ERROR)
		events = WIN32_IO_EVENT_ERROR;
	else {
		whw->state = WIN32_HANDLE_WRITER_CLOSED;
		events = WIN32_IO_EVENT_WRITE_CLOSED;
	}
	LeaveCriticalSection(&whw->lock);
	if (handle != NULL && !whw->borrowed)
		CloseHandle(handle);
	win32_io_service_enqueue_writer(whw, events);
	free(buf);
}

static struct win32_io_endpoint *
win32_io_reader_new_worker(HANDLE handle, enum win32_handle_event_backend backend,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	struct win32_handle_event	*whe;

	if (handle == NULL || handle == INVALID_HANDLE_VALUE)
		return (NULL);
	if (eventcb == NULL)
		return (NULL);
	if (win32_io_service_init() != 0)
		return (NULL);
	if (backend == WIN32_HANDLE_EVENT_CONSOLE &&
	    win32_console_reader_service_init() != 0)
		return (NULL);

	whe = xcalloc(1, sizeof *whe);
	whe->handle = handle;
	whe->backend = backend;
	whe->input = evbuffer_new();
	if (whe->input == NULL) {
		free(whe);
		return (NULL);
	}
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
	if (backend == WIN32_HANDLE_EVENT_CONSOLE) {
		whe->changed = CreateEventW(NULL, TRUE, FALSE, NULL);
		if (whe->changed == NULL) {
			CloseHandle(whe->ready);
			CloseHandle(whe->stop);
			evbuffer_free(whe->input);
			free(whe);
			return (NULL);
		}
		whe->complete = CreateEventW(NULL, TRUE, TRUE, NULL);
		if (whe->complete == NULL) {
			CloseHandle(whe->changed);
			CloseHandle(whe->ready);
			CloseHandle(whe->stop);
			evbuffer_free(whe->input);
			free(whe);
			return (NULL);
		}
	}
	InitializeCriticalSection(&whe->lock);
	win32_io_endpoint_init(&whe->endpoint, WIN32_IO_ENDPOINT_READER, whe,
	    eventcb, arg);
	if (backend == WIN32_HANDLE_EVENT_CONSOLE) {
		EnterCriticalSection(&whe->lock);
		win32_handle_event_update_ready(whe);
		LeaveCriticalSection(&whe->lock);
	} else {
		whe->thread = CreateThread(NULL, 0, win32_handle_event_thread,
		    whe, 0, NULL);
	}
	if (backend != WIN32_HANDLE_EVENT_CONSOLE && whe->thread == NULL) {
		win32_handle_event_free(whe);
		return (NULL);
	}
	return (&whe->endpoint);
}

struct win32_io_endpoint *
win32_io_reader_new_console(HANDLE handle,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	if (!win32_console_handle(handle)) {
		errno = ENOTTY;
		return (NULL);
	}
	return (win32_io_reader_new_worker(handle, WIN32_HANDLE_EVENT_CONSOLE,
	    eventcb, arg));
}

struct win32_io_endpoint *
win32_io_reader_new_stdio(HANDLE handle,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	/*
	 * Inherited stdio handles are borrowed and may be pipes, files, or
	 * other synchronous objects. Overlapped capability is fixed when the
	 * handle is created, so non-console stdio stays on the worker fallback.
	 */
	return (win32_io_reader_new_worker(handle, WIN32_HANDLE_EVENT_STDIO,
	    eventcb, arg));
}

struct win32_io_endpoint *
win32_io_reader_new_terminal(HANDLE handle,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	/*
	 * Direct terminal handles are supplied by the client and are not owned
	 * or opened here. Actual console input still needs the console reader;
	 * pipes and redirected terminal input use the worker fallback.
	 */
	if (win32_console_handle(handle))
		return (win32_io_reader_new_console(handle, eventcb, arg));
	return (win32_io_reader_new_worker(handle, WIN32_HANDLE_EVENT_TERMINAL,
	    eventcb, arg));
}

static struct win32_io_endpoint *
win32_io_reader_new_iocp(HANDLE handle, uint64_t offset, int use_offset,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	struct win32_handle_event	*whe;
	uint32_t			 events;

	if (handle == NULL || handle == INVALID_HANDLE_VALUE)
		return (NULL);
	if (eventcb == NULL)
		return (NULL);
	if (win32_io_service_init_iocp() != 0)
		return (NULL);

	whe = xcalloc(1, sizeof *whe);
	whe->handle = handle;
	whe->backend = WIN32_HANDLE_EVENT_IOCP;
	whe->offset = offset;
	whe->use_offset = use_offset;
	whe->input = evbuffer_new();
	if (whe->input == NULL) {
		free(whe);
		return (NULL);
	}
	whe->complete = CreateEventW(NULL, TRUE, TRUE, NULL);
	if (whe->complete == NULL) {
		evbuffer_free(whe->input);
		free(whe);
		return (NULL);
	}
	InitializeCriticalSection(&whe->lock);
	win32_io_endpoint_init(&whe->endpoint, WIN32_IO_ENDPOINT_READER, whe,
	    eventcb, arg);
	if (CreateIoCompletionPort(handle, win32_io.iocp,
	    (ULONG_PTR)&whe->endpoint, 0) == NULL) {
		win32_handle_event_free(whe);
		return (NULL);
	}
	EnterCriticalSection(&whe->lock);
	events = win32_handle_event_iocp_start(whe);
	LeaveCriticalSection(&whe->lock);
	if (events != 0)
		win32_io_service_enqueue_endpoint(&whe->endpoint, events);
	return (&whe->endpoint);
}

struct win32_io_endpoint *
win32_io_reader_new_overlapped(HANDLE handle,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	return (win32_io_reader_new_iocp(handle, 0, 0, eventcb, arg));
}

struct win32_io_endpoint *
win32_io_reader_new_file(HANDLE handle, uint64_t offset,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	return (win32_io_reader_new_iocp(handle, offset, 1, eventcb, arg));
}

static void
win32_handle_event_free(struct win32_handle_event *whe)
{
	if (whe == NULL)
		return;
	if (whe->backend == WIN32_HANDLE_EVENT_IOCP) {
		EnterCriticalSection(&whe->lock);
		whe->stopping = 1;
		if (whe->pending)
			CancelIoEx(whe->handle, &whe->overlapped);
		LeaveCriticalSection(&whe->lock);
		if (win32_wait_iocp_endpoint(whe->complete, __func__) != 0) {
			win32_io_service_deactivate_endpoint(&whe->endpoint);
			return;
		}
	} else if (whe->backend == WIN32_HANDLE_EVENT_CONSOLE) {
		EnterCriticalSection(&win32_console_reader.lock);
		EnterCriticalSection(&whe->lock);
		whe->stopping = 1;
		ResetEvent(whe->complete);
		if (whe->stop != NULL)
			SetEvent(whe->stop);
		if (whe->changed != NULL)
			SetEvent(whe->changed);
		win32_console_reader_enqueue_locked(whe);
		LeaveCriticalSection(&whe->lock);
		LeaveCriticalSection(&win32_console_reader.lock);
		if (win32_wait_console_reader_endpoint(whe->complete,
		    __func__) != 0) {
			win32_io_service_deactivate_endpoint(&whe->endpoint);
			return;
		}
	} else if (whe->stop != NULL) {
		EnterCriticalSection(&whe->lock);
		whe->stopping = 1;
		SetEvent(whe->stop);
		LeaveCriticalSection(&whe->lock);
	}
	if (whe->thread != NULL) {
		CancelSynchronousIo(whe->thread);
		if (win32_wait_worker_thread(whe->thread, __func__) != 0) {
			win32_io_service_deactivate_endpoint(&whe->endpoint);
			return;
		}
	}
	win32_io_service_deactivate_endpoint(&whe->endpoint);
	if (whe->input != NULL)
		evbuffer_free(whe->input);
	if (whe->thread != NULL)
		CloseHandle(whe->thread);
	if (whe->stop != NULL)
		CloseHandle(whe->stop);
	if (whe->ready != NULL)
		CloseHandle(whe->ready);
	if (whe->changed != NULL)
		CloseHandle(whe->changed);
	if (whe->complete != NULL)
		CloseHandle(whe->complete);
	DeleteCriticalSection(&whe->lock);
	free(whe);
}

void
win32_io_reader_drain(struct win32_io_endpoint *endpoint,
    struct evbuffer *dst)
{
	win32_io_reader_drain_limit(endpoint, dst, (size_t)-1);
}

void
win32_io_reader_drain_limit(struct win32_io_endpoint *endpoint,
    struct evbuffer *dst, size_t limit)
{
	struct win32_handle_event	*whe = endpoint->owner;
	size_t				 size;

	EnterCriticalSection(&whe->lock);
	size = EVBUFFER_LENGTH(whe->input);
	if (limit >= size)
		evbuffer_add_buffer(dst, whe->input);
	else if (limit != 0) {
		if (evbuffer_remove_buffer(whe->input, dst, limit) < 0)
			fatalx("out of memory");
	}
	win32_handle_event_update_ready(whe);
	LeaveCriticalSection(&whe->lock);
}

void
win32_io_reader_drain_bev(struct win32_io_endpoint *endpoint,
    struct bufferevent *bev)
{
	struct win32_handle_event	*whe = endpoint->owner;
	struct evbuffer	*dst = bev->input;

	evbuffer_unfreeze(dst, 0);
	EnterCriticalSection(&whe->lock);
	evbuffer_add_buffer(dst, whe->input);
	win32_handle_event_update_ready(whe);
	LeaveCriticalSection(&whe->lock);
	evbuffer_freeze(dst, 0);
}

void
win32_io_reader_set_reading(struct win32_io_endpoint *endpoint, int enabled)
{
	struct win32_handle_event	*whe;

	if (endpoint == NULL)
		return;
	whe = endpoint->owner;
	if (whe == NULL)
		return;
	EnterCriticalSection(&whe->lock);
	whe->paused = !enabled;
	win32_handle_event_update_ready(whe);
	LeaveCriticalSection(&whe->lock);
}

size_t
win32_io_reader_buffered(struct win32_io_endpoint *endpoint)
{
	struct win32_handle_event	*whe = endpoint->owner;
	size_t	size;

	EnterCriticalSection(&whe->lock);
	size = EVBUFFER_LENGTH(whe->input);
	LeaveCriticalSection(&whe->lock);
	return (size);
}

int
win32_io_reader_done(struct win32_io_endpoint *endpoint)
{
	struct win32_handle_event	*whe = endpoint->owner;
	int	done;

	EnterCriticalSection(&whe->lock);
	done = (whe->state != WIN32_HANDLE_EVENT_RUNNING);
	LeaveCriticalSection(&whe->lock);
	return (done);
}

int
win32_io_reader_eof(struct win32_io_endpoint *endpoint)
{
	struct win32_handle_event	*whe = endpoint->owner;
	int	eof;

	EnterCriticalSection(&whe->lock);
	eof = (whe->state == WIN32_HANDLE_EVENT_EOF);
	LeaveCriticalSection(&whe->lock);
	return (eof);
}

int
win32_io_reader_error(struct win32_io_endpoint *endpoint)
{
	struct win32_handle_event	*whe = endpoint->owner;
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
	int				 stopped;
	DWORD				 error;
	uint32_t			 events;

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

			error = ERROR_SUCCESS;
			written = win32_handle_write_file(handle, buf, size,
			    &error);
			if (written == -1 || written == 0) {
				EnterCriticalSection(&whw->lock);
				stopped = whw->stop;
				if (stopped)
					whw->state = WIN32_HANDLE_WRITER_CLOSED;
				else
					whw->state = WIN32_HANDLE_WRITER_ERROR;
				evbuffer_drain(whw->output,
				    EVBUFFER_LENGTH(whw->output));
				handle = whw->handle;
				whw->handle = NULL;
				LeaveCriticalSection(&whw->lock);
				if (!stopped) {
					if (written == -1) {
						log_debug("%s: WriteFile failed: "
						    "%s", __func__,
						    win32_strerror(error));
					} else {
						log_debug("%s: WriteFile wrote "
						    "zero bytes", __func__);
					}
				}
				if (handle != NULL && !whw->borrowed)
					CloseHandle(handle);
				win32_io_service_enqueue_writer(whw,
				    stopped ? WIN32_IO_EVENT_WRITE_CLOSED :
				    WIN32_IO_EVENT_ERROR);
				free(buf);
				return (0);
			}

		EnterCriticalSection(&whw->lock);
		evbuffer_drain(whw->output, written);
		win32_handle_writer_record_progress(whw, written);
		notify = EVBUFFER_LENGTH(whw->output) == 0;
		LeaveCriticalSection(&whw->lock);
		win32_io_service_enqueue_writer(whw,
		    WIN32_IO_EVENT_WRITE_PROGRESS |
		    (notify ? WIN32_IO_EVENT_WRITE_DRAINED : 0));
		}
	}

stop:
	evbuffer_drain(whw->output, EVBUFFER_LENGTH(whw->output));
	handle = whw->handle;
	whw->handle = NULL;
	if (whw->state == WIN32_HANDLE_WRITER_ERROR)
		events = WIN32_IO_EVENT_ERROR;
	else {
		whw->state = WIN32_HANDLE_WRITER_CLOSED;
		events = WIN32_IO_EVENT_WRITE_CLOSED;
	}
	LeaveCriticalSection(&whw->lock);
	if (handle != NULL && !whw->borrowed)
		CloseHandle(handle);
	win32_io_service_enqueue_writer(whw, events);
	free(buf);
	return (0);
}

static uint32_t
win32_handle_writer_iocp_start(struct win32_handle_writer *whw)
{
	DWORD	error;
	size_t	size;

	if (whw->backend != WIN32_HANDLE_WRITER_IOCP)
		return (0);
	if (whw->stop || whw->pending || whw->handle == NULL)
		return (0);
	if (whw->state == WIN32_HANDLE_WRITER_ERROR ||
	    whw->state == WIN32_HANDLE_WRITER_CLOSED)
		return (0);

	size = EVBUFFER_LENGTH(whw->output);
	if (size == 0) {
		if (whw->state == WIN32_HANDLE_WRITER_CLOSING) {
			whw->state = WIN32_HANDLE_WRITER_CLOSED;
			if (!whw->borrowed)
				CloseHandle(whw->handle);
			whw->handle = NULL;
			return (WIN32_IO_EVENT_WRITE_CLOSED);
		}
		return (WIN32_IO_EVENT_WRITE_DRAINED);
	}
	if (size > WIN32_HANDLE_WRITER_CHUNK)
		size = WIN32_HANDLE_WRITER_CHUNK;
	memcpy(whw->iocp_buf, EVBUFFER_DATA(whw->output), size);

	memset(&whw->overlapped, 0, sizeof whw->overlapped);
	if (whw->append) {
		whw->overlapped.Offset = 0xffffffff;
		whw->overlapped.OffsetHigh = 0xffffffff;
	} else if (whw->use_offset) {
		whw->overlapped.Offset = (DWORD)(whw->offset & 0xffffffff);
		whw->overlapped.OffsetHigh = (DWORD)(whw->offset >> 32);
	}
	ResetEvent(whw->complete);
	whw->iocp_size = size;
	whw->pending = 1;
	if (WriteFile(whw->handle, whw->iocp_buf, size, NULL, &whw->overlapped))
		return (0);

	error = GetLastError();
	if (error == ERROR_IO_PENDING)
		return (0);

	whw->pending = 0;
	whw->state = WIN32_HANDLE_WRITER_ERROR;
	evbuffer_drain(whw->output, EVBUFFER_LENGTH(whw->output));
	if (!whw->borrowed)
		CloseHandle(whw->handle);
	whw->handle = NULL;
	SetEvent(whw->complete);
	return (WIN32_IO_EVENT_ERROR);
}

static void
win32_handle_writer_iocp_complete(struct win32_handle_writer *whw,
    DWORD error, DWORD nwritten)
{
	uint32_t	events = 0;
	int		complete = 0;

	EnterCriticalSection(&whw->lock);
	if (whw->pending)
		whw->pending = 0;
	if (whw->stop)
		goto out;
	if (whw->state != WIN32_HANDLE_WRITER_RUNNING &&
	    whw->state != WIN32_HANDLE_WRITER_CLOSING) {
		events = WIN32_IO_EVENT_ERROR;
		goto out;
	}
	if (error != ERROR_SUCCESS || nwritten == 0) {
		whw->state = WIN32_HANDLE_WRITER_ERROR;
		evbuffer_drain(whw->output, EVBUFFER_LENGTH(whw->output));
		if (!whw->borrowed)
			CloseHandle(whw->handle);
		whw->handle = NULL;
		events = WIN32_IO_EVENT_ERROR;
		goto out;
	}
	if (nwritten > whw->iocp_size)
		nwritten = whw->iocp_size;
	evbuffer_drain(whw->output, nwritten);
	win32_handle_writer_record_progress(whw, nwritten);
	if (whw->use_offset && !whw->append)
		whw->offset += nwritten;
	whw->iocp_size = 0;
	events = WIN32_IO_EVENT_WRITE_PROGRESS |
	    win32_handle_writer_iocp_start(whw);

out:
	if (events != 0)
		win32_io_service_enqueue_endpoint(&whw->endpoint, events);
	if (!whw->pending)
		complete = 1;
	if (complete)
		SetEvent(whw->complete);
	LeaveCriticalSection(&whw->lock);
}

static struct win32_handle_writer *
win32_handle_writer_new1(HANDLE *handle,
    enum win32_handle_writer_backend backend,
    void (*eventcb)(void *, uint32_t), void *arg, int borrowed)
{
	struct win32_handle_writer	*whw;

	if (win32_io_service_init() != 0)
		return (NULL);
	if (backend == WIN32_HANDLE_WRITER_CONSOLE &&
	    win32_console_writer_service_init() != 0)
		return (NULL);
	if (handle == NULL || *handle == NULL || *handle == INVALID_HANDLE_VALUE)
		return (NULL);
	if (eventcb == NULL)
		return (NULL);

	whw = xcalloc(1, sizeof *whw);
	whw->handle = *handle;
	whw->borrowed = borrowed;
	whw->backend = backend;
	whw->output = evbuffer_new();
	if (whw->output == NULL) {
		free(whw);
		return (NULL);
	}
	if (backend == WIN32_HANDLE_WRITER_CONSOLE)
		whw->complete = CreateEventW(NULL, TRUE, TRUE, NULL);
	else
		whw->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (whw->ready == NULL && whw->complete == NULL)
		goto fail;
	InitializeCriticalSection(&whw->lock);
	win32_io_endpoint_init(&whw->endpoint, WIN32_IO_ENDPOINT_WRITER, whw,
	    eventcb, arg);
	if (backend != WIN32_HANDLE_WRITER_CONSOLE) {
		whw->thread = CreateThread(NULL, 0, win32_handle_writer_thread,
		    whw, 0, NULL);
		if (whw->thread == NULL) {
			DeleteCriticalSection(&whw->lock);
			goto fail;
		}
	}
	if (!borrowed)
		*handle = NULL;
	win32_io_service_enqueue_writer(whw, WIN32_IO_EVENT_WRITE_DRAINED);
	return (whw);

fail:
	if (whw->ready != NULL)
		CloseHandle(whw->ready);
	if (whw->complete != NULL)
		CloseHandle(whw->complete);
	evbuffer_free(whw->output);
	free(whw);
	return (NULL);
}

static struct win32_handle_writer *
win32_handle_writer_new_iocp(HANDLE *handle, uint64_t offset, int use_offset,
    int append, void (*eventcb)(void *, uint32_t), void *arg, int borrowed)
{
	struct win32_handle_writer	*whw;
	uint32_t			 events;

	if (win32_io_service_init_iocp() != 0)
		return (NULL);
	if (handle == NULL || *handle == NULL || *handle == INVALID_HANDLE_VALUE)
		return (NULL);
	if (eventcb == NULL)
		return (NULL);

	whw = xcalloc(1, sizeof *whw);
	whw->handle = *handle;
	whw->borrowed = borrowed;
	whw->backend = WIN32_HANDLE_WRITER_IOCP;
	whw->offset = offset;
	whw->use_offset = use_offset;
	whw->append = append;
	whw->iocp_buf = xmalloc(WIN32_HANDLE_WRITER_CHUNK);
	whw->output = evbuffer_new();
	if (whw->output == NULL) {
		free(whw->iocp_buf);
		free(whw);
		return (NULL);
	}
	whw->complete = CreateEventW(NULL, TRUE, TRUE, NULL);
	if (whw->complete == NULL) {
		free(whw->iocp_buf);
		evbuffer_free(whw->output);
		free(whw);
		return (NULL);
	}
	InitializeCriticalSection(&whw->lock);
	win32_io_endpoint_init(&whw->endpoint, WIN32_IO_ENDPOINT_WRITER, whw,
	    eventcb, arg);
	if (CreateIoCompletionPort(*handle, win32_io.iocp,
	    (ULONG_PTR)&whw->endpoint, 0) == NULL) {
		DeleteCriticalSection(&whw->lock);
		CloseHandle(whw->complete);
		free(whw->iocp_buf);
		evbuffer_free(whw->output);
		free(whw);
		return (NULL);
	}
	if (!borrowed)
		*handle = NULL;
	EnterCriticalSection(&whw->lock);
	events = win32_handle_writer_iocp_start(whw);
	LeaveCriticalSection(&whw->lock);
	if (events != 0)
		win32_io_service_enqueue_writer(whw, events);
	return (whw);
}

struct win32_io_endpoint *
win32_io_writer_new_overlapped(HANDLE *handle,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	struct win32_handle_writer	*whw;

	whw = win32_handle_writer_new_iocp(handle, 0, 0, 0, eventcb, arg, 0);
	if (whw == NULL)
		return (NULL);
	return (&whw->endpoint);
}

struct win32_io_endpoint *
win32_io_writer_new_file_borrowed(HANDLE *handle, uint64_t offset, int append,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	struct win32_handle_writer	*whw;

	whw = win32_handle_writer_new_iocp(handle, offset, 1, append,
	    eventcb, arg,
	    1);
	if (whw == NULL)
		return (NULL);
	return (&whw->endpoint);
}

static struct win32_io_endpoint *
win32_io_writer_new_worker_borrowed(HANDLE *handle,
    enum win32_handle_writer_backend backend, void (*eventcb)(void *, uint32_t),
    void *arg)
{
	struct win32_handle_writer	*whw;

	whw = win32_handle_writer_new1(handle, backend, eventcb, arg, 1);
	if (whw == NULL)
		return (NULL);
	return (&whw->endpoint);
}

struct win32_io_endpoint *
win32_io_writer_new_console_borrowed(HANDLE *handle,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	if (handle == NULL || *handle == NULL || *handle == INVALID_HANDLE_VALUE)
		return (NULL);
	if (!win32_console_handle(*handle)) {
		errno = ENOTTY;
		return (NULL);
	}
	return (win32_io_writer_new_worker_borrowed(handle,
	    WIN32_HANDLE_WRITER_CONSOLE, eventcb, arg));
}

struct win32_io_endpoint *
win32_io_writer_new_stdio_borrowed(HANDLE *handle,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	/*
	 * Borrowed stdio output cannot be reopened with FILE_FLAG_OVERLAPPED.
	 * Console handles are routed to the shared console writer; everything
	 * else remains on the explicit worker fallback.
	 */
	return (win32_io_writer_new_worker_borrowed(handle,
	    win32_handle_writer_backend_for_handle(handle), eventcb, arg));
}

struct win32_io_endpoint *
win32_io_writer_new_terminal_borrowed(HANDLE *handle,
    void (*eventcb)(void *, uint32_t), void *arg)
{
	/*
	 * The client remains the close owner for direct terminal output. Keep
	 * non-console borrowed handles on the worker fallback and only select
	 * the shared console writer when the handle is an actual console.
	 */
	return (win32_io_writer_new_worker_borrowed(handle,
	    win32_handle_writer_backend_for_handle(handle), eventcb, arg));
}

static void
win32_handle_writer_free(struct win32_handle_writer *whw)
{
	if (whw == NULL)
		return;
	if (whw->backend == WIN32_HANDLE_WRITER_IOCP) {
		EnterCriticalSection(&whw->lock);
		whw->stop = 1;
		if (whw->pending)
			CancelIoEx(whw->handle, &whw->overlapped);
		LeaveCriticalSection(&whw->lock);
	} else if (whw->backend == WIN32_HANDLE_WRITER_CONSOLE) {
		EnterCriticalSection(&win32_console_writer.lock);
		EnterCriticalSection(&whw->lock);
		whw->stop = 1;
		ResetEvent(whw->complete);
		win32_console_writer_enqueue_locked(whw);
		LeaveCriticalSection(&whw->lock);
		LeaveCriticalSection(&win32_console_writer.lock);
	} else {
		EnterCriticalSection(&whw->lock);
		whw->stop = 1;
		SetEvent(whw->ready);
		LeaveCriticalSection(&whw->lock);
	}
	if (whw->backend == WIN32_HANDLE_WRITER_IOCP &&
	    win32_wait_iocp_endpoint(whw->complete, __func__) != 0) {
		win32_io_service_deactivate_endpoint(&whw->endpoint);
		return;
	}
	if (whw->backend == WIN32_HANDLE_WRITER_CONSOLE &&
	    win32_wait_console_endpoint(whw->complete, __func__) != 0) {
		win32_io_service_deactivate_endpoint(&whw->endpoint);
		return;
	}
	if (whw->thread != NULL) {
		CancelSynchronousIo(whw->thread);
		if (win32_wait_worker_thread(whw->thread, __func__) != 0) {
			win32_io_service_deactivate_endpoint(&whw->endpoint);
			return;
		}
	}
	win32_io_service_deactivate_endpoint(&whw->endpoint);
	if (whw->handle != NULL && !whw->borrowed)
		CloseHandle(whw->handle);
	if (whw->thread != NULL)
		CloseHandle(whw->thread);
	if (whw->ready != NULL)
		CloseHandle(whw->ready);
	if (whw->complete != NULL)
		CloseHandle(whw->complete);
	free(whw->iocp_buf);
	evbuffer_free(whw->output);
	DeleteCriticalSection(&whw->lock);
	free(whw);
}

int
win32_io_writer_write(struct win32_io_endpoint *endpoint, const void *data,
    size_t size)
{
	struct win32_handle_writer	*whw;
	size_t	buffered, nwrite;
	uint32_t events = 0;
	int	console = 0;

	if (endpoint == NULL) {
		errno = EPIPE;
		return (-1);
	}
	whw = endpoint->owner;
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
	buffered = EVBUFFER_LENGTH(whw->output);
	if (buffered >= WIN32_HANDLE_WRITER_HIGH) {
		LeaveCriticalSection(&whw->lock);
		errno = EAGAIN;
		return (-1);
	}
	if (nwrite > WIN32_HANDLE_WRITER_HIGH - buffered) {
		LeaveCriticalSection(&whw->lock);
		errno = EAGAIN;
		return (-1);
	}
	if (evbuffer_add(whw->output, data, nwrite) != 0) {
		LeaveCriticalSection(&whw->lock);
		errno = ENOMEM;
		return (-1);
	}
	if (whw->backend == WIN32_HANDLE_WRITER_IOCP)
		events = win32_handle_writer_iocp_start(whw);
	else if (whw->backend == WIN32_HANDLE_WRITER_CONSOLE) {
		ResetEvent(whw->complete);
		console = 1;
	} else
		SetEvent(whw->ready);
	LeaveCriticalSection(&whw->lock);
	if (console)
		win32_console_writer_enqueue(whw);
	if (events != 0)
		win32_io_service_enqueue_writer(whw, events);
	return ((int)nwrite);
}

void
win32_io_writer_close(struct win32_io_endpoint *endpoint)
{
	struct win32_handle_writer	*whw;
	uint32_t			 events = 0;
	int				 console = 0;

	if (endpoint == NULL)
		return;
	whw = endpoint->owner;
	if (whw == NULL)
		return;
	EnterCriticalSection(&whw->lock);
	if (whw->state != WIN32_HANDLE_WRITER_RUNNING) {
		LeaveCriticalSection(&whw->lock);
		return;
	}
	whw->state = WIN32_HANDLE_WRITER_CLOSING;
	if (whw->backend == WIN32_HANDLE_WRITER_IOCP)
		events = win32_handle_writer_iocp_start(whw);
	else if (whw->backend == WIN32_HANDLE_WRITER_CONSOLE) {
		ResetEvent(whw->complete);
		console = 1;
	} else
		SetEvent(whw->ready);
	LeaveCriticalSection(&whw->lock);
	if (console)
		win32_console_writer_enqueue(whw);
	if (events != 0)
		win32_io_service_enqueue_writer(whw, events);
}

size_t
win32_io_writer_consume_progress(struct win32_io_endpoint *endpoint)
{
	struct win32_handle_writer	*whw;
	size_t				 size;

	if (endpoint == NULL)
		return (0);
	whw = endpoint->owner;
	if (whw == NULL)
		return (0);
	EnterCriticalSection(&whw->lock);
	size = whw->write_progress;
	whw->write_progress = 0;
	LeaveCriticalSection(&whw->lock);
	return (size);
}

size_t
win32_io_writer_buffered(struct win32_io_endpoint *endpoint)
{
	struct win32_handle_writer	*whw;
	size_t	size;

	if (endpoint == NULL)
		return (0);
	whw = endpoint->owner;
	EnterCriticalSection(&whw->lock);
	size = EVBUFFER_LENGTH(whw->output);
	LeaveCriticalSection(&whw->lock);
	return (size);
}

int
win32_io_writer_drained(struct win32_io_endpoint *endpoint)
{
	struct win32_handle_writer	*whw;
	int	drained;

	if (endpoint == NULL)
		return (1);
	whw = endpoint->owner;
	EnterCriticalSection(&whw->lock);
	drained = (EVBUFFER_LENGTH(whw->output) == 0 && !whw->pending);
	LeaveCriticalSection(&whw->lock);
	return (drained);
}

int
win32_io_writer_writable(struct win32_io_endpoint *endpoint)
{
	struct win32_handle_writer	*whw;
	int	writable;

	if (endpoint == NULL)
		return (0);
	whw = endpoint->owner;
	EnterCriticalSection(&whw->lock);
	writable = (whw->state == WIN32_HANDLE_WRITER_RUNNING &&
	    !whw->stop && whw->handle != NULL);
	LeaveCriticalSection(&whw->lock);
	return (writable);
}

static int
win32_handle_write_file(HANDLE handle, const void *data, size_t size,
    DWORD *error)
{
	DWORD	written, nwrite;

	if (error != NULL)
		*error = ERROR_SUCCESS;
	nwrite = size > INT_MAX ? INT_MAX : (DWORD)size;
	if (nwrite == 0)
		return (0);
	if (!WriteFile(handle, data, nwrite, &written, NULL)) {
		if (error != NULL)
			*error = GetLastError();
		errno = EIO;
		return (-1);
	}
	return ((int)written);
}

static int
win32_console_handle(HANDLE handle)
{
	DWORD	mode;

	if (handle == NULL || handle == INVALID_HANDLE_VALUE)
		return (0);
	return (GetConsoleMode(handle, &mode));
}

static int
win32_utf8_expected(u_char ch)
{
	if (ch < 0x80)
		return (1);
	if (ch >= 0xc2 && ch <= 0xdf)
		return (2);
	if (ch >= 0xe0 && ch <= 0xef)
		return (3);
	if (ch >= 0xf0 && ch <= 0xf4)
		return (4);
	return (0);
}

static size_t
win32_utf8_complete_len(const u_char *data, size_t size)
{
	size_t	i, remaining;
	int	needed;

	i = 0;
	while (i < size) {
		needed = win32_utf8_expected(data[i]);
		if (needed == 0)
			return (size);
		remaining = size - i;
		if (remaining < (size_t)needed) {
			for (size_t j = 1; j < remaining; j++) {
				if ((data[i + j] & 0xc0) != 0x80)
					return (size);
			}
			return (i);
		}
		i += needed;
	}
	return (size);
}

static int
win32_handle_write_console(HANDLE handle, const u_char *data, size_t size)
{
	DWORD	 written, total;
	wchar_t	*wdata;
	int	 n, nbytes;

	nbytes = size > INT_MAX ? INT_MAX : (int)size;
	if (nbytes == 0)
		return (0);

	n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, nbytes,
	    NULL, 0);
	if (n == 0) {
		log_debug("%s: MultiByteToWideChar failed: %s", __func__,
		    win32_strerror(GetLastError()));
		errno = EILSEQ;
		return (-1);
	}

	wdata = xcalloc(n, sizeof *wdata);
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, nbytes,
	    wdata, n) == 0) {
		log_debug("%s: MultiByteToWideChar failed: %s", __func__,
		    win32_strerror(GetLastError()));
		free(wdata);
		errno = EILSEQ;
		return (-1);
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
			log_debug("%s: WriteConsoleW wrote nothing", __func__);
			free(wdata);
			errno = EIO;
			return (-1);
		}
		total += written;
	}
	free(wdata);
	if (log_get_level() > 1) {
		log_debug("%s: WriteConsoleW wrote %lu UTF-16 units from "
		    "%d UTF-8 bytes", __func__, (unsigned long)total,
		    nbytes);
	}
	return (nbytes);
}

static int
win32_handle_write_console_sanitized(HANDLE handle, const u_char *data,
    size_t size)
{
	static const wchar_t replacement = 0xfffd;
	DWORD		 written, total;
	wchar_t		*wdata;
	size_t		 i, remaining, wlen, wcap;
	int		 needed;

	if (size == 0)
		return (0);

	wcap = size;
	wdata = xcalloc(wcap, sizeof *wdata);
	wlen = 0;

	for (i = 0; i < size; ) {
		needed = win32_utf8_expected(data[i]);
		if (needed == 0) {
			wdata[wlen++] = replacement;
			i++;
			continue;
		}
		remaining = size - i;
		if (remaining < (size_t)needed) {
			wdata[wlen++] = replacement;
			break;
		}
		if (needed > 1) {
			size_t j;

			for (j = 1; j < (size_t)needed; j++) {
				if ((data[i + j] & 0xc0) != 0x80)
					break;
			}
			if (j != (size_t)needed) {
				wdata[wlen++] = replacement;
				i++;
				continue;
			}
		}
		total = (DWORD)MultiByteToWideChar(CP_UTF8, 0, data + i, needed,
		    wdata + wlen, (int)(wcap - wlen));
		if (total == 0) {
			wdata[wlen++] = replacement;
			i++;
			continue;
		}
		wlen += total;
		i += (size_t)needed;
	}

	total = 0;
	while (total < wlen) {
		if (!WriteConsoleW(handle, wdata + total, (DWORD)(wlen - total),
		    &written, NULL)) {
			log_debug("%s: WriteConsoleW failed: %s", __func__,
			    win32_strerror(GetLastError()));
			free(wdata);
			errno = EIO;
			return (-1);
		}
		if (written == 0) {
			log_debug("%s: WriteConsoleW wrote nothing", __func__);
			free(wdata);
			errno = EIO;
			return (-1);
		}
		total += written;
	}
	free(wdata);
	log_debug("%s: sanitized %zu UTF-8 bytes for console output", __func__,
	    size);
	return ((int)size);
}

static int
win32_handle_write_console_utf8(struct win32_handle_writer *whw, HANDLE handle,
    const void *data, size_t size)
{
	u_char		*buf;
	const u_char	*input = data;
	size_t		 total, complete, keep, len;
	int		 written;

	if (size > INT_MAX)
		size = INT_MAX;
	if (size == 0)
		return (0);

	if (whw->utf8_partial_len != 0 && log_get_level() > 1) {
		log_debug("%s: resuming with %zu carried UTF-8 bytes", __func__,
		    whw->utf8_partial_len);
	}
	total = whw->utf8_partial_len + size;
	buf = xmalloc(total);
	if (whw->utf8_partial_len != 0)
		memcpy(buf, whw->utf8_partial, whw->utf8_partial_len);
	memcpy(buf + whw->utf8_partial_len, input, size);

	complete = win32_utf8_complete_len(buf, total);
	keep = total - complete;
	if (keep > sizeof whw->utf8_partial) {
		complete = total;
		keep = 0;
	}
	if (keep != 0 && log_get_level() > 1) {
		log_debug("%s: preserving %zu trailing UTF-8 bytes", __func__,
		    keep);
	}
	written = 0;
	if (complete != 0) {
		written = win32_handle_write_console(handle, buf, complete);
		if (written == -1) {
			if (errno != EILSEQ) {
				free(buf);
				return (-1);
			}
			log_debug("%s: invalid UTF-8 reached console boundary; "
			    "sanitizing output", __func__);
			written = win32_handle_write_console_sanitized(handle,
			    buf, complete);
			if (written == -1) {
				free(buf);
				return (-1);
			}
		}
		if ((size_t)written != complete) {
			free(buf);
			errno = EIO;
			return (-1);
		}
	}
	if (keep != 0)
		memcpy(whw->utf8_partial, buf + complete, keep);
	whw->utf8_partial_len = keep;
	len = size;
	free(buf);
	return ((int)len);
}

static enum win32_handle_writer_backend
win32_handle_writer_backend_for_handle(HANDLE *handle)
{
	if (handle != NULL && win32_console_handle(*handle))
		return (WIN32_HANDLE_WRITER_CONSOLE);
	return (WIN32_HANDLE_WRITER_WORKER);
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
