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

int
win32_server_spawn(const char *path, uint64_t flags, char **cause)
{
	wchar_t		*wexe = NULL, *cmd;
	STARTUPINFOW	 si;
	PROCESS_INFORMATION pi;
	BOOL		 ok;
	char		*exe_utf8, **argv;
	int		 argc, i, log_level;
	u_int		 j, cfg_args;

	exe_utf8 = win32_get_module_path_utf8();
	if (exe_utf8 == NULL) {
		xasprintf(cause, "couldn't convert executable path");
		return (-1);
	}
	log_level = log_get_level();
	cfg_args = (cfg_quiet ? 0 : 2 * cfg_nfiles);
	argc = 6 + log_level + ((flags & CLIENT_WIN32_HELPER) != 0) +
	    cfg_args;
	argv = xcalloc((size_t)argc, sizeof *argv);
	i = 0;
	argv[i++] = exe_utf8;
	argv[i++] = xstrdup("-D");
	argv[i++] = xstrdup("-W");
	while (log_level-- > 0)
		argv[i++] = xstrdup("-v");
	argv[i++] = xstrdup("-S");
	argv[i++] = xstrdup(path);
	for (j = 0; j < cfg_nfiles && !cfg_quiet; j++) {
		argv[i++] = xstrdup("-f");
		argv[i++] = xstrdup(cfg_files[j]);
	}
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
	wexe = win32_utf8_to_wide(exe_utf8);
	if (wexe == NULL) {
		free(cmd);
		xasprintf(cause, "couldn't convert executable path");
		return (-1);
	}

	memset(&si, 0, sizeof si);
	memset(&pi, 0, sizeof pi);
	si.cb = sizeof si;
	ok = CreateProcessW(wexe, cmd, NULL, NULL, FALSE,
	    CREATE_NEW_PROCESS_GROUP|DETACHED_PROCESS, NULL, NULL, &si, &pi);
	free(wexe);
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
