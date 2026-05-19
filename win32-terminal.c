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
static UINT	saved_input_cp;
static UINT	saved_output_cp;
static int	saved_modes;

static DWORD
win32_terminal_client_input_mode(int mouse_active)
{
	DWORD	mode;

	mode = saved_in_mode;
	mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
	mode &= ~(ENABLE_ECHO_INPUT|ENABLE_LINE_INPUT|ENABLE_PROCESSED_INPUT);
	if (mouse_active) {
		mode |= ENABLE_EXTENDED_FLAGS;
#ifdef ENABLE_QUICK_EDIT_MODE
		mode &= ~ENABLE_QUICK_EDIT_MODE;
#endif
	}
	return (mode);
}

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
	win32_log_handle("client stdin before init", hin);
	win32_log_handle("client stdout before init", hout);
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
	log_debug("%s: saved console modes in %#lx out %#lx", __func__,
	    (unsigned long)saved_in_mode, (unsigned long)saved_out_mode);
	saved_input_cp = GetConsoleCP();
	saved_output_cp = GetConsoleOutputCP();
	log_debug("%s: saved code pages in %u out %u", __func__,
	    saved_input_cp, saved_output_cp);
	saved_modes = 1;

	mode = win32_terminal_client_input_mode(0);
	if (!SetConsoleMode(hin, mode)) {
		xasprintf(cause, "couldn't set console input mode: %s",
		    win32_strerror(GetLastError()));
		goto fail;
	}

	mode = saved_out_mode;
	mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
#ifdef DISABLE_NEWLINE_AUTO_RETURN
	mode |= DISABLE_NEWLINE_AUTO_RETURN;
#endif
	if (!SetConsoleMode(hout, mode)) {
#ifdef DISABLE_NEWLINE_AUTO_RETURN
		mode &= ~DISABLE_NEWLINE_AUTO_RETURN;
		if (!SetConsoleMode(hout, mode))
#endif
		{
			xasprintf(cause, "couldn't set console output mode: %s",
			    win32_strerror(GetLastError()));
			goto fail;
		}
	}
	SetConsoleCP(CP_UTF8);
	SetConsoleOutputCP(CP_UTF8);
	win32_log_handle("client stdin after init", hin);
	win32_log_handle("client stdout after init", hout);
	return (0);

fail:
	SetConsoleMode(hin, saved_in_mode);
	SetConsoleMode(hout, saved_out_mode);
	if (saved_input_cp != 0)
		SetConsoleCP(saved_input_cp);
	if (saved_output_cp != 0)
		SetConsoleOutputCP(saved_output_cp);
	saved_modes = 0;
	return (-1);
}

void
win32_terminal_restore_client(void)
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
	if (saved_input_cp != 0)
		SetConsoleCP(saved_input_cp);
	if (saved_output_cp != 0)
		SetConsoleOutputCP(saved_output_cp);
	win32_log_handle("client stdin after restore", hin);
	win32_log_handle("client stdout after restore", hout);
	saved_modes = 0;
}

int
win32_terminal_set_client_mouse_mode(uint32_t mouse_mode)
{
	HANDLE	hin;
	DWORD	mode;

	if (!saved_modes)
		return (0);
	hin = GetStdHandle(STD_INPUT_HANDLE);
	if (hin == INVALID_HANDLE_VALUE)
		return (-1);
	mode = win32_terminal_client_input_mode(mouse_mode != 0);
	if (!SetConsoleMode(hin, mode)) {
		log_debug("%s: couldn't set console input mode: %s", __func__,
		    win32_strerror(GetLastError()));
		return (-1);
	}
	log_debug("%s: console mouse mode %#x -> input mode %#lx", __func__,
	    mouse_mode, (unsigned long)mode);
	return (0);
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
	    !GetConsoleScreenBufferInfo(hout, &csbi)) {
		log_debug("%s: GetConsoleScreenBufferInfo failed: %s", __func__,
		    win32_strerror(GetLastError()));
		return (-1);
	}
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
