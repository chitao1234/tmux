/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef TMUX_GLOB_H
#define TMUX_GLOB_H

#ifdef TMUX_WIN32
#include <sys/types.h>

#define GLOB_NOSPACE 1
#define GLOB_ABORTED 2
#define GLOB_NOMATCH 3

#define GLOB_ERR 0x01
#define GLOB_MARK 0x02
#define GLOB_NOSORT 0x04
#define GLOB_NOCHECK 0x08
#define GLOB_NOESCAPE 0x10

typedef struct {
	size_t	 gl_pathc;
	char	**gl_pathv;
	size_t	 gl_offs;
} glob_t;

int	 glob(const char *, int, int (*)(const char *, int), glob_t *);
void	 globfree(glob_t *);
#else
#include_next <glob.h>
#endif

#endif /* TMUX_GLOB_H */
