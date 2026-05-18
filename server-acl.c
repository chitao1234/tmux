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
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
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

struct server_acl_user {
#ifdef TMUX_WIN32
	char				*sid;
#else
	uid_t				uid;
#endif

	int				flags;
#define SERVER_ACL_READONLY 0x1

	RB_ENTRY(server_acl_user)	entry;
};

static int
server_acl_cmp(struct server_acl_user *user1, struct server_acl_user *user2)
{
#ifdef TMUX_WIN32
	if (user1->sid == NULL)
		return (user2->sid == NULL ? 0 : -1);
	if (user2->sid == NULL)
		return (1);
	return (strcmp(user1->sid, user2->sid));
#else
	if (user1->uid < user2->uid)
		return (-1);
	return (user1->uid > user2->uid);
#endif
}

RB_HEAD(server_acl_entries, server_acl_user) server_acl_entries;
RB_GENERATE_STATIC(server_acl_entries, server_acl_user, entry, server_acl_cmp);

static void	server_acl_free_user(struct server_acl_user *);
static int	server_acl_user_matches_peer(struct server_acl_user *,
		    struct tmuxpeer *);

/* Initialize server_acl tree. */
void
server_acl_init(void)
{
#ifdef TMUX_WIN32
	const char	*sid;
#endif

	RB_INIT(&server_acl_entries);

#ifndef TMUX_WIN32
	if (getuid() != 0)
		server_acl_user_allow(0);
	server_acl_user_allow(getuid());
#else
	sid = win32_ipc_current_user_sid();
	if (sid != NULL)
		server_acl_user_allow_sid(sid);
#endif
}

/* Find user entry. */
#ifndef TMUX_WIN32
struct server_acl_user*
server_acl_user_find(uid_t uid)
{
	struct server_acl_user	find = { .uid = uid };

	return (RB_FIND(server_acl_entries, &server_acl_entries, &find));
}
#else
struct server_acl_user*
server_acl_user_find_sid(const char *sid)
{
	struct server_acl_user	find = { .sid = (char *)sid };

	if (sid == NULL || *sid == '\0')
		return (NULL);
	return (RB_FIND(server_acl_entries, &server_acl_entries, &find));
}
#endif

/* Display the tree. */
void
server_acl_display(struct cmdq_item *item)
{
	struct server_acl_user	*loop;
	const char		*name;
#ifndef TMUX_WIN32
	struct passwd		*pw;
#endif

	RB_FOREACH(loop, server_acl_entries, &server_acl_entries) {
#ifndef TMUX_WIN32
		if (loop->uid == 0)
			continue;
		if ((pw = getpwuid(loop->uid)) != NULL)
			name = pw->pw_name;
		else
			name = "unknown";
#else
		name = loop->sid;
#endif
		if (loop->flags == SERVER_ACL_READONLY)
			cmdq_print(item, "%s (R)", name);
		else
			cmdq_print(item, "%s (W)", name);
	}
}

/* Allow a user. */
#ifndef TMUX_WIN32
void
server_acl_user_allow(uid_t uid)
{
	struct server_acl_user	*user;

	user = server_acl_user_find(uid);
	if (user == NULL) {
		user = xcalloc(1, sizeof *user);
		user->uid = uid;
		RB_INSERT(server_acl_entries, &server_acl_entries, user);
	}
}
#else
void
server_acl_user_allow_sid(const char *sid)
{
	struct server_acl_user	*user;

	if (sid == NULL || *sid == '\0')
		return;
	user = server_acl_user_find_sid(sid);
	if (user == NULL) {
		user = xcalloc(1, sizeof *user);
		user->sid = xstrdup(sid);
		RB_INSERT(server_acl_entries, &server_acl_entries, user);
	}
}
#endif

/* Deny a user (remove from the tree). */
#ifndef TMUX_WIN32
void
server_acl_user_deny(uid_t uid)
{
	struct server_acl_user	*user;

	user = server_acl_user_find(uid);
	if (user != NULL) {
		RB_REMOVE(server_acl_entries, &server_acl_entries, user);
		server_acl_free_user(user);
	}
}
#else
void
server_acl_user_deny_sid(const char *sid)
{
	struct server_acl_user	*user;

	if (sid == NULL || *sid == '\0')
		return;
	user = server_acl_user_find_sid(sid);
	if (user != NULL) {
		RB_REMOVE(server_acl_entries, &server_acl_entries, user);
		server_acl_free_user(user);
	}
}
#endif

/* Allow this user write access. */
#ifndef TMUX_WIN32
void
server_acl_user_allow_write(uid_t uid)
{
	struct server_acl_user	*user;
	struct client		*c;

	user = server_acl_user_find(uid);
	if (user == NULL)
		return;
	user->flags &= ~SERVER_ACL_READONLY;

	TAILQ_FOREACH(c, &clients, entry) {
		if (server_acl_user_matches_peer(user, c->peer))
			c->flags &= ~CLIENT_READONLY;
	}
}
#else
void
server_acl_user_allow_write_sid(const char *sid)
{
	struct server_acl_user	*user;
	struct client		*c;

	if (sid == NULL || *sid == '\0')
		return;
	user = server_acl_user_find_sid(sid);
	if (user == NULL)
		return;
	user->flags &= ~SERVER_ACL_READONLY;

	TAILQ_FOREACH(c, &clients, entry) {
		if (server_acl_user_matches_peer(user, c->peer))
			c->flags &= ~CLIENT_READONLY;
	}
}
#endif

/* Deny this user write access. */
#ifndef TMUX_WIN32
void
server_acl_user_deny_write(uid_t uid)
{
	struct server_acl_user	*user;
	struct client		*c;

	user = server_acl_user_find(uid);
	if (user == NULL)
		return;
	user->flags |= SERVER_ACL_READONLY;

	TAILQ_FOREACH(c, &clients, entry) {
		if (server_acl_user_matches_peer(user, c->peer))
			c->flags |= CLIENT_READONLY;
	}
}
#else
void
server_acl_user_deny_write_sid(const char *sid)
{
	struct server_acl_user	*user;
	struct client		*c;

	if (sid == NULL || *sid == '\0')
		return;
	user = server_acl_user_find_sid(sid);
	if (user == NULL)
		return;
	user->flags |= SERVER_ACL_READONLY;

	TAILQ_FOREACH(c, &clients, entry) {
		if (server_acl_user_matches_peer(user, c->peer))
			c->flags |= CLIENT_READONLY;
	}
}
#endif

/*
 * Check if the client's UID exists in the ACL list and if so, set as read only
 * if needed. Return false if the user does not exist.
 */
int
server_acl_join(struct client *c)
{
	struct server_acl_user	*user;
#ifndef TMUX_WIN32
	uid_t			 uid;
#endif

#ifdef TMUX_WIN32
	user = server_acl_user_find_sid(proc_get_peer_user_sid(c->peer));
#else
	uid = proc_get_peer_uid(c->peer);
	if (uid == (uid_t)-1)
		return (0);

	user = server_acl_user_find(uid);
#endif
	if (user == NULL)
		return (0);
	if (user->flags & SERVER_ACL_READONLY)
		c->flags |= CLIENT_READONLY;
	return (1);
}

/* Get UID for user entry. */
#ifndef TMUX_WIN32
uid_t
server_acl_get_uid(struct server_acl_user *user)
{
	return (user->uid);
}
#else
const char *
server_acl_get_sid(struct server_acl_user *user)
{
	return (user->sid);
}
#endif

static void
server_acl_free_user(struct server_acl_user *user)
{
#ifdef TMUX_WIN32
	free(user->sid);
#endif
	free(user);
}

static int
server_acl_user_matches_peer(struct server_acl_user *user, struct tmuxpeer *peer)
{
#ifdef TMUX_WIN32
	const char	*sid;

	if (user == NULL || peer == NULL)
		return (0);
	sid = proc_get_peer_user_sid(peer);
	if (sid == NULL)
		return (0);
	return (strcmp(user->sid, sid) == 0);
#else
	uid_t	 uid;

	if (user == NULL || peer == NULL)
		return (0);
	uid = proc_get_peer_uid(peer);
	if (uid == (uid_t)-1)
		return (0);
	return (uid == user->uid);
#endif
}
