/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef TMUX_SYS_UTSNAME_H
#define TMUX_SYS_UTSNAME_H

#ifdef TMUX_WIN32
struct utsname {
	char	sysname[32];
	char	nodename[256];
	char	release[32];
	char	version[128];
	char	machine[32];
};

int	 uname(struct utsname *);
#else
#include_next <sys/utsname.h>
#endif

#endif /* TMUX_SYS_UTSNAME_H */
