/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef TMUX_RESOLV_H
#define TMUX_RESOLV_H

#ifdef TMUX_WIN32
#include <sys/types.h>

int	 b64_ntop(const u_char *, size_t, char *, size_t);
int	 b64_pton(const char *, u_char *, size_t);
#else
#include_next <resolv.h>
#endif

#endif /* TMUX_RESOLV_H */
