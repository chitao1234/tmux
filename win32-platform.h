/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef TMUX_WIN32_PLATFORM_H
#define TMUX_WIN32_PLATFORM_H

#ifdef TMUX_WIN32

#include "compat/win32-compat.h"

struct bufferevent;
struct client;
struct environ;
struct evbuffer;
struct job;
struct session;
struct spawn_context;
struct window_pane;

struct win32_pane;
struct win32_job;
struct win32_handle_event;
struct win32_handle_writer;

const char	*win32_strerror(DWORD);
char		*win32_getenv_utf8(const char *);
wchar_t		*win32_utf8_to_wide(const char *);
char		*win32_wide_to_utf8(const wchar_t *);
int		 win32_path_is_dir(const char *);
const char	*win32_default_cwd(void);
char		*win32_resolve_cwd(const char *, const char *, char **);
char		*win32_sanitize_cwd(const char *);
wchar_t		*win32_build_argv_command(int, char **);

int		 win32_init(void);
void		 win32_fini(void);
long		 win32_console_ctrl_c_events(void);
void		 win32_check_children(void);

struct win32_handle_event *win32_handle_event_new(HANDLE, void (*)(void *),
		     void (*)(void *), void *);
void		 win32_handle_event_free(struct win32_handle_event *);
struct evbuffer *win32_handle_event_input(struct win32_handle_event *);
void		 win32_handle_event_drain(struct win32_handle_event *,
		     struct evbuffer *);
void		 win32_handle_event_drain_bev(struct win32_handle_event *,
		     struct bufferevent *);
size_t		 win32_handle_event_buffered(struct win32_handle_event *);
int		 win32_handle_event_done(struct win32_handle_event *);
int		 win32_handle_event_write(struct win32_handle_event *,
		     const void *, size_t);
struct win32_handle_writer *win32_handle_writer_new(HANDLE *);
void		 win32_handle_writer_free(struct win32_handle_writer *);
int		 win32_handle_writer_write(struct win32_handle_writer *,
		     const void *, size_t);
void		 win32_handle_writer_close(struct win32_handle_writer *);
int		 win32_handle_write(HANDLE, const void *, size_t);
void		 win32_log_handle(const char *, HANDLE);

const char	*win32_default_socket_dir(void);
int		 win32_ipc_ensure_socket_dir(const char *, char **);
HANDLE		 win32_ipc_startup_lock(const char *, char **);
void		 win32_ipc_startup_unlock(HANDLE);
int		 win32_ipc_server_create(const char *, char **);
int		 win32_ipc_client_connect(const char *, uint64_t, char **);
int		 win32_ipc_socket_accept(int, char **);
int		 win32_ipc_close(int);
SOCKET		 win32_ipc_socket(int);
int		 win32_server_spawn(const char *, uint64_t, char **);

int		 win32_terminal_prepare_terminfo(void);
int		 win32_terminal_is_client_console(void);
int		 win32_terminal_init_client(char **);
int		 win32_terminal_get_size(struct client *, u_int *, u_int *,
		     u_int *, u_int *);
void		 win32_terminal_restore_client(void);
void		 win32_terminal_check_resize(struct client *);

int		 win32_pane_spawn(struct spawn_context *, struct window_pane *,
		     struct environ *, const char *, char **);
void		 win32_pane_close(struct window_pane *);
void		 win32_pane_resize(struct window_pane *, u_int, u_int);
void		 win32_pane_drain(struct window_pane *);
int		 win32_pane_exited(struct window_pane *, int *);
size_t		 win32_pane_buffered(struct window_pane *);
int		 win32_pane_output_done(struct window_pane *);
char		*win32_pane_get_cwd(struct window_pane *);

struct bufferevent *win32_pane_get_event(struct window_pane *);
int		 win32_pane_write(struct window_pane *, const void *, size_t);

struct win32_job *win32_job_spawn(const char *, const char *, int, char **,
		     struct environ *, struct session *, const char *, int,
		     int, int, char **);
void		 win32_job_close(struct win32_job *);
void		 win32_job_resize(struct win32_job *, u_int, u_int);
int		 win32_job_exited(struct win32_job *, int *);
int		 win32_job_output_done(struct win32_job *);
void		 win32_job_drain(struct win32_job *);
int		 win32_job_get_pid(struct win32_job *, pid_t *);
int		 win32_job_get_status(struct win32_job *);
struct bufferevent *win32_job_get_event(struct win32_job *);
int		 win32_job_write(struct win32_job *, const void *, size_t);
void		 win32_job_close_stdin(struct win32_job *);

#endif /* TMUX_WIN32 */

#endif /* TMUX_WIN32_PLATFORM_H */
