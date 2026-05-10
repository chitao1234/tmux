/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef TMUX_PWD_H
#define TMUX_PWD_H

#ifdef TMUX_WIN32
#include <sys/types.h>

#ifndef TMUX_WIN32_UID_T_DEFINED
#define TMUX_WIN32_UID_T_DEFINED
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#endif

struct passwd {
	char	*pw_name;
	uid_t	 pw_uid;
	gid_t	 pw_gid;
	char	*pw_dir;
	char	*pw_shell;
};

struct passwd *getpwuid(uid_t);
struct passwd *getpwnam(const char *);
uid_t getuid(void);
#else
#include_next <pwd.h>
#endif

#endif /* TMUX_PWD_H */
