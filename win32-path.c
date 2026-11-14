/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

/*
 * Win32 path layer.
 *
 * Every path used by the port is described by a struct win32_path with two
 * independent views:
 *
 *   display   - the spelling to show the user: drive letter case, separator
 *               style and the trailing separator are preserved.
 *   identity  - an ordinal-case-insensitive, separator-normalized form used
 *               for comparison, guards and hashing.
 *
 * Canonicalization happens in exactly one place so that no caller has to
 * re-implement "is this absolute", "is this a root", "does this alias that".
 * Shell expansion (~ and $VAR) is deliberately *not* part of this layer.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#ifdef TMUX_WIN32

#include <windows.h>

/*
 * Resolve a path against a base directory without touching the filesystem
 * beyond what GetFullPathNameW does (it normalizes . and .. but does not
 * resolve reparse points). Returns NULL on failure with errno set.
 */
static char *
win32_path_resolve(const char *path, const char *base)
{
	wchar_t	*wpath, *wbase = NULL, *wcombined = NULL, *wresolved = NULL;
	char	*resolved = NULL;
	size_t	 len, base_len, path_len, used;
	DWORD	 n;

	if (path == NULL || *path == '\0') {
		errno = EINVAL;
		return (NULL);
	}
	if (path_is_drive_relative(path)) {
		errno = EINVAL;
		return (NULL);
	}

	wpath = win32_utf8_to_wide(path);
	if (wpath == NULL) {
		errno = EINVAL;
		return (NULL);
	}

	if (path_is_absolute(path) || base == NULL || *base == '\0' ||
	    !path_is_absolute(base)) {
		len = wcslen(wpath);
		wcombined = xcalloc(len + 1, sizeof *wcombined);
		memcpy(wcombined, wpath, (len + 1) * sizeof *wcombined);
	} else {
		wbase = win32_utf8_to_wide(base);
		if (wbase == NULL)
			goto out;
		base_len = wcslen(wbase);
		path_len = wcslen(wpath);
		used = base_len;
		if (base_len != 0 && wbase[base_len - 1] != L'\\' &&
		    wbase[base_len - 1] != L'/')
			used++;
		wcombined = xcalloc(used + path_len + 1, sizeof *wcombined);
		memcpy(wcombined, wbase, base_len * sizeof *wcombined);
		if (used != base_len)
			wcombined[base_len] = L'\\';
		memcpy(wcombined + used, wpath,
		    (path_len + 1) * sizeof *wcombined);
	}

	n = GetFullPathNameW(wcombined, 0, NULL, NULL);
	if (n == 0)
		goto out;
	wresolved = xcalloc(n, sizeof *wresolved);
	if (GetFullPathNameW(wcombined, n, wresolved, NULL) == 0)
		goto out;

	resolved = win32_wide_to_utf8(wresolved);
	if (resolved == NULL)
		goto out;

out:
	free(wpath);
	free(wbase);
	free(wcombined);
	free(wresolved);
	return (resolved);
}

/*
 * Build the identity view of a resolved path: ordinal case folding (not
 * locale dependent), forward separators and no trailing separator beyond a
 * drive or share root.
 */
static char *
win32_path_identity(const char *path)
{
	wchar_t	*wpath;
	char	*identity;
	int	 n;
	size_t	 i, len;

	if (path == NULL)
		return (NULL);
	wpath = win32_utf8_to_wide(path);
	if (wpath == NULL)
		return (NULL);

	n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wpath, -1,
	    NULL, 0, NULL, NULL, 0);
	if (n <= 0) {
		free(wpath);
		return (NULL);
	}
	{
		wchar_t	*lower = xcalloc(n, sizeof *lower);

		if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wpath,
		    -1, lower, n, NULL, NULL, 0) <= 0) {
			free(lower);
			free(wpath);
			return (NULL);
		}
		identity = win32_wide_to_utf8(lower);
		free(lower);
	}
	free(wpath);
	if (identity == NULL)
		return (NULL);

	for (i = 0; identity[i] != '\0'; i++) {
		if (identity[i] == '\\')
			identity[i] = '/';
	}
	len = strlen(identity);
	while (len > 3 && identity[len - 1] == '/')
		identity[--len] = '\0';
	return (identity);
}

/*
 * Resolve and normalize a path. The display view keeps the resolved spelling;
 * the identity view is the comparison form. Either output may be NULL if the
 * caller is not interested.
 */
int
win32_path_canon(const char *path, const char *base, char **displayp,
    char **identityp, char **cause)
{
	char	*resolved, *identity;

	if (displayp != NULL)
		*displayp = NULL;
	if (identityp != NULL)
		*identityp = NULL;
	if (cause != NULL)
		*cause = NULL;

	resolved = win32_path_resolve(path, base);
	if (resolved == NULL) {
		if (cause != NULL) {
			if (path_is_drive_relative(path))
				xasprintf(cause, "path must not be relative to "
				    "a drive: %s", path);
			else
				xasprintf(cause, "couldn't resolve path: %s",
				    path);
		}
		errno = EINVAL;
		return (-1);
	}
	identity = win32_path_identity(resolved);
	if (identity == NULL) {
		if (cause != NULL)
			xasprintf(cause, "couldn't normalize path: %s", resolved);
		free(resolved);
		errno = EINVAL;
		return (-1);
	}

	if (displayp != NULL)
		*displayp = resolved;
	else
		free(resolved);
	if (identityp != NULL)
		*identityp = identity;
	else
		free(identity);
	return (0);
}

/*
 * Comparison form only. This replaces the adapted per-caller helpers.
 */
char *
win32_path_canon_identity(const char *path, const char *base, char **cause)
{
	char	*identity;

	if (win32_path_canon(path, base, NULL, &identity, cause) != 0)
		return (NULL);
	return (identity);
}

/*
 * Join two paths with a single separator, without resolving. Callers that
 * need a normalized result should use win32_path_canon afterwards.
 */
#endif /* TMUX_WIN32 */
