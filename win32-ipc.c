/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include <sys/types.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

#ifdef TMUX_WIN32

struct win32_ipc_port {
	uint32_t	magic;
	uint16_t	port;
	uint16_t	reserved;
};

struct win32_ipc_socket_entry {
	int			 id;
	SOCKET			 socket;
	TAILQ_ENTRY(win32_ipc_socket_entry) entry;
};

#define WIN32_IPC_MAGIC 0x31584d54U /* TMX1 */
#define WIN32_IPC_PORT_BASE 45000
#define WIN32_IPC_PORT_SPAN 20000

static TAILQ_HEAD(, win32_ipc_socket_entry) win32_ipc_sockets =
    TAILQ_HEAD_INITIALIZER(win32_ipc_sockets);
static int win32_ipc_next_id = 3;

static int
win32_ipc_save_socket(SOCKET socket)
{
	struct win32_ipc_socket_entry	*entry;

	entry = xcalloc(1, sizeof *entry);
	entry->id = win32_ipc_next_id++;
	entry->socket = socket;
	TAILQ_INSERT_TAIL(&win32_ipc_sockets, entry, entry);
	return (entry->id);
}

SOCKET
win32_ipc_socket(int fd)
{
	struct win32_ipc_socket_entry	*entry;

	TAILQ_FOREACH(entry, &win32_ipc_sockets, entry) {
		if (entry->id == fd)
			return (entry->socket);
	}
	return (INVALID_SOCKET);
}

static int
win32_ipc_errno(int error)
{
	switch (error) {
	case WSAEWOULDBLOCK:
		return (EAGAIN);
	case WSAEINTR:
		return (EINTR);
	case WSAECONNABORTED:
		return (ECONNABORTED);
	case WSAECONNREFUSED:
		return (ECONNREFUSED);
	case WSAECONNRESET:
	case WSAENETRESET:
		return (ECONNRESET);
	case WSAENOTCONN:
		return (ENOTCONN);
	case WSAETIMEDOUT:
		return (ETIMEDOUT);
	case WSAEMFILE:
		return (EMFILE);
	default:
		return (error);
	}
}

static int
win32_ipc_set_blocking(SOCKET fd, int state, char **cause)
{
	u_long	mode = state ? 0 : 1;

	if (ioctlsocket(fd, FIONBIO, &mode) == SOCKET_ERROR) {
		int error = WSAGetLastError();

		if (cause != NULL) {
			xasprintf(cause, "couldn't set IPC socket mode: %s",
			    win32_strerror(error));
		}
		errno = win32_ipc_errno(error);
		return (-1);
	}
	return (0);
}

static int
win32_ipc_send_all(SOCKET fd, const void *buf, size_t len, const char *what,
    char **cause)
{
	const char	*ptr = buf;
	int		 n;

	while (len != 0) {
		n = send(fd, ptr, (int)(len > INT_MAX ? INT_MAX : len), 0);
		if (n == SOCKET_ERROR) {
			int error = WSAGetLastError();

			if (cause != NULL) {
				xasprintf(cause, "%s: %s", what,
				    win32_strerror(error));
			}
			errno = win32_ipc_errno(error);
			return (-1);
		}
		if (n == 0) {
			if (cause != NULL)
				xasprintf(cause, "%s: connection closed", what);
			errno = EIO;
			return (-1);
		}
		ptr += n;
		len -= n;
	}
	return (0);
}

static int
win32_ipc_recv_all(SOCKET fd, void *buf, size_t len, const char *what,
    char **cause)
{
	char	*ptr = buf;
	int	 n;

	while (len != 0) {
		n = recv(fd, ptr, (int)(len > INT_MAX ? INT_MAX : len), 0);
		if (n == SOCKET_ERROR) {
			int error = WSAGetLastError();

			if (cause != NULL) {
				xasprintf(cause, "%s: %s", what,
				    win32_strerror(error));
			}
			errno = win32_ipc_errno(error);
			return (-1);
		}
		if (n == 0) {
			if (cause != NULL)
				xasprintf(cause, "%s: connection closed", what);
			errno = ECONNRESET;
			return (-1);
		}
		ptr += n;
		len -= n;
	}
	return (0);
}

static int
win32_ipc_listen(uint16_t port, char **cause)
{
	SOCKET			 fd;
	struct sockaddr_in	 sin;
	int			 on = 1;

	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd == INVALID_SOCKET) {
		int error = WSAGetLastError();

		if (cause != NULL) {
			xasprintf(cause, "socket failed: %s",
			    win32_strerror(error));
		}
		errno = win32_ipc_errno(error);
		return (-1);
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on,
	    sizeof on);

	memset(&sin, 0, sizeof sin);
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&sin, sizeof sin) != 0 ||
	    listen(fd, 128) != 0) {
		int error = WSAGetLastError();

		if (cause != NULL) {
			xasprintf(cause, "error creating port %u (%s)",
			    (u_int)port, win32_strerror(error));
		}
		errno = win32_ipc_errno(error);
		closesocket(fd);
		return (-1);
	}
	return (win32_ipc_save_socket(fd));
}

static uint32_t
win32_ipc_hash(const char *path)
{
	uint32_t	hash = 2166136261U;
	u_char		ch;

	while ((ch = *path++) != '\0') {
		if (ch == '\\')
			ch = '/';
		ch = (u_char)tolower(ch);
		hash ^= ch;
		hash *= 16777619U;
	}
	return (hash);
}

static char *
win32_ipc_port_path(const char *path)
{
	char	*out;

	xasprintf(&out, "%s.port", path);
	return (out);
}

static int
win32_ipc_write_port_file(const char *path, uint16_t port, char **cause)
{
	struct win32_ipc_port	 info;
	char			*portpath;
	FILE			*f;

	portpath = win32_ipc_port_path(path);
	f = fopen(portpath, "wb");
	if (f == NULL) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't create %s: %s", portpath,
			    strerror(errno));
		}
		free(portpath);
		return (-1);
	}

	info.magic = WIN32_IPC_MAGIC;
	info.port = port;
	info.reserved = 0;
	if (fwrite(&info, sizeof info, 1, f) != 1) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't write %s: %s", portpath,
			    strerror(errno));
		}
		fclose(f);
		unlink(portpath);
		free(portpath);
		return (-1);
	}
	if (fclose(f) != 0) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't close %s: %s", portpath,
			    strerror(errno));
		}
		unlink(portpath);
		free(portpath);
		return (-1);
	}
	free(portpath);
	return (0);
}

static int
win32_ipc_read_port_file(const char *path, uint16_t *port, char **cause)
{
	struct win32_ipc_port	 info;
	char			*portpath;
	FILE			*f;

	portpath = win32_ipc_port_path(path);
	f = fopen(portpath, "rb");
	if (f == NULL) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't open %s: %s", portpath,
			    strerror(errno));
		}
		free(portpath);
		return (-1);
	}
	if (fread(&info, sizeof info, 1, f) != 1) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't read %s: %s", portpath,
			    strerror(errno));
		}
		fclose(f);
		free(portpath);
		return (-1);
	}
	fclose(f);
	free(portpath);

	if (info.magic != WIN32_IPC_MAGIC || info.port == 0) {
		if (cause != NULL)
			xasprintf(cause, "invalid Win32 IPC port file");
		errno = EINVAL;
		return (-1);
	}
	*port = info.port;
	return (0);
}

int
win32_ipc_server_create(const char *path, char **cause)
{
	uint16_t		 port;
	int			 fd;

	port = WIN32_IPC_PORT_BASE +
	    (win32_ipc_hash(path) % WIN32_IPC_PORT_SPAN);
	fd = win32_ipc_listen(port, cause);
	if (fd == -1)
		return (-1);

	if (win32_ipc_write_port_file(path, port, cause) != 0) {
		win32_ipc_close(fd);
		return (-1);
	}
	return (fd);
}

int
win32_ipc_client_connect(const char *path, __unused uint64_t flags, char **cause)
{
	SOCKET			 fd;
	struct sockaddr_in	 sin;
	uint16_t		 port;
	uint32_t		 token, reply;
	int			 saved_errno;

	if (win32_ipc_read_port_file(path, &port, cause) != 0)
		return (-1);

	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd == INVALID_SOCKET) {
		int error = WSAGetLastError();

		if (cause != NULL) {
			xasprintf(cause, "socket failed: %s",
			    win32_strerror(error));
		}
		errno = win32_ipc_errno(error);
		return (-1);
	}

	memset(&sin, 0, sizeof sin);
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(port);
	if (connect(fd, (struct sockaddr *)&sin, sizeof sin) != 0) {
		int error = WSAGetLastError();

		if (cause != NULL) {
			xasprintf(cause, "couldn't connect to %s port %u: %s",
			    path, (u_int)port, win32_strerror(error));
		}
		errno = win32_ipc_errno(error);
		closesocket(fd);
		return (-1);
	}

	token = htonl(win32_ipc_hash(path));
	if (win32_ipc_send_all(fd, &token, sizeof token,
	    "couldn't verify Win32 IPC token", cause) != 0) {
		saved_errno = errno;
		closesocket(fd);
		errno = saved_errno;
		return (-1);
	}
	if (win32_ipc_recv_all(fd, &reply, sizeof reply,
	    "couldn't read Win32 IPC token reply", cause) != 0) {
		saved_errno = errno;
		closesocket(fd);
		errno = saved_errno;
		return (-1);
	}
	if (reply != token) {
		if (cause != NULL)
			xasprintf(cause, "Win32 IPC token mismatch");
		closesocket(fd);
		errno = EACCES;
		return (-1);
	}
	return (win32_ipc_save_socket(fd));
}

int
win32_ipc_server_accept(int fd, char **cause)
{
	SOCKET			 newfd;
	struct sockaddr_storage	 ss;
	int			 len = sizeof ss, saved_errno;
	uint32_t		 token, expected;

	newfd = accept(win32_ipc_socket(fd), (struct sockaddr *)&ss, &len);
	if (newfd == INVALID_SOCKET) {
		int error = WSAGetLastError();

		if (cause != NULL) {
			xasprintf(cause, "accept failed: %s",
			    win32_strerror(error));
		}
		errno = win32_ipc_errno(error);
		return (-1);
	}

	if (win32_ipc_set_blocking(newfd, 1, cause) != 0) {
		saved_errno = errno;
		closesocket(newfd);
		errno = saved_errno;
		return (-1);
	}

	if (win32_ipc_recv_all(newfd, &token, sizeof token,
	    "couldn't read Win32 IPC token", cause) != 0) {
		saved_errno = errno;
		closesocket(newfd);
		errno = saved_errno;
		return (-1);
	}
	expected = htonl(socket_path == NULL ? token : win32_ipc_hash(socket_path));
	if (token != expected) {
		if (cause != NULL)
			xasprintf(cause, "Win32 IPC token mismatch");
		closesocket(newfd);
		errno = EACCES;
		return (-1);
	}
	if (win32_ipc_send_all(newfd, &token, sizeof token,
	    "couldn't acknowledge Win32 IPC token", cause) != 0) {
		saved_errno = errno;
		closesocket(newfd);
		errno = saved_errno;
		return (-1);
	}
	return (win32_ipc_save_socket(newfd));
}

int
win32_ipc_close(int fd)
{
	struct win32_ipc_socket_entry	*entry, *entry1;
	int				 retval;

	TAILQ_FOREACH_SAFE(entry, &win32_ipc_sockets, entry, entry1) {
		if (entry->id != fd)
			continue;
		retval = closesocket(entry->socket);
		TAILQ_REMOVE(&win32_ipc_sockets, entry, entry);
		free(entry);
		return (retval);
	}
	errno = EBADF;
	return (-1);
}

#endif /* TMUX_WIN32 */
