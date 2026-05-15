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
#include <time.h>
#include <wchar.h>

#ifdef environ
#undef environ
#endif

struct environ;

#ifndef TMUX_WIN32_UID_T_DEFINED
#define TMUX_WIN32_UID_T_DEFINED
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#endif

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

typedef unsigned char cc_t;

#ifndef VERASE
#define VERASE 2
#endif
#ifndef _POSIX_VDISABLE
#define _POSIX_VDISABLE 0xff
#endif

struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 1
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
uid_t getuid(void);
uid_t geteuid(void);
gid_t getegid(void);
int getpagesize(void);
char *ctime_r(const time_t *, char *);
struct tm *gmtime_r(const time_t *, struct tm *);
struct tm *localtime_r(const time_t *, struct tm *);
ssize_t readv(int, const struct iovec *, int);
char *ttyname(int);
int wcwidth(wchar_t);
ssize_t writev(int, const struct iovec *, int);
char *getenv(const char *);
void win32_copy_environ(struct environ *);
int win32_setenv_utf8(const char *, const char *, int);
int win32_unsetenv_utf8(const char *);
void win32_refresh_environ(void);
FILE *win32_fopen_utf8(const char *, const char *);
int win32_access_utf8(const char *, int);
char *win32_getcwd_utf8(void);
int win32_unlink_utf8(const char *);

#ifndef TTY_NAME_MAX
#define TTY_NAME_MAX 128
#endif

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

#ifndef WAIT_ANY
#define WAIT_ANY (-1)
#endif
#ifndef WNOHANG
#define WNOHANG 1
#endif
#ifndef WUNTRACED
#define WUNTRACED 2
#endif

#ifndef W_EXITCODE
#define W_EXITCODE(ret, sig) (((ret) << 8) | (sig))
#endif
#ifndef WIFEXITED
#define WIFEXITED(status) (((status) & 0x7f) == 0)
#endif
#ifndef WEXITSTATUS
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#endif
#ifndef WIFSIGNALED
#define WIFSIGNALED(status) (((status) & 0x7f) != 0)
#endif
#ifndef WTERMSIG
#define WTERMSIG(status) ((status) & 0x7f)
#endif
#ifndef WIFSTOPPED
#define WIFSTOPPED(status) (0)
#endif
#ifndef WSTOPSIG
#define WSTOPSIG(status) (0)
#endif

#ifndef _PATH_BSHELL
#define _PATH_BSHELL "cmd.exe"
#endif

#ifndef _PATH_DEVNULL
#define _PATH_DEVNULL "NUL"
#endif

#ifndef _PATH_TMP
#define _PATH_TMP "C:/Temp/"
#endif

#ifndef _PATH_DEFPATH
#define _PATH_DEFPATH "C:\\Windows\\System32;C:\\Windows"
#endif

#endif /* TMUX_WIN32_COMPAT_H */
