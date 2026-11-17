/* $OpenBSD: server-acl.c,v 1.3 2026/06/08 21:38:19 nicm Exp $ */

/*
 * Copyright (c) 2021 Holland Schutte, Jayson Morberg
 * Copyright (c) 2021 Dallas Lyons <dallasdlyons@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#ifndef TMUX_WIN32
#include <sys/stat.h>
#include <sys/socket.h>
#endif

#include <ctype.h>
#ifndef TMUX_WIN32
#include <grp.h>
#include <pwd.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * The access list is keyed by id_t. On POSIX that is a numeric uid or gid, on
 * Windows it is a string SID (see compat/win32-compat.h). Everything below
 * except the three identity helpers is shared: insertion, removal, the
 * read-only flag, matching a client and updating clients all work the same way
 * on both, because they only ever compare one id_t against another.
 *
 * This list is an application-level filter. It decides whether a client that
 * already reached the server may stay; it never widens the transport.
 */
struct server_acl_entry {
	id_t				 id;

	int				 flags;

	RB_ENTRY(server_acl_entry)	 entry;
};

static int
server_acl_cmp(struct server_acl_entry *entry1,
    struct server_acl_entry *entry2)
{
#ifdef TMUX_WIN32
	/* Windows has no groups, so only the SID string is compared. */
	int	comparison;

	if (entry1->id == NULL)
		return (entry2->id == NULL ? 0 : -1);
	if (entry2->id == NULL)
		return (1);
	comparison = strcmp(entry1->id, entry2->id);
	if (comparison < 0)
		return (-1);
	return (comparison > 0);
#else
	if ((entry1->flags ^ entry2->flags) & SERVER_ACL_IS_GROUP) {
		if (entry1->flags & SERVER_ACL_IS_GROUP)
			return (1);
		return (-1);
	}

	if (entry1->id < entry2->id)
		return (-1);
	return (entry1->id > entry2->id);
#endif
}

RB_HEAD(server_acl_entries, server_acl_entry) server_acl_entries;
RB_GENERATE_STATIC(server_acl_entries, server_acl_entry, entry, server_acl_cmp);

static struct server_acl_entry *
server_acl_entry_find(id_t id, int flags)
{
	struct server_acl_entry	find = {
		.id = id,
		.flags = flags & SERVER_ACL_IS_GROUP
	};

#ifdef TMUX_WIN32
	if (id == NULL)
		return (NULL);
#endif
	return (RB_FIND(server_acl_entries, &server_acl_entries, &find));
}

static struct server_acl_entry *
server_acl_check(struct client *c)
{
	struct server_acl_entry	*entry;
#ifdef TMUX_WIN32
	const char		*sid;
#else
	uid_t			 uid;
	gid_t			 gid;
#endif

#ifdef TMUX_WIN32
	/*
	 * A Windows peer is identified by SID and has no group identity, so
	 * there is only one lookup to make.
	 */
	sid = proc_get_peer_user_sid(c->peer);
	if (sid == NULL)
		return (NULL);
	return (server_acl_entry_find(sid, 0));
#else
	uid = proc_get_peer_uid(c->peer);
	if (uid == (uid_t)-1)
		return (NULL);

	entry = server_acl_entry_find(uid, 0);
	if (entry != NULL)
		return (entry);

	gid = proc_get_peer_gid(c->peer);
	if (gid == (gid_t)-1)
		return (NULL);

	return (server_acl_entry_find(gid, SERVER_ACL_IS_GROUP));
#endif
}

static void
server_acl_update(void)
{
	struct server_acl_entry	*entry;
	struct client		*c;

	TAILQ_FOREACH(c, &clients, entry) {
		entry = server_acl_check(c);
		if (entry == NULL) {
			c->exit_message = xstrdup("access not allowed");
			c->flags |= CLIENT_EXIT;
		} else if (entry->flags & SERVER_ACL_READONLY)
			c->flags |= CLIENT_READONLY;
		else
			c->flags &= ~CLIENT_READONLY;
	}
}

/* Initialize ACL tree. */
void
server_acl_init(void)
{
	id_t	me;

	RB_INIT(&server_acl_entries);

#ifdef TMUX_WIN32
	me = win32_ipc_current_user_sid();
	if (me == NULL)
		return;
	server_acl_allow(me, 0);
#else
	if (getuid() != 0)
		server_acl_allow(0, 0);
	me = getuid();
	server_acl_allow(me, 0);
#endif
}

/* Check if an ACL entry exists. */
int
server_acl_find(id_t id, int flags)
{
	return (server_acl_entry_find(id, flags) != NULL);
}

/* Display the tree. */
void
server_acl_display(struct cmdq_item *item)
{
	struct server_acl_entry	*loop;
	const char		*name;
	char			 type;
#ifdef TMUX_WIN32
	char			*owned;
#else
	struct passwd		*pw;
	struct group		*gr;
#endif

	RB_FOREACH(loop, server_acl_entries, &server_acl_entries) {
#ifdef TMUX_WIN32
		owned = win32_ipc_name_from_sid(loop->id, NULL);
		if (owned != NULL)
			name = owned;
		else
			name = loop->id;
		type = 'U';
		if (loop->flags & SERVER_ACL_READONLY)
			cmdq_print(item, "%s (%c,R)", name, type);
		else
			cmdq_print(item, "%s (%c,W)", name, type);
		free(owned);
#else
		if (~loop->flags & SERVER_ACL_IS_GROUP) {
			if (loop->id == 0)
				continue;
			if ((pw = getpwuid(loop->id)) != NULL)
				name = pw->pw_name;
			else
				name = "unknown";
			type = 'U';
		} else {
			if ((gr = getgrgid(loop->id)) != NULL)
				name = gr->gr_name;
			else
				name = "unknown";
			type = 'G';
		}
		if (loop->flags & SERVER_ACL_READONLY)
			cmdq_print(item, "%s (%c,R)", name, type);
		else
			cmdq_print(item, "%s (%c,W)", name, type);
#endif
	}
}

/* Allow an ACL entry. */
void
server_acl_allow(id_t id, int flags)
{
	struct server_acl_entry	*entry;

#ifdef TMUX_WIN32
	if (id == NULL)
		return;
#endif
	entry = server_acl_entry_find(id, flags);
	if (entry == NULL) {
		entry = xcalloc(1, sizeof *entry);
#ifdef TMUX_WIN32
		/*
		 * The tree owns its key: the caller's string is its own, and
		 * the caller frees it after this returns.
		 */
		entry->id = xstrdup(id);
#else
		entry->id = id;
#endif
		entry->flags = flags & SERVER_ACL_IS_GROUP;
		RB_INSERT(server_acl_entries, &server_acl_entries, entry);
	}
}

/* Deny an ACL entry (remove it from the tree). */
void
server_acl_deny(id_t id, int flags)
{
	struct server_acl_entry	*entry;

	entry = server_acl_entry_find(id, flags);
	if (entry != NULL) {
		RB_REMOVE(server_acl_entries, &server_acl_entries, entry);
#ifdef TMUX_WIN32
		free((void *)entry->id);
#endif
		free(entry);
		server_acl_update();
	}
}

/* Allow this ACL entry write access. */
void
server_acl_allow_write(id_t id, int flags)
{
	struct server_acl_entry	*entry;

	entry = server_acl_entry_find(id, flags);
	if (entry == NULL)
		return;
	entry->flags &= ~SERVER_ACL_READONLY;
	server_acl_update();
}

/* Deny this ACL entry write access. */
void
server_acl_deny_write(id_t id, int flags)
{
	struct server_acl_entry	*entry;

	entry = server_acl_entry_find(id, flags);
	if (entry == NULL)
		return;
	entry->flags |= SERVER_ACL_READONLY;
	server_acl_update();
}

/*
 * Check if the client's UID or GID exists in the ACL list and if so, set as
 * read only if needed. UID entries take precedence over GID entries. Return
 * false if no entry exists.
 */
int
server_acl_join(struct client *c)
{
	struct server_acl_entry	*entry;

	entry = server_acl_check(c);
	if (entry == NULL)
		return (0);
	if (entry->flags & SERVER_ACL_READONLY)
		c->flags |= CLIENT_READONLY;
	return (1);
}
