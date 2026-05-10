/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef TMUX_WIN32_COMPAT_H
#define TMUX_WIN32_COMPAT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdint.h>
#include <sys/types.h>

#ifdef environ
#undef environ
#endif

typedef unsigned int uid_t;
typedef unsigned int gid_t;

struct iovec {
	void	*iov_base;
	size_t	 iov_len;
};

struct termios {
	unsigned int c_iflag;
	unsigned int c_oflag;
	unsigned int c_cflag;
	unsigned int c_lflag;
	unsigned char c_cc[32];
};

struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

#ifndef FNM_NOMATCH
#define FNM_NOMATCH 1
#endif
#ifndef FNM_NOESCAPE
#define FNM_NOESCAPE 0x01
#endif
#ifndef FNM_PATHNAME
#define FNM_PATHNAME 0x02
#endif
#ifndef FNM_PERIOD
#define FNM_PERIOD 0x04
#endif
#ifndef FNM_CASEFOLD
#define FNM_CASEFOLD 0x08
#endif

int fnmatch(const char *, const char *, int);

#ifndef TTY_NAME_MAX
#define TTY_NAME_MAX 128
#endif

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

#ifndef _PATH_BSHELL
#define _PATH_BSHELL "cmd.exe"
#endif

#ifndef _PATH_DEVNULL
#define _PATH_DEVNULL "NUL"
#endif

#ifndef _PATH_TMP
#define _PATH_TMP "."
#endif

#ifndef _PATH_DEFPATH
#define _PATH_DEFPATH "C:\\Windows\\System32;C:\\Windows"
#endif

#endif /* TMUX_WIN32_COMPAT_H */
