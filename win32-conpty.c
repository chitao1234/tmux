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
win32_quote_argument(const char *arg)
{
	const char	*src;
	char	*out, *dst;
	size_t	 len, bs;
	int	 quote;
	wchar_t	*wout;

	quote = (*arg == '\0' || strpbrk(arg, " \t\n\v\"") != NULL);
	if (!quote)
		return (win32_utf8_to_wide(arg));

	len = 3;
	for (src = arg; *src != '\0'; src++) {
		if (*src == '\\' || *src == '"')
			len += 2;
		else
			len++;
	}
	out = xmalloc(len);
	dst = out;
	*dst++ = '"';
	for (;;) {
		bs = 0;
		while (*arg == '\\') {
			bs++;
			arg++;
		}
		if (*arg == '\0') {
			while (bs-- != 0) {
				*dst++ = '\\';
				*dst++ = '\\';
			}
			break;
		}
		if (*arg == '"') {
			while (bs-- != 0) {
				*dst++ = '\\';
				*dst++ = '\\';
			}
			*dst++ = '\\';
			*dst++ = *arg++;
			continue;
		}
		while (bs-- != 0)
			*dst++ = '\\';
		*dst++ = *arg++;
	}
	*dst++ = '"';
	*dst = '\0';

	wout = win32_utf8_to_wide(out);
	free(out);
	return (wout);
}

static wchar_t *
win32_build_argv_command(int argc, char **argv)
{
	wchar_t	*line = NULL, *quoted;
	size_t	 used = 0, len = 1, qlen;
	int	 i;

	line = xcalloc(len, sizeof *line);
	for (i = 0; i < argc; i++) {
		quoted = win32_quote_argument(argv[i]);
		if (quoted == NULL)
			continue;
		qlen = wcslen(quoted);
		len = used + qlen + 2;
		line = xreallocarray(line, len, sizeof *line);
		if (used != 0)
			line[used++] = L' ';
		memcpy(line + used, quoted, (qlen + 1) * sizeof *line);
		used += qlen;
		free(quoted);
	}
	return (line);
}

static wchar_t *
win32_build_environment(struct environ *env)
{
	struct environ_entry	*envent;
	size_t			 len, total = 1, used = 0;
	char			*entry;
	wchar_t			*wentry, *block;

	if (env == NULL)
		return (NULL);
	for (envent = environ_first(env); envent != NULL;
	    envent = environ_next(envent)) {
		if (envent->value == NULL || *envent->name == '\0' ||
		    (envent->flags & ENVIRON_HIDDEN))
			continue;
		xasprintf(&entry, "%s=%s", envent->name, envent->value);
		wentry = win32_utf8_to_wide(entry);
		free(entry);
		if (wentry == NULL)
			continue;
		total += wcslen(wentry) + 1;
		free(wentry);
	}

	block = xcalloc(total, sizeof *block);
	for (envent = environ_first(env); envent != NULL;
	    envent = environ_next(envent)) {
		if (envent->value == NULL || *envent->name == '\0' ||
		    (envent->flags & ENVIRON_HIDDEN))
			continue;
		xasprintf(&entry, "%s=%s", envent->name, envent->value);
		wentry = win32_utf8_to_wide(entry);
		free(entry);
		if (wentry == NULL)
			continue;
		len = wcslen(wentry) + 1;
		memcpy(block + used, wentry, len * sizeof *block);
		used += len;
		free(wentry);
	}
	return (block);
}

static wchar_t *
win32_build_shell_command(const char *shell, const char *cmd)
{
	char	*line = NULL;
	wchar_t	*wline;

	if (shell == NULL || *shell == '\0')
		shell = "cmd.exe";
	if (cmd != NULL)
		xasprintf(&line, "\"%s\" /d /s /c \"%s\"", shell, cmd);
	else
		xasprintf(&line, "\"%s\"", shell);
	wline = win32_utf8_to_wide(line);
	free(line);
	return (wline);
}

static wchar_t *
win32_build_job_command(const char *cmd, const char *shell, int argc,
    char **argv)
{
	if (cmd != NULL)
		return (win32_build_shell_command(shell, cmd));
	return (win32_build_argv_command(argc, argv));
}

static wchar_t *
win32_build_command(struct window_pane *wp)
{
	const char	*shell, *cmd;

	shell = wp->shell;
	if (wp->argc == 1) {
		cmd = wp->argv[0];
		return (win32_build_shell_command(shell, cmd));
	} else if (wp->argc > 1)
		return (win32_build_argv_command(wp->argc, wp->argv));
	return (win32_build_shell_command(shell, NULL));
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
	if (wp->win32 == NULL || wp->win32->output_event == NULL ||
	    wp->event == NULL)
		return;
	win32_handle_event_drain_bev(wp->win32->output_event, wp->event);
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
    struct environ *env, const char *cwd, char **cause)
{
	struct win32_pane	*pw;
	STARTUPINFOEXW		 si;
	PROCESS_INFORMATION	 pi;
	SIZE_T			 attr_size = 0;
	COORD			 size;
	wchar_t			*wcmd = NULL, *wcwd = NULL, *wenv = NULL;
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
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
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
	wenv = win32_build_environment(env);
	memset(&pi, 0, sizeof pi);
	ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE,
	    EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT,
	    wenv, wcwd, &si.StartupInfo, &pi);
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
	free(wenv);
	(void)sc;
	return (0);

fail:
	if (si.lpAttributeList != NULL) {
		DeleteProcThreadAttributeList(si.lpAttributeList);
		free(si.lpAttributeList);
	}
	free(wcmd);
	free(wcwd);
	free(wenv);
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
	if (wp->event != NULL) {
		bufferevent_free(wp->event);
		wp->event = NULL;
	}
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
	win32_handle_event_drain_bev(wj->output_event, wj->event);
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
win32_job_spawn(const char *cmd, const char *shell, int argc, char **argv,
    struct environ *env, __unused struct session *s, const char *cwd,
    int flags, __unused int sx, __unused int sy, char **cause)
{
	struct win32_job		*wj;
	STARTUPINFOW		 si;
	PROCESS_INFORMATION	 pi;
	wchar_t			*wcmd = NULL, *wcwd = NULL, *wenv = NULL;
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

	wcmd = win32_build_job_command(cmd, shell, argc, argv);
	if (cwd != NULL)
		wcwd = win32_utf8_to_wide(cwd);
	wenv = win32_build_environment(env);
	ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE,
	    CREATE_UNICODE_ENVIRONMENT, wenv, wcwd, &si, &pi);
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
	free(wenv);
	return (wj);

fail:
	free(wcmd);
	free(wcwd);
	free(wenv);
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

int
win32_job_write(struct win32_job *wj, const void *data, size_t size)
{
	const char	*buf = data;
	size_t		 left = size;
	DWORD		 written;

	while (left != 0) {
		if (wj->stdin_write == NULL) {
			errno = EPIPE;
			return (-1);
		}
		if (!WriteFile(wj->stdin_write, buf,
		    left > MAXDWORD ? MAXDWORD : left, &written, NULL)) {
			errno = EIO;
			return (-1);
		}
		if (written == 0)
			break;
		buf += written;
		left -= written;
	}
	return ((int)(size - left));
}

void
win32_job_close_stdin(struct win32_job *wj)
{
	win32_close_handle(&wj->stdin_write);
}

#endif /* TMUX_WIN32 */
