/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef TMUX_SYS_UN_H
#define TMUX_SYS_UN_H

#ifdef TMUX_WIN32
#include <sys/types.h>

#ifndef AF_UNIX
#define AF_UNIX 1
#endif

struct sockaddr_un {
	short	sun_family;
	char	sun_path[108];
};
#else
#include_next <sys/un.h>
#endif

#endif /* TMUX_SYS_UN_H */
