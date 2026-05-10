/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef TMUX_LANGINFO_H
#define TMUX_LANGINFO_H

#ifdef TMUX_WIN32
typedef int nl_item;

#ifndef CODESET
#define CODESET 1
#endif

static inline char *
nl_langinfo(nl_item item)
{
	(void)item;
	return ((char *)"UTF-8");
}
#else
#include_next <langinfo.h>
#endif

#endif /* TMUX_LANGINFO_H */
