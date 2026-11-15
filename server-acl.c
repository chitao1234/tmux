/* $OpenBSD$ */

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
#include <pwd.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

/*
 * The access list is keyed on an opaque identity string, so the tree and every
 * operation on it are shared between platforms:
 *
 *   Unix     decimal uid ("1000", "0")
 *   Windows  string SID ("S-1-5-21-...-1001")
 *
 * Only the four identity helpers at the bottom of this file know the
 * difference: name to key, key to display name, the owner's key, and a peer's
 * key. Everything else - insertion, removal, read-only flags, matching a
 * client to an entry, display - is one implementation.
 *
 * This list is an application-level filter. It decides whether a client that
 * already reached the server may stay; it never widens the transport.
 */
struct server_acl_user {
	char	*id;

	int	 flags;
#define SERVER_ACL_READONLY 0x1

	RB_ENTRY(server_acl_user)	entry;
};

static int
server_acl_cmp(struct server_acl_user *user1, struct server_acl_user *user2)
{
	return (strcmp(user1->id, user2->id));
}

RB_HEAD(server_acl_entries, server_acl_user) server_acl_entries;
RB_GENERATE_STATIC(server_acl_entries, server_acl_user, entry, server_acl_cmp);

static void	server_acl_free_user(struct server_acl_user *);
static int	server_acl_user_matches_peer(struct server_acl_user *,
		    struct tmuxpeer *);

/*
 * Owner key. On Unix this also means "the account that must be present but is
 * never shown": root and the owner's own uid are implicit.
 */
char *
server_acl_owner_id(void)
{
#ifndef TMUX_WIN32
	char	*out;

	xasprintf(&out, "%ld", (long)getuid());
	return (out);
#else
	const char	*sid = win32_ipc_current_user_sid();

	if (sid == NULL)
		return (NULL);
	return (xstrdup(sid));
#endif
}

char *
server_acl_id_from_name(const char *name, char **cause)
{
#ifndef TMUX_WIN32
	struct passwd	*pw;
	char		*out;

	if (cause != NULL)
		*cause = NULL;
	if (name == NULL || *name == '\0') {
		if (cause != NULL)
			xasprintf(cause, "empty user name");
		return (NULL);
	}
	if ((pw = getpwnam(name)) == NULL) {
		if (cause != NULL)
			xasprintf(cause, "no such account");
		return (NULL);
	}
	xasprintf(&out, "%ld", (long)pw->pw_uid);
	return (out);
#else
	return (win32_ipc_sid_from_name(name, cause));
#endif
}

char *
server_acl_id_to_name(const char *id)
{
#ifndef TMUX_WIN32
	struct passwd	*pw;
	char		*end;
	long		 uid;

	if (id == NULL)
		return (xstrdup("unknown"));
	uid = strtol(id, &end, 10);
	if (*end != '\0')
		return (xstrdup("unknown"));
	if ((pw = getpwuid((uid_t)uid)) == NULL)
		return (xstrdup("unknown"));
	return (xstrdup(pw->pw_name));
#else
	char	*name;

	name = win32_ipc_name_from_sid(id, NULL);
	if (name == NULL)
		return (xstrdup(id != NULL ? id : "unknown"));
	return (name);
#endif
}

static char *
server_acl_peer_id(struct tmuxpeer *peer)
{
#ifndef TMUX_WIN32
	uid_t	uid;
	char	*out;

	if (peer == NULL)
		return (NULL);
	if ((uid = proc_get_peer_uid(peer)) == (uid_t)-1)
		return (NULL);
	xasprintf(&out, "%ld", (long)uid);
	return (out);
#else
	const char	*sid;

	if (peer == NULL)
		return (NULL);
	if ((sid = proc_get_peer_user_sid(peer)) == NULL)
		return (NULL);
	return (xstrdup(sid));
#endif
}

/* Initialize server_acl tree. */
void
server_acl_init(void)
{
	char	*owner;

	RB_INIT(&server_acl_entries);

#ifndef TMUX_WIN32
	/* root is always allowed but never displayed */
	server_acl_user_allow("0");
#endif
	if ((owner = server_acl_owner_id()) != NULL) {
		server_acl_user_allow(owner);
		free(owner);
	}
}

/* Find user entry. */
struct server_acl_user *
server_acl_user_find(const char *id)
{
	struct server_acl_user	find = { .id = (char *)id };

	if (id == NULL || *id == '\0')
		return (NULL);
	return (RB_FIND(server_acl_entries, &server_acl_entries, &find));
}

/* Display the tree. */
void
server_acl_display(struct cmdq_item *item)
{
	struct server_acl_user	*loop;
	char			*name;

	RB_FOREACH(loop, server_acl_entries, &server_acl_entries) {
#ifndef TMUX_WIN32
		if (strcmp(loop->id, "0") == 0)
			continue;
#endif
		name = server_acl_id_to_name(loop->id);
		if (loop->flags == SERVER_ACL_READONLY)
			cmdq_print(item, "%s (R)", name);
		else
			cmdq_print(item, "%s (W)", name);
		free(name);
	}
}

/* Allow a user. */
void
server_acl_user_allow(const char *id)
{
	struct server_acl_user	*user;

	if (id == NULL || *id == '\0')
		return;
	if (server_acl_user_find(id) != NULL)
		return;
	user = xcalloc(1, sizeof *user);
	user->id = xstrdup(id);
	RB_INSERT(server_acl_entries, &server_acl_entries, user);
}

/* Deny a user (remove from the tree). */
void
server_acl_user_deny(const char *id)
{
	struct server_acl_user	*user;

	if ((user = server_acl_user_find(id)) == NULL)
		return;
	RB_REMOVE(server_acl_entries, &server_acl_entries, user);
	server_acl_free_user(user);
}

/* Allow this user write access. */
void
server_acl_user_allow_write(const char *id)
{
	struct server_acl_user	*user;
	struct client		*c;

	if ((user = server_acl_user_find(id)) == NULL)
		return;
	user->flags &= ~SERVER_ACL_READONLY;

	TAILQ_FOREACH(c, &clients, entry) {
		if (server_acl_user_matches_peer(user, c->peer))
			c->flags &= ~CLIENT_READONLY;
	}
}

/* Deny this user write access. */
void
server_acl_user_deny_write(const char *id)
{
	struct server_acl_user	*user;
	struct client		*c;

	if ((user = server_acl_user_find(id)) == NULL)
		return;
	user->flags |= SERVER_ACL_READONLY;

	TAILQ_FOREACH(c, &clients, entry) {
		if (server_acl_user_matches_peer(user, c->peer))
			c->flags |= CLIENT_READONLY;
	}
}

/* Get the identity key for a user entry. */
const char *
server_acl_user_id(struct server_acl_user *user)
{
	return (user->id);
}

/*
 * Check if the client's identity exists in the ACL list and if so, set as
 * read only if needed. Return false if the user does not exist.
 */
int
server_acl_join(struct client *c)
{
	struct server_acl_user	*user;
	char			*id;
	int			 retval;

	if ((id = server_acl_peer_id(c->peer)) == NULL)
		return (0);
	user = server_acl_user_find(id);
	free(id);
	if (user == NULL)
		return (0);
	if (user->flags & SERVER_ACL_READONLY)
		c->flags |= CLIENT_READONLY;
	retval = 1;
	return (retval);
}

/*
 * Mark every attached client whose identity matches, so a deny can take
 * effect immediately instead of at the next attach.
 */
void
server_acl_user_disconnect(const char *id)
{
	struct server_acl_user	*user;
	struct client		*c;

	if ((user = server_acl_user_find(id)) == NULL)
		return;
	TAILQ_FOREACH(c, &clients, entry) {
		if (server_acl_user_matches_peer(user, c->peer)) {
			c->exit_message = xstrdup("access not allowed");
			c->flags |= CLIENT_EXIT;
		}
	}
}

static void
server_acl_free_user(struct server_acl_user *user)
{
	free(user->id);
	free(user);
}

static int
server_acl_user_matches_peer(struct server_acl_user *user, struct tmuxpeer *peer)
{
	char	*id;
	int	 match;

	if (user == NULL || peer == NULL)
		return (0);
	if ((id = server_acl_peer_id(peer)) == NULL)
		return (0);
	match = (strcmp(user->id, id) == 0);
	free(id);
	return (match);
}
