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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

#ifdef TMUX_WIN32
#define LOG_WIN32_QUEUE_LIMIT (1024 * 1024)
#define LOG_WIN32_ITEM_LIMIT (64 * 1024)
#define LOG_WIN32_STOP_TIMEOUT 1000

struct win32_diagnostic_item {
	struct win32_diagnostic_item	*next;
	size_t				 size;
	char				 data[];
};

struct win32_diagnostic_file {
	HANDLE				 file;
	HANDLE				 thread;
	CRITICAL_SECTION		 lock;
	CONDITION_VARIABLE		 cond;
	struct win32_diagnostic_item	*head;
	struct win32_diagnostic_item	*tail;
	size_t				 queued;
	int				 stopping;
};

static INIT_ONCE			 log_win32_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION		 log_win32_lock;
static struct win32_diagnostic_file	*log_win32_file;

static BOOL CALLBACK log_win32_once_cb(PINIT_ONCE, PVOID, PVOID *);
static void	 log_win32_lock_current(void);
static int	 log_win32_is_open(void);
static void	 log_win32_set_current(struct win32_diagnostic_file *);
static struct win32_diagnostic_file *log_win32_clear_current(void);
static DWORD WINAPI log_win32_diagnostic_thread(void *);
static void	 log_win32_diagnostic_write_chunk(
		     struct win32_diagnostic_file *, const void *, size_t);
static void	 log_win32_diagnostic_write_all(HANDLE, const char *,
		     size_t);
#else
static FILE	*log_file;
#endif
static int	 log_level;

/* Log callback for libevent. */
static void
log_event_cb(__unused int severity, const char *msg)
{
	log_debug("%s", msg);
}

/* Increment log level. */
void
log_add_level(void)
{
	log_level++;
}

/* Get log level. */
int
log_get_level(void)
{
	return (log_level);
}

#ifdef TMUX_WIN32
static BOOL CALLBACK
log_win32_once_cb(__unused PINIT_ONCE once, __unused PVOID param,
    __unused PVOID *context)
{
	InitializeCriticalSection(&log_win32_lock);
	return (TRUE);
}

static void
log_win32_lock_current(void)
{
	InitOnceExecuteOnce(&log_win32_once, log_win32_once_cb, NULL, NULL);
	EnterCriticalSection(&log_win32_lock);
}

static int
log_win32_is_open(void)
{
	int	open;

	log_win32_lock_current();
	open = (log_win32_file != NULL);
	LeaveCriticalSection(&log_win32_lock);
	return (open);
}

static void
log_win32_set_current(struct win32_diagnostic_file *file)
{
	log_win32_lock_current();
	log_win32_file = file;
	LeaveCriticalSection(&log_win32_lock);
}

static struct win32_diagnostic_file *
log_win32_clear_current(void)
{
	struct win32_diagnostic_file	*file;

	log_win32_lock_current();
	file = log_win32_file;
	log_win32_file = NULL;
	LeaveCriticalSection(&log_win32_lock);
	return (file);
}

static void
log_win32_diagnostic_write_all(HANDLE file, const char *data, size_t size)
{
	DWORD	written, nwrite;

	while (size != 0) {
		nwrite = (DWORD)size;
		if (!WriteFile(file, data, nwrite, &written, NULL))
			return;
		if (written == 0)
			return;
		data += written;
		size -= written;
	}
}

static DWORD WINAPI
log_win32_diagnostic_thread(void *arg)
{
	struct win32_diagnostic_file	*file = arg;
	struct win32_diagnostic_item	*item;

	for (;;) {
		EnterCriticalSection(&file->lock);
		while (file->head == NULL && !file->stopping) {
			SleepConditionVariableCS(&file->cond, &file->lock,
			    INFINITE);
		}
		item = file->head;
		if (item == NULL && file->stopping) {
			LeaveCriticalSection(&file->lock);
			break;
		}
		file->head = item->next;
		if (file->head == NULL)
			file->tail = NULL;
		file->queued -= item->size;
		LeaveCriticalSection(&file->lock);

		log_win32_diagnostic_write_all(file->file, item->data,
		    item->size);
		free(item);
	}

	CloseHandle(file->file);
	file->file = NULL;
	return (0);
}

struct win32_diagnostic_file *
log_win32_diagnostic_open(const char *path, int append)
{
	struct win32_diagnostic_file	*file;
	wchar_t				*wpath;
	HANDLE				 handle;
	DWORD				 access, disposition;

	wpath = win32_utf8_to_wide(path);
	if (wpath == NULL)
		return (NULL);
	access = append ? FILE_APPEND_DATA : GENERIC_WRITE;
	disposition = append ? OPEN_ALWAYS : CREATE_ALWAYS;
	handle = CreateFileW(wpath, access,
	    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL,
	    disposition, FILE_ATTRIBUTE_NORMAL, NULL);
	free(wpath);
	if (handle == INVALID_HANDLE_VALUE)
		return (NULL);

	file = calloc(1, sizeof *file);
	if (file == NULL) {
		CloseHandle(handle);
		return (NULL);
	}
	file->file = handle;
	InitializeCriticalSection(&file->lock);
	InitializeConditionVariable(&file->cond);
	file->thread = CreateThread(NULL, 0, log_win32_diagnostic_thread,
	    file, 0, NULL);
	if (file->thread == NULL) {
		DeleteCriticalSection(&file->lock);
		CloseHandle(handle);
		free(file);
		return (NULL);
	}
	return (file);
}

void
log_win32_diagnostic_close(struct win32_diagnostic_file *file)
{
	DWORD	wait;

	if (file == NULL)
		return;

	EnterCriticalSection(&file->lock);
	file->stopping = 1;
	WakeAllConditionVariable(&file->cond);
	LeaveCriticalSection(&file->lock);

	wait = WaitForSingleObject(file->thread, LOG_WIN32_STOP_TIMEOUT);
	if (wait != WAIT_OBJECT_0) {
		CancelSynchronousIo(file->thread);
		wait = WaitForSingleObject(file->thread,
		    LOG_WIN32_STOP_TIMEOUT);
	}
	if (wait != WAIT_OBJECT_0) {
		/*
		 * Leave the writer state allocated: the thread may still own it
		 * after a blocked diagnostic write.
		 */
		CloseHandle(file->thread);
		return;
	}

	CloseHandle(file->thread);
	DeleteCriticalSection(&file->lock);
	free(file);
}

static void
log_win32_diagnostic_write_chunk(struct win32_diagnostic_file *file,
    const void *data, size_t size)
{
	struct win32_diagnostic_item	*item;

	if (file == NULL || size == 0)
		return;
	item = malloc(sizeof *item + size);
	if (item == NULL)
		return;
	memcpy(item->data, data, size);
	item->size = size;
	item->next = NULL;

	EnterCriticalSection(&file->lock);
	if (file->stopping || size > LOG_WIN32_QUEUE_LIMIT ||
	    file->queued > LOG_WIN32_QUEUE_LIMIT - size) {
		LeaveCriticalSection(&file->lock);
		free(item);
		return;
	}
	if (file->tail == NULL)
		file->head = item;
	else
		file->tail->next = item;
	file->tail = item;
	file->queued += size;
	WakeConditionVariable(&file->cond);
	LeaveCriticalSection(&file->lock);
}

void
log_win32_diagnostic_write(struct win32_diagnostic_file *file,
    const void *data, size_t size)
{
	const char	*ptr = data;
	size_t		 nwrite;

	while (size != 0) {
		nwrite = size;
		if (nwrite > LOG_WIN32_ITEM_LIMIT)
			nwrite = LOG_WIN32_ITEM_LIMIT;
		log_win32_diagnostic_write_chunk(file, ptr, nwrite);
		ptr += nwrite;
		size -= nwrite;
	}
}
#endif

/* Open logging to file. */
void
log_open(const char *name)
{
	char	*path;

	if (log_level == 0)
		return;
	log_close();

	xasprintf(&path, "tmux-%s-%ld.log", name, (long)getpid());
#ifdef TMUX_WIN32
	log_win32_set_current(log_win32_diagnostic_open(path, 1));
	free(path);
	if (!log_win32_is_open())
		return;
#else
	log_file = fopen(path, "a");
	free(path);
	if (log_file == NULL)
		return;
	setvbuf(log_file, NULL, _IOLBF, 0);
#endif
	event_set_log_callback(log_event_cb);
}

/* Toggle logging. */
void
log_toggle(const char *name)
{
	if (log_level == 0) {
		log_level = 1;
		log_open(name);
		log_debug("log opened");
	} else {
		log_debug("log closed");
		log_level = 0;
		log_close();
	}
}

/* Close logging. */
void
log_close(void)
{
#ifdef TMUX_WIN32
	log_win32_diagnostic_close(log_win32_clear_current());
#else
	if (log_file != NULL)
		fclose(log_file);
	log_file = NULL;
#endif

	event_set_log_callback(NULL);
}

/* Write a log message. */
static void printflike(1, 0)
log_vwrite(const char *msg, va_list ap, const char *prefix)
{
	char		*s, *out;
	struct timeval	 tv;
#ifdef TMUX_WIN32
	char		*line;
#endif

#ifdef TMUX_WIN32
	if (!log_win32_is_open())
		return;
#else
	if (log_file == NULL)
		return;
#endif

	if (vasprintf(&s, msg, ap) == -1)
		return;
	if (stravis(&out, s, VIS_OCTAL|VIS_CSTYLE|VIS_TAB|VIS_NL) == -1) {
		free(s);
		return;
	}
	free(s);

	gettimeofday(&tv, NULL);
#ifdef TMUX_WIN32
	if (asprintf(&line, "%lld.%06d %s%s\n", (long long)tv.tv_sec,
	    (int)tv.tv_usec, prefix, out) != -1) {
		log_win32_lock_current();
		log_win32_diagnostic_write(log_win32_file, line, strlen(line));
		LeaveCriticalSection(&log_win32_lock);
		free(line);
	}
#else
	if (fprintf(log_file, "%lld.%06d %s%s\n", (long long)tv.tv_sec,
	    (int)tv.tv_usec, prefix, out) != -1)
		fflush(log_file);
#endif
	free(out);
}

/* Log a debug message. */
void
log_debug(const char *msg, ...)
{
	va_list	ap;

#ifdef TMUX_WIN32
	if (!log_win32_is_open())
		return;
#else
	if (log_file == NULL)
		return;
#endif

	va_start(ap, msg);
	log_vwrite(msg, ap, "");
	va_end(ap);
}

/* Log a critical error with error string and die. */
__dead void
fatal(const char *msg, ...)
{
	char	 tmp[256];
	va_list	 ap;

	if (snprintf(tmp, sizeof tmp, "fatal: %s: ", strerror(errno)) < 0)
		exit(1);

	va_start(ap, msg);
	log_vwrite(msg, ap, tmp);
	va_end(ap);

#ifdef TMUX_WIN32
	log_close();
#endif
	exit(1);
}

/* Log a critical error and die. */
__dead void
fatalx(const char *msg, ...)
{
	va_list	 ap;

	va_start(ap, msg);
	log_vwrite(msg, ap, "fatal: ");
	va_end(ap);

#ifdef TMUX_WIN32
	log_close();
#endif
	exit(1);
}
