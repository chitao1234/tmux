/* $OpenBSD$ */

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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "tmux.h"

/*
 * Controls access to session.
 *
 * All platforms share this file: a user is an opaque identity string owned by
 * server-acl.c, so only identity resolution and the "owns the server" rule
 * differ, and those live in server_acl_id_from_name() and
 * server_acl_owner_id().
 */

static enum cmd_retval cmd_server_access_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_server_access_entry = {
	.name = "server-access",
	.alias = NULL,

	.args = { "adlrw", 0, 1, NULL },
	.usage = "[-adlrw] " CMD_TARGET_PANE_USAGE " [user]",

	.flags = CMD_CLIENT_CANFAIL,
	.exec = cmd_server_access_exec
};

static enum cmd_retval
cmd_server_access_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct client		*c = cmdq_get_target_client(item);
	struct server_acl_user	*user;
	char			*name = NULL, *id = NULL, *owner = NULL, *cause = NULL;
	enum cmd_retval		 retval = CMD_RETURN_ERROR;

	if (args_has(args, 'l')) {
		server_acl_display(item);
		return (CMD_RETURN_NORMAL);
	}

	if (args_count(args) == 0) {
		cmdq_error(item, "missing user argument");
		return (CMD_RETURN_ERROR);
	}

	name = format_single(item, args_string(args, 0), c, NULL, NULL, NULL);
	if (*name == '\0') {
		cmdq_error(item, "empty user name");
		goto out;
	}
	id = server_acl_id_from_name(name, &cause);
	if (id == NULL) {
		cmdq_error(item, "unknown user: %s (%s)", name,
		    cause != NULL ? cause : "not found");
		goto out;
	}

	/*
	 * The owner is already in the list and cannot be changed, so that a
	 * mistake here cannot lock the server out of its own access list.
	 */
	owner = server_acl_owner_id();
	if (owner == NULL) {
		cmdq_error(item, "couldn't determine server owner");
		goto out;
	}
	if (strcmp(id, owner) == 0) {
		cmdq_error(item, "%s owns the server, can't change access",
		    name);
		goto out;
	}

	if (args_has(args, 'a') && args_has(args, 'd')) {
		cmdq_error(item, "-a and -d cannot be used together");
		goto out;
	}
	if (args_has(args, 'w') && args_has(args, 'r')) {
		cmdq_error(item, "-r and -w cannot be used together");
		goto out;
	}

	if (args_has(args, 'd')) {
		if (server_acl_user_find(id) == NULL) {
			cmdq_error(item, "user %s not found", name);
			goto out;
		}
		server_acl_user_disconnect(id);
		server_acl_user_deny(id);
		retval = CMD_RETURN_NORMAL;
		goto out;
	}

	if (args_has(args, 'a')) {
		if (server_acl_user_find(id) != NULL) {
			cmdq_error(item, "user %s is already added", name);
			goto out;
		}
		server_acl_user_allow(id);
		/* Do not return - allow -r or -w with -a. */
	} else if (args_has(args, 'r') || args_has(args, 'w')) {
		/* -r or -w implies -a if user does not exist. */
		if ((user = server_acl_user_find(id)) == NULL) {
			server_acl_user_allow(id);
			user = server_acl_user_find(id);
		}
	}

	if (args_has(args, 'w')) {
		if (server_acl_user_find(id) == NULL) {
			cmdq_error(item, "user %s not found", name);
			goto out;
		}
		server_acl_user_allow_write(id);
		retval = CMD_RETURN_NORMAL;
		goto out;
	}

	if (args_has(args, 'r')) {
		if (server_acl_user_find(id) == NULL) {
			cmdq_error(item, "user %s not found", name);
			goto out;
		}
		server_acl_user_deny_write(id);
		retval = CMD_RETURN_NORMAL;
		goto out;
	}

	retval = CMD_RETURN_NORMAL;

out:
	free(cause);
	free(owner);
	free(id);
	free(name);
	return (retval);
}
