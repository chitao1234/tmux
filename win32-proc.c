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

void
win32_check_children(void)
{
	struct window		*w, *w1;
	struct window_pane	*wp, *wp1;
	int			 status;

	RB_FOREACH_SAFE(w, windows, &windows, w1) {
		TAILQ_FOREACH_SAFE(wp, &w->panes, entry, wp1) {
			if (wp->win32 == NULL)
				continue;
			if (wp->flags & PANE_STATUSREADY)
				continue;
			if (!win32_pane_exited(wp, &status))
				continue;
			wp->status = status;
			wp->flags |= PANE_STATUSREADY;
			if (window_pane_destroy_ready(wp))
				server_destroy_pane(wp, 1);
		}
	}
	job_check_died();
}

int
win32_server_spawn(const char *path, uint64_t flags, char **cause)
{
	wchar_t		 exe[MAX_PATH], *cmd;
	STARTUPINFOW	 si;
	PROCESS_INFORMATION pi;
	DWORD		 n;
	BOOL		 ok;
	char		*exe_utf8, **argv;
	int		 argc, i, log_level;

	n = GetModuleFileNameW(NULL, exe, MAX_PATH);
	if (n == 0 || n == MAX_PATH) {
		xasprintf(cause, "GetModuleFileName failed: %s",
		    win32_strerror(GetLastError()));
		return (-1);
	}
	exe_utf8 = win32_wide_to_utf8(exe);
	if (exe_utf8 == NULL) {
		xasprintf(cause, "couldn't convert executable path");
		return (-1);
	}
	log_level = log_get_level();
	argc = 5 + log_level + ((flags & CLIENT_WIN32_HELPER) != 0);
	argv = xcalloc((size_t)argc, sizeof *argv);
	i = 0;
	argv[i++] = exe_utf8;
	argv[i++] = xstrdup("-D");
	while (log_level-- > 0)
		argv[i++] = xstrdup("-v");
	argv[i++] = xstrdup("-S");
	argv[i++] = xstrdup(path);
	if (flags & CLIENT_WIN32_HELPER)
		argv[i++] = xstrdup("-w");
	cmd = win32_build_argv_command(i, argv);
	while (i-- > 0)
		free(argv[i]);
	free(argv);
	if (cmd == NULL) {
		xasprintf(cause, "couldn't build server command line");
		return (-1);
	}

	memset(&si, 0, sizeof si);
	memset(&pi, 0, sizeof pi);
	si.cb = sizeof si;
	ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE,
	    CREATE_NEW_PROCESS_GROUP|DETACHED_PROCESS, NULL, NULL, &si, &pi);
	free(cmd);
	if (!ok) {
		xasprintf(cause, "CreateProcess server failed: %s",
		    win32_strerror(GetLastError()));
		return (-1);
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	(void)flags;
	return (0);
}

#endif /* TMUX_WIN32 */
