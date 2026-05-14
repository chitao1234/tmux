/* $OpenBSD$ */

/*
 * Copyright (c) 2008 Nicholas Marriott <nicholas.marriott@gmail.com>
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

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

struct client		 *cfg_client;
int			  cfg_finished;
static char		**cfg_causes;
static u_int		  cfg_ncauses;
static struct cmdq_item	 *cfg_item;
static int		  cfg_started;
#ifdef TMUX_WIN32
static struct client	 *cfg_win32_client;
static char		 *cfg_win32_cwd;
static char		**cfg_win32_paths;
static struct cmd_list	**cfg_win32_cmdlists;
static u_int		  cfg_win32_next;
static int		  cfg_win32_flags;
#endif

int                       cfg_quiet = 1;
char                    **cfg_files;
u_int                     cfg_nfiles;

static enum cmd_retval
cfg_client_done(__unused struct cmdq_item *item, __unused void *data)
{
	if (!cfg_finished)
		return (CMD_RETURN_WAIT);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cfg_done(__unused struct cmdq_item *item, __unused void *data)
{
	if (cfg_finished)
		return (CMD_RETURN_NORMAL);
	cfg_finished = 1;

	cfg_show_causes(NULL);

	if (cfg_item != NULL)
		cmdq_continue(cfg_item);

	status_prompt_load_history();

	return (CMD_RETURN_NORMAL);
}

#ifdef TMUX_WIN32
static void	cfg_win32_finish(void);
static void	cfg_win32_next_file(void);

static char *
cfg_win32_get_path(const char *path)
{
	const char	*home;
	char		*expanded, *full_path;

	if (strncmp(path, "~/", 2) == 0) {
		home = find_home();
		if (home == NULL)
			home = "";
		xasprintf(&expanded, "%s%s", home, path + 1);
	} else
		expanded = xstrdup(path);

	if (path_is_absolute(expanded))
		return (expanded);
	xasprintf(&full_path, "%s/%s", cfg_win32_cwd, expanded);
	free(expanded);
	return (full_path);
}

static void
cfg_win32_append_commands(void)
{
	struct cmdq_item	*item;
	struct cmdq_state	*state;
	u_int			 i;

	for (i = 0; i < cfg_nfiles; i++) {
		if (cfg_win32_cmdlists[i] == NULL)
			continue;
		state = cmdq_new_state(NULL, NULL, 0);
		cmdq_add_format(state, "current_file", "%s",
		    cfg_win32_paths[i]);
		item = cmdq_get_command(cfg_win32_cmdlists[i], state);
		cmdq_append(NULL, item);
		cmd_list_free(cfg_win32_cmdlists[i]);
		cfg_win32_cmdlists[i] = NULL;
		cmdq_free_state(state);
	}
}

static void
cfg_win32_free_state(void)
{
	u_int	i;

	if (cfg_win32_cmdlists != NULL) {
		for (i = 0; i < cfg_nfiles; i++) {
			if (cfg_win32_cmdlists[i] != NULL)
				cmd_list_free(cfg_win32_cmdlists[i]);
		}
		free(cfg_win32_cmdlists);
		cfg_win32_cmdlists = NULL;
	}
	if (cfg_win32_paths != NULL) {
		for (i = 0; i < cfg_nfiles; i++)
			free(cfg_win32_paths[i]);
		free(cfg_win32_paths);
		cfg_win32_paths = NULL;
	}
	free(cfg_win32_cwd);
	cfg_win32_cwd = NULL;
	if (cfg_win32_client != NULL) {
		server_client_unref(cfg_win32_client);
		cfg_win32_client = NULL;
	}
}

static void
cfg_win32_finish(void)
{
	cfg_win32_append_commands();
	cfg_win32_free_state();
	cmdq_append(NULL, cmdq_get_callback(cfg_done, NULL));
}

static void
cfg_win32_file_done(struct client *c, const char *path, int error, int closed,
    struct evbuffer *buffer, __unused void *data)
{
	struct cmd_parse_input	 pi;
	struct cmd_parse_result	*pr;
	u_int			 idx = cfg_win32_next - 1;

	if (!closed)
		return;

	if (error != 0) {
		if (error != ENOENT || (~cfg_win32_flags & CMD_PARSE_QUIET))
			cfg_add_cause("%s: %s", path, strerror(error));
	} else {
		if (cfg_win32_client != NULL &&
		    (~cfg_win32_client->flags & CLIENT_DEAD))
			c = cfg_win32_client;
		else
			c = NULL;

		memset(&pi, 0, sizeof pi);
		pi.flags = cfg_win32_flags;
		pi.file = path;
		pi.line = 1;
		pi.c = c;

		pr = cmd_parse_from_buffer(EVBUFFER_DATA(buffer),
		    EVBUFFER_LENGTH(buffer), &pi);
		if (pr->status == CMD_PARSE_ERROR) {
			cfg_add_cause("%s", pr->error);
			free(pr->error);
		} else if (cfg_win32_flags & CMD_PARSE_PARSEONLY) {
			cmd_list_free(pr->cmdlist);
		} else {
			cfg_win32_cmdlists[idx] = pr->cmdlist;
		}
	}

	cfg_win32_next_file();
}

static void
cfg_win32_next_file(void)
{
	if (cfg_win32_next == cfg_nfiles) {
		cfg_win32_finish();
		return;
	}
	file_read(NULL, cfg_win32_paths[cfg_win32_next++], cfg_win32_file_done,
	    NULL);
}
#endif

void
start_cfg(void)
{
	struct client	 *c;
	u_int		  i;
	int		  flags = 0;

	if (cfg_started)
		return;
	cfg_started = 1;

	/*
	 * Configuration files are loaded without a client, so commands are run
	 * in the global queue with item->client NULL.
	 *
	 * However, we must block the initial client (but just the initial
	 * client) so that its command runs after the configuration is loaded.
	 * Because start_cfg() is called so early, we can be sure the client's
	 * command queue is currently empty and our callback will be at the
	 * front - we need to get in before MSG_COMMAND.
	 */
	cfg_client = c = TAILQ_FIRST(&clients);
	if (c != NULL) {
		cfg_item = cmdq_get_callback(cfg_client_done, NULL);
		cmdq_append(c, cfg_item);
	}

	if (cfg_quiet)
		flags = CMD_PARSE_QUIET;
#ifdef TMUX_WIN32
	if (c != NULL) {
		cfg_win32_client = c;
		cfg_win32_client->references++;
	}
	if (cfg_nfiles == 0) {
		cfg_win32_finish();
		return;
	}
	cfg_win32_cwd = xstrdup(server_client_get_cwd(c, NULL));
	cfg_win32_paths = xcalloc(cfg_nfiles, sizeof *cfg_win32_paths);
	cfg_win32_cmdlists = xcalloc(cfg_nfiles, sizeof *cfg_win32_cmdlists);
	for (i = 0; i < cfg_nfiles; i++)
		cfg_win32_paths[i] = cfg_win32_get_path(cfg_files[i]);
	cfg_win32_flags = flags;
	cfg_win32_next = 0;
	cfg_win32_next_file();
#else
	for (i = 0; i < cfg_nfiles; i++)
		load_cfg(cfg_files[i], c, NULL, NULL, flags, NULL);

	cmdq_append(NULL, cmdq_get_callback(cfg_done, NULL));
#endif
}

void
cfg_client_lost(struct client *c)
{
	if (cfg_client == c) {
		cfg_client = NULL;
		cfg_item = NULL;
	}
}

int
load_cfg(const char *path, struct client *c, struct cmdq_item *item,
    struct cmd_find_state *current, int flags, struct cmdq_item **new_item)
{
	FILE			*f;
	struct cmd_parse_input	 pi;
	struct cmd_parse_result	*pr;
	struct cmdq_item	*new_item0;
	struct cmdq_state	*state;

	if (new_item != NULL)
		*new_item = NULL;

	log_debug("loading %s", path);
	if ((f = fopen(path, "rb")) == NULL) {
		if (errno == ENOENT && (flags & CMD_PARSE_QUIET))
			return (0);
		cfg_add_cause("%s: %s", path, strerror(errno));
		return (-1);
	}

	memset(&pi, 0, sizeof pi);
	pi.flags = flags;
	pi.file = path;
	pi.line = 1;
	pi.item = item;
	pi.c = c;

	pr = cmd_parse_from_file(f, &pi);
	fclose(f);
	if (pr->status == CMD_PARSE_ERROR) {
		cfg_add_cause("%s", pr->error);
		free(pr->error);
		return (-1);
	}
	if (flags & CMD_PARSE_PARSEONLY) {
		cmd_list_free(pr->cmdlist);
		return (0);
	}

	if (item != NULL)
		state = cmdq_copy_state(cmdq_get_state(item), current);
	else
		state = cmdq_new_state(NULL, NULL, 0);
	cmdq_add_format(state, "current_file", "%s", pi.file);

	new_item0 = cmdq_get_command(pr->cmdlist, state);
	if (item != NULL)
		new_item0 = cmdq_insert_after(item, new_item0);
	else
		new_item0 = cmdq_append(NULL, new_item0);
	cmd_list_free(pr->cmdlist);
	cmdq_free_state(state);

	if (new_item != NULL)
		*new_item = new_item0;
	return (0);
}

int
load_cfg_from_buffer(const void *buf, size_t len, const char *path,
    struct client *c, struct cmdq_item *item, struct cmd_find_state *current,
    int flags, struct cmdq_item **new_item)
{
	struct cmd_parse_input	 pi;
	struct cmd_parse_result	*pr;
	struct cmdq_item	*new_item0;
	struct cmdq_state	*state;

	if (new_item != NULL)
		*new_item = NULL;

	log_debug("loading %s", path);

	memset(&pi, 0, sizeof pi);
	pi.flags = flags;
	pi.file = path;
	pi.line = 1;
	pi.item = item;
	pi.c = c;

	pr = cmd_parse_from_buffer(buf, len, &pi);
	if (pr->status == CMD_PARSE_ERROR) {
		cfg_add_cause("%s", pr->error);
		free(pr->error);
		return (-1);
	}
	if (flags & CMD_PARSE_PARSEONLY) {
		cmd_list_free(pr->cmdlist);
		return (0);
	}

	if (item != NULL)
		state = cmdq_copy_state(cmdq_get_state(item), current);
	else
		state = cmdq_new_state(NULL, NULL, 0);
	cmdq_add_format(state, "current_file", "%s", pi.file);

	new_item0 = cmdq_get_command(pr->cmdlist, state);
	if (item != NULL)
		new_item0 = cmdq_insert_after(item, new_item0);
	else
		new_item0 = cmdq_append(NULL, new_item0);
	cmd_list_free(pr->cmdlist);
	cmdq_free_state(state);

	if (new_item != NULL)
		*new_item = new_item0;
	return (0);
}

void
cfg_add_cause(const char *fmt, ...)
{
	va_list	 ap;
	char	*msg;

	va_start(ap, fmt);
	xvasprintf(&msg, fmt, ap);
	va_end(ap);

	cfg_ncauses++;
	cfg_causes = xreallocarray(cfg_causes, cfg_ncauses, sizeof *cfg_causes);
	cfg_causes[cfg_ncauses - 1] = msg;
}

void
cfg_print_causes(struct cmdq_item *item)
{
	struct client	*c = cmdq_get_client(item);
	u_int		 i;

	for (i = 0; i < cfg_ncauses; i++) {
		if (c != NULL && (c->flags & CLIENT_CONTROL))
			control_write(c, "%%config-error %s", cfg_causes[i]);
		else
			cmdq_print(item, "%s", cfg_causes[i]);
		free(cfg_causes[i]);
	}

	free(cfg_causes);
	cfg_causes = NULL;
	cfg_ncauses = 0;
}

void
cfg_show_causes(struct session *s)
{
	struct client			*c = TAILQ_FIRST(&clients);
	struct window_pane		*wp;
	struct window_mode_entry	*wme;
	u_int				 i;

	if (cfg_ncauses == 0)
		return;

	if (c != NULL && (c->flags & CLIENT_CONTROL)) {
		for (i = 0; i < cfg_ncauses; i++) {
			control_write(c, "%%config-error %s", cfg_causes[i]);
			free(cfg_causes[i]);
		}
		goto out;
	}

	if (s == NULL) {
		if (c != NULL && c->session != NULL)
			s = c->session;
		else
			s = RB_MIN(sessions, &sessions);
	}
	if (s == NULL || s->attached == 0) /* wait for an attached session */
		return;
	wp = s->curw->window->active;

	wme = TAILQ_FIRST(&wp->modes);
	if (wme == NULL || wme->mode != &window_view_mode)
		window_pane_set_mode(wp, NULL, &window_view_mode, NULL, NULL);
	for (i = 0; i < cfg_ncauses; i++) {
		window_copy_add(wp, 0, "%s", cfg_causes[i]);
		free(cfg_causes[i]);
	}

out:
	free(cfg_causes);
	cfg_causes = NULL;
	cfg_ncauses = 0;
}
