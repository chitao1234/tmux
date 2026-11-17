/* $OpenBSD: cmd-server-access.c,v 1.7 2026/10/09 13:12:14 nicm Exp $ */

/*
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

#include <sys/stat.h>
#include <sys/types.h>

#ifndef TMUX_WIN32
#include <grp.h>
#include <pwd.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Controls access to session.
 *
 * The mutation path is shared: it resolves a name to an id_t and then calls
 * into server-acl.c. Only the resolution differs, because a POSIX id_t is a
 * numeric uid/gid while a Windows id_t is a string SID.
 */

static enum cmd_retval cmd_server_access_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_server_access_entry = {
	.name = "server-access",
	.description = "Manage access to the tmux server.",
	.alias = NULL,

	.args = { "adglrw", 0, 1, NULL },
	.usage = "[-adglrw] " CMD_TARGET_PANE_USAGE " [user|group]",

	.flags = CMD_CLIENT_CANFAIL,
	.exec = cmd_server_access_exec
};

static enum cmd_retval
cmd_server_access_deny(struct cmdq_item *item, id_t id, int flags,
    const char *type, const char *name)
{
	if (!server_acl_find(id, flags)) {
		cmdq_error(item, "%s %s not found", type, name);
		return (CMD_RETURN_ERROR);
	}
	server_acl_deny(id, flags);
	return (CMD_RETURN_NORMAL);
}

/*
 * Resolve the argument to an id_t.
 *
 * On Windows the id is a SID string that must be freed once the tree holds its
 * own copy, and there are no groups, so -g is rejected rather than silently
 * ignored.
 */
static enum cmd_retval
cmd_server_access_resolve(struct cmdq_item *item, struct args *args,
    const char *arg, id_t *idp, const char **namep, int *flagsp,
    const char **typep, char **ownedp)
{
#ifdef TMUX_WIN32
	char	*sid, *cause = NULL;
#endif

	*ownedp = NULL;
#ifdef TMUX_WIN32
	if (args_has(args, 'g')) {
		cmdq_error(item, "groups are not supported on Windows");
		return (CMD_RETURN_ERROR);
	}
#endif
#ifdef TMUX_WIN32
	*typep = "user";
	sid = win32_ipc_sid_from_name(arg, &cause);
	if (sid == NULL) {
		cmdq_error(item, "unknown user: %s (%s)", arg,
		    cause != NULL ? cause : "not found");
		free(cause);
		return (CMD_RETURN_ERROR);
	}
	*idp = sid;
	*ownedp = sid;
	*namep = arg;
	*flagsp = 0;
#else
	{
		struct passwd	*pw;
		struct group	*gr;

		if (args_has(args, 'g')) {
			*typep = "group";
			if ((gr = getgrnam(arg)) == NULL) {
				cmdq_error(item, "unknown group: %s", arg);
				return (CMD_RETURN_ERROR);
			}
			*idp = gr->gr_gid;
			*namep = gr->gr_name;
			*flagsp = SERVER_ACL_IS_GROUP;
			return (CMD_RETURN_NORMAL);
		}
		*typep = "user";
		if ((pw = getpwnam(arg)) == NULL) {
			cmdq_error(item, "unknown user: %s", arg);
			return (CMD_RETURN_ERROR);
		}
		*idp = pw->pw_uid;
		*namep = pw->pw_name;
		*flagsp = 0;
	}
#endif
	return (CMD_RETURN_NORMAL);
}

/* Does this id refer to the server owner? */
static int
cmd_server_access_is_owner(id_t id, int flags)
{
#ifdef TMUX_WIN32
	const char	*me = win32_ipc_current_user_sid();

	if (me == NULL)
		return (0);
	return (strcmp(id, me) == 0);
#else
	if (flags & SERVER_ACL_IS_GROUP)
		return (0);
	return (id == 0 || id == getuid());
#endif
}

static enum cmd_retval
cmd_server_access_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args	*args = cmd_get_args(self);
	struct client	*c = cmdq_get_target_client(item);
	char		*arg, *owned = NULL;
	const char	*name = NULL, *type = NULL;
	id_t		 id;
	int		 flags = 0;
	enum cmd_retval	 retval;

	if (args_has(args, 'l')) {
		server_acl_display(item);
		return (CMD_RETURN_NORMAL);
	}
	if (args_count(args) == 0) {
		cmdq_error(item, "missing user or group argument");
		return (CMD_RETURN_ERROR);
	}

	arg = format_single(item, args_string(args, 0), c, NULL, NULL, NULL);
	retval = cmd_server_access_resolve(item, args, arg, &id, &name,
	    &flags, &type, &owned);
	free(arg);
	if (retval != CMD_RETURN_NORMAL)
		return (retval);

	if (cmd_server_access_is_owner(id, flags)) {
		cmdq_error(item, "%s owns the server, can't change access",
		    name);
		free(owned);
		return (CMD_RETURN_ERROR);
	}

	if (args_has(args, 'a') && args_has(args, 'd')) {
		cmdq_error(item, "-a and -d cannot be used together");
		free(owned);
		return (CMD_RETURN_ERROR);
	}
	if (args_has(args, 'w') && args_has(args, 'r')) {
		cmdq_error(item, "-r and -w cannot be used together");
		free(owned);
		return (CMD_RETURN_ERROR);
	}

	if (args_has(args, 'd')) {
		retval = cmd_server_access_deny(item, id, flags, type, name);
		free(owned);
		return (retval);
	}
	if (args_has(args, 'a')) {
		if (server_acl_find(id, flags)) {
			cmdq_error(item, "%s %s is already added", type, name);
			free(owned);
			return (CMD_RETURN_ERROR);
		}
		server_acl_allow(id, flags);
		/* Do not return - allow -r or -w with -a. */
	} else if (args_has(args, 'r') || args_has(args, 'w')) {
		/* -r or -w implies -a if the entry does not exist. */
		if (!server_acl_find(id, flags))
			server_acl_allow(id, flags);
	}
	free(owned);

	if (args_has(args, 'w')) {
		if (!server_acl_find(id, flags)) {
			cmdq_error(item, "%s %s not found", type, name);
			return (CMD_RETURN_ERROR);
		}
		server_acl_allow_write(id, flags);
		return (CMD_RETURN_NORMAL);
	}

	if (args_has(args, 'r')) {
		if (!server_acl_find(id, flags)) {
			cmdq_error(item, "%s %s not found", type, name);
			return (CMD_RETURN_ERROR);
		}
		server_acl_deny_write(id, flags);
		return (CMD_RETURN_NORMAL);
	}

	return (CMD_RETURN_NORMAL);
}
