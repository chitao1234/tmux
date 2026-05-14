/* $OpenBSD$ */

/*
 * Copyright (c) 2019 Nicholas Marriott <nicholas.marriott@gmail.com>
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef TMUX_WIN32
#include <io.h>
#endif

#include "tmux.h"

/*
 * IPC file handling. Both client and server use the same data structures
 * (client_file and client_files) to store list of active files. Most functions
 * are for use either in client or server but not both.
 */

static int	file_next_stream = 3;

#define FILE_WRITE_WINDOW (1024 * 1024)

static void	file_push_close(struct client_file *);
static void	file_write_record(struct client_file *, size_t);
static void	file_write_flush_ack(struct client_file *, size_t, int);
static void	file_write_acknowledge(struct client_file *, size_t, int);
#ifdef TMUX_WIN32
static void	file_write_win32_local_callback(void *);
static void	file_write_win32_local_close(struct client_file *);
static void	file_write_win32_local_event_callback(void *, uint32_t);
static int	file_write_win32_local_flush(struct client_file *);
static int	file_write_win32_local_start(struct client_file *,
		    const void *, size_t);
static void	file_read_win32_local_callback(void *);
static void	file_read_win32_local_close(struct client_file *);
static void	file_read_win32_local_done_callback(void *);
static void	file_read_win32_local_event_callback(void *, uint32_t);
static int	file_read_win32_local_start(struct client_file *);
static void	file_write_win32_callback(void *);
static void	file_write_win32_error_callback(void *);
static void	file_write_win32_event_callback(void *, uint32_t);
static void	file_read_win32_callback(void *);
static void	file_read_win32_done_callback(void *);
static void	file_read_win32_event_callback(void *, uint32_t);
static int	file_write_console_text(struct client_file *, const char *,
		    size_t);
static int	file_write_win32_open_path(struct client_file *, const char *,
		    int);
static int	file_read_win32_open_path(struct client_file *, const char *,
		    int);
static void	file_win32_close_handle(HANDLE *);
#endif

RB_GENERATE(client_files, client_file, entry, file_cmp);

/* Get path for file, either as given or from working directory. */
static char *
file_get_path(struct client *c, const char *file)
{
	const char	*home;
	char		*path, *full_path;

	if (strncmp(file, "~/", 2) != 0)
		path = xstrdup(file);
	else {
		home = find_home();
		if (home == NULL)
			home = "";
		xasprintf(&path, "%s%s", home, file + 1);
	}
	if (path_is_absolute(path))
		return (path);
	xasprintf(&full_path, "%s/%s", server_client_get_cwd(c, NULL), path);
	free(path);
	return (full_path);
}

/* Tree comparison function. */
int
file_cmp(struct client_file *cf1, struct client_file *cf2)
{
	if (cf1->stream < cf2->stream)
		return (-1);
	if (cf1->stream > cf2->stream)
		return (1);
	return (0);
}

/*
 * Create a file object in the client process - the peer is the server to send
 * messages to. Check callback is fired when the file is finished with so the
 * process can decide if it needs to exit (if it is waiting for files to
 * flush).
 */
struct client_file *
file_create_with_peer(struct tmuxpeer *peer, struct client_files *files,
    int stream, client_file_cb cb, void *cbdata)
{
	struct client_file	*cf;

	cf = xcalloc(1, sizeof *cf);
	cf->c = NULL;
	cf->references = 1;
	cf->stream = stream;

	cf->buffer = evbuffer_new();
	if (cf->buffer == NULL)
		fatalx("out of memory");

	cf->cb = cb;
	cf->data = cbdata;

	cf->peer = peer;
	cf->tree = files;
	RB_INSERT(client_files, files, cf);

	return (cf);
}

/* Create a file object in the server, communicating with the given client. */
struct client_file *
file_create_with_client(struct client *c, int stream, client_file_cb cb,
    void *cbdata)
{
	struct client_file	*cf;

	if (c != NULL && (c->flags & CLIENT_ATTACHED))
		c = NULL;

	cf = xcalloc(1, sizeof *cf);
	cf->c = c;
	cf->references = 1;
	cf->stream = stream;

	cf->buffer = evbuffer_new();
	if (cf->buffer == NULL)
		fatalx("out of memory");

	cf->cb = cb;
	cf->data = cbdata;

	if (cf->c != NULL) {
		cf->peer = cf->c->peer;
		cf->tree = &cf->c->files;
		RB_INSERT(client_files, &cf->c->files, cf);
		cf->c->references++;
	}

	return (cf);
}

/* Free a file. */
void
file_free(struct client_file *cf)
{
	if (--cf->references != 0)
		return;

	evbuffer_free(cf->buffer);
	free(cf->path);

#ifdef TMUX_WIN32
	if (cf->win32_writer != NULL)
		win32_io_endpoint_free(cf->win32_writer);
	if (cf->win32_reader != NULL)
		win32_io_endpoint_free(cf->win32_reader);
	file_win32_close_handle(&cf->win32_handle);
#endif

	if (cf->tree != NULL)
		RB_REMOVE(client_files, cf->tree, cf);
	if (cf->c != NULL)
		server_client_unref(cf->c);

	free(cf);
}

/* Event to fire the done callback. */
static void
file_fire_done_cb(__unused tmux_event_fd fd, __unused short events, void *arg)
{
	struct client_file	*cf = arg;
	struct client		*c = cf->c;

	if (cf->cb != NULL &&
	    (cf->closed || c == NULL || (~c->flags & CLIENT_DEAD)))
		cf->cb(c, cf->path, cf->error, 1, cf->buffer, cf->data);
	file_free(cf);
}

/* Add an event to fire the done callback (used by the server). */
void
file_fire_done(struct client_file *cf)
{
	event_once(-1, EV_TIMEOUT, file_fire_done_cb, cf, NULL);
}

/* Fire the read callback. */
void
file_fire_read(struct client_file *cf)
{
	if (cf->cb != NULL)
		cf->cb(cf->c, cf->path, cf->error, 0, cf->buffer, cf->data);
}

/* Can this file be printed to? */
int
file_can_print(struct client *c)
{
	if (c == NULL ||
	    (c->flags & CLIENT_ATTACHED) ||
	    (c->flags & CLIENT_CONTROL))
		return (0);
	return (1);
}

/* Print a message to a file. */
void
file_print(struct client *c, const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	file_vprint(c, fmt, ap);
	va_end(ap);
}

/* Print a message to a file. */
void
file_vprint(struct client *c, const char *fmt, va_list ap)
{
	struct client_file	 find, *cf;
	struct msg_write_open	 msg;

	if (!file_can_print(c))
		return;

	find.stream = 1;
	if ((cf = RB_FIND(client_files, &c->files, &find)) == NULL) {
		cf = file_create_with_client(c, 1, NULL, NULL);
		cf->path = xstrdup("-");

		evbuffer_add_vprintf(cf->buffer, fmt, ap);

		msg.stream = 1;
		msg.fd = STDOUT_FILENO;
		msg.flags = 0;
		proc_send(c->peer, MSG_WRITE_OPEN, -1, &msg, sizeof msg);
	} else {
		evbuffer_add_vprintf(cf->buffer, fmt, ap);
		file_push(cf);
	}
}

/* Print a buffer to a file. */
void
file_print_buffer(struct client *c, void *data, size_t size)
{
	struct client_file	 find, *cf;
	struct msg_write_open	 msg;

	if (!file_can_print(c))
		return;

	find.stream = 1;
	if ((cf = RB_FIND(client_files, &c->files, &find)) == NULL) {
		cf = file_create_with_client(c, 1, NULL, NULL);
		cf->path = xstrdup("-");

		evbuffer_add(cf->buffer, data, size);

		msg.stream = 1;
		msg.fd = STDOUT_FILENO;
		msg.flags = 0;
		proc_send(c->peer, MSG_WRITE_OPEN, -1, &msg, sizeof msg);
	} else {
		evbuffer_add(cf->buffer, data, size);
		file_push(cf);
	}
}

/* Report an error to a file. */
void
file_error(struct client *c, const char *fmt, ...)
{
	struct client_file	 find, *cf;
	struct msg_write_open	 msg;
	va_list			 ap;

	if (!file_can_print(c))
		return;

	va_start(ap, fmt);

	find.stream = 2;
	if ((cf = RB_FIND(client_files, &c->files, &find)) == NULL) {
		cf = file_create_with_client(c, 2, NULL, NULL);
		cf->path = xstrdup("-");

		evbuffer_add_vprintf(cf->buffer, fmt, ap);

		msg.stream = 2;
		msg.fd = STDERR_FILENO;
		msg.flags = 0;
		proc_send(c->peer, MSG_WRITE_OPEN, -1, &msg, sizeof msg);
	} else {
		evbuffer_add_vprintf(cf->buffer, fmt, ap);
		file_push(cf);
	}

	va_end(ap);
}

/* Write data to a file. */
void
file_write(struct client *c, const char *path, int flags, const void *bdata,
    size_t bsize, client_file_cb cb, void *cbdata)
{
	struct client_file	*cf;
	struct msg_write_open	*msg;
	size_t			 msglen;
	int			 fd = -1;
	u_int			 stream = file_next_stream++;
#ifndef TMUX_WIN32
	FILE			*f;
	const char		*mode;
#endif

	if (strcmp(path, "-") == 0) {
		cf = file_create_with_client(c, stream, cb, cbdata);
		cf->path = xstrdup("-");

		fd = STDOUT_FILENO;
		if (c == NULL ||
		    (c->flags & CLIENT_ATTACHED) ||
		    (c->flags & CLIENT_CONTROL)) {
			cf->error = EBADF;
			goto done;
		}
		goto skip;
	}

	cf = file_create_with_client(c, stream, cb, cbdata);
	cf->path = file_get_path(c, path);

	if (c == NULL || c->flags & CLIENT_ATTACHED) {
#ifdef TMUX_WIN32
		if (file_write_win32_open_path(cf, cf->path, flags|O_WRONLY|
		    O_CREAT) != 0) {
			cf->error = errno;
			goto done;
		}
		if (file_write_win32_local_start(cf, bdata, bsize) == 0)
			return;
		cf->error = errno;
		file_write_win32_local_close(cf);
		goto done;
#else
		if (flags & O_APPEND)
			mode = "ab";
		else
			mode = "wb";
		f = fopen(cf->path, mode);
		if (f == NULL) {
			cf->error = errno;
			goto done;
		}
		if (fwrite(bdata, 1, bsize, f) != bsize) {
			fclose(f);
			cf->error = EIO;
			goto done;
		}
		fclose(f);
		goto done;
#endif
	}

skip:
	evbuffer_add(cf->buffer, bdata, bsize);

	msglen = strlen(cf->path) + 1 + sizeof *msg;
	if (msglen > MAX_IMSGSIZE - IMSG_HEADER_SIZE) {
		cf->error = E2BIG;
		goto done;
	}
	msg = xmalloc(msglen);
	msg->stream = cf->stream;
	msg->fd = fd;
	msg->flags = flags;
	memcpy(msg + 1, cf->path, msglen - sizeof *msg);
	if (proc_send(cf->peer, MSG_WRITE_OPEN, -1, msg, msglen) != 0) {
		free(msg);
		cf->error = EINVAL;
		goto done;
	}
	free(msg);
	return;

done:
	file_fire_done(cf);
}

/* Read a file. */
struct client_file *
file_read(struct client *c, const char *path, client_file_cb cb, void *cbdata)
{
	struct client_file	*cf;
	struct msg_read_open	*msg;
	size_t			 msglen;
	int			 fd = -1;
	u_int			 stream = file_next_stream++;
#ifndef TMUX_WIN32
	FILE			*f = NULL;
	size_t			 size;
	char			 buffer[BUFSIZ];
#endif

	if (strcmp(path, "-") == 0) {
		cf = file_create_with_client(c, stream, cb, cbdata);
		cf->path = xstrdup("-");

		fd = STDIN_FILENO;
		if (c == NULL ||
		    (c->flags & CLIENT_ATTACHED) ||
		    (c->flags & CLIENT_CONTROL)) {
			cf->error = EBADF;
			goto done;
		}
		goto skip;
	}

	cf = file_create_with_client(c, stream, cb, cbdata);
	cf->path = file_get_path(c, path);

	if (c == NULL || c->flags & CLIENT_ATTACHED) {
#ifdef TMUX_WIN32
		if (file_read_win32_open_path(cf, cf->path, O_RDONLY) != 0) {
			cf->error = errno;
			goto done;
		}
		if (file_read_win32_local_start(cf) == 0)
			return cf;
		cf->error = errno;
		file_read_win32_local_close(cf);
		goto done;
#else
		f = fopen(cf->path, "rb");
		if (f == NULL) {
			cf->error = errno;
			goto done;
		}
		for (;;) {
			size = fread(buffer, 1, sizeof buffer, f);
			if (evbuffer_add(cf->buffer, buffer, size) != 0) {
				cf->error = ENOMEM;
				goto done;
			}
			if (size != sizeof buffer)
				break;
		}
		if (ferror(f)) {
			cf->error = EIO;
			goto done;
		}
		goto done;
#endif
	}

skip:
	msglen = strlen(cf->path) + 1 + sizeof *msg;
	if (msglen > MAX_IMSGSIZE - IMSG_HEADER_SIZE) {
		cf->error = E2BIG;
		goto done;
	}
	msg = xmalloc(msglen);
	msg->stream = cf->stream;
	msg->fd = fd;
	memcpy(msg + 1, cf->path, msglen - sizeof *msg);
	if (proc_send(cf->peer, MSG_READ_OPEN, -1, msg, msglen) != 0) {
		free(msg);
		cf->error = EINVAL;
		goto done;
	}
	free(msg);
	return cf;

done:
#ifndef TMUX_WIN32
	if (f != NULL)
		fclose(f);
#endif
	file_fire_done(cf);
	return NULL;
}

/* Cancel a file read. */
void
file_cancel(struct client_file *cf)
{
	struct msg_read_cancel	 msg;

	log_debug("read cancel file %d", cf->stream);

	if (cf->closed)
		return;
	cf->closed = 1;

	msg.stream = cf->stream;
	proc_send(cf->peer, MSG_READ_CANCEL, -1, &msg, sizeof msg);
}

/* Push event, fired if there is more writing to be done. */
static void
file_push_close(struct client_file *cf)
{
	struct msg_write_close	close;

	if (cf->stream <= 2 || EVBUFFER_LENGTH(cf->buffer) != 0 ||
	    cf->write_inflight != 0)
		return;
	close.stream = cf->stream;
	proc_send(cf->peer, MSG_WRITE_CLOSE, -1, &close, sizeof close);
	file_fire_done(cf);
}

/* Push event, fired if there is more writing to be done. */
static void
file_push_cb(__unused tmux_event_fd fd, __unused short events, void *arg)
{
	struct client_file	*cf = arg;

	if (cf->c == NULL || ~cf->c->flags & CLIENT_DEAD)
		file_push(cf);
	file_free(cf);
}

/* Push uwritten data to the client for a file, if it will accept it. */
void
file_push(struct client_file *cf)
{
	struct msg_write_data	*msg;
	size_t			 available, msglen, sent, left, window;

	msg = xmalloc(sizeof *msg);
	left = EVBUFFER_LENGTH(cf->buffer);
	while (left != 0 && cf->write_inflight < FILE_WRITE_WINDOW) {
		sent = left;
		window = FILE_WRITE_WINDOW - cf->write_inflight;
		if (sent > window)
			sent = window;
		if (sent > MAX_IMSGSIZE - IMSG_HEADER_SIZE - sizeof *msg)
			sent = MAX_IMSGSIZE - IMSG_HEADER_SIZE - sizeof *msg;

		msglen = (sizeof *msg) + sent;
		msg = xrealloc(msg, msglen);
		msg->stream = cf->stream;
		memcpy(msg + 1, EVBUFFER_DATA(cf->buffer), sent);
		if (proc_send(cf->peer, MSG_WRITE, -1, msg, msglen) != 0)
			break;
		evbuffer_drain(cf->buffer, sent);
		cf->write_inflight += sent;

		left = EVBUFFER_LENGTH(cf->buffer);
		log_debug("file %d sent %zu, left %zu, inflight %zu",
		    cf->stream, sent, left, cf->write_inflight);
	}
	available = EVBUFFER_LENGTH(cf->buffer);
	if (available != 0 && cf->write_inflight < FILE_WRITE_WINDOW) {
		cf->references++;
		event_once(-1, EV_TIMEOUT, file_push_cb, cf, NULL);
	} else
		file_push_close(cf);
	free(msg);
}

/* Check if any files have data left to write. */
int
file_write_left(struct client_files *files)
{
	struct client_file	*cf;
	size_t			 left;
	int			 waiting = 0;

	RB_FOREACH(cf, client_files, files) {
		if (cf->write_inflight != 0) {
			waiting++;
			log_debug("file %u %zu bytes in flight", cf->stream,
			    cf->write_inflight);
		}
		if (cf->write_pending != 0) {
			waiting++;
			log_debug("file %u %zu bytes pending ack", cf->stream,
			    cf->write_pending);
		}
		if (cf->event == NULL)
			continue;
		left = EVBUFFER_LENGTH(cf->event->output);
		if (left != 0) {
			waiting++;
			log_debug("file %u %zu bytes left", cf->stream, left);
		}
	}
	return (waiting != 0);
}

#ifndef TMUX_WIN32
/* Client file write error callback. */
static void
file_write_error_callback(__unused struct bufferevent *bev, __unused short what,
    void *arg)
{
	struct client_file	*cf = arg;

	log_debug("write error file %d", cf->stream);

	file_write_acknowledge(cf, cf->write_pending, EIO);

	bufferevent_free(cf->event);
	cf->event = NULL;

	close(cf->fd);
	cf->fd = -1;

	if (cf->cb != NULL)
		cf->cb(NULL, NULL, 0, -1, NULL, cf->data);
}

/* Client file write callback. */
static void
file_write_callback(__unused struct bufferevent *bev, void *arg)
{
	struct client_file	*cf = arg;
	size_t			 left, written;

	log_debug("write check file %d", cf->stream);

	left = EVBUFFER_LENGTH(cf->event->output);
	if (cf->write_pending >= left) {
		written = cf->write_pending - left;
		file_write_acknowledge(cf, written, 0);
	}

	if (cf->cb != NULL)
		cf->cb(NULL, NULL, 0, -1, NULL, cf->data);

	if (cf->closed && EVBUFFER_LENGTH(cf->event->output) == 0) {
		bufferevent_free(cf->event);
		cf->event = NULL;
		close(cf->fd);
		cf->fd = -1;
		file_free(cf);
	}
}
#endif

static void
file_write_record(struct client_file *cf, size_t size)
{
	cf->write_pending += size;
}

static void
file_write_flush_ack(struct client_file *cf, size_t size, int error)
{
	struct msg_write_ack	msg;
	size_t			left, nsend;

	if (cf->peer == NULL || size == 0)
		return;
	msg.stream = cf->stream;
	msg.error = error;
	left = size;
	do {
		nsend = left;
		if (nsend > UINT32_MAX)
			nsend = UINT32_MAX;
		msg.size = (uint32_t)nsend;
		proc_send(cf->peer, MSG_WRITE_ACK, -1, &msg, sizeof msg);
		left -= nsend;
	} while (left != 0);
}

static void
file_write_acknowledge(struct client_file *cf, size_t size, int error)
{
	size_t	acknowledged;

	if (size > cf->write_pending)
		size = cf->write_pending;
	acknowledged = size;
	cf->write_pending -= acknowledged;
	file_write_flush_ack(cf, acknowledged, error);
}

#ifdef TMUX_WIN32
static int
file_win32_errno(DWORD error)
{
	switch (error) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		return (ENOENT);
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		return (EACCES);
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:
		return (EEXIST);
	case ERROR_INVALID_NAME:
	case ERROR_INVALID_PARAMETER:
		return (EINVAL);
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
		return (ENOMEM);
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL:
		return (ENOSPC);
	default:
		return (EIO);
	}
}

static void
file_win32_close_handle(HANDLE *handle)
{
	if (*handle != NULL && *handle != INVALID_HANDLE_VALUE)
		CloseHandle(*handle);
	*handle = NULL;
}

static int
file_win32_regular_handle(HANDLE handle)
{
	DWORD	type, error;

	SetLastError(NO_ERROR);
	type = GetFileType(handle);
	if (type == FILE_TYPE_DISK)
		return (1);
	if (type == FILE_TYPE_UNKNOWN) {
		error = GetLastError();
		if (error != NO_ERROR) {
			errno = file_win32_errno(error);
			return (-1);
		}
	}
	return (0);
}

static DWORD
file_write_win32_disposition(int flags)
{
	if ((flags & O_CREAT) && (flags & O_EXCL))
		return (CREATE_NEW);
	if (flags & O_TRUNC)
		return (CREATE_ALWAYS);
	return (OPEN_ALWAYS);
}

static int
file_write_win32_open_path(struct client_file *cf, const char *path, int flags)
{
	wchar_t	*wpath;
	HANDLE	 handle;
	int	 regular;

	wpath = win32_utf8_to_wide(path);
	if (wpath == NULL) {
		errno = EINVAL;
		return (-1);
	}
	handle = CreateFileW(wpath, GENERIC_WRITE,
	    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL,
	    file_write_win32_disposition(flags),
	    FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED, NULL);
	free(wpath);
	if (handle == INVALID_HANDLE_VALUE) {
		errno = file_win32_errno(GetLastError());
		return (-1);
	}

	regular = file_win32_regular_handle(handle);
	if (regular == 1) {
		cf->win32_handle = handle;
		cf->win32_offset = 0;
		cf->win32_append = !!(flags & O_APPEND);
		return (0);
	}
	file_win32_close_handle(&handle);
	if (regular == -1)
		return (-1);

	cf->fd = open(path, flags, 0644);
	if (cf->fd == -1)
		return (-1);
	return (0);
}

static int
file_read_win32_open_path(struct client_file *cf, const char *path, int flags)
{
	wchar_t	*wpath;
	HANDLE	 handle;
	int	 regular;

	wpath = win32_utf8_to_wide(path);
	if (wpath == NULL) {
		errno = EINVAL;
		return (-1);
	}
	handle = CreateFileW(wpath, GENERIC_READ,
	    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL,
	    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED, NULL);
	free(wpath);
	if (handle == INVALID_HANDLE_VALUE) {
		errno = file_win32_errno(GetLastError());
		return (-1);
	}

	regular = file_win32_regular_handle(handle);
	if (regular == 1) {
		cf->win32_handle = handle;
		cf->win32_offset = 0;
		return (0);
	}
	file_win32_close_handle(&handle);
	if (regular == -1)
		return (-1);

	cf->fd = open(path, flags);
	if (cf->fd == -1)
		return (-1);
	return (0);
}

static void
file_write_win32_close(struct client_file *cf)
{
	int	fd;

	if (cf->win32_writer != NULL) {
		win32_io_endpoint_free(cf->win32_writer);
		cf->win32_writer = NULL;
	}
	fd = cf->fd;
	cf->fd = -1;
	if (fd != -1)
		close(fd);
	file_win32_close_handle(&cf->win32_handle);
}

static int
file_write_win32_start(struct client_file *cf)
{
	intptr_t	osfhandle;
	HANDLE	handle;

	if (cf->win32_writer != NULL)
		return (0);
	if (cf->win32_handle != NULL) {
		cf->win32_writer = win32_io_writer_new_file_borrowed(
		    &cf->win32_handle, cf->win32_offset, cf->win32_append,
		    file_write_win32_event_callback, cf);
		if (cf->win32_writer == NULL) {
			errno = EIO;
			return (-1);
		}
		return (0);
	}
	if (cf->fd == -1) {
		errno = EBADF;
		return (-1);
	}
	osfhandle = _get_osfhandle(cf->fd);
	handle = (HANDLE)osfhandle;
	if (handle == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return (-1);
	}
	cf->win32_writer = win32_io_writer_new_worker_borrowed(&handle,
	    file_write_win32_event_callback, cf);
	if (cf->win32_writer == NULL) {
		errno = EIO;
		return (-1);
	}
	return (0);
}

static int
file_write_win32_raw(struct client_file *cf, const void *data, size_t size)
{
	int	written;

	if (file_write_win32_start(cf) != 0)
		return (-1);
	written = win32_io_writer_write(cf->win32_writer, data, size);
	if (written != (int)size) {
		errno = EIO;
		return (-1);
	}
	return (0);
}

static int
file_write_win32_queue(struct client_file *cf, const void *data, size_t size)
{
	if (cf->flags & CLIENT_FILE_WIN32_CONSOLE)
		return (file_write_console_text(cf, data, size) == 1 ? 0 : -1);
	return (file_write_win32_raw(cf, data, size));
}

static void
file_write_win32_fail(struct client_file *cf, int error)
{
	if (error == 0)
		error = EIO;
	if (cf->write_pending != 0)
		file_write_acknowledge(cf, cf->write_pending, error);
	if (cf->cb != NULL)
		cf->cb(NULL, NULL, 0, -1, NULL, cf->data);
	file_write_win32_close(cf);
}

static void
file_write_win32_queue_data(struct client_file *cf, const void *data,
    size_t size)
{
	int	error;

	file_write_record(cf, size);
	if (file_write_win32_queue(cf, data, size) == 0)
		return;
	error = errno;
	file_write_win32_fail(cf, error);
}

static void
file_write_win32_open(struct client_file *cf)
{
	intptr_t	osfhandle;
	HANDLE	handle;
	DWORD	mode;

	if (file_write_win32_start(cf) != 0)
		return;
	if (cf->win32_handle != NULL)
		return;
	osfhandle = _get_osfhandle(cf->fd);
	handle = (HANDLE)osfhandle;
	if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode))
		cf->flags |= CLIENT_FILE_WIN32_CONSOLE;
}

static void
file_write_win32_callback(void *arg)
{
	struct client_file	*cf = arg;
	size_t			 size;

	size = cf->write_pending;
	if (size == 0) {
		if (cf->closed &&
		    win32_io_writer_buffered(cf->win32_writer) == 0) {
			file_write_win32_close(cf);
			file_free(cf);
		}
		return;
	}
	file_write_acknowledge(cf, size, 0);
	if (cf->cb != NULL)
		cf->cb(NULL, NULL, 0, -1, NULL, cf->data);
	if (cf->closed && win32_io_writer_drained(cf->win32_writer)) {
		file_write_win32_close(cf);
		file_free(cf);
	}
}

static void
file_write_win32_error_callback(void *arg)
{
	struct client_file	*cf = arg;

	if (cf->fd == -1 && cf->win32_handle == NULL &&
	    cf->win32_writer == NULL)
		return;
	file_write_win32_fail(cf, EIO);
}

static void
file_write_win32_event_callback(void *arg, uint32_t events)
{
	if (events & WIN32_IO_EVENT_ERROR) {
		file_write_win32_error_callback(arg);
		return;
	}
	if (events & (WIN32_IO_EVENT_WRITE_DRAINED|
	    WIN32_IO_EVENT_WRITE_CLOSED))
		file_write_win32_callback(arg);
}

static void
file_read_win32_close(struct client_file *cf)
{
	int	fd;

	if (cf->win32_reader != NULL) {
		win32_io_endpoint_free(cf->win32_reader);
		cf->win32_reader = NULL;
	}
	fd = cf->fd;
	cf->fd = -1;
	if (fd != -1)
		close(fd);
	file_win32_close_handle(&cf->win32_handle);
	file_free(cf);
}

static void
file_read_win32_callback(void *arg)
{
	struct client_file	*cf = arg;
	struct evbuffer		*input;
	struct msg_read_data	*msg;
	size_t			 bsize, msglen;
	void			*bdata;

	if (cf->win32_reader == NULL)
		return;

	input = evbuffer_new();
	if (input == NULL)
		fatalx("out of memory");
	win32_io_reader_drain(cf->win32_reader, input);

	msg = xmalloc(sizeof *msg);
	for (;;) {
		bdata = EVBUFFER_DATA(input);
		bsize = EVBUFFER_LENGTH(input);
		if (bsize == 0)
			break;
		if (bsize > MAX_IMSGSIZE - IMSG_HEADER_SIZE - sizeof *msg)
			bsize = MAX_IMSGSIZE - IMSG_HEADER_SIZE - sizeof *msg;
		log_debug("read %zu from file %d", bsize, cf->stream);

		msglen = sizeof *msg + bsize;
		msg = xrealloc(msg, msglen);
		msg->stream = cf->stream;
		memcpy(msg + 1, bdata, bsize);
		proc_send(cf->peer, MSG_READ, -1, msg, msglen);

		evbuffer_drain(input, bsize);
	}
	free(msg);
	evbuffer_free(input);
}

static void
file_read_win32_event_callback(void *arg, uint32_t events)
{
	if (events & WIN32_IO_EVENT_READ)
		file_read_win32_callback(arg);
	if (events & (WIN32_IO_EVENT_READ_EOF|WIN32_IO_EVENT_ERROR|
	    WIN32_IO_EVENT_CANCELED))
		file_read_win32_done_callback(arg);
}

static void
file_read_win32_done_callback(void *arg)
{
	struct client_file	*cf = arg;
	struct msg_read_done	 msg;

	file_read_win32_callback(cf);

	msg.stream = cf->stream;
	msg.error = win32_io_reader_error(cf->win32_reader) ? EIO : 0;
	proc_send(cf->peer, MSG_READ_DONE, -1, &msg, sizeof msg);

	file_read_win32_close(cf);
}

static int
file_read_win32_start(struct client_file *cf)
{
	intptr_t	osfhandle;
	HANDLE	handle;

	if (cf->win32_handle != NULL) {
		cf->win32_reader = win32_io_reader_new_file(cf->win32_handle,
		    cf->win32_offset, file_read_win32_event_callback, cf);
		if (cf->win32_reader == NULL) {
			errno = EIO;
			return (-1);
		}
		return (0);
	}
	osfhandle = _get_osfhandle(cf->fd);
	handle = (HANDLE)osfhandle;
	if (handle == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return (-1);
	}
	cf->win32_reader = win32_io_reader_new_worker(handle,
	    file_read_win32_event_callback, cf);
	if (cf->win32_reader == NULL) {
		errno = EIO;
		return (-1);
	}
	return (0);
}

static int
file_write_win32_local_flush(struct client_file *cf)
{
	size_t	size, nwrite;
	int	written;

	for (;;) {
		size = EVBUFFER_LENGTH(cf->buffer);
		if (size == 0) {
			if (!cf->closed &&
			    win32_io_writer_drained(cf->win32_writer)) {
				cf->closed = 1;
				win32_io_writer_close(cf->win32_writer);
			}
			return (0);
		}
		nwrite = size;
		if (nwrite > 1024 * 1024)
			nwrite = 1024 * 1024;
		written = win32_io_writer_write(cf->win32_writer,
		    EVBUFFER_DATA(cf->buffer), nwrite);
		if (written == -1) {
			if (errno == EAGAIN)
				return (0);
			return (-1);
		}
		evbuffer_drain(cf->buffer, written);
	}
}

static void
file_write_win32_local_close(struct client_file *cf)
{
	int	fd;

	if (cf->win32_writer != NULL) {
		win32_io_endpoint_free(cf->win32_writer);
		cf->win32_writer = NULL;
	}
	fd = cf->fd;
	cf->fd = -1;
	if (fd != -1)
		close(fd);
	file_win32_close_handle(&cf->win32_handle);
}

static void
file_write_win32_local_callback(void *arg)
{
	struct client_file	*cf = arg;

	if (cf->win32_writer == NULL)
		return;
	if (file_write_win32_local_flush(cf) != 0) {
		cf->error = errno;
		file_write_win32_local_close(cf);
		file_fire_done(cf);
	}
}

static void
file_write_win32_local_event_callback(void *arg, uint32_t events)
{
	struct client_file	*cf = arg;

	if (events & WIN32_IO_EVENT_ERROR) {
		cf->error = EIO;
		file_write_win32_local_close(cf);
		file_fire_done(cf);
		return;
	}
	if (events & WIN32_IO_EVENT_WRITE_CLOSED) {
		cf->closed = 1;
		file_write_win32_local_close(cf);
		file_fire_done(cf);
		return;
	}
	if (events & WIN32_IO_EVENT_WRITE_DRAINED)
		file_write_win32_local_callback(arg);
}

static int
file_write_win32_local_start(struct client_file *cf, const void *data,
    size_t size)
{
	if (cf->win32_handle == NULL) {
		errno = EINVAL;
		return (-1);
	}
	cf->win32_writer = win32_io_writer_new_file_borrowed(
	    &cf->win32_handle, cf->win32_offset, cf->win32_append,
	    file_write_win32_local_event_callback, cf);
	if (cf->win32_writer == NULL) {
		errno = EIO;
		return (-1);
	}
	if (evbuffer_add(cf->buffer, data, size) != 0) {
		errno = ENOMEM;
		return (-1);
	}
	return (file_write_win32_local_flush(cf));
}

static void
file_read_win32_local_close(struct client_file *cf)
{
	int	fd;

	if (cf->win32_reader != NULL) {
		win32_io_endpoint_free(cf->win32_reader);
		cf->win32_reader = NULL;
	}
	fd = cf->fd;
	cf->fd = -1;
	if (fd != -1)
		close(fd);
	file_win32_close_handle(&cf->win32_handle);
}

static void
file_read_win32_local_callback(void *arg)
{
	struct client_file	*cf = arg;

	if (cf->win32_reader == NULL)
		return;
	win32_io_reader_drain(cf->win32_reader, cf->buffer);
	file_fire_read(cf);
}

static void
file_read_win32_local_done_callback(void *arg)
{
	struct client_file	*cf = arg;

	file_read_win32_local_callback(cf);
	if (win32_io_reader_error(cf->win32_reader))
		cf->error = EIO;
	cf->closed = 1;
	file_read_win32_local_close(cf);
	file_fire_done(cf);
}

static void
file_read_win32_local_event_callback(void *arg, uint32_t events)
{
	if (events & WIN32_IO_EVENT_READ)
		file_read_win32_local_callback(arg);
	if (events & (WIN32_IO_EVENT_READ_EOF|WIN32_IO_EVENT_ERROR|
	    WIN32_IO_EVENT_CANCELED))
		file_read_win32_local_done_callback(arg);
}

static int
file_read_win32_local_start(struct client_file *cf)
{
	if (cf->win32_handle == NULL) {
		errno = EINVAL;
		return (-1);
	}
	cf->win32_reader = win32_io_reader_new_file(cf->win32_handle,
	    cf->win32_offset, file_read_win32_local_event_callback, cf);
	if (cf->win32_reader == NULL) {
		errno = EIO;
		return (-1);
	}
	return (0);
}

static int
file_write_console_text(struct client_file *cf, const char *data, size_t size)
{
	HANDLE		 handle;
	intptr_t	 osfhandle;
	DWORD		 mode;
	struct evbuffer	*buffer;
	const char	*ptr, *start, *end;
	int		 error;

	errno = 0;
	if (cf->win32_handle != NULL)
		return (0);
	osfhandle = _get_osfhandle(cf->fd);
	handle = (HANDLE)osfhandle;
	if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode))
		return (0);
	if (cf->win32_writer == NULL) {
		errno = EIO;
		return (-1);
	}

	buffer = evbuffer_new();
	if (buffer == NULL)
		fatalx("out of memory");

	start = ptr = data;
	end = data + size;
	while (ptr != end) {
		if (*ptr == '\n' && (~cf->flags & CLIENT_FILE_LAST_CR)) {
			if (ptr != start)
				evbuffer_add(buffer, start, ptr - start);
			evbuffer_add(buffer, "\r\n", 2);
			start = ptr + 1;
		}
		cf->flags &= ~CLIENT_FILE_LAST_CR;
		if (*ptr == '\r')
			cf->flags |= CLIENT_FILE_LAST_CR;
		ptr++;
	}
	if (ptr != start)
		evbuffer_add(buffer, start, ptr - start);

	error = 0;
	if (EVBUFFER_LENGTH(buffer) != 0) {
		if (win32_io_writer_write(cf->win32_writer,
		    EVBUFFER_DATA(buffer), EVBUFFER_LENGTH(buffer)) == -1)
			error = errno;
	}
	evbuffer_free(buffer);

	if (error != 0) {
		log_debug("write error file %d: %s", cf->stream,
		    strerror(error));
		errno = error;
		return (-1);
	}

	return (1);
}
#endif

/* Handle a file write open message (client). */
void
file_write_open(struct client_files *files, struct tmuxpeer *peer,
    struct imsg *imsg, int allow_streams, int close_received,
    client_file_cb cb, void *cbdata)
{
	struct msg_write_open	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	const char		*path;
	struct msg_write_ready	 reply;
	struct client_file	 find, *cf = NULL;
	const int		 flags = O_NONBLOCK|O_WRONLY|O_CREAT;
	int			 error = 0;

	if (msglen < sizeof *msg)
		fatalx("bad MSG_WRITE_OPEN size");
	if (msglen == sizeof *msg)
		path = "-";
	else
		path = (const char *)(msg + 1);
	log_debug("open write file %d %s", msg->stream, path);

	find.stream = msg->stream;
	if (RB_FIND(client_files, files, &find) != NULL) {
		error = EBADF;
		goto reply;
	}
	cf = file_create_with_peer(peer, files, msg->stream, cb, cbdata);
	if (cf->closed) {
		error = EBADF;
		goto reply;
	}

	cf->fd = -1;
#ifdef TMUX_WIN32
	if (msg->fd == -1) {
		if (file_write_win32_open_path(cf, path,
		    msg->flags|flags) != 0) {
			error = errno;
			goto reply;
		}
	}
#else
	if (msg->fd == -1)
		cf->fd = open(path, msg->flags|flags, 0644);
#endif
	else if (allow_streams) {
		if (msg->fd != STDOUT_FILENO && msg->fd != STDERR_FILENO)
			errno = EBADF;
		else {
			cf->fd = dup(msg->fd);
			if (close_received)
				close(msg->fd); /* can only be used once */
		}
	} else
	      errno = EBADF;
	if (cf->fd == -1
#ifdef TMUX_WIN32
	    && cf->win32_handle == NULL
#endif
	    ) {
		error = errno;
		goto reply;
	}
#ifdef TMUX_WIN32
	if (msg->fd == STDOUT_FILENO || msg->fd == STDERR_FILENO) {
		if (msg->stream == STDOUT_FILENO || msg->stream == STDERR_FILENO)
			cf->flags |= CLIENT_FILE_TEXT;
	}
	file_write_win32_open(cf);
	if (cf->win32_writer == NULL) {
		error = errno;
		goto reply;
	}
#endif

#ifndef TMUX_WIN32
	cf->event = bufferevent_new(cf->fd, NULL, file_write_callback,
	    file_write_error_callback, cf);
	if (cf->event == NULL)
		fatalx("out of memory");
	bufferevent_enable(cf->event, EV_WRITE);
#endif
	goto reply;

reply:
	if (error != 0 && cf != NULL) {
#ifdef TMUX_WIN32
		if (cf->win32_writer != NULL) {
			win32_io_endpoint_free(cf->win32_writer);
			cf->win32_writer = NULL;
		}
		file_win32_close_handle(&cf->win32_handle);
#endif
		if (cf->fd != -1)
			close(cf->fd);
		cf->fd = -1;
		file_free(cf);
	}
	reply.stream = msg->stream;
	reply.error = error;
	proc_send(peer, MSG_WRITE_READY, -1, &reply, sizeof reply);
}

/* Handle a file write data message (client). */
void
file_write_data(struct client_files *files, struct imsg *imsg)
{
	struct msg_write_data	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	struct client_file	 find, *cf;
	size_t			 size = msglen - sizeof *msg;

	if (msglen < sizeof *msg)
		fatalx("bad MSG_WRITE size");
	find.stream = msg->stream;
	if ((cf = RB_FIND(client_files, files, &find)) == NULL)
		fatalx("unknown stream number");
	log_debug("write %zu to file %d", size, cf->stream);

#ifdef TMUX_WIN32
	if (cf->event == NULL) {
		if (cf->fd != -1 || cf->win32_handle != NULL)
			file_write_win32_queue_data(cf, msg + 1, size);
		return;
	}
#endif
	if (cf->event != NULL) {
		file_write_record(cf, size);
		bufferevent_write(cf->event, msg + 1, size);
	}
}

/* Handle a file write close message (client). */
void
file_write_close(struct client_files *files, struct imsg *imsg)
{
	struct msg_write_close	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	struct client_file	 find, *cf;

	if (msglen != sizeof *msg)
		fatalx("bad MSG_WRITE_CLOSE size");
	find.stream = msg->stream;
	if ((cf = RB_FIND(client_files, files, &find)) == NULL)
		fatalx("unknown stream number");
	log_debug("close file %d", cf->stream);

#ifdef TMUX_WIN32
	if (cf->win32_writer != NULL) {
		if (cf->write_pending == 0) {
			file_write_win32_close(cf);
			file_free(cf);
		} else
			cf->closed = 1;
		return;
	}
#endif
	if (cf->event == NULL || EVBUFFER_LENGTH(cf->event->output) == 0) {
		if (cf->event != NULL)
			bufferevent_free(cf->event);
		cf->event = NULL;
		if (cf->fd != -1)
			close(cf->fd);
		cf->fd = -1;
		file_free(cf);
	} else
		cf->closed = 1;
}

/* Client file read error callback. */
static void
file_read_error_callback(__unused struct bufferevent *bev, __unused short what,
    void *arg)
{
	struct client_file	*cf = arg;
	struct msg_read_done	 msg;

	log_debug("read error file %d", cf->stream);

	msg.stream = cf->stream;
	msg.error = 0;
	proc_send(cf->peer, MSG_READ_DONE, -1, &msg, sizeof msg);

	bufferevent_free(cf->event);
	close(cf->fd);
	RB_REMOVE(client_files, cf->tree, cf);
	file_free(cf);
}

/* Client file read callback. */
static void
file_read_callback(__unused struct bufferevent *bev, void *arg)
{
	struct client_file	*cf = arg;
	void			*bdata;
	size_t			 bsize;
	struct msg_read_data	*msg;
	size_t			 msglen;

	msg = xmalloc(sizeof *msg);
	for (;;) {
		bdata = EVBUFFER_DATA(cf->event->input);
		bsize = EVBUFFER_LENGTH(cf->event->input);

		if (bsize == 0)
			break;
		if (bsize > MAX_IMSGSIZE - IMSG_HEADER_SIZE - sizeof *msg)
			bsize = MAX_IMSGSIZE - IMSG_HEADER_SIZE - sizeof *msg;
		log_debug("read %zu from file %d", bsize, cf->stream);

		msglen = (sizeof *msg) + bsize;
		msg = xrealloc(msg, msglen);
		msg->stream = cf->stream;
		memcpy(msg + 1, bdata, bsize);
		proc_send(cf->peer, MSG_READ, -1, msg, msglen);

		evbuffer_drain(cf->event->input, bsize);
	}
	free(msg);
}

/* Handle a file read open message (client). */
void
file_read_open(struct client_files *files, struct tmuxpeer *peer,
    struct imsg *imsg, int allow_streams, int close_received, client_file_cb cb,
    void *cbdata)
{
	struct msg_read_open	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	const char		*path;
	struct msg_read_done	 reply;
	struct client_file	 find, *cf = NULL;
	const int		 flags = O_NONBLOCK|O_RDONLY;
	int			 error;

	if (msglen < sizeof *msg)
		fatalx("bad MSG_READ_OPEN size");
	if (msglen == sizeof *msg)
		path = "-";
	else
		path = (const char *)(msg + 1);
	log_debug("open read file %d %s", msg->stream, path);

	find.stream = msg->stream;
	if (RB_FIND(client_files, files, &find) != NULL) {
		error = EBADF;
		goto reply;
	}
	cf = file_create_with_peer(peer, files, msg->stream, cb, cbdata);
	if (cf->closed) {
		error = EBADF;
		goto reply;
	}

	cf->fd = -1;
#ifdef TMUX_WIN32
	if (msg->fd == -1) {
		if (file_read_win32_open_path(cf, path, flags) != 0) {
			error = errno;
			goto reply;
		}
	}
#else
	if (msg->fd == -1)
		cf->fd = open(path, flags);
#endif
	else if (allow_streams) {
		if (msg->fd != STDIN_FILENO)
			errno = EBADF;
		else {
			cf->fd = dup(msg->fd);
			if (close_received)
				close(msg->fd); /* can only be used once */
		}
	} else
		errno = EBADF;
	if (cf->fd == -1
#ifdef TMUX_WIN32
	    && cf->win32_handle == NULL
#endif
	    ) {
		error = errno;
		goto reply;
	}

#ifdef TMUX_WIN32
	if (msg->fd == -1) {
		if (file_read_win32_start(cf) != 0) {
			error = errno;
			goto reply;
		}
		return;
	}
#endif
	cf->event = bufferevent_new(cf->fd, file_read_callback, NULL,
	    file_read_error_callback, cf);
	if (cf->event == NULL)
		fatalx("out of memory");
	bufferevent_enable(cf->event, EV_READ);
	return;

reply:
	if (error != 0 && cf != NULL) {
#ifdef TMUX_WIN32
		if (cf->win32_reader != NULL) {
			win32_io_endpoint_free(cf->win32_reader);
			cf->win32_reader = NULL;
		}
		file_win32_close_handle(&cf->win32_handle);
#endif
		if (cf->event != NULL) {
			bufferevent_free(cf->event);
			cf->event = NULL;
		}
		if (cf->fd != -1)
			close(cf->fd);
		cf->fd = -1;
		file_free(cf);
	}
	reply.stream = msg->stream;
	reply.error = error;
	proc_send(peer, MSG_READ_DONE, -1, &reply, sizeof reply);
}

/* Handle a read cancel message (client). */
void
file_read_cancel(struct client_files *files, struct imsg *imsg)
{
	struct msg_read_cancel	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	struct client_file	 find, *cf;

	if (msglen != sizeof *msg)
		fatalx("bad MSG_READ_CANCEL size");
	find.stream = msg->stream;
	if ((cf = RB_FIND(client_files, files, &find)) == NULL)
		fatalx("unknown stream number");
	log_debug("cancel file %d", cf->stream);

#ifdef TMUX_WIN32
	if (cf->win32_reader != NULL) {
		file_read_win32_done_callback(cf);
		return;
	}
#endif
	file_read_error_callback(NULL, 0, cf);
}

/* Handle a write ready message (server). */
int
file_write_ready(struct client_files *files, struct imsg *imsg)
{
	struct msg_write_ready	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	struct client_file	 find, *cf;

	if (msglen != sizeof *msg)
		return (-1);
	find.stream = msg->stream;
	if ((cf = RB_FIND(client_files, files, &find)) == NULL)
		return (0);
	if (msg->error != 0) {
		cf->error = msg->error;
		file_fire_done(cf);
	} else
		file_push(cf);
	return (0);
}

/* Handle a write ack message (server). */
int
file_write_ack(struct client_files *files, struct imsg *imsg)
{
	struct msg_write_ack	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	struct client_file	 find, *cf;

	if (msglen != sizeof *msg)
		return (-1);
	find.stream = msg->stream;
	if ((cf = RB_FIND(client_files, files, &find)) == NULL)
		return (0);
	if (msg->size > cf->write_inflight)
		return (-1);

	cf->write_inflight -= msg->size;
	log_debug("file %d ack %u, inflight %zu", cf->stream, msg->size,
	    cf->write_inflight);
	if (msg->error != 0) {
		cf->error = msg->error;
		file_fire_done(cf);
	} else if (EVBUFFER_LENGTH(cf->buffer) != 0)
		file_push(cf);
	else
		file_push_close(cf);
	return (0);
}

/* Handle read data message (server). */
int
file_read_data(struct client_files *files, struct imsg *imsg)
{
	struct msg_read_data	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	struct client_file	 find, *cf;
	void			*bdata = msg + 1;
	size_t			 bsize = msglen - sizeof *msg;

	if (msglen < sizeof *msg)
		return (-1);
	find.stream = msg->stream;
	if ((cf = RB_FIND(client_files, files, &find)) == NULL)
		return (0);

	log_debug("file %d read %zu bytes", cf->stream, bsize);
	if (cf->error == 0 && !cf->closed) {
		if (evbuffer_add(cf->buffer, bdata, bsize) != 0) {
			cf->error = ENOMEM;
			file_fire_done(cf);
		} else
			file_fire_read(cf);
	}
	return (0);
}

/* Handle a read done message (server). */
int
file_read_done(struct client_files *files, struct imsg *imsg)
{
	struct msg_read_done	*msg = imsg->data;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;
	struct client_file	 find, *cf;

	if (msglen != sizeof *msg)
		return (-1);
	find.stream = msg->stream;
	if ((cf = RB_FIND(client_files, files, &find)) == NULL)
		return (0);

	log_debug("file %d read done", cf->stream);
	cf->error = msg->error;
	file_fire_done(cf);
	return (0);
}
