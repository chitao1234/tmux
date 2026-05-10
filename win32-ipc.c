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

static char *
win32_pipe_name(const char *label)
{
	char	*name, *copy, *cp;

	copy = xstrdup(label == NULL ? "default" : label);
	for (cp = copy; *cp != '\0'; cp++) {
		if (*cp == '/' || *cp == '\\' || *cp == ':' || *cp == ' ')
			*cp = '_';
	}
	xasprintf(&name, "\\\\.\\pipe\\tmux-%lu-%s",
	    (unsigned long)GetCurrentProcessId(), copy);
	free(copy);
	return (name);
}

int
win32_ipc_server_create(const char *path, char **cause)
{
	(void)path;
	xasprintf(cause, "native Win32 named-pipe IPC is not connected yet");
	errno = ENOSYS;
	return (-1);
}

int
win32_ipc_client_connect(const char *path, __unused uint64_t flags,
    char **cause)
{
	char	*name;

	name = win32_pipe_name(path);
	xasprintf(cause, "couldn't connect to %s: IPC bridge not connected yet",
	    name);
	free(name);
	errno = ENOSYS;
	return (-1);
}

#endif /* TMUX_WIN32 */
