/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include <sys/types.h>
#include <sys/utsname.h>

#include <errno.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "tmux.h"

#ifdef TMUX_WIN32

static struct passwd win32_passwd;
static char    *win32_passwd_name;
static char    *win32_passwd_dir;
static char    *win32_passwd_shell;
static LONG	win32_ctrl_c_events;
static LONG	win32_ctrl_close_event;
char	      **environ;

static void	 win32_free_environ_block(void);
static int	 win32_import_environment(struct environ *);
static int	 win32_import_environment_entry(const wchar_t *,
		     struct environ *);
static int	 win32_import_utf8_entry(char *, struct environ *);
static int	 win32_errno_from_last_error(DWORD);
static const char *win32_getenv_canonical(const char *);

static const char *
win32_getenv_canonical(const char *name)
{
	struct environ_entry	*envent;
	const char		*value;

	if (name == NULL || *name == '\0')
		return (NULL);
	if (global_environ != NULL) {
		envent = environ_find(global_environ, name);
		if (envent != NULL)
			return (envent->value);
	}
	value = getenv(name);
	return (value);
}

static BOOL WINAPI
win32_console_ctrl_handler(DWORD type)
{
	if (type == CTRL_C_EVENT) {
		InterlockedIncrement(&win32_ctrl_c_events);
		return (TRUE);
	}
	if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
	    type == CTRL_SHUTDOWN_EVENT) {
		InterlockedExchange(&win32_ctrl_close_event, (LONG)type);
		return (TRUE);
	}
	return (FALSE);
}

long
win32_console_ctrl_c_events(void)
{
	return (InterlockedExchange(&win32_ctrl_c_events, 0));
}

DWORD
win32_console_ctrl_close_event(void)
{
	return ((DWORD)InterlockedExchange(&win32_ctrl_close_event, 0));
}

void
win32_refresh_environ(void)
{
	struct environ	*env;
	struct environ_entry *envent;
	size_t		 count, i;

	win32_free_environ_block();

	env = environ_create();
	if (win32_import_environment(env) != 0) {
		environ_free(env);
		environ = xcalloc(1, sizeof *environ);
		return;
	}

	count = 0;
	for (envent = environ_first(env); envent != NULL;
	    envent = environ_next(envent))
		count++;

	environ = xcalloc(count + 1, sizeof *environ);
	i = 0;
	for (envent = environ_first(env); envent != NULL;
	    envent = environ_next(envent)) {
		if (envent->value == NULL)
			continue;
		xasprintf(&environ[i++], "%s=%s", envent->name, envent->value);
	}
	environ_free(env);
}

const char *
win32_strerror(DWORD error)
{
	static char	buffer[512];
	wchar_t		*wmsg = NULL;
	char		*msg;
	DWORD		 flags;

	flags = FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|
	    FORMAT_MESSAGE_IGNORE_INSERTS;
	if (FormatMessageW(flags, NULL, error, 0, (LPWSTR)&wmsg, 0,
	    NULL) == 0) {
		xsnprintf(buffer, sizeof buffer, "Win32 error %lu", error);
		return (buffer);
	}

	msg = win32_wide_to_utf8(wmsg);
	LocalFree(wmsg);
	if (msg == NULL) {
		xsnprintf(buffer, sizeof buffer, "Win32 error %lu", error);
		return (buffer);
	}

	strlcpy(buffer, msg, sizeof buffer);
	free(msg);
	buffer[strcspn(buffer, "\r\n")] = '\0';
	return (buffer);
}

wchar_t *
win32_utf8_to_wide(const char *s)
{
	wchar_t	*out;
	int	 n;

	if (s == NULL)
		return (NULL);
	n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (n == 0)
		return (NULL);
	out = xcalloc(n, sizeof *out);
	if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n) == 0) {
		free(out);
		return (NULL);
	}
	return (out);
}

char *
win32_wide_to_utf8(const wchar_t *s)
{
	char	*out;
	int	 n;

	if (s == NULL)
		return (NULL);
	n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
	if (n == 0)
		return (NULL);
	out = xcalloc(n, sizeof *out);
	if (WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL) == 0) {
		free(out);
		return (NULL);
	}
	return (out);
}

char *
win32_getenv_utf8(const char *name)
{
	wchar_t	*wname, *value;
	char	*out;
	DWORD	 n;

	wname = win32_utf8_to_wide(name);
	if (wname == NULL)
		return (NULL);
	n = GetEnvironmentVariableW(wname, NULL, 0);
	if (n == 0) {
		free(wname);
		return (NULL);
	}
	value = xcalloc(n, sizeof *value);
	if (GetEnvironmentVariableW(wname, value, n) == 0) {
		free(value);
		free(wname);
		return (NULL);
	}
	out = win32_wide_to_utf8(value);
	free(value);
	free(wname);
	return (out);
}

char *
getenv(const char *name)
{
	char	**entry;
	size_t	 name_len;

	if (name == NULL || *name == '\0' || environ == NULL)
		return (NULL);

	name_len = strlen(name);
	for (entry = environ; *entry != NULL; entry++) {
		char	*eq;
		size_t	 entry_len;

		if ((*entry)[0] == '=')
			continue;
		eq = strchr(*entry, '=');
		if (eq == NULL)
			continue;
		entry_len = (size_t)(eq - *entry);
		if (entry_len == name_len &&
		    _strnicmp(*entry, name, name_len) == 0)
			return (*entry + name_len + 1);
	}
	return (NULL);
}

void
win32_copy_environ(struct environ *env)
{
	char	**entry;

	if (env == NULL)
		return;
	if (environ == NULL)
		win32_refresh_environ();
	if (environ == NULL)
		return;
	for (entry = environ; *entry != NULL; entry++)
		environ_put(env, *entry, 0);
}

int
win32_setenv_utf8(const char *name, const char *value, int overwrite)
{
	wchar_t	*wname, *wvalue;
	BOOL	 ok;

	if (name == NULL || *name == '\0' || strchr(name, '=') != NULL ||
	    value == NULL) {
		errno = EINVAL;
		return (-1);
	}

	if (!overwrite) {
		char	*existing;

		existing = win32_getenv_utf8(name);
		if (existing != NULL) {
			free(existing);
			return (0);
		}
	}

	wname = win32_utf8_to_wide(name);
	wvalue = win32_utf8_to_wide(value);
	if (wname == NULL || wvalue == NULL) {
		free(wname);
		free(wvalue);
		errno = EINVAL;
		return (-1);
	}
	ok = SetEnvironmentVariableW(wname, wvalue);
	free(wname);
	free(wvalue);
	if (!ok) {
		errno = EINVAL;
		return (-1);
	}

	win32_refresh_environ();
	return (0);
}

int
win32_unsetenv_utf8(const char *name)
{
	wchar_t	*wname;
	BOOL	 ok;

	if (name == NULL || *name == '\0' || strchr(name, '=') != NULL) {
		errno = EINVAL;
		return (-1);
	}

	wname = win32_utf8_to_wide(name);
	if (wname == NULL) {
		errno = EINVAL;
		return (-1);
	}
	ok = SetEnvironmentVariableW(wname, NULL);
	free(wname);
	if (!ok) {
		errno = EINVAL;
		return (-1);
	}

	win32_refresh_environ();
	return (0);
}

FILE *
win32_fopen_utf8(const char *path, const char *mode)
{
	wchar_t	*wpath, *wmode;
	FILE	*f;

	if (path == NULL || mode == NULL) {
		errno = EINVAL;
		return (NULL);
	}
	wpath = win32_utf8_to_wide(path);
	wmode = win32_utf8_to_wide(mode);
	if (wpath == NULL || wmode == NULL) {
		free(wpath);
		free(wmode);
		errno = EINVAL;
		return (NULL);
	}
	f = _wfopen(wpath, wmode);
	free(wpath);
	free(wmode);
	return (f);
}

int
win32_access_utf8(const char *path, int mode)
{
	wchar_t	*wpath;
	DWORD	 attr;

	if (path == NULL) {
		errno = EINVAL;
		return (-1);
	}
	wpath = win32_utf8_to_wide(path);
	if (wpath == NULL) {
		errno = EINVAL;
		return (-1);
	}
	attr = GetFileAttributesW(wpath);
	free(wpath);
	if (attr == INVALID_FILE_ATTRIBUTES) {
		errno = win32_errno_from_last_error(GetLastError());
		return (-1);
	}
	if (mode == F_OK)
		return (0);
	if (mode != X_OK) {
		errno = EINVAL;
		return (-1);
	}
	if (attr & FILE_ATTRIBUTE_DIRECTORY) {
		errno = EACCES;
		return (-1);
	}
	return (0);
}

char *
win32_getcwd_utf8(void)
{
	wchar_t	*wbuf;
	char	*cwd;
	DWORD	 n;

	n = GetCurrentDirectoryW(0, NULL);
	if (n == 0)
		return (NULL);
	wbuf = xcalloc(n, sizeof *wbuf);
	if (GetCurrentDirectoryW(n, wbuf) == 0) {
		free(wbuf);
		return (NULL);
	}
	cwd = win32_wide_to_utf8(wbuf);
	free(wbuf);
	return (cwd);
}

int
win32_unlink_utf8(const char *path)
{
	wchar_t	*wpath;

	if (path == NULL) {
		errno = EINVAL;
		return (-1);
	}
	wpath = win32_utf8_to_wide(path);
	if (wpath == NULL) {
		errno = EINVAL;
		return (-1);
	}
	if (!DeleteFileW(wpath)) {
		free(wpath);
		errno = win32_errno_from_last_error(GetLastError());
		return (-1);
	}
	free(wpath);
	return (0);
}

char *
win32_get_module_path_utf8(void)
{
	wchar_t	*wbuf;
	char	*path;
	DWORD	 n, size;

	size = MAX_PATH;
	for (;;) {
		wbuf = xcalloc(size, sizeof *wbuf);
		n = GetModuleFileNameW(NULL, wbuf, size);
		if (n == 0) {
			free(wbuf);
			return (NULL);
		}
		if (n < size - 1)
			break;
		free(wbuf);
		if (size > ((DWORD)-1) / 2)
			return (NULL);
		size *= 2;
	}
	path = win32_wide_to_utf8(wbuf);
	free(wbuf);
	return (path);
}

int
win32_path_is_dir(const char *path)
{
	wchar_t	*wpath;
	DWORD	 attr;
	int	 ok;

	wpath = win32_utf8_to_wide(path);
	if (wpath == NULL)
		return (0);
	attr = GetFileAttributesW(wpath);
	ok = (attr != INVALID_FILE_ATTRIBUTES &&
	    (attr & FILE_ATTRIBUTE_DIRECTORY));
	free(wpath);
	return (ok);
}

char *
win32_resolve_cwd(const char *cwd, const char *base, char **cause)
{
	wchar_t	*winput = NULL, *wbase = NULL, *wcombined = NULL;
	wchar_t	*wresolved = NULL;
	char	*resolved = NULL;
	const char	*root;
	DWORD	 n;
	size_t	 len, base_len, input_len, used;

	if (cwd == NULL)
		return (NULL);
	if (cwd[0] == '/') {
		if (cause != NULL) {
			xasprintf(cause,
			    "working directory must be a Windows path on "
			    "Win32: %s", cwd);
		}
		errno = EINVAL;
		return (NULL);
	}

	winput = win32_utf8_to_wide(cwd);
	if (winput == NULL)
		goto fail;

	if (path_is_absolute(cwd))
		root = NULL;
	else if (base != NULL && base[0] != '/')
		root = base;
	else
		root = win32_default_cwd();

	if (root != NULL) {
		wbase = win32_utf8_to_wide(root);
		if (wbase == NULL)
			goto fail;

		base_len = wcslen(wbase);
		input_len = wcslen(winput);
		used = base_len;
		if (base_len != 0 &&
		    wbase[base_len - 1] != L'\\' &&
		    wbase[base_len - 1] != L'/')
			used++;
		wcombined = xcalloc(used + input_len + 1, sizeof *wcombined);
		memcpy(wcombined, wbase, base_len * sizeof *wcombined);
		if (used != base_len)
			wcombined[base_len] = L'\\';
		memcpy(wcombined + used, winput,
		    (input_len + 1) * sizeof *wcombined);
	} else {
		len = wcslen(winput);
		wcombined = xcalloc(len + 1, sizeof *wcombined);
		memcpy(wcombined, winput, (len + 1) * sizeof *wcombined);
	}
	if (wcombined == NULL)
		goto fail;

	n = GetFullPathNameW(wcombined, 0, NULL, NULL);
	if (n == 0)
		goto fail;
	wresolved = xcalloc(n, sizeof *wresolved);
	if (GetFullPathNameW(wcombined, n, wresolved, NULL) == 0)
		goto fail;

	resolved = win32_wide_to_utf8(wresolved);
	if (resolved == NULL)
		goto fail;

	free(winput);
	free(wbase);
	free(wcombined);
	free(wresolved);
	return (resolved);

fail:
	if (cause != NULL && *cause == NULL)
		xasprintf(cause, "couldn't resolve working directory: %s", cwd);
	free(winput);
	free(wbase);
	free(wcombined);
	free(wresolved);
	free(resolved);
	return (NULL);
}

char *
win32_sanitize_cwd(const char *cwd)
{
	const char	*actual_cwd;

	if (cwd == NULL)
		return (NULL);
	if (cwd[0] != '/' && path_is_absolute(cwd) && win32_path_is_dir(cwd))
		return (xstrdup(cwd));
	actual_cwd = win32_default_cwd();
	if (actual_cwd != NULL)
		return (xstrdup(actual_cwd));
	return (NULL);
}

const char *
win32_default_cwd(void)
{
	const char	*home;

	home = find_home();
	if (home != NULL && win32_path_is_dir(home))
		return (home);
	if (win32_path_is_dir("C:\\"))
		return ("C:\\");
	return (NULL);
}

int
win32_terminal_prepare_terminfo(void)
{
	const char	*terminfo, *terminfo_dirs;
	const char	*paths[] = {
		"\\..\\sysroot\\share\\terminfo",
		"\\..\\share\\terminfo",
		"\\share\\terminfo",
		"C:\\msys64\\usr\\share\\terminfo",
		"C:\\msys64\\ucrt64\\share\\terminfo",
		NULL
	};
	char		*exe_utf8, *dir, *candidate;
	size_t		 i;

	terminfo = win32_getenv_canonical("TERMINFO");
	terminfo_dirs = win32_getenv_canonical("TERMINFO_DIRS");
	if ((terminfo != NULL && *terminfo != '\0') ||
	    (terminfo_dirs != NULL && *terminfo_dirs != '\0'))
		return (0);

	exe_utf8 = win32_get_module_path_utf8();
	if (exe_utf8 == NULL)
		return (-1);
	dir = xstrdup(exe_utf8);
	free(exe_utf8);
	if (dir == NULL)
		return (-1);
	if ((candidate = strrchr(dir, '\\')) != NULL)
		*candidate = '\0';
	else if ((candidate = strrchr(dir, '/')) != NULL)
		*candidate = '\0';
	else {
		free(dir);
		return (-1);
	}

	for (i = 0; paths[i] != NULL; i++) {
		if (paths[i][1] == ':')
			candidate = xstrdup(paths[i]);
		else
			xasprintf(&candidate, "%s%s", dir, paths[i]);
		if (win32_path_is_dir(candidate)) {
			setenv("TERMINFO", candidate, 1);
			free(candidate);
			free(dir);
			return (0);
		}
		free(candidate);
	}

	free(dir);
	return (-1);
}

static void
win32_passwd_set(const char *name)
{
	const char	*home, *shell;

	home = win32_getenv_canonical("HOME");
	if (home == NULL || *home == '\0')
		home = win32_getenv_canonical("USERPROFILE");
	if (home == NULL || *home == '\0')
		home = "C:\\";

	shell = win32_getenv_canonical("SHELL");
	if (shell == NULL || *shell == '\0')
		shell = _PATH_BSHELL;

	free(win32_passwd_name);
	free(win32_passwd_dir);
	free(win32_passwd_shell);
	win32_passwd_name = xstrdup(name);
	win32_passwd_dir = xstrdup(home);
	win32_passwd_shell = xstrdup(shell);

	win32_passwd.pw_name = win32_passwd_name;
	win32_passwd.pw_uid = 0;
	win32_passwd.pw_gid = 0;
	win32_passwd.pw_dir = win32_passwd_dir;
	win32_passwd.pw_shell = win32_passwd_shell;
}

struct passwd *
getpwuid(__unused uid_t uid)
{
	const char	*name;

	name = win32_getenv_canonical("USER");
	if (name == NULL || *name == '\0')
		name = win32_getenv_canonical("USERNAME");
	if (name == NULL || *name == '\0')
		name = "win32";
	win32_passwd_set(name);
	return (&win32_passwd);
}

struct passwd *
getpwnam(const char *name)
{
	if (name == NULL || *name == '\0')
		return (NULL);
	win32_passwd_set(name);
	return (&win32_passwd);
}

uid_t
getuid(void)
{
	return (0);
}

uid_t
geteuid(void)
{
	return (getuid());
}

gid_t
getegid(void)
{
	return (0);
}

int
getpagesize(void)
{
	SYSTEM_INFO si;

	GetSystemInfo(&si);
	return ((int)si.dwPageSize);
}

char *
ctime_r(const time_t *timep, char *buf)
{
	char	*s;

	if (buf == NULL) {
		errno = EINVAL;
		return (NULL);
	}
	s = ctime(timep);
	if (s == NULL)
		return (NULL);
	strlcpy(buf, s, 26);
	return (buf);
}

struct tm *
gmtime_r(const time_t *timep, struct tm *result)
{
	struct tm	*tmp;

	if (result == NULL) {
		errno = EINVAL;
		return (NULL);
	}
	tmp = gmtime(timep);
	if (tmp == NULL)
		return (NULL);
	memcpy(result, tmp, sizeof *result);
	return (result);
}

struct tm *
localtime_r(const time_t *timep, struct tm *result)
{
	struct tm	*tmp;

	if (result == NULL) {
		errno = EINVAL;
		return (NULL);
	}
	tmp = localtime(timep);
	if (tmp == NULL)
		return (NULL);
	memcpy(result, tmp, sizeof *result);
	return (result);
}

char *
ttyname(__unused int fd)
{
	return (NULL);
}

int
wcwidth(wchar_t wc)
{
	if (wc == 0)
		return (0);
	if ((wc < 0x20) || (wc >= 0x7f && wc < 0xa0))
		return (-1);
	return (1);
}

int
uname(struct utsname *u)
{
	if (u == NULL) {
		errno = EFAULT;
		return (-1);
	}
	memset(u, 0, sizeof *u);
	strlcpy(u->sysname, "Windows", sizeof u->sysname);
	strlcpy(u->nodename, "localhost", sizeof u->nodename);
	strlcpy(u->release, "10", sizeof u->release);
	strlcpy(u->version, "ConPTY", sizeof u->version);
	strlcpy(u->machine, "x86_64", sizeof u->machine);
	return (0);
}

int
win32_init(void)
{
	WSADATA	wsa;

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return (-1);
	SetConsoleCtrlHandler(NULL, FALSE);
	SetConsoleCtrlHandler(win32_console_ctrl_handler, TRUE);
	return (0);
}

void
win32_fini(void)
{
	win32_io_service_fini();
	win32_free_environ_block();
	SetConsoleCtrlHandler(win32_console_ctrl_handler, FALSE);
	WSACleanup();
}

static void
win32_free_environ_block(void)
{
	size_t	i;

	if (environ == NULL)
		return;
	for (i = 0; environ[i] != NULL; i++)
		free(environ[i]);
	free(environ);
	environ = NULL;
}

static int
win32_import_environment(struct environ *env)
{
	wchar_t		*block, *entry;

	block = GetEnvironmentStringsW();
	if (block == NULL)
		return (-1);

	entry = block;
	while (*entry != L'\0') {
		if (win32_import_environment_entry(entry, env) != 0) {
			FreeEnvironmentStringsW(block);
			return (-1);
		}
		entry += wcslen(entry) + 1;
	}

	FreeEnvironmentStringsW(block);
	return (0);
}

static int
win32_import_environment_entry(const wchar_t *entry, struct environ *env)
{
	char	*utf8;
	int	 rc;

	if (entry == NULL || env == NULL)
		return (0);
	if (*entry == L'=')
		return (0);

	utf8 = win32_wide_to_utf8(entry);
	if (utf8 == NULL)
		return (-1);
	rc = win32_import_utf8_entry(utf8, env);
	free(utf8);
	return (rc);
}

static int
win32_import_utf8_entry(char *entry, struct environ *env)
{
	char	*eq;

	eq = strchr(entry, '=');
	if (eq == NULL || eq == entry)
		return (0);
	environ_put(env, entry, 0);
	return (0);
}

static int
win32_errno_from_last_error(DWORD error)
{
	switch (error) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		return (ENOENT);
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		return (EACCES);
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		return (EEXIST);
	case ERROR_DIRECTORY:
	case ERROR_INVALID_NAME:
	case ERROR_INVALID_PARAMETER:
		return (EINVAL);
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
		return (ENOMEM);
	default:
		return (EIO);
	}
}

#endif /* TMUX_WIN32 */
