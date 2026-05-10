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

struct win32_pane {
	HPCON		 hpcon;
	HANDLE		 input_read;
	HANDLE		 input_write;
	HANDLE		 output_read;
	HANDLE		 output_write;
	HANDLE		 process;
	HANDLE		 thread;
	DWORD		 process_id;
	struct bufferevent *event;
	struct win32_handle_event *output_event;
	int		 exited;
	int		 status;
};

struct win32_job {
	HANDLE		 process;
	HANDLE		 thread;
	DWORD		 process_id;
	HANDLE		 stdin_read;
	HANDLE		 stdin_write;
	HANDLE		 stdout_read;
	HANDLE		 stdout_write;
	struct win32_handle_event *output_event;
	struct bufferevent *event;
	int		 status;
};

static int
win32_make_pipe(HANDLE *readp, HANDLE *writep, int inherit_read,
    int inherit_write)
{
	SECURITY_ATTRIBUTES sa;

	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.bInheritHandle = TRUE;
	if (!CreatePipe(readp, writep, &sa, 0))
		return (-1);
	if (!inherit_read && !SetHandleInformation(*readp, HANDLE_FLAG_INHERIT,
	    0))
		return (-1);
	if (!inherit_write && !SetHandleInformation(*writep, HANDLE_FLAG_INHERIT,
	    0))
		return (-1);
	return (0);
}

static wchar_t *
win32_build_job_command(const char *cmd, int argc, char **argv)
{
	char	*line = NULL;
	wchar_t	*wline;

	if (cmd != NULL)
		xasprintf(&line, "%s", cmd);
	else
		line = cmd_stringify_argv(argc, argv);
	wline = win32_utf8_to_wide(line);
	free(line);
	return (wline);
}

static wchar_t *
win32_build_command(struct window_pane *wp)
{
	const char	*shell, *cmd;
	char		*line;
	wchar_t		*wline;

	shell = wp->shell;
	if (shell == NULL || *shell == '\0')
		shell = "cmd.exe";
	if (wp->argc == 1) {
		cmd = wp->argv[0];
		xasprintf(&line, "\"%s\" /d /s /c \"%s\"", shell, cmd);
	} else
		xasprintf(&line, "\"%s\"", shell);
	wline = win32_utf8_to_wide(line);
	free(line);
	return (wline);
}

static void
win32_close_handle(HANDLE *h)
{
	if (*h != NULL && *h != INVALID_HANDLE_VALUE)
		CloseHandle(*h);
	*h = NULL;
}

static void
win32_pane_read_cb(void *arg)
{
	struct window_pane	*wp = arg;

	win32_pane_drain(wp);
}

void
win32_pane_drain(struct window_pane *wp)
{
	struct evbuffer		*dst;

	if (wp->win32 == NULL || wp->win32->output_event == NULL ||
	    wp->event == NULL)
		return;
	dst = wp->event->input;
	win32_handle_event_drain(wp->win32->output_event, dst);
	window_pane_read_callback(wp->event, wp);
}

static void
win32_pane_error_cb(void *arg)
{
	struct window_pane	*wp = arg;

	window_pane_error_callback(wp->event, 0, wp);
}

int
win32_pane_spawn(struct spawn_context *sc, struct window_pane *wp,
    __unused struct environ *env, const char *cwd, char **cause)
{
	struct win32_pane	*pw;
	STARTUPINFOEXW		 si;
	PROCESS_INFORMATION	 pi;
	SIZE_T			 attr_size = 0;
	COORD			 size;
	wchar_t			*wcmd = NULL, *wcwd = NULL;
	HRESULT			 hr;
	BOOL			 ok;

	memset(&si, 0, sizeof si);
	pw = xcalloc(1, sizeof *pw);
	size.X = screen_size_x(&wp->base);
	size.Y = screen_size_y(&wp->base);
	if (size.X <= 0)
		size.X = 80;
	if (size.Y <= 0)
		size.Y = 24;

	if (win32_make_pipe(&pw->input_read, &pw->input_write, 1, 0) != 0 ||
	    win32_make_pipe(&pw->output_read, &pw->output_write, 0, 1) != 0) {
		xasprintf(cause, "CreatePipe failed: %s",
		    win32_strerror(GetLastError()));
		goto fail;
	}
	hr = CreatePseudoConsole(size, pw->input_read, pw->output_write, 0,
	    &pw->hpcon);
	if (FAILED(hr)) {
		xasprintf(cause, "CreatePseudoConsole failed: 0x%08lx",
		    (unsigned long)hr);
		goto fail;
	}

	si.StartupInfo.cb = sizeof si;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
	si.lpAttributeList = xcalloc(1, attr_size);
	if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0,
	    &attr_size)) {
		xasprintf(cause, "InitializeProcThreadAttributeList failed: %s",
		    win32_strerror(GetLastError()));
		goto fail;
	}
	if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
	    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pw->hpcon, sizeof pw->hpcon,
	    NULL, NULL)) {
		xasprintf(cause, "UpdateProcThreadAttribute failed: %s",
		    win32_strerror(GetLastError()));
		goto fail;
	}

	wcmd = win32_build_command(wp);
	wcwd = win32_utf8_to_wide(cwd);
	memset(&pi, 0, sizeof pi);
	ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE,
	    EXTENDED_STARTUPINFO_PRESENT, NULL, wcwd, &si.StartupInfo, &pi);
	if (!ok) {
		xasprintf(cause, "CreateProcess failed: %s",
		    win32_strerror(GetLastError()));
		goto fail;
	}

	win32_close_handle(&pw->input_read);
	win32_close_handle(&pw->output_write);

	pw->process = pi.hProcess;
	pw->thread = pi.hThread;
	pw->process_id = pi.dwProcessId;
	wp->pid = (pid_t)pi.dwProcessId;
	wp->win32 = pw;

	pw->output_event = win32_handle_event_new(pw->output_read,
	    win32_pane_read_cb, win32_pane_error_cb, wp);
	if (pw->output_event == NULL) {
		xasprintf(cause, "couldn't create pane output event");
		goto fail;
	}

	DeleteProcThreadAttributeList(si.lpAttributeList);
	free(si.lpAttributeList);
	free(wcmd);
	free(wcwd);
	(void)sc;
	return (0);

fail:
	if (si.lpAttributeList != NULL) {
		DeleteProcThreadAttributeList(si.lpAttributeList);
		free(si.lpAttributeList);
	}
	free(wcmd);
	free(wcwd);
	if (pw != NULL) {
		if (wp->win32 == pw)
			wp->win32 = NULL;
		if (pw->output_event != NULL)
			win32_handle_event_free(pw->output_event);
		if (pw->process != NULL)
			TerminateProcess(pw->process, 1);
		if (pw->hpcon != NULL)
			ClosePseudoConsole(pw->hpcon);
		win32_close_handle(&pw->input_read);
		win32_close_handle(&pw->input_write);
		win32_close_handle(&pw->output_read);
		win32_close_handle(&pw->output_write);
		win32_close_handle(&pw->thread);
		win32_close_handle(&pw->process);
		free(pw);
	}
	return (-1);
}

void
win32_pane_resize(struct window_pane *wp, u_int sx, u_int sy)
{
	COORD	size;

	if (wp->win32 == NULL || wp->win32->hpcon == NULL)
		return;
	size.X = sx == 0 ? 80 : sx;
	size.Y = sy == 0 ? 24 : sy;
	ResizePseudoConsole(wp->win32->hpcon, size);
}

void
win32_pane_close(struct window_pane *wp)
{
	struct win32_pane *pw = wp->win32;

	if (pw == NULL)
		return;
	if (pw->event != NULL)
		bufferevent_free(pw->event);
	if (pw->output_event != NULL)
		win32_handle_event_free(pw->output_event);
	if (pw->hpcon != NULL)
		ClosePseudoConsole(pw->hpcon);
	win32_close_handle(&pw->input_read);
	win32_close_handle(&pw->input_write);
	win32_close_handle(&pw->output_read);
	win32_close_handle(&pw->output_write);
	win32_close_handle(&pw->thread);
	win32_close_handle(&pw->process);
	free(pw);
	wp->win32 = NULL;
}

int
win32_pane_exited(struct window_pane *wp, int *status)
{
	DWORD	code;

	if (wp->win32 == NULL || wp->win32->process == NULL)
		return (0);
	if (WaitForSingleObject(wp->win32->process, 0) != WAIT_OBJECT_0)
		return (0);
	if (!GetExitCodeProcess(wp->win32->process, &code))
		code = 1;
	wp->win32->exited = 1;
	wp->win32->status = W_EXITCODE((int)code, 0);
	if (status != NULL)
		*status = wp->win32->status;
	return (1);
}

size_t
win32_pane_buffered(struct window_pane *wp)
{
	if (wp->win32 == NULL || wp->win32->output_event == NULL)
		return (0);
	return (win32_handle_event_buffered(wp->win32->output_event));
}

int
win32_pane_write(struct window_pane *wp, const void *data, size_t size)
{
	DWORD	written;

	if (wp->win32 == NULL || wp->win32->input_write == NULL) {
		errno = EIO;
		return (-1);
	}
	if (!WriteFile(wp->win32->input_write, data, size, &written, NULL)) {
		errno = EIO;
		return (-1);
	}
	return ((int)written);
}

struct bufferevent *
win32_pane_get_event(__unused struct window_pane *wp)
{
	return (NULL);
}

static void
win32_job_read_cb(void *arg)
{
	struct win32_job	*wj = arg;

	if (wj->event == NULL || wj->output_event == NULL)
		return;
	win32_handle_event_drain(wj->output_event, wj->event->input);
	if (wj->event->readcb != NULL)
		wj->event->readcb(wj->event, wj->event->cbarg);
}

static void
win32_job_error_cb(void *arg)
{
	struct win32_job	*wj = arg;
	DWORD		 code;

	if (wj->process != NULL && GetExitCodeProcess(wj->process, &code))
		wj->status = W_EXITCODE((int)code, 0);
	if (wj->event != NULL && wj->event->errorcb != NULL)
		wj->event->errorcb(wj->event, 0, wj->event->cbarg);
}

struct win32_job *
win32_job_spawn(const char *cmd, int argc, char **argv,
    __unused struct environ *env, __unused struct session *s, const char *cwd,
    int flags, __unused int sx, __unused int sy, char **cause)
{
	struct win32_job		*wj;
	STARTUPINFOW		 si;
	PROCESS_INFORMATION	 pi;
	wchar_t			*wcmd = NULL, *wcwd = NULL;
	BOOL			 ok;

	if (flags & JOB_PTY) {
		if (cause != NULL) {
			xasprintf(cause,
			    "PTY jobs are not supported in the native Windows MVP");
		}
		errno = ENOSYS;
		return (NULL);
	}

	wj = xcalloc(1, sizeof *wj);
	if (win32_make_pipe(&wj->stdin_read, &wj->stdin_write, 1, 0) != 0 ||
	    win32_make_pipe(&wj->stdout_read, &wj->stdout_write, 0, 1) != 0) {
		if (cause != NULL) {
			xasprintf(cause, "CreatePipe failed: %s",
			    win32_strerror(GetLastError()));
		}
		goto fail;
	}

	memset(&si, 0, sizeof si);
	memset(&pi, 0, sizeof pi);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = wj->stdin_read;
	si.hStdOutput = wj->stdout_write;
	si.hStdError = (flags & JOB_SHOWSTDERR) ?
	    wj->stdout_write : GetStdHandle(STD_ERROR_HANDLE);

	wcmd = win32_build_job_command(cmd, argc, argv);
	if (cwd != NULL)
		wcwd = win32_utf8_to_wide(cwd);
	ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, wcwd, &si,
	    &pi);
	if (!ok) {
		if (cause != NULL) {
			xasprintf(cause, "CreateProcess job failed: %s",
			    win32_strerror(GetLastError()));
		}
		goto fail;
	}

	wj->process = pi.hProcess;
	wj->thread = pi.hThread;
	wj->process_id = pi.dwProcessId;
	win32_close_handle(&wj->stdin_read);
	win32_close_handle(&wj->stdout_write);

	wj->event = bufferevent_new(-1, NULL, NULL, NULL, NULL);
	if (wj->event == NULL)
		fatalx("out of memory");
	wj->output_event = win32_handle_event_new(wj->stdout_read,
	    win32_job_read_cb, win32_job_error_cb, wj);
	if (wj->output_event == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't create job output event");
		goto fail;
	}

	free(wcmd);
	free(wcwd);
	return (wj);

fail:
	free(wcmd);
	free(wcwd);
	if (wj != NULL) {
		if (wj->output_event != NULL)
			win32_handle_event_free(wj->output_event);
		if (wj->event != NULL)
			bufferevent_free(wj->event);
		if (wj->process != NULL)
			TerminateProcess(wj->process, 1);
		win32_close_handle(&wj->stdin_read);
		win32_close_handle(&wj->stdin_write);
		win32_close_handle(&wj->stdout_read);
		win32_close_handle(&wj->stdout_write);
		win32_close_handle(&wj->thread);
		win32_close_handle(&wj->process);
		free(wj);
	}
	return (NULL);
}

void
win32_job_close(struct win32_job *wj)
{
	if (wj == NULL)
		return;
	if (wj->process != NULL)
		TerminateProcess(wj->process, 1);
	if (wj->output_event != NULL)
		win32_handle_event_free(wj->output_event);
	wj->event = NULL;
	win32_close_handle(&wj->stdin_read);
	win32_close_handle(&wj->stdin_write);
	win32_close_handle(&wj->stdout_read);
	win32_close_handle(&wj->stdout_write);
	win32_close_handle(&wj->thread);
	win32_close_handle(&wj->process);
	free(wj);
}

void
win32_job_resize(__unused struct win32_job *wj, __unused u_int sx,
    __unused u_int sy)
{
}

int
win32_job_get_pid(struct win32_job *wj, pid_t *pid)
{
	if (pid != NULL)
		*pid = (pid_t)wj->process_id;
	return (0);
}

int
win32_job_get_status(struct win32_job *wj)
{
	DWORD	code;

	if (wj->process != NULL && GetExitCodeProcess(wj->process, &code))
		wj->status = W_EXITCODE((int)code, 0);
	return (wj->status);
}

struct bufferevent *
win32_job_get_event(struct win32_job *wj)
{
	return (wj->event);
}

#endif /* TMUX_WIN32 */
