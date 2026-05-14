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

#include <winternl.h>

#ifdef TMUX_WIN32

struct win32_pane {
	HPCON		 hpcon;
	HANDLE		 input_read;
	HANDLE		 input_write;
	HANDLE		 output_read;
	HANDLE		 output_write;
	HANDLE		 job;
	HANDLE		 process;
	HANDLE		 thread;
	DWORD		 process_id;
	struct bufferevent *event;
	struct win32_io_endpoint *output_event;
	struct win32_io_endpoint *input_writer;
	struct win32_io_endpoint *process_event;
	struct evbuffer	*input_queue;
	int		 output_paused;
	int		 exited;
	int		 status;
};

struct win32_job {
	HANDLE		 job;
	HANDLE		 process;
	HANDLE		 thread;
	DWORD		 process_id;
	HPCON		 hpcon;
	HANDLE		 stdin_read;
	HANDLE		 stdin_write;
	HANDLE		 stdout_read;
	HANDLE		 stdout_write;
	HANDLE		 stderr_write;
	struct win32_io_endpoint *output_event;
	struct win32_io_endpoint *stdin_writer;
	struct win32_io_endpoint *process_event;
	struct bufferevent *event;
	void		(*exitcb)(void *);
	void		 *exitarg;
	int		 exit_pending;
	int		 stdin_closing;
	int		 pty;
	int		 exited;
	int		 status;
};

struct win32_curdir {
	UNICODE_STRING	 DosPath;
	HANDLE		 Handle;
};

struct win32_process_parameters {
	ULONG			 MaximumLength;
	ULONG			 Length;
	ULONG			 Flags;
	ULONG			 DebugFlags;
	HANDLE			 ConsoleHandle;
	ULONG			 ConsoleFlags;
	HANDLE			 StandardInput;
	HANDLE			 StandardOutput;
	HANDLE			 StandardError;
	struct win32_curdir	 CurrentDirectory;
	UNICODE_STRING		 DllPath;
	UNICODE_STRING		 ImagePathName;
	UNICODE_STRING		 CommandLine;
};

struct win32_job_process {
	DWORD		 pid;
	DWORD		 ppid;
	HANDLE		 process;
	uint64_t	 created;
	u_int		 depth;
	int		 has_child;
	int		 close_process;
};

#define WIN32_CHILD_KILL_TIMEOUT 1000
#define WIN32_INPUT_WRITER_HIGH (1024 * 1024)
#define WIN32_INPUT_WRITER_CHUNK (256 * 1024)

typedef NTSTATUS (NTAPI *win32_nt_query_information_process)(HANDLE,
    PROCESSINFOCLASS, PVOID, ULONG, PULONG);

static int
win32_make_pipe_flags(HANDLE *readp, HANDLE *writep, int inherit_read,
    int inherit_write, DWORD read_flags, DWORD write_flags)
{
	static LONG	    serial;
	SECURITY_ATTRIBUTES rsa, wsa;
	OVERLAPPED	    ov;
	HANDLE		    event = NULL, read = INVALID_HANDLE_VALUE;
	HANDLE		    write = INVALID_HANDLE_VALUE;
	DWORD		    open_flags;
	wchar_t		    name[128];
	DWORD		    error, n;
	int		    pending = 0, connected = 0;

	*readp = NULL;
	*writep = NULL;

	if (swprintf(name, nitems(name), L"\\\\.\\pipe\\tmux-%lu-%ld-%llx",
	    (unsigned long)GetCurrentProcessId(),
	    (long)InterlockedIncrement(&serial),
	    (unsigned long long)GetTickCount64()) < 0) {
		SetLastError(ERROR_FILENAME_EXCED_RANGE);
		return (-1);
	}

	memset(&rsa, 0, sizeof rsa);
	rsa.nLength = sizeof rsa;
	rsa.bInheritHandle = inherit_read;
	memset(&wsa, 0, sizeof wsa);
	wsa.nLength = sizeof wsa;
	wsa.bInheritHandle = inherit_write;

	read = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND|read_flags,
	    PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT, 1, 0, 0, 0, &rsa);
	if (read == INVALID_HANDLE_VALUE)
		return (-1);

	if (read_flags & FILE_FLAG_OVERLAPPED) {
		memset(&ov, 0, sizeof ov);
		event = CreateEventW(NULL, TRUE, FALSE, NULL);
		if (event == NULL)
			goto fail;
		ov.hEvent = event;
		if (ConnectNamedPipe(read, &ov))
			connected = 1;
		else {
			error = GetLastError();
			if (error == ERROR_IO_PENDING)
				pending = 1;
			else if (error == ERROR_PIPE_CONNECTED)
				connected = 1;
			else
				goto fail;
		}
	}

	open_flags = FILE_ATTRIBUTE_NORMAL|write_flags;
	if (open_flags & FILE_FLAG_OVERLAPPED)
		open_flags &= ~FILE_ATTRIBUTE_NORMAL;
	write = CreateFileW(name, GENERIC_WRITE, 0, &wsa, OPEN_EXISTING,
	    open_flags, NULL);
	if (write == INVALID_HANDLE_VALUE)
		goto fail;

	if (read_flags & FILE_FLAG_OVERLAPPED) {
		if (pending &&
		    !GetOverlappedResult(read, &ov, &n, TRUE))
			goto fail;
	} else if (!ConnectNamedPipe(read, NULL)) {
		error = GetLastError();
		if (error != ERROR_PIPE_CONNECTED)
			goto fail;
		connected = 1;
	} else
		connected = 1;

	if (!connected && !pending) {
		SetLastError(ERROR_PIPE_NOT_CONNECTED);
		goto fail;
	}
	if (!inherit_read &&
	    !SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0))
		goto fail;
	if (!inherit_write &&
	    !SetHandleInformation(write, HANDLE_FLAG_INHERIT, 0))
		goto fail;
	*readp = read;
	*writep = write;
	if (event != NULL)
		CloseHandle(event);
	return (0);

fail:
	error = GetLastError();
	if (event != NULL)
		CloseHandle(event);
	if (read != INVALID_HANDLE_VALUE)
		CloseHandle(read);
	if (write != INVALID_HANDLE_VALUE)
		CloseHandle(write);
	SetLastError(error);
	return (-1);
}

static int
win32_make_pipe(HANDLE *readp, HANDLE *writep, int inherit_read,
    int inherit_write)
{
	return (win32_make_pipe_flags(readp, writep, inherit_read,
	    inherit_write, 0, 0));
}

static int
win32_make_output_pipe(HANDLE *readp, HANDLE *writep)
{
	return (win32_make_pipe_flags(readp, writep, 0, 1,
	    FILE_FLAG_OVERLAPPED, 0));
}

static int
win32_make_input_pipe(HANDLE *readp, HANDLE *writep)
{
	return (win32_make_pipe_flags(readp, writep, 1, 0, 0,
	    FILE_FLAG_OVERLAPPED));
}

static win32_nt_query_information_process
win32_get_nt_query_information_process(void)
{
	static win32_nt_query_information_process	 fn;
	static int					 loaded;
	HMODULE						 ntdll;
	union {
		FARPROC					 proc;
		win32_nt_query_information_process	 fn;
	}						 cast;

	if (loaded)
		return (fn);
	loaded = 1;

	ntdll = GetModuleHandleW(L"ntdll.dll");
	if (ntdll == NULL)
		return (NULL);
	cast.proc = GetProcAddress(ntdll, "NtQueryInformationProcess");
	if (cast.proc == NULL)
		return (NULL);
	fn = cast.fn;
	return (fn);
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

wchar_t *
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

static const char *
win32_path_basename(const char *path)
{
	const char	*slash, *backslash;

	if (path == NULL)
		return ("");
	slash = strrchr(path, '/');
	backslash = strrchr(path, '\\');
	if (slash == NULL || backslash > slash)
		slash = backslash;
	if (slash != NULL && slash[1] != '\0')
		return (slash + 1);
	return (path);
}

static int
win32_shell_is_cmd(const char *shell)
{
	const char	*name = win32_path_basename(shell);

	return (strcasecmp(name, "cmd.exe") == 0 ||
	    strcasecmp(name, "cmd") == 0);
}

static int
win32_validate_cwd(const char *cwd, char **cause)
{
	if (cwd == NULL || cwd[0] != '/')
		return (0);
	if (cause != NULL) {
		xasprintf(cause, "working directory must be a Windows path on "
		    "Win32: %s", cwd);
	}
	errno = EINVAL;
	return (-1);
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
	char	*argv[3];

	if (shell == NULL || *shell == '\0')
		shell = "cmd.exe";
	if (win32_shell_is_cmd(shell)) {
		if (cmd == NULL)
			xasprintf(&line, "\"%s\"", shell);
		else
			xasprintf(&line, "\"%s\" /d /s /c \"%s\"", shell, cmd);
		wline = win32_utf8_to_wide(line);
		free(line);
		return (wline);
	}
	argv[0] = (char *)shell;
	if (cmd != NULL) {
		argv[1] = (char *)"-c";
		argv[2] = (char *)cmd;
		return (win32_build_argv_command(3, argv));
	}
	return (win32_build_argv_command(1, argv));
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
win32_close_pseudoconsole(HPCON *hpcon)
{
	if (*hpcon != NULL)
		ClosePseudoConsole(*hpcon);
	*hpcon = NULL;
}

static int
win32_process_read(HANDLE process, const void *base, void *buf, size_t size)
{
	SIZE_T	nread = 0;

	if (size == 0)
		return (1);
	if (!ReadProcessMemory(process, base, buf, size, &nread))
		return (0);
	return (nread == size);
}

static int
win32_process_get_basic_information(HANDLE process,
    PROCESS_BASIC_INFORMATION *pbi)
{
	win32_nt_query_information_process	 query_information_process;
	ULONG					 len = 0;
	NTSTATUS				 status;

	if (process == NULL)
		return (0);
	query_information_process = win32_get_nt_query_information_process();
	if (query_information_process == NULL)
		return (0);
	status = query_information_process(process, ProcessBasicInformation,
	    pbi, sizeof *pbi, &len);
	return (NT_SUCCESS(status));
}

static uint64_t
win32_process_get_create_time(HANDLE process)
{
	FILETIME	 create, exit, kernel, user;
	ULARGE_INTEGER when;

	if (!GetProcessTimes(process, &create, &exit, &kernel, &user))
		return (0);
	when.LowPart = create.dwLowDateTime;
	when.HighPart = create.dwHighDateTime;
	return (when.QuadPart);
}

static char *
win32_process_get_cwd(HANDLE process)
{
	struct win32_process_parameters	 params;
	PROCESS_BASIC_INFORMATION	 pbi;
	wchar_t				*wpath;
	SIZE_T				 size;
	PEB				 peb;
	char				*cwd;

	if (process == NULL)
		return (NULL);
	if (!win32_process_get_basic_information(process, &pbi) ||
	    pbi.PebBaseAddress == NULL)
		return (NULL);
	if (!win32_process_read(process, pbi.PebBaseAddress, &peb, sizeof peb))
		return (NULL);
	if (peb.ProcessParameters == NULL)
		return (NULL);
	if (!win32_process_read(process, peb.ProcessParameters, &params,
	    sizeof params))
		return (NULL);
	if (params.CurrentDirectory.DosPath.Length == 0 ||
	    params.CurrentDirectory.DosPath.Buffer == NULL)
		return (NULL);
	size = (size_t)params.CurrentDirectory.DosPath.Length;
	if (size % sizeof *wpath != 0)
		return (NULL);
	wpath = xcalloc((size / sizeof *wpath) + 1, sizeof *wpath);
	if (!win32_process_read(process, params.CurrentDirectory.DosPath.Buffer,
	    wpath, size)) {
		free(wpath);
		return (NULL);
	}
	wpath[size / sizeof *wpath] = L'\0';
	cwd = win32_wide_to_utf8(wpath);
	free(wpath);
	return (cwd);
}

static ssize_t
win32_job_find_process(struct win32_job_process *processes, u_int count,
    DWORD pid)
{
	u_int	i;

	for (i = 0; i < count; i++) {
		if (processes[i].pid == pid)
			return ((ssize_t)i);
	}
	return (-1);
}

static int
win32_job_process_depth(struct win32_job_process *processes, u_int count,
    u_int index, DWORD root_pid)
{
	ssize_t	parent;
	DWORD	pid;
	u_int	depth, limit;

	pid = processes[index].pid;
	if (pid == root_pid)
		return (0);

	depth = 0;
	limit = count;
	while (pid != root_pid) {
		if (limit-- == 0)
			return (-1);
		parent = win32_job_find_process(processes, count,
		    processes[index].ppid);
		if (parent == -1)
			return (-1);
		index = (u_int)parent;
		pid = processes[index].pid;
		depth++;
	}
	return ((int)depth);
}

static char *
win32_job_get_cwd(HANDLE job, HANDLE root_process, DWORD root_pid)
{
	JOBOBJECT_BASIC_PROCESS_ID_LIST	*list = NULL;
	struct win32_job_process	*processes = NULL;
	PROCESS_BASIC_INFORMATION	 pbi;
	size_t				 size;
	DWORD				 needed = 0, pid;
	u_int				 assigned, count, used = 0, i, j, best;
	int				 depth;
	char				*cwd = NULL;
	HANDLE				 process;
	int				 have_best = 0;

	if (root_process == NULL)
		return (NULL);
	if (job == NULL)
		return (win32_process_get_cwd(root_process));

	size = sizeof *list + (15 * sizeof(ULONG_PTR));
	for (;;) {
		list = xmalloc(size);
		if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList,
		    list, (DWORD)size, &needed)) {
			free(list);
			return (win32_process_get_cwd(root_process));
		}
		assigned = list->NumberOfAssignedProcesses;
		if (list->NumberOfProcessIdsInList >= assigned)
			break;
		free(list);
		size = sizeof *list;
		if (assigned > 1)
			size += (assigned - 1) * sizeof(ULONG_PTR);
	}

	count = list->NumberOfProcessIdsInList;
	processes = xcalloc(count == 0 ? 1 : count, sizeof *processes);
	for (i = 0; i < count; i++) {
		pid = (DWORD)list->ProcessIdList[i];
		if (pid == 0)
			continue;
		if (pid == root_pid)
			process = root_process;
		else {
			process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|
			    PROCESS_VM_READ, FALSE, pid);
			if (process == NULL)
				continue;
		}
		if (!win32_process_get_basic_information(process, &pbi)) {
			if (process != root_process)
				CloseHandle(process);
			continue;
		}
		processes[used].pid = pid;
		processes[used].ppid =
		    (DWORD)(ULONG_PTR)pbi.InheritedFromUniqueProcessId;
		processes[used].process = process;
		processes[used].created = win32_process_get_create_time(process);
		processes[used].close_process = (process != root_process);
		used++;
	}
	free(list);

	best = 0;
	for (i = 0; i < used; i++) {
		depth = win32_job_process_depth(processes, used, i, root_pid);
		if (depth < 0)
			continue;
		processes[i].depth = (u_int)depth;
		for (j = 0; j < used; j++) {
			if (i != j && processes[j].ppid == processes[i].pid)
				processes[i].has_child = 1;
		}
		if (!have_best ||
		    (processes[best].has_child && !processes[i].has_child) ||
		    (processes[best].has_child == processes[i].has_child &&
		    (processes[i].depth > processes[best].depth ||
		    (processes[i].depth == processes[best].depth &&
		    processes[i].created > processes[best].created)))) {
			best = i;
			have_best = 1;
		}
	}
	if (have_best)
		cwd = win32_process_get_cwd(processes[best].process);
	if (cwd == NULL)
		cwd = win32_process_get_cwd(root_process);

	for (i = 0; i < used; i++) {
		if (processes[i].close_process)
			CloseHandle(processes[i].process);
	}
	free(processes);
	return (cwd);
}

static HANDLE
win32_child_create_job(HANDLE process, DWORD pid)
{
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION	info;
	HANDLE					job;

	job = CreateJobObjectW(NULL, NULL);
	if (job == NULL) {
		log_debug("%s %lu create job failed: %s", __func__,
		    (unsigned long)pid, win32_strerror(GetLastError()));
		return (NULL);
	}
	memset(&info, 0, sizeof info);
	info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
	    &info, sizeof info)) {
		log_debug("%s %lu set job info failed: %s", __func__,
		    (unsigned long)pid, win32_strerror(GetLastError()));
		CloseHandle(job);
		return (NULL);
	}
	if (!AssignProcessToJobObject(job, process)) {
		log_debug("%s %lu assign job failed: %s", __func__,
		    (unsigned long)pid, win32_strerror(GetLastError()));
		CloseHandle(job);
		return (NULL);
	}
	return (job);
}

static int
win32_wait_process(HANDLE process, DWORD timeout, int *status)
{
	DWORD	code;

	if (process == NULL)
		return (0);
	if (WaitForSingleObject(process, timeout) != WAIT_OBJECT_0)
		return (0);
	if (!GetExitCodeProcess(process, &code))
		code = 1;
	if (status != NULL)
		*status = W_EXITCODE((int)code, 0);
	return (1);
}

static void
win32_child_kill(const char *name, DWORD pid, HANDLE job, HANDLE process)
{
	if (job != NULL) {
		if (!TerminateJobObject(job, 1)) {
			log_debug("%s %lu job kill failed: %s", name,
			    (unsigned long)pid, win32_strerror(GetLastError()));
		}
	} else if (process != NULL && !win32_wait_process(process, 0, NULL)) {
		if (!TerminateProcess(process, 1)) {
			log_debug("%s %lu hard-kill failed: %s", name,
			    (unsigned long)pid, win32_strerror(GetLastError()));
			return;
		}
	}
	(void)win32_wait_process(process, WIN32_CHILD_KILL_TIMEOUT, NULL);
}

static void
win32_child_disconnect(HPCON *hpcon, HANDLE *input_read, HANDLE *input_write,
    HANDLE *output_write)
{
	win32_close_handle(input_write);
	win32_close_pseudoconsole(hpcon);
	win32_close_handle(input_read);
	win32_close_handle(output_write);
}

static void
win32_pane_disconnect(struct win32_pane *pw)
{
	if (pw->input_writer != NULL) {
		win32_io_endpoint_free(pw->input_writer);
		pw->input_writer = NULL;
	}
	if (pw->input_queue != NULL) {
		evbuffer_free(pw->input_queue);
		pw->input_queue = NULL;
	}
	win32_child_disconnect(&pw->hpcon, &pw->input_read, &pw->input_write,
	    &pw->output_write);
}

static void
win32_pane_close_stdin(struct win32_pane *pw)
{
	if (pw->input_queue != NULL)
		evbuffer_drain(pw->input_queue, EVBUFFER_LENGTH(pw->input_queue));
	if (pw->input_writer != NULL) {
		win32_io_endpoint_free(pw->input_writer);
		pw->input_writer = NULL;
	}
	win32_close_handle(&pw->input_write);
	win32_close_handle(&pw->input_read);
}

static int
win32_pane_flush_input(struct window_pane *wp)
{
	struct win32_pane	*pw;
	struct evbuffer		*evb;
	size_t			 buffered, available, nwrite;
	int			 written;

	if (wp == NULL || wp->win32 == NULL)
		return (0);
	pw = wp->win32;
	if (pw->input_queue == NULL)
		return (0);
	if (pw->input_writer == NULL) {
		errno = EPIPE;
		return (-1);
	}

	evb = pw->input_queue;
	for (;;) {
		buffered = win32_io_writer_buffered(pw->input_writer);
		if (buffered >= WIN32_INPUT_WRITER_HIGH)
			return (0);

		available = EVBUFFER_LENGTH(evb);
		if (available == 0)
			return (0);
		nwrite = WIN32_INPUT_WRITER_HIGH - buffered;
		if (nwrite > available)
			nwrite = available;
		if (nwrite > WIN32_INPUT_WRITER_CHUNK)
			nwrite = WIN32_INPUT_WRITER_CHUNK;

		written = win32_io_writer_write(pw->input_writer,
		    EVBUFFER_DATA(evb), nwrite);
		if (written <= 0)
			return (-1);
		evbuffer_drain(evb, written);
	}
}

static void
win32_pane_input_write_cb(void *arg)
{
	struct window_pane	*wp = arg;

	(void)win32_pane_flush_input(wp);
}

static void
win32_pane_input_error_cb(void *arg)
{
	struct window_pane	*wp = arg;
	struct win32_pane	*pw;

	if (wp == NULL || wp->win32 == NULL)
		return;
	pw = wp->win32;
	if (pw->input_queue != NULL)
		evbuffer_drain(pw->input_queue,
		    EVBUFFER_LENGTH(pw->input_queue));
	if (pw->input_writer != NULL) {
		win32_io_endpoint_free(pw->input_writer);
		pw->input_writer = NULL;
	}
}

static void
win32_pane_input_event_cb(void *arg, uint32_t events)
{
	if (events & (WIN32_IO_EVENT_ERROR|WIN32_IO_EVENT_CANCELED)) {
		win32_pane_input_error_cb(arg);
		return;
	}
	if (events & (WIN32_IO_EVENT_WRITE_DRAINED|
	    WIN32_IO_EVENT_WRITE_CLOSED))
		win32_pane_input_write_cb(arg);
}

static void
win32_job_disconnect(struct win32_job *wj)
{
	if (wj->stdin_writer != NULL) {
		win32_io_endpoint_free(wj->stdin_writer);
		wj->stdin_writer = NULL;
	}
	win32_child_disconnect(&wj->hpcon, &wj->stdin_read, &wj->stdin_write,
	    &wj->stdout_write);
}

static void
win32_job_close_input(struct win32_job *wj)
{
	if (wj->event != NULL)
		evbuffer_drain(wj->event->output,
		    EVBUFFER_LENGTH(wj->event->output));
	if (wj->stdin_writer != NULL) {
		win32_io_endpoint_free(wj->stdin_writer);
		wj->stdin_writer = NULL;
	}
	win32_close_handle(&wj->stdin_write);
	win32_close_handle(&wj->stdin_read);
}

static int
win32_job_flush_input(struct win32_job *wj)
{
	struct evbuffer	*evb;
	size_t		 buffered, available, nwrite;
	int		 written;

	if (wj == NULL || wj->event == NULL)
		return (0);
	if (wj->stdin_writer == NULL) {
		errno = EPIPE;
		return (-1);
	}

	evb = wj->event->output;
	for (;;) {
		buffered = win32_io_writer_buffered(wj->stdin_writer);
		if (buffered >= WIN32_INPUT_WRITER_HIGH)
			return (0);

		available = EVBUFFER_LENGTH(evb);
		if (available == 0)
			return (0);
		nwrite = WIN32_INPUT_WRITER_HIGH - buffered;
		if (nwrite > available)
			nwrite = available;
		if (nwrite > WIN32_INPUT_WRITER_CHUNK)
			nwrite = WIN32_INPUT_WRITER_CHUNK;

		written = win32_io_writer_write(wj->stdin_writer,
		    EVBUFFER_DATA(evb), nwrite);
		if (written <= 0)
			return (-1);
		evbuffer_drain(evb, written);
	}
}

static void
win32_job_maybe_close_stdin(struct win32_job *wj)
{
	size_t	queued = 0, buffered = 0;

	if (wj == NULL || !wj->stdin_closing || wj->stdin_writer == NULL)
		return;
	if (wj->event != NULL)
		queued = EVBUFFER_LENGTH(wj->event->output);
	buffered = win32_io_writer_buffered(wj->stdin_writer);
	if (queued == 0 && buffered == 0)
		win32_io_writer_close(wj->stdin_writer);
}

void
win32_pane_drain(struct window_pane *wp)
{
	if (wp->win32 == NULL || wp->win32->output_event == NULL ||
	    wp->event == NULL)
		return;
	win32_io_reader_drain_bev(wp->win32->output_event, wp->event);
	window_pane_read_callback(wp->event, wp);
}

static void
win32_pane_output_event_cb(void *arg, uint32_t events)
{
	struct window_pane	*wp = arg;
	int			 status;

	if (events & WIN32_IO_EVENT_READ)
		win32_pane_drain(wp);
	if (~events & (WIN32_IO_EVENT_READ_EOF|WIN32_IO_EVENT_ERROR|
	    WIN32_IO_EVENT_CANCELED))
		return;
	if (~events & WIN32_IO_EVENT_READ)
		win32_pane_drain(wp);
	if (events & WIN32_IO_EVENT_ERROR) {
		log_debug("%%%u output read error", wp->id);
	} else if (events & WIN32_IO_EVENT_READ_EOF)
		log_debug("%%%u output EOF", wp->id);
	else
		log_debug("%%%u output canceled", wp->id);
	if (win32_pane_exited(wp, &status)) {
		wp->status = status;
		wp->flags |= PANE_STATUSREADY;
	}
	window_pane_error_callback(wp->event, 0, wp);
}

static int
win32_process_status(HANDLE process, int *status)
{
	return (win32_wait_process(process, 0, status));
}

static int
win32_pane_set_exited(struct window_pane *wp, int *status)
{
	struct win32_pane	*pw;

	if (wp->win32 == NULL || wp->win32->process == NULL)
		return (0);
	pw = wp->win32;
	if (pw->exited) {
		if (status != NULL)
			*status = pw->status;
		return (1);
	}
	if (status != NULL)
		pw->status = *status;
	else
		(void)win32_process_status(pw->process, &pw->status);
	pw->exited = 1;
	win32_pane_close_stdin(pw);
	return (1);
}

static void
win32_pane_exit_cb(void *arg)
{
	struct window_pane	*wp = arg;
	int			 status;

	if (!win32_pane_exited(wp, &status))
		return;
	wp->status = status;
	wp->flags |= PANE_STATUSREADY;
	log_debug("%%%u exited", wp->id);
	if (window_pane_destroy_ready(wp))
		server_destroy_pane(wp, 1);
}

static void
win32_pane_process_event_cb(void *arg, uint32_t events)
{
	if (events & WIN32_IO_EVENT_PROCESS_EXIT)
		win32_pane_exit_cb(arg);
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

	if (win32_make_input_pipe(&pw->input_read, &pw->input_write) != 0 ||
	    win32_make_output_pipe(&pw->output_read, &pw->output_write) != 0) {
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
	if (win32_validate_cwd(cwd, cause) != 0)
		goto fail;
	wcwd = win32_utf8_to_wide(cwd);
	wenv = win32_build_environment(env);
	memset(&pi, 0, sizeof pi);
	ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE,
	    EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT|
	    CREATE_SUSPENDED,
	    wenv, wcwd, &si.StartupInfo, &pi);
	if (!ok) {
		xasprintf(cause, "CreateProcess failed: %s",
		    win32_strerror(GetLastError()));
		goto fail;
	}

	pw->process = pi.hProcess;
	pw->thread = pi.hThread;
	pw->process_id = pi.dwProcessId;
	pw->job = win32_child_create_job(pw->process, pw->process_id);
	if (pw->job == NULL) {
		xasprintf(cause, "AssignProcessToJobObject pane failed");
		goto fail;
	}
	if (ResumeThread(pw->thread) == (DWORD)-1) {
		xasprintf(cause, "ResumeThread failed: %s",
		    win32_strerror(GetLastError()));
		goto fail;
	}
	win32_close_handle(&pw->input_read);
	win32_close_handle(&pw->output_write);
	pw->input_queue = evbuffer_new();
	if (pw->input_queue == NULL)
		fatalx("out of memory");
	pw->input_writer = win32_io_writer_new_overlapped(&pw->input_write,
	    win32_pane_input_event_cb, wp);
	if (pw->input_writer == NULL) {
		xasprintf(cause, "couldn't create pane input writer");
		goto fail;
	}
	wp->pid = (pid_t)pi.dwProcessId;
	wp->win32 = pw;

	pw->output_event = win32_io_reader_new_overlapped(pw->output_read,
	    win32_pane_output_event_cb, wp);
	if (pw->output_event == NULL) {
		xasprintf(cause, "couldn't create pane output event");
		goto fail;
	}
	pw->process_event = win32_io_process_new(pw->process,
	    win32_pane_process_event_cb, wp);
	if (pw->process_event == NULL) {
		xasprintf(cause, "couldn't create pane process event");
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
		win32_child_kill("pane", pw->process_id, pw->job, pw->process);
		win32_pane_disconnect(pw);
		if (pw->process_event != NULL)
			win32_io_endpoint_free(pw->process_event);
		if (pw->output_event != NULL)
			win32_io_endpoint_free(pw->output_event);
		win32_close_handle(&pw->output_read);
		win32_close_handle(&pw->job);
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
	struct win32_pane	*pw = wp->win32;
	struct bufferevent	*event;

	if (pw == NULL)
		return;
	wp->win32 = NULL;
	event = wp->event;
	wp->event = NULL;
	win32_child_kill("pane", pw->process_id, pw->job, pw->process);
	win32_pane_disconnect(pw);
	if (pw->process_event != NULL)
		win32_io_endpoint_free(pw->process_event);
	if (pw->output_event != NULL)
		win32_io_endpoint_free(pw->output_event);
	win32_close_handle(&pw->output_read);
	if (event != NULL)
		bufferevent_free(event);
	win32_close_handle(&pw->job);
	win32_close_handle(&pw->thread);
	win32_close_handle(&pw->process);
	free(pw);
}

int
win32_pane_exited(struct window_pane *wp, int *status)
{
	int	found;

	if (wp->win32 == NULL || wp->win32->process == NULL)
		return (0);
	if (wp->win32->exited)
		return (win32_pane_set_exited(wp, status));
	found = win32_process_status(wp->win32->process, status);
	if (!found)
		return (0);
	return (win32_pane_set_exited(wp, status));
}

size_t
win32_pane_buffered(struct window_pane *wp)
{
	if (wp->win32 == NULL || wp->win32->output_event == NULL)
		return (0);
	return (win32_io_reader_buffered(wp->win32->output_event));
}

int
win32_pane_output_done(struct window_pane *wp)
{
	if (wp->win32 == NULL || wp->win32->output_event == NULL)
		return (1);
	return (win32_io_reader_done(wp->win32->output_event));
}

int
win32_pane_output_eof(struct window_pane *wp)
{
	if (wp->win32 == NULL || wp->win32->output_event == NULL)
		return (1);
	return (win32_io_reader_eof(wp->win32->output_event));
}

void
win32_pane_set_reading(struct window_pane *wp, int enabled)
{
	if (wp->win32 == NULL || wp->win32->output_event == NULL)
		return;
	wp->win32->output_paused = !enabled;
	win32_io_reader_set_reading(wp->win32->output_event, enabled);
}

int
win32_pane_reading_paused(struct window_pane *wp)
{
	if (wp->win32 == NULL)
		return (0);
	return (wp->win32->output_paused);
}

int
win32_pane_write(struct window_pane *wp, const void *data, size_t size)
{
	struct evbuffer	*evb;
	size_t		 nwrite;

	if (wp->win32 == NULL || wp->win32->input_writer == NULL) {
		errno = EPIPE;
		return (-1);
	}
	if (wp->win32->input_queue == NULL) {
		errno = EPIPE;
		return (-1);
	}
	if (!win32_io_writer_writable(wp->win32->input_writer)) {
		errno = EPIPE;
		return (-1);
	}

	nwrite = size;
	if (nwrite > INT_MAX)
		nwrite = INT_MAX;
	if (nwrite == 0)
		return (0);

	evb = wp->win32->input_queue;
	if (evbuffer_add(evb, data, nwrite) != 0) {
		errno = ENOMEM;
		return (-1);
	}
	if (win32_pane_flush_input(wp) != 0 &&
	    (wp->win32->input_writer == NULL ||
	    !win32_io_writer_writable(wp->win32->input_writer))) {
		errno = EPIPE;
		return (-1);
	}
	return ((int)nwrite);
}

char *
win32_pane_get_cwd(struct window_pane *wp)
{
	if (wp == NULL || wp->win32 == NULL)
		return (NULL);
	return (win32_job_get_cwd(wp->win32->job, wp->win32->process,
	    wp->win32->process_id));
}

struct bufferevent *
win32_pane_get_event(__unused struct window_pane *wp)
{
	return (NULL);
}

static void
win32_job_read_event(struct win32_job *wj)
{
	if (wj->event == NULL)
		return;
	win32_job_drain(wj);
	if (wj->event->readcb != NULL)
		wj->event->readcb(wj->event, wj->event->cbarg);
}

static void
win32_job_output_event_cb(void *arg, uint32_t events)
{
	struct win32_job	*wj = arg;
	int		 status;

	if (events & WIN32_IO_EVENT_READ)
		win32_job_read_event(wj);
	if (~events & (WIN32_IO_EVENT_READ_EOF|WIN32_IO_EVENT_ERROR|
	    WIN32_IO_EVENT_CANCELED))
		return;
	if (~events & WIN32_IO_EVENT_READ)
		win32_job_read_event(wj);
	if (events & WIN32_IO_EVENT_ERROR) {
		log_debug("job output read error, pid %ld",
		    (long)wj->process_id);
	} else if (events & WIN32_IO_EVENT_READ_EOF)
		log_debug("job output EOF, pid %ld", (long)wj->process_id);
	else
		log_debug("job output canceled, pid %ld",
		    (long)wj->process_id);
	if (win32_job_exited(wj, &status))
		wj->status = status;
	if (wj->event != NULL && wj->event->errorcb != NULL)
		wj->event->errorcb(wj->event, 0, wj->event->cbarg);
}

static void
win32_job_write_cb(void *arg)
{
	struct win32_job	*wj = arg;

	(void)win32_job_flush_input(wj);
	win32_job_maybe_close_stdin(wj);
	if (wj->event != NULL && wj->event->writecb != NULL)
		wj->event->writecb(wj->event, wj->event->cbarg);
}

static void
win32_job_write_error_cb(void *arg)
{
	struct win32_job	*wj = arg;

	/*
	 * Stdin write failure is separate from stdout EOF and process exit.
	 * Notify the write side only; output completion is delivered by the
	 * output reader.
	 */
	wj->stdin_closing = 1;
	if (wj->event != NULL)
		evbuffer_drain(wj->event->output,
		    EVBUFFER_LENGTH(wj->event->output));
	if (wj->event != NULL && wj->event->writecb != NULL)
		wj->event->writecb(wj->event, wj->event->cbarg);
}

static void
win32_job_write_event_cb(void *arg, uint32_t events)
{
	if (events & (WIN32_IO_EVENT_ERROR|WIN32_IO_EVENT_CANCELED)) {
		win32_job_write_error_cb(arg);
		return;
	}
	if (events & (WIN32_IO_EVENT_WRITE_DRAINED|
	    WIN32_IO_EVENT_WRITE_CLOSED))
		win32_job_write_cb(arg);
}

static int
win32_job_set_exited(struct win32_job *wj, int *status)
{
	if (wj == NULL || wj->process == NULL)
		return (0);
	if (wj->exited) {
		if (status != NULL)
			*status = wj->status;
		return (1);
	}
	if (status != NULL)
		wj->status = *status;
	else
		(void)win32_process_status(wj->process, &wj->status);
	wj->exited = 1;
	win32_job_close_input(wj);
	return (1);
}

static void
win32_job_exit_cb(void *arg)
{
	struct win32_job	*wj = arg;

	if (wj->exitcb != NULL)
		wj->exitcb(wj->exitarg);
	else
		wj->exit_pending = 1;
}

static void
win32_job_process_event_cb(void *arg, uint32_t events)
{
	if (events & WIN32_IO_EVENT_PROCESS_EXIT)
		win32_job_exit_cb(arg);
}

struct win32_job *
win32_job_spawn(const char *cmd, const char *shell, int argc, char **argv,
    struct environ *env, __unused struct session *s, const char *cwd,
    int flags, int sx, int sy, char **cause)
{
	struct win32_job		*wj;
	STARTUPINFOEXW		 six;
	PROCESS_INFORMATION	 pi;
	SIZE_T			 attr_size = 0;
	COORD			 size;
	wchar_t			*wcmd = NULL, *wcwd = NULL, *wenv = NULL;
	HANDLE			 handles[3];
	DWORD			 creation_flags;
	DWORD			 handle_count;
	HRESULT			 hr;
	SECURITY_ATTRIBUTES	 sa;
	BOOL			 ok;

	wj = xcalloc(1, sizeof *wj);
	wj->pty = !!(flags & JOB_PTY);
	if (win32_make_input_pipe(&wj->stdin_read, &wj->stdin_write) != 0 ||
	    win32_make_output_pipe(&wj->stdout_read, &wj->stdout_write) != 0) {
		if (cause != NULL) {
			xasprintf(cause, "CreatePipe failed: %s",
			    win32_strerror(GetLastError()));
		}
		goto fail;
	}

	memset(&six, 0, sizeof six);
	memset(&pi, 0, sizeof pi);
	wcmd = win32_build_job_command(cmd, shell, argc, argv);
	if (win32_validate_cwd(cwd, cause) != 0)
		goto fail;
	if (cwd != NULL)
		wcwd = win32_utf8_to_wide(cwd);
	wenv = win32_build_environment(env);
	creation_flags = CREATE_UNICODE_ENVIRONMENT;

	if (wj->pty) {
		size.X = sx <= 0 ? 80 : sx;
		size.Y = sy <= 0 ? 24 : sy;
		hr = CreatePseudoConsole(size, wj->stdin_read,
		    wj->stdout_write, 0, &wj->hpcon);
		if (FAILED(hr)) {
			if (cause != NULL) {
				xasprintf(cause,
				    "CreatePseudoConsole job failed: 0x%08lx",
				    (unsigned long)hr);
			}
			goto fail;
		}

		six.StartupInfo.cb = sizeof six;
		six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
		six.lpAttributeList = xcalloc(1, attr_size);
		if (!InitializeProcThreadAttributeList(six.lpAttributeList, 1,
		    0, &attr_size)) {
			if (cause != NULL) {
				xasprintf(cause, "InitializeProcThreadAttributeList "
				    "job failed: %s",
				    win32_strerror(GetLastError()));
			}
			goto fail;
		}
		if (!UpdateProcThreadAttribute(six.lpAttributeList, 0,
		    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, wj->hpcon,
		    sizeof wj->hpcon, NULL, NULL)) {
			if (cause != NULL) {
				xasprintf(cause,
				    "UpdateProcThreadAttribute job failed: %s",
				    win32_strerror(GetLastError()));
			}
			goto fail;
		}
		creation_flags |= EXTENDED_STARTUPINFO_PRESENT;
		ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE,
		    creation_flags|CREATE_SUSPENDED, wenv, wcwd,
		    &six.StartupInfo, &pi);
	} else {
		six.StartupInfo.cb = sizeof six;
		six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		six.StartupInfo.hStdInput = wj->stdin_read;
		six.StartupInfo.hStdOutput = wj->stdout_write;
		if (flags & JOB_SHOWSTDERR)
			six.StartupInfo.hStdError = wj->stdout_write;
		else {
			memset(&sa, 0, sizeof sa);
			sa.nLength = sizeof sa;
			sa.bInheritHandle = TRUE;
			wj->stderr_write = CreateFileW(L"NUL",
			    GENERIC_READ|GENERIC_WRITE,
			    FILE_SHARE_READ|FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
			    FILE_ATTRIBUTE_NORMAL, NULL);
			if (wj->stderr_write == INVALID_HANDLE_VALUE) {
				wj->stderr_write = NULL;
				if (cause != NULL) {
					xasprintf(cause, "CreateFileW NUL failed: %s",
					    win32_strerror(GetLastError()));
				}
				goto fail;
			}
			six.StartupInfo.hStdError = wj->stderr_write;
		}
		handle_count = 0;
		handles[handle_count++] = wj->stdin_read;
		handles[handle_count++] = wj->stdout_write;
		if (six.StartupInfo.hStdError != wj->stdout_write)
			handles[handle_count++] = six.StartupInfo.hStdError;
		InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
		six.lpAttributeList = xcalloc(1, attr_size);
		if (!InitializeProcThreadAttributeList(six.lpAttributeList, 1,
		    0, &attr_size)) {
			if (cause != NULL) {
				xasprintf(cause, "InitializeProcThreadAttributeList "
				    "job failed: %s",
				    win32_strerror(GetLastError()));
			}
			goto fail;
		}
		if (!UpdateProcThreadAttribute(six.lpAttributeList, 0,
		    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
		    handle_count * sizeof handles[0], NULL, NULL)) {
			if (cause != NULL) {
				xasprintf(cause,
				    "UpdateProcThreadAttribute handles job failed: %s",
				    win32_strerror(GetLastError()));
			}
			goto fail;
		}
		creation_flags |= EXTENDED_STARTUPINFO_PRESENT;
		ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE,
		    creation_flags|CREATE_SUSPENDED, wenv, wcwd,
		    &six.StartupInfo, &pi);
	}
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
	wj->job = win32_child_create_job(wj->process, wj->process_id);
	if (wj->job == NULL) {
		if (cause != NULL)
			xasprintf(cause, "AssignProcessToJobObject job failed");
		goto fail;
	}
	if (ResumeThread(wj->thread) == (DWORD)-1) {
		if (cause != NULL) {
			xasprintf(cause, "ResumeThread job failed: %s",
			    win32_strerror(GetLastError()));
		}
		goto fail;
	}
	win32_close_handle(&wj->stdin_read);
	win32_close_handle(&wj->stdout_write);
	win32_close_handle(&wj->stderr_write);
	wj->stdin_writer = win32_io_writer_new_overlapped(&wj->stdin_write,
	    win32_job_write_event_cb, wj);
	if (wj->stdin_writer == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't create job input writer");
		goto fail;
	}

	wj->event = bufferevent_new(-1, NULL, NULL, NULL, NULL);
	if (wj->event == NULL)
		fatalx("out of memory");
	wj->output_event = win32_io_reader_new_overlapped(wj->stdout_read,
	    win32_job_output_event_cb, wj);
	if (wj->output_event == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't create job output event");
		goto fail;
	}
	wj->process_event = win32_io_process_new(wj->process,
	    win32_job_process_event_cb, wj);
	if (wj->process_event == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't create job process event");
		goto fail;
	}

	if (six.lpAttributeList != NULL) {
		DeleteProcThreadAttributeList(six.lpAttributeList);
		free(six.lpAttributeList);
	}
	free(wcmd);
	free(wcwd);
	free(wenv);
	return (wj);

fail:
	if (six.lpAttributeList != NULL) {
		DeleteProcThreadAttributeList(six.lpAttributeList);
		free(six.lpAttributeList);
	}
	free(wcmd);
	free(wcwd);
	free(wenv);
	if (wj != NULL) {
		if (wj->event != NULL) {
			bufferevent_free(wj->event);
			wj->event = NULL;
		}
		win32_child_kill("job", wj->process_id, wj->job, wj->process);
		win32_job_disconnect(wj);
		if (wj->process_event != NULL)
			win32_io_endpoint_free(wj->process_event);
		if (wj->output_event != NULL)
			win32_io_endpoint_free(wj->output_event);
		win32_close_handle(&wj->stderr_write);
		win32_close_handle(&wj->stdout_read);
		win32_close_handle(&wj->job);
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
	wj->event = NULL;
	win32_child_kill("job", wj->process_id, wj->job, wj->process);
	win32_job_disconnect(wj);
	if (wj->process_event != NULL)
		win32_io_endpoint_free(wj->process_event);
	if (wj->output_event != NULL)
		win32_io_endpoint_free(wj->output_event);
	win32_close_handle(&wj->stderr_write);
	win32_close_handle(&wj->stdout_read);
	win32_close_handle(&wj->job);
	win32_close_handle(&wj->thread);
	win32_close_handle(&wj->process);
	free(wj);
}

void
win32_job_resize(struct win32_job *wj, u_int sx, u_int sy)
{
	COORD	size;

	if (wj->hpcon == NULL)
		return;
	size.X = sx == 0 ? 80 : sx;
	size.Y = sy == 0 ? 24 : sy;
	ResizePseudoConsole(wj->hpcon, size);
}

int
win32_job_exited(struct win32_job *wj, int *status)
{
	int	found;

	if (wj == NULL || wj->process == NULL)
		return (0);
	if (wj->exited)
		return (win32_job_set_exited(wj, status));
	found = win32_process_status(wj->process, status);
	if (!found)
		return (0);
	return (win32_job_set_exited(wj, status));
}

int
win32_job_output_done(struct win32_job *wj)
{
	if (wj == NULL || wj->output_event == NULL)
		return (1);
	return (win32_io_reader_done(wj->output_event));
}

int
win32_job_output_eof(struct win32_job *wj)
{
	if (wj == NULL || wj->output_event == NULL)
		return (1);
	return (win32_io_reader_eof(wj->output_event));
}

void
win32_job_drain(struct win32_job *wj)
{
	if (wj == NULL || wj->event == NULL || wj->output_event == NULL)
		return;
	win32_io_reader_drain_bev(wj->output_event, wj->event);
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
	int	status;

	if (win32_job_exited(wj, &status))
		wj->status = status;
	return (wj->status);
}

struct bufferevent *
win32_job_get_event(struct win32_job *wj)
{
	return (wj->event);
}

void
win32_job_set_exit_callback(struct win32_job *wj, void (*exitcb)(void *),
    void *arg)
{
	int	pending;

	if (wj == NULL)
		return;
	wj->exitcb = exitcb;
	wj->exitarg = arg;
	pending = wj->exit_pending;
	wj->exit_pending = 0;
	if (pending)
		win32_io_process_notify(wj->process_event);
}

int
win32_job_write(struct win32_job *wj, const void *data, size_t size)
{
	struct evbuffer	*evb;
	size_t		 nwrite;

	if (wj == NULL || wj->stdin_writer == NULL) {
		errno = EPIPE;
		return (-1);
	}
	if (wj->event == NULL || wj->stdin_closing ||
	    !win32_io_writer_writable(wj->stdin_writer)) {
		errno = EPIPE;
		return (-1);
	}

	nwrite = size;
	if (nwrite > INT_MAX)
		nwrite = INT_MAX;
	if (nwrite == 0)
		return (0);

	evb = wj->event->output;
	if (evbuffer_add(evb, data, nwrite) != 0) {
		errno = ENOMEM;
		return (-1);
	}
	if (win32_job_flush_input(wj) != 0 &&
	    (wj->stdin_writer == NULL ||
	    !win32_io_writer_writable(wj->stdin_writer))) {
		errno = EPIPE;
		return (-1);
	}
	return ((int)nwrite);
}

size_t
win32_job_stdin_buffered(struct win32_job *wj)
{
	size_t	buffered = 0;

	if (wj == NULL || wj->stdin_writer == NULL)
		return (0);
	if (wj->event != NULL)
		buffered += EVBUFFER_LENGTH(wj->event->output);
	buffered += win32_io_writer_buffered(wj->stdin_writer);
	return (buffered);
}

void
win32_job_close_stdin(struct win32_job *wj)
{
	if (wj == NULL)
		return;
	if (wj->stdin_writer != NULL) {
		wj->stdin_closing = 1;
		(void)win32_job_flush_input(wj);
		win32_job_maybe_close_stdin(wj);
	} else
		win32_close_handle(&wj->stdin_write);
}

#endif /* TMUX_WIN32 */
