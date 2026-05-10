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
char	      **environ;

void
win32_refresh_environ(void)
{
	environ = _environ;
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

static int
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
	wchar_t		 exe[MAX_PATH];
	DWORD		 n;
	char		*exe_utf8, *dir, *candidate;
	size_t		 i;

	terminfo = getenv("TERMINFO");
	terminfo_dirs = getenv("TERMINFO_DIRS");
	if ((terminfo != NULL && *terminfo != '\0') ||
	    (terminfo_dirs != NULL && *terminfo_dirs != '\0'))
		return (0);

	n = GetModuleFileNameW(NULL, exe, MAX_PATH);
	if (n == 0 || n == MAX_PATH)
		return (-1);
	exe_utf8 = win32_wide_to_utf8(exe);
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

	home = getenv("HOME");
	if (home == NULL || *home == '\0')
		home = getenv("USERPROFILE");
	if (home == NULL || *home == '\0')
		home = "C:\\";

	shell = getenv("SHELL");
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

	name = getenv("USER");
	if (name == NULL || *name == '\0')
		name = getenv("USERNAME");
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
	return (0);
}

void
win32_fini(void)
{
	WSACleanup();
}

#endif /* TMUX_WIN32 */
