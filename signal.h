/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef TMUX_SIGNAL_H
#define TMUX_SIGNAL_H

#include_next <signal.h>

#ifdef TMUX_WIN32
#ifndef SIGHUP
#define SIGHUP 1
#endif
#endif

#endif /* TMUX_SIGNAL_H */
