/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef TMUX_TERMIOS_H
#define TMUX_TERMIOS_H

#ifdef TMUX_WIN32
#include "compat/win32-compat.h"
#else
#include_next <termios.h>
#endif

#endif /* TMUX_TERMIOS_H */
