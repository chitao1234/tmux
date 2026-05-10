/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include <sys/types.h>

#include <event.h>
#include <stdlib.h>

#include "tmux.h"

char *
osdep_get_name(__unused int fd, __unused char *tty)
{
	return (NULL);
}

char *
osdep_get_cwd(__unused int fd)
{
	return (NULL);
}

struct event_base *
osdep_event_init(void)
{
	if (win32_init() != 0)
		fatal("WSAStartup failed");
	atexit(win32_fini);
	return (event_init());
}
