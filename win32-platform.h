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
struct win32_io_endpoint;

enum win32_io_event {
	WIN32_IO_EVENT_READ = 0x1,
	WIN32_IO_EVENT_READ_EOF = 0x2,
	WIN32_IO_EVENT_WRITE_DRAINED = 0x4,
	WIN32_IO_EVENT_WRITE_CLOSED = 0x8,
	WIN32_IO_EVENT_PROCESS_EXIT = 0x10,
	WIN32_IO_EVENT_ERROR = 0x20,
	WIN32_IO_EVENT_CANCELED = 0x40,
	WIN32_IO_EVENT_WRITE_PROGRESS = 0x80
};

const char	*win32_strerror(DWORD);
char		*win32_getenv_utf8(const char *);
wchar_t		*win32_utf8_to_wide(const char *);
char		*win32_wide_to_utf8(const wchar_t *);
int		 win32_wide_to_argv(const wchar_t *, int *, char ***);
int		 win32_utf8_to_argv(const char *, int *, char ***);
int		 win32_path_is_dir(const char *);
const char	*win32_default_cwd(void);
const char	*win32_default_shell(void);
char		*win32_resolve_cwd(const char *, const char *, char **);
char		*win32_sanitize_cwd(const char *);
wchar_t		*win32_build_argv_command(int, char **);

int		 win32_init(void);
void		 win32_fini(void);
long		 win32_console_ctrl_c_events(void);
DWORD		 win32_console_ctrl_close_event(void);

void		 win32_io_service_fini(void);
struct win32_io_endpoint *win32_io_reader_new_console(HANDLE,
		     void (*)(void *, uint32_t), void *);
struct win32_io_endpoint *win32_io_reader_new_stdio(HANDLE,
		     void (*)(void *, uint32_t), void *);
struct win32_io_endpoint *win32_io_reader_new_terminal(HANDLE,
		     void (*)(void *, uint32_t), void *);
struct win32_io_endpoint *win32_io_reader_new_overlapped(HANDLE,
		     void (*)(void *, uint32_t), void *);
struct win32_io_endpoint *win32_io_reader_new_file(HANDLE, uint64_t,
		     void (*)(void *, uint32_t), void *);
void		 win32_io_endpoint_free(struct win32_io_endpoint *);
void		 win32_io_reader_drain(struct win32_io_endpoint *,
		     struct evbuffer *);
void		 win32_io_reader_drain_limit(struct win32_io_endpoint *,
		     struct evbuffer *, size_t);
void		 win32_io_reader_drain_bev(struct win32_io_endpoint *,
		     struct bufferevent *);
void		 win32_io_reader_set_reading(struct win32_io_endpoint *,
		     int);
size_t		 win32_io_reader_buffered(struct win32_io_endpoint *);
int		 win32_io_reader_done(struct win32_io_endpoint *);
int		 win32_io_reader_eof(struct win32_io_endpoint *);
int		 win32_io_reader_error(struct win32_io_endpoint *);
struct win32_io_endpoint *win32_io_writer_new_overlapped(HANDLE *,
		     void (*)(void *, uint32_t), void *);
struct win32_io_endpoint *win32_io_writer_new_file_borrowed(HANDLE *,
		     uint64_t, int, void (*)(void *, uint32_t), void *);
struct win32_io_endpoint *win32_io_writer_new_console_borrowed(HANDLE *,
		     void (*)(void *, uint32_t), void *);
struct win32_io_endpoint *win32_io_writer_new_stdio_borrowed(HANDLE *,
		     void (*)(void *, uint32_t), void *);
struct win32_io_endpoint *win32_io_writer_new_terminal_borrowed(HANDLE *,
		     void (*)(void *, uint32_t), void *);
int		 win32_io_writer_write(struct win32_io_endpoint *,
		     const void *, size_t);
void		 win32_io_writer_close(struct win32_io_endpoint *);
size_t		 win32_io_writer_consume_progress(struct win32_io_endpoint *);
size_t		 win32_io_writer_buffered(struct win32_io_endpoint *);
int		 win32_io_writer_drained(struct win32_io_endpoint *);
int		 win32_io_writer_writable(struct win32_io_endpoint *);
struct win32_io_endpoint *win32_io_process_new(HANDLE,
		     void (*)(void *, uint32_t), void *);
void		 win32_io_process_notify(struct win32_io_endpoint *);
void		 win32_log_handle(const char *, HANDLE);

const char	*win32_default_socket_dir(void);
int		 win32_ipc_errno(int);
int		 win32_ipc_ensure_socket_dir(const char *, char **);
int		 win32_ipc_ensure_parent_dir(const char *, char **);
int		 win32_ipc_validate_socket_parent(const char *, char **);
char		*win32_ipc_startup_guard_path(const char *, char **);
int		 win32_ipc_server_create(const char *, char **);
int		 win32_ipc_client_connect(const char *, uint64_t, char **);
int		 win32_ipc_socket_accept(int, char **);
int		 win32_ipc_close(int);
SOCKET		 win32_ipc_socket(int);
int		 win32_random_bytes(void *, size_t, char **);
struct win32_ipc_peer_identity *win32_ipc_peer_identity_create(
		     pid_t, HANDLE, char **);
void		 win32_ipc_peer_identity_free(struct win32_ipc_peer_identity *);
int		 win32_ipc_peer_identity_same_user(
		     const struct win32_ipc_peer_identity *, char **);
int		 win32_ipc_peer_identity_meets_integrity_floor(
		     const struct win32_ipc_peer_identity *, char **);
int		 win32_ipc_verify_auth_bind(
		     const struct msg_win32_auth_challenge *,
		     const struct msg_win32_auth_bind *,
		     struct win32_ipc_peer_identity **, char **);
int		 win32_ipc_duplicate_client_handle(
		     const struct win32_ipc_peer_identity *, uint64_t, DWORD,
		     HANDLE *, char **);
const char	*win32_ipc_peer_identity_user_sid(
		     const struct win32_ipc_peer_identity *);
const char	*win32_ipc_current_user_sid(void);
int		 win32_server_spawn(const char *, uint64_t, char **);

int		 win32_terminal_prepare_terminfo(void);
int		 win32_terminal_is_client_console(void);
int		 win32_terminal_init_client(char **);
int		 win32_terminal_set_client_mouse_mode(uint32_t);
int		 win32_terminal_get_size(struct client *, u_int *, u_int *,
		     u_int *, u_int *);
void		 win32_terminal_restore_client(void);
void		 win32_terminal_check_resize(struct client *);

int		 win32_pane_spawn(struct spawn_context *, struct window_pane *,
		     struct environ *, const char *, char **);
void		 win32_pane_cleanup(struct window_pane *);
void		 win32_pane_terminate(struct window_pane *);
void		 win32_pane_resize(struct window_pane *, u_int, u_int);
void		 win32_pane_drain(struct window_pane *);
int		 win32_pane_exited(struct window_pane *, int *);
int		 win32_pane_quiesced(struct window_pane *);
size_t		 win32_pane_buffered(struct window_pane *);
int		 win32_pane_output_done(struct window_pane *);
int		 win32_pane_output_eof(struct window_pane *);
void		 win32_pane_set_reading(struct window_pane *, int);
int		 win32_pane_reading_paused(struct window_pane *);
int		 win32_pane_input_ready(struct window_pane *);
char		*win32_pane_get_name(struct window_pane *);
char		*win32_pane_get_cwd(struct window_pane *);

struct bufferevent *win32_pane_get_event(struct window_pane *);
int		 win32_pane_write(struct window_pane *, const void *, size_t);

struct win32_job *win32_job_spawn(const char *, const char *, int, char **,
		     struct environ *, struct session *, const char *, int,
		     int, int, char **);
void		 win32_job_cleanup(struct win32_job *);
void		 win32_job_terminate(struct win32_job *);
void		 win32_job_resize(struct win32_job *, u_int, u_int);
int		 win32_job_exited(struct win32_job *, int *);
int		 win32_job_output_done(struct win32_job *);
int		 win32_job_output_eof(struct win32_job *);
void		 win32_job_drain(struct win32_job *);
void		 win32_job_set_reading(struct win32_job *, int);
int		 win32_job_get_pid(struct win32_job *, pid_t *);
int		 win32_job_get_status(struct win32_job *);
struct bufferevent *win32_job_get_event(struct win32_job *);
void		 win32_job_set_exit_callback(struct win32_job *,
		     void (*)(void *), void *);
int		 win32_job_write(struct win32_job *, const void *, size_t);
size_t		 win32_job_stdin_buffered(struct win32_job *);
void		 win32_job_close_stdin(struct win32_job *);

#endif /* TMUX_WIN32 */

#endif /* TMUX_WIN32_PLATFORM_H */
