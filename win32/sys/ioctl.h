/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef TMUX_SYS_IOCTL_H
#define TMUX_SYS_IOCTL_H

#ifdef TMUX_WIN32
#include "../compat/win32-compat.h"

#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
#endif
#ifndef TIOCSWINSZ
#define TIOCSWINSZ 0x5414
#endif
#ifndef FIONREAD
#define FIONREAD 0x541b
#endif

int	 ioctl(int, unsigned long, ...);
#else
#include_next <sys/ioctl.h>
#endif

#endif /* TMUX_SYS_IOCTL_H */
