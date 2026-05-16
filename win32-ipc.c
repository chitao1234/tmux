/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include <sys/types.h>
#include <sys/un.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>

#ifdef TMUX_WIN32

struct win32_ipc_socket_entry {
	int			 id;
	SOCKET			 socket;
	char			*cleanup_path;
	TAILQ_ENTRY(win32_ipc_socket_entry) entry;
};

static TAILQ_HEAD(, win32_ipc_socket_entry) win32_ipc_sockets =
    TAILQ_HEAD_INITIALIZER(win32_ipc_sockets);
static int win32_ipc_next_id = 3;
static char *win32_ipc_socket_dir;
static char *win32_ipc_current_user_sid_value;
static char *win32_ipc_current_integrity_value;
static DWORD win32_ipc_current_integrity_rid;

struct win32_ipc_peer_identity {
	pid_t		 pid;
	HANDLE		 process;
	char		*user_sid;
	DWORD		 integrity_rid;
};

static int	win32_ipc_errno(int);
static int	win32_ipc_set_blocking(SOCKET, int, char **);
static int	win32_ipc_path_to_sockaddr(const char *, struct sockaddr_un *,
		    char **);
static char    *win32_ipc_sid_to_string(PSID);
static int	win32_ipc_capture_token_identity(HANDLE, char **, char **);
static char    *win32_ipc_integrity_level_to_name(DWORD);
static int	win32_ipc_capture_token_integrity(HANDLE, char **, char **);
static int	win32_ipc_capture_token_integrity_rid(HANDLE, DWORD *, char **);
static int	win32_ipc_cache_current_identity(void);
static int	win32_ipc_make_managed_root(char **, char **);
static int	win32_ipc_set_path_security(const char *, char **);
static int	win32_ipc_ensure_dir(const char *, char **);
static char    *win32_ipc_normalize_path(const char *);
static int	win32_ipc_path_is_root(const char *);

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

static struct win32_ipc_socket_entry *
win32_ipc_find_socket(int fd)
{
	struct win32_ipc_socket_entry	*entry;

	TAILQ_FOREACH(entry, &win32_ipc_sockets, entry) {
		if (entry->id == fd)
			return (entry);
	}
	return (NULL);
}

static int
win32_ipc_set_cleanup_path(int fd, const char *path)
{
	struct win32_ipc_socket_entry	*entry;

	entry = win32_ipc_find_socket(fd);
	if (entry == NULL) {
		errno = EBADF;
		return (-1);
	}
	free(entry->cleanup_path);
	entry->cleanup_path = xstrdup(path);
	return (0);
}

SOCKET
win32_ipc_socket(int fd)
{
	struct win32_ipc_socket_entry	*entry;

	entry = win32_ipc_find_socket(fd);
	if (entry == NULL)
		return (INVALID_SOCKET);
	return (entry->socket);
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
	case WSAEAFNOSUPPORT:
		return (EAFNOSUPPORT);
	case WSAEADDRINUSE:
		return (EADDRINUSE);
	case WSAEADDRNOTAVAIL:
		return (EADDRNOTAVAIL);
	case WSAEACCES:
		return (EACCES);
	case WSAEINVAL:
		return (EINVAL);
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

const char *
win32_default_socket_dir(void)
{
	if (win32_ipc_socket_dir != NULL)
		return (win32_ipc_socket_dir);
	if (win32_ipc_make_managed_root(&win32_ipc_socket_dir, NULL) != 0)
		return (NULL);
	return (win32_ipc_socket_dir);
}

static int
win32_ipc_path_to_sockaddr(const char *path, struct sockaddr_un *sun,
    char **cause)
{
	size_t	 i, size;

	if (path == NULL || !path_is_absolute(path)) {
		if (cause != NULL)
			xasprintf(cause, "socket path must be absolute");
		errno = EINVAL;
		return (-1);
	}

	memset(sun, 0, sizeof *sun);
	sun->sun_family = AF_UNIX;
	size = strlen(path);
	if (size >= sizeof sun->sun_path) {
		if (cause != NULL)
			xasprintf(cause, "socket path too long: %s", path);
		errno = ENAMETOOLONG;
		return (-1);
	}
	strlcpy(sun->sun_path, path, sizeof sun->sun_path);
	for (i = 0; sun->sun_path[i] != '\0'; i++) {
		if (sun->sun_path[i] == '/')
			sun->sun_path[i] = '\\';
	}
	return (0);
}

static char *
win32_ipc_normalize_path(const char *path)
{
	char	*copy;
	u_int	 i;

	if (path == NULL)
		return (NULL);
	copy = xstrdup(path);
	for (i = 0; copy[i] != '\0'; i++) {
		if (copy[i] == '\\')
			copy[i] = '/';
	}
	while (strlen(copy) > 3 && copy[strlen(copy) - 1] == '/')
		copy[strlen(copy) - 1] = '\0';
	return (copy);
}

static int
win32_ipc_path_is_root(const char *path)
{
	const char	*p;

	if (path == NULL || *path == '\0')
		return (0);

	if (isalpha((u_char)path[0]) && path[1] == ':' &&
	    (path[2] == '\0' || (path[2] == '/' && path[3] == '\0')))
		return (1);

	if (path[0] != '/' || path[1] != '/')
		return (0);

	p = strchr(path + 2, '/');
	if (p == NULL)
		return (1);
	p = strchr(p + 1, '/');
	if (p == NULL || p[1] == '\0')
		return (1);
	return (0);
}

static char *
win32_ipc_sid_to_string(PSID sid)
{
	LPWSTR	 string_sid = NULL;
	char	*copy;

	if (sid == NULL)
		return (NULL);
	if (!ConvertSidToStringSidW(sid, &string_sid))
		return (NULL);
	copy = win32_wide_to_utf8(string_sid);
	LocalFree(string_sid);
	return (copy);
}

int
win32_random_bytes(void *buf, size_t len, char **cause)
{
	NTSTATUS	status;

	status = BCryptGenRandom(NULL, buf, (ULONG)len,
	    BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	if (status != 0) {
		if (cause != NULL)
			xasprintf(cause, "BCryptGenRandom failed: %#lx",
			    (unsigned long)status);
		errno = EACCES;
		return (-1);
	}
	return (0);
}

static int
win32_ipc_capture_token_identity(HANDLE token, char **user_sid, char **cause)
{
	DWORD		 size;
	TOKEN_USER	*user = NULL;

	*user_sid = NULL;

	if (!GetTokenInformation(token, TokenUser, NULL, 0, &size) &&
	    GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
		if (cause != NULL) {
			xasprintf(cause, "GetTokenInformation(TokenUser) failed:"
			    " %s", win32_strerror(GetLastError()));
		}
		return (-1);
	}
	user = xmalloc(size);
	if (!GetTokenInformation(token, TokenUser, user, size, &size)) {
		if (cause != NULL) {
			xasprintf(cause, "GetTokenInformation(TokenUser) failed:"
			    " %s", win32_strerror(GetLastError()));
		}
		free(user);
		return (-1);
	}
	*user_sid = win32_ipc_sid_to_string(user->User.Sid);
	free(user);
	if (*user_sid == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't convert token user SID");
		return (-1);
	}
	return (0);
}

static char *
win32_ipc_integrity_level_to_name(DWORD rid)
{
	if (rid < SECURITY_MANDATORY_MEDIUM_RID)
		return (xstrdup("l"));
	if (rid < SECURITY_MANDATORY_HIGH_RID)
		return (xstrdup("m"));
	if (rid < SECURITY_MANDATORY_SYSTEM_RID)
		return (xstrdup("h"));
	return (xstrdup("s"));
}

static int
win32_ipc_capture_token_integrity_rid(HANDLE token, DWORD *integrity_rid,
    char **cause)
{
	DWORD			 size;
	TOKEN_MANDATORY_LABEL	*label = NULL;
	DWORD			 count;
	DWORD			*subauth;

	*integrity_rid = 0;

	if (!GetTokenInformation(token, TokenIntegrityLevel, NULL, 0, &size) &&
	    GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
		if (cause != NULL) {
			xasprintf(cause,
			    "GetTokenInformation(TokenIntegrityLevel) failed:"
			    " %s", win32_strerror(GetLastError()));
		}
		return (-1);
	}
	label = xmalloc(size);
	if (!GetTokenInformation(token, TokenIntegrityLevel, label, size,
	    &size)) {
		if (cause != NULL) {
			xasprintf(cause,
			    "GetTokenInformation(TokenIntegrityLevel) failed:"
			    " %s", win32_strerror(GetLastError()));
		}
		free(label);
		return (-1);
	}
	count = *GetSidSubAuthorityCount(label->Label.Sid);
	subauth = GetSidSubAuthority(label->Label.Sid, count - 1);
	*integrity_rid = *subauth;
	free(label);
	return (0);
}

static int
win32_ipc_capture_token_integrity(HANDLE token, char **integrity,
    char **cause)
{
	DWORD			 size;
	TOKEN_MANDATORY_LABEL	*label = NULL;
	DWORD			 count;
	DWORD			*subauth;

	*integrity = NULL;

	if (!GetTokenInformation(token, TokenIntegrityLevel, NULL, 0, &size) &&
	    GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
		if (cause != NULL) {
			xasprintf(cause,
			    "GetTokenInformation(TokenIntegrityLevel) failed:"
			    " %s", win32_strerror(GetLastError()));
		}
		return (-1);
	}
	label = xmalloc(size);
	if (!GetTokenInformation(token, TokenIntegrityLevel, label, size,
	    &size)) {
		if (cause != NULL) {
			xasprintf(cause,
			    "GetTokenInformation(TokenIntegrityLevel) failed:"
			    " %s", win32_strerror(GetLastError()));
		}
		free(label);
		return (-1);
	}
	count = *GetSidSubAuthorityCount(label->Label.Sid);
	subauth = GetSidSubAuthority(label->Label.Sid, count - 1);
	*integrity = win32_ipc_integrity_level_to_name(*subauth);
	free(label);
	return (*integrity == NULL ? -1 : 0);
}

struct win32_ipc_peer_identity *
win32_ipc_peer_identity_create(pid_t pid, HANDLE process, char **cause)
{
	struct win32_ipc_peer_identity	*peer;
	HANDLE				 token = NULL;
	char				*user_sid = NULL;

	if (pid <= 0 || process == NULL) {
		if (cause != NULL)
			xasprintf(cause, "invalid peer identity");
		errno = EINVAL;
		return (NULL);
	}
	if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't open client process token: %s",
			    win32_strerror(GetLastError()));
		}
		errno = EACCES;
		return (NULL);
	}
	if (win32_ipc_capture_token_identity(token, &user_sid, cause) != 0) {
		CloseHandle(token);
		return (NULL);
	}

	peer = xcalloc(1, sizeof *peer);
	peer->pid = pid;
	peer->process = process;
	peer->user_sid = user_sid;
	if (win32_ipc_capture_token_integrity_rid(token, &peer->integrity_rid,
	    cause) != 0) {
		win32_ipc_peer_identity_free(peer);
		CloseHandle(token);
		return (NULL);
	}
	CloseHandle(token);
	return (peer);
}

void
win32_ipc_peer_identity_free(struct win32_ipc_peer_identity *peer)
{
	if (peer == NULL)
		return;
	if (peer->process != NULL)
		CloseHandle(peer->process);
	free(peer->user_sid);
	free(peer);
}

int
win32_ipc_peer_identity_same_user(const struct win32_ipc_peer_identity *peer,
    char **cause)
{
	if (win32_ipc_cache_current_identity() != 0 ||
	    win32_ipc_current_user_sid_value == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't determine server identity");
		errno = EACCES;
		return (-1);
	}
	if (peer == NULL || peer->user_sid == NULL ||
	    strcmp(peer->user_sid, win32_ipc_current_user_sid_value) != 0) {
		if (cause != NULL)
			xasprintf(cause, "client process user SID mismatch");
		errno = EACCES;
		return (-1);
	}
	return (0);
}

int
win32_ipc_peer_identity_meets_integrity_floor(
    const struct win32_ipc_peer_identity *peer, char **cause)
{
	if (win32_ipc_cache_current_identity() != 0 ||
	    win32_ipc_current_integrity_value == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't determine server integrity");
		errno = EACCES;
		return (-1);
	}
	if (peer == NULL || peer->integrity_rid < win32_ipc_current_integrity_rid) {
		if (cause != NULL)
			xasprintf(cause, "client integrity too low");
		errno = EACCES;
		return (-1);
	}
	return (0);
}

int
win32_ipc_verify_auth_bind(
    const struct msg_win32_auth_challenge *challenge,
    const struct msg_win32_auth_bind *bind,
    struct win32_ipc_peer_identity **peer_out, char **cause)
{
	HANDLE				 process = NULL;
	HANDLE				 duplicate = NULL;
	struct win32_ipc_peer_identity	*peer = NULL;
	void				*view = NULL;
	int				 retval = -1;

	*peer_out = NULL;
	if (challenge == NULL || bind == NULL || bind->pid == 0 ||
	    bind->handle == 0 ||
	    bind->handle == (uint64_t)(uintptr_t)INVALID_HANDLE_VALUE) {
		if (cause != NULL)
			xasprintf(cause, "invalid auth bind");
		errno = EINVAL;
		return (-1);
	}

	process = OpenProcess(PROCESS_DUP_HANDLE|
	    PROCESS_QUERY_LIMITED_INFORMATION, FALSE, bind->pid);
	if (process == NULL) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't open client process: %s",
			    win32_strerror(GetLastError()));
		}
		errno = EACCES;
		return (-1);
	}
	if (!DuplicateHandle(process, (HANDLE)(uintptr_t)bind->handle,
	    GetCurrentProcess(), &duplicate, FILE_MAP_READ, FALSE, 0)) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't duplicate auth proof: %s",
			    win32_strerror(GetLastError()));
		}
		errno = EACCES;
		goto out;
	}
	view = MapViewOfFile(duplicate, FILE_MAP_READ, 0, 0,
	    sizeof challenge->nonce);
	if (view == NULL) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't map auth proof: %s",
			    win32_strerror(GetLastError()));
		}
		errno = EACCES;
		goto out;
	}
	if (memcmp(view, challenge->nonce, sizeof challenge->nonce) != 0) {
		if (cause != NULL)
			xasprintf(cause, "auth nonce mismatch");
		errno = EACCES;
		goto out;
	}
	peer = win32_ipc_peer_identity_create((pid_t)bind->pid, process, cause);
	if (peer == NULL)
		goto out;
	process = NULL;
	*peer_out = peer;
	peer = NULL;
	retval = 0;

out:
	if (view != NULL)
		UnmapViewOfFile(view);
	if (duplicate != NULL)
		CloseHandle(duplicate);
	if (process != NULL)
		CloseHandle(process);
	win32_ipc_peer_identity_free(peer);
	return (retval);
}

static int
win32_ipc_cache_current_identity(void)
{
	HANDLE	token = NULL;
	char	*user_sid = NULL;
	char	*cause = NULL;
	int	 retval;

	if (win32_ipc_current_user_sid_value != NULL)
		return (0);

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return (-1);
	retval = win32_ipc_capture_token_identity(token, &user_sid, &cause);
	if (retval == 0) {
		retval = win32_ipc_capture_token_integrity(token,
		    &win32_ipc_current_integrity_value, &cause);
		if (retval == 0) {
			retval = win32_ipc_capture_token_integrity_rid(token,
			    &win32_ipc_current_integrity_rid, &cause);
		}
	}
	CloseHandle(token);
	free(cause);
	if (retval != 0) {
		free(user_sid);
		free(win32_ipc_current_integrity_value);
		win32_ipc_current_integrity_value = NULL;
		return (-1);
	}
	win32_ipc_current_user_sid_value = user_sid;
	return (0);
}

int
win32_ipc_duplicate_client_handle(const struct win32_ipc_peer_identity *peer,
    uint64_t value, DWORD access, HANDLE *out, char **cause)
{
	HANDLE	probe = NULL, duplicate = NULL;
	DWORD	 error;
	int	 retval = -1;

	*out = NULL;
	if (peer == NULL || peer->process == NULL || value == 0 ||
	    value == (uint64_t)(uintptr_t)INVALID_HANDLE_VALUE) {
		if (cause != NULL)
			xasprintf(cause, "invalid client handle claim");
		errno = EINVAL;
		return (-1);
	}
	if (win32_ipc_peer_identity_same_user(peer, cause) != 0)
		return (-1);

	if (access != 0 && !DuplicateHandle(peer->process,
	    (HANDLE)(uintptr_t)value,
	    GetCurrentProcess(), &probe, access, FALSE, 0)) {
		error = GetLastError();
		if (cause != NULL) {
			xasprintf(cause, "client handle lacks requested access: %s",
			    win32_strerror(error));
		}
		errno = EACCES;
		goto out;
	}
	if (probe != NULL) {
		CloseHandle(probe);
		probe = NULL;
	}

	if (!DuplicateHandle(peer->process, (HANDLE)(uintptr_t)value,
	    GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
		error = GetLastError();
		if (cause != NULL) {
			xasprintf(cause, "couldn't duplicate client handle: %s",
			    win32_strerror(error));
		}
		errno = EACCES;
		goto out;
	}

	*out = duplicate;
	duplicate = NULL;
	retval = 0;

out:
	if (duplicate != NULL)
		CloseHandle(duplicate);
	if (probe != NULL)
		CloseHandle(probe);
	return (retval);
}

static int
win32_ipc_make_managed_root(char **path, char **cause)
{
	char		*localappdata;
	char		*base = NULL, *normalized;

	*path = NULL;
	localappdata = win32_getenv_utf8("LOCALAPPDATA");
	if (localappdata == NULL || *localappdata == '\0' ||
	    !path_is_absolute(localappdata)) {
		if (cause != NULL)
			xasprintf(cause, "LOCALAPPDATA is unavailable");
		free(localappdata);
		errno = ENOENT;
		return (-1);
	}
	if (win32_ipc_cache_current_identity() != 0 ||
	    win32_ipc_current_integrity_value == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't determine current integrity");
		free(localappdata);
		errno = EACCES;
		return (-1);
	}

	xasprintf(&base, "%s/tmux-%s", localappdata,
	    win32_ipc_current_integrity_value);
	free(localappdata);
	normalized = win32_ipc_normalize_path(base);
	free(base);
	if (normalized == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't normalize managed socket path");
		errno = EINVAL;
		return (-1);
	}
	*path = normalized;
	return (0);
}

static int
win32_ipc_set_path_security(const char *path, char **cause)
{
	PSECURITY_DESCRIPTOR	 sd = NULL;
	PACL			 dacl = NULL, sacl = NULL;
	char			*sddl = NULL;
	wchar_t			*wsddl = NULL, *wpath = NULL;
	BOOL			 dacl_present, dacl_defaulted;
	BOOL			 sacl_present, sacl_defaulted;
	DWORD			 error;
	int			 retval = -1;

	if (win32_ipc_cache_current_identity() != 0 ||
	    win32_ipc_current_user_sid_value == NULL ||
	    win32_ipc_current_integrity_value == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't determine current token");
		errno = EACCES;
		return (-1);
	}
	wpath = win32_utf8_to_wide(path);
	if (wpath == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't convert IPC path: %s", path);
		errno = EINVAL;
		goto out;
	}

	if (strcmp(win32_ipc_current_integrity_value, "l") == 0) {
		xasprintf(&sddl,
		    "D:P(A;;GA;;;%s)(A;;GA;;;SY)S:(ML;;NW;;;LW)",
		    win32_ipc_current_user_sid_value);
	} else if (strcmp(win32_ipc_current_integrity_value, "h") == 0) {
		xasprintf(&sddl,
		    "D:P(A;;GA;;;%s)(A;;GA;;;SY)S:(ML;;NW;;;HI)",
		    win32_ipc_current_user_sid_value);
	} else if (strcmp(win32_ipc_current_integrity_value, "s") == 0) {
		xasprintf(&sddl,
		    "D:P(A;;GA;;;%s)(A;;GA;;;SY)S:(ML;;NW;;;SI)",
		    win32_ipc_current_user_sid_value);
	} else {
		xasprintf(&sddl,
		    "D:P(A;;GA;;;%s)(A;;GA;;;SY)S:(ML;;NW;;;ME)",
		    win32_ipc_current_user_sid_value);
	}
	wsddl = win32_utf8_to_wide(sddl);
	if (wsddl == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't convert security descriptor");
		errno = EACCES;
		goto out;
	}
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(wsddl,
	    SDDL_REVISION_1, &sd, NULL)) {
		error = GetLastError();
		if (cause != NULL) {
			xasprintf(cause, "couldn't build security descriptor:"
			    " %s", win32_strerror(error));
		}
		errno = EACCES;
		goto out;
	}
	if (!GetSecurityDescriptorDacl(sd, &dacl_present, &dacl,
	    &dacl_defaulted) ||
	    !GetSecurityDescriptorSacl(sd, &sacl_present, &sacl,
	    &sacl_defaulted)) {
		error = GetLastError();
		if (cause != NULL) {
			xasprintf(cause, "couldn't read security descriptor:"
			    " %s", win32_strerror(error));
		}
		errno = EACCES;
		goto out;
	}
	error = SetNamedSecurityInfoW(wpath, SE_FILE_OBJECT,
	    DACL_SECURITY_INFORMATION|LABEL_SECURITY_INFORMATION|
	    PROTECTED_DACL_SECURITY_INFORMATION|
	    PROTECTED_SACL_SECURITY_INFORMATION,
	    NULL, NULL, dacl, sacl);
	if (error == ERROR_PRIVILEGE_NOT_HELD) {
		log_debug("%s: integrity label skipped for %s (%s)", __func__,
		    path, win32_strerror(error));
		error = SetNamedSecurityInfoW(wpath, SE_FILE_OBJECT,
		    DACL_SECURITY_INFORMATION|
		    PROTECTED_DACL_SECURITY_INFORMATION,
		    NULL, NULL, dacl, NULL);
	}
	if (error != ERROR_SUCCESS) {
		if (cause != NULL) {
			xasprintf(cause, "couldn't secure %s: %s", path,
			    win32_strerror(error));
		}
		errno = EACCES;
		goto out;
	}
	retval = 0;

out:
	free(sddl);
	free(wsddl);
	free(wpath);
	if (sd != NULL)
		LocalFree(sd);
	return (retval);
}

static int
win32_ipc_ensure_dir(const char *path, char **cause)
{
	char		*normalized, *copy, *slash;
	wchar_t		*wnormalized;
	DWORD		 attr;

	normalized = win32_ipc_normalize_path(path);
	if (normalized == NULL) {
		errno = EINVAL;
		return (-1);
	}
	if (win32_ipc_path_is_root(normalized)) {
		free(normalized);
		return (0);
	}

	copy = xstrdup(normalized);
	slash = strrchr(copy, '/');
	if (slash != NULL)
		*slash = '\0';
	if (*copy != '\0' && win32_ipc_ensure_dir(copy, cause) != 0) {
		free(normalized);
		free(copy);
		return (-1);
	}

	wnormalized = win32_utf8_to_wide(normalized);
	if (wnormalized == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't convert directory path: %s",
			    normalized);
		free(normalized);
		free(copy);
		errno = EINVAL;
		return (-1);
	}

	attr = GetFileAttributesW(wnormalized);
	if (attr != INVALID_FILE_ATTRIBUTES) {
		free(wnormalized);
		free(normalized);
		free(copy);
		if (attr & FILE_ATTRIBUTE_DIRECTORY)
			return (0);
		if (cause != NULL)
			xasprintf(cause, "%s exists and is not a directory",
			    normalized);
		errno = ENOTDIR;
		return (-1);
	}

	if (!CreateDirectoryW(wnormalized, NULL)) {
		DWORD error = GetLastError();

		if (error != ERROR_ALREADY_EXISTS) {
			if (cause != NULL) {
				xasprintf(cause, "couldn't create directory %s"
				    " (%s)", normalized, win32_strerror(error));
			}
			free(wnormalized);
			free(normalized);
			free(copy);
			errno = EACCES;
			return (-1);
		}
	}
	free(wnormalized);
	free(normalized);
	free(copy);
	return (0);
}

int
win32_ipc_ensure_socket_dir(const char *path, char **cause)
{
	if (win32_ipc_ensure_dir(path, cause) != 0)
		return (-1);
	if (win32_ipc_set_path_security(path, cause) != 0)
		return (-1);
	return (0);
}

int
win32_ipc_server_create(const char *path, char **cause)
{
	struct sockaddr_un	 sun;
	char			*normalized, *parent;
	char			*slash;
	SOCKET			 fd;
	int			 saved_errno, wrapped_fd;

	normalized = win32_ipc_normalize_path(path);
	if (normalized == NULL) {
		if (cause != NULL)
			xasprintf(cause, "invalid socket path");
		errno = EINVAL;
		return (-1);
	}
	if (win32_ipc_path_to_sockaddr(normalized, &sun, cause) != 0) {
		free(normalized);
		return (-1);
	}

	parent = xstrdup(normalized);
	slash = strrchr(parent, '/');
	if (slash != NULL) {
		*slash = '\0';
		if (win32_ipc_ensure_dir(parent, cause) != 0) {
			free(parent);
			free(normalized);
			return (-1);
		}
	}
	free(parent);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == INVALID_SOCKET) {
		int error = WSAGetLastError();

		if (cause != NULL) {
			xasprintf(cause, "socket failed: %s",
			    win32_strerror(error));
		}
		errno = win32_ipc_errno(error);
		return (-1);
	}
	if (bind(fd, (struct sockaddr *)&sun, sizeof sun) != 0) {
		int error = WSAGetLastError();

		if (cause != NULL) {
			xasprintf(cause, "error creating %s (%s)", normalized,
			    win32_strerror(error));
		}
		errno = win32_ipc_errno(error);
		closesocket(fd);
		free(normalized);
		return (-1);
	}
	if (listen(fd, 128) != 0) {
		int error = WSAGetLastError();

		if (cause != NULL) {
			xasprintf(cause, "error listening on %s (%s)", normalized,
			    win32_strerror(error));
		}
		errno = win32_ipc_errno(error);
		closesocket(fd);
		free(normalized);
		return (-1);
	}
	if (win32_ipc_set_blocking(fd, 0, cause) != 0) {
		saved_errno = errno;
		closesocket(fd);
		free(normalized);
		errno = saved_errno;
		return (-1);
	}

	wrapped_fd = win32_ipc_save_socket(fd);
	if (win32_ipc_set_cleanup_path(wrapped_fd, normalized) != 0) {
		if (cause != NULL)
			xasprintf(cause, "couldn't track Win32 IPC socket path");
		free(normalized);
		win32_ipc_close(wrapped_fd);
		return (-1);
	}
	free(normalized);
	return (wrapped_fd);
}

int
win32_ipc_client_connect(const char *path, __unused uint64_t flags, char **cause)
{
	struct sockaddr_un	 sun;
	char			*normalized;
	SOCKET			 fd;
	int			 wrapped_fd;

	normalized = win32_ipc_normalize_path(path);
	if (normalized == NULL) {
		if (cause != NULL)
			xasprintf(cause, "invalid socket path");
		errno = EINVAL;
		return (-1);
	}
	if (win32_ipc_path_to_sockaddr(normalized, &sun, cause) != 0) {
		free(normalized);
		return (-1);
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == INVALID_SOCKET) {
		int error = WSAGetLastError();

		if (cause != NULL) {
			xasprintf(cause, "socket failed: %s",
			    win32_strerror(error));
		}
		errno = win32_ipc_errno(error);
		free(normalized);
		return (-1);
	}
	if (connect(fd, (struct sockaddr *)&sun, sizeof sun) != 0) {
		int error = WSAGetLastError();

		if (cause != NULL) {
			xasprintf(cause, "couldn't connect to %s: %s", normalized,
			    win32_strerror(error));
		}
		errno = win32_ipc_errno(error);
		closesocket(fd);
		free(normalized);
		return (-1);
	}

	wrapped_fd = win32_ipc_save_socket(fd);
	free(normalized);
	return (wrapped_fd);
}

int
win32_ipc_socket_accept(int fd, char **cause)
{
	SOCKET				 newfd;
	struct sockaddr_storage		 ss;
	int				 len = sizeof ss;
	int				 saved_errno;

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
	if (win32_ipc_set_blocking(newfd, 0, cause) != 0) {
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
	struct win32_ipc_socket_entry	*entry;
	int				 retval;

	entry = win32_ipc_find_socket(fd);
	if (entry == NULL) {
		errno = EBADF;
		return (-1);
	}

	retval = closesocket(entry->socket);
	if (entry->cleanup_path != NULL) {
		(void)win32_unlink_utf8(entry->cleanup_path);
		free(entry->cleanup_path);
	}
	TAILQ_REMOVE(&win32_ipc_sockets, entry, entry);
	free(entry);
	return (retval);
}

#endif /* TMUX_WIN32 */
