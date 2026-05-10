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
#include <stdio.h>
#include <stdlib.h>

#include "tmux.h"

#ifdef TMUX_WIN32

static DWORD	saved_in_mode;
static DWORD	saved_out_mode;
static int	saved_modes;

int
win32_terminal_is_client_console(void)
{
	HANDLE	hin, hout;
	DWORD	mode;

	hin = GetStdHandle(STD_INPUT_HANDLE);
	hout = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hin == INVALID_HANDLE_VALUE || hout == INVALID_HANDLE_VALUE)
		return (0);
	if (!GetConsoleMode(hin, &mode) || !GetConsoleMode(hout, &mode))
		return (0);
	return (1);
}

int
win32_terminal_init_client(char **cause)
{
	HANDLE	hin, hout;
	DWORD	mode;

	hin = GetStdHandle(STD_INPUT_HANDLE);
	hout = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hin == INVALID_HANDLE_VALUE || hout == INVALID_HANDLE_VALUE) {
		xasprintf(cause, "couldn't get console handles: %s",
		    win32_strerror(GetLastError()));
		return (-1);
	}
	if (!GetConsoleMode(hin, &saved_in_mode) ||
	    !GetConsoleMode(hout, &saved_out_mode)) {
		xasprintf(cause, "not a Windows console");
		return (-1);
	}
	saved_modes = 1;

	mode = saved_in_mode;
	mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
	mode &= ~(ENABLE_ECHO_INPUT|ENABLE_LINE_INPUT);
	SetConsoleMode(hin, mode);

	mode = saved_out_mode;
	mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
#ifdef DISABLE_NEWLINE_AUTO_RETURN
	mode |= DISABLE_NEWLINE_AUTO_RETURN;
#endif
	SetConsoleMode(hout, mode);
	return (0);
}

void
win32_terminal_restore_client(__unused struct client *c)
{
	HANDLE	hin, hout;

	if (!saved_modes)
		return;
	hin = GetStdHandle(STD_INPUT_HANDLE);
	hout = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hin != INVALID_HANDLE_VALUE)
		SetConsoleMode(hin, saved_in_mode);
	if (hout != INVALID_HANDLE_VALUE)
		SetConsoleMode(hout, saved_out_mode);
}

int
win32_terminal_get_size(__unused struct client *c, u_int *sx, u_int *sy,
    u_int *xpixel, u_int *ypixel)
{
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	HANDLE hout;

	if (c != NULL && c->win32_stdout != NULL)
		hout = c->win32_stdout;
	else
		hout = GetStdHandle(STD_OUTPUT_HANDLE);
	if (hout == INVALID_HANDLE_VALUE ||
	    !GetConsoleScreenBufferInfo(hout, &csbi))
		return (-1);
	*sx = csbi.srWindow.Right - csbi.srWindow.Left + 1;
	*sy = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
	*xpixel = 0;
	*ypixel = 0;
	return (0);
}

void
win32_terminal_check_resize(struct client *c)
{
	u_int sx, sy, xpixel, ypixel;

	if (win32_terminal_get_size(c, &sx, &sy, &xpixel, &ypixel) != 0)
		return;
	if (c->tty.sx != sx || c->tty.sy != sy)
		proc_send(c->peer, MSG_RESIZE, -1, NULL, 0);
}

#endif /* TMUX_WIN32 */
