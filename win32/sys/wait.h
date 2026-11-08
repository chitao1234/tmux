/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef TMUX_SYS_WAIT_H
#define TMUX_SYS_WAIT_H

#ifdef TMUX_WIN32
#ifndef WAIT_ANY
#define WAIT_ANY (-1)
#endif
#ifndef WNOHANG
#define WNOHANG 1
#endif
#ifndef WUNTRACED
#define WUNTRACED 2
#endif

#ifndef W_EXITCODE
#define W_EXITCODE(ret, sig) (((ret) << 8) | (sig))
#endif
#ifndef WIFEXITED
#define WIFEXITED(status) (((status) & 0x7f) == 0)
#endif
#ifndef WEXITSTATUS
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#endif
#ifndef WIFSIGNALED
#define WIFSIGNALED(status) (((status) & 0x7f) != 0)
#endif
#ifndef WTERMSIG
#define WTERMSIG(status) ((status) & 0x7f)
#endif
#ifndef WIFSTOPPED
#define WIFSTOPPED(status) (0)
#endif
#ifndef WSTOPSIG
#define WSTOPSIG(status) (0)
#endif
#else
#include_next <sys/wait.h>
#endif

#endif /* TMUX_SYS_WAIT_H */
