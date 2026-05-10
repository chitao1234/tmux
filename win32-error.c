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
