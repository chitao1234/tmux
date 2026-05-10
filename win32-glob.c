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
#include <glob.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#ifdef TMUX_WIN32

static int	win32_fnmatch(const char *, const char *, int);

static int
win32_fnmatch_range(const char **pattern, int ch, int flags)
{
	const char	*cp = *pattern, *start;
	int		 negate = 0, matched = 0, c, first, last;

	if (*cp == '!' || *cp == '^') {
		negate = 1;
		cp++;
	}
	start = cp;
	for (;;) {
		c = (unsigned char)*cp++;
		if (c == '\0') {
			*pattern = start;
			return (0);
		}
		if (c == ']' && cp - 1 != start)
			break;
		if (c == '\\' && (~flags & FNM_NOESCAPE) && *cp != '\0')
			c = (unsigned char)*cp++;
		first = c;
		last = c;
		if (*cp == '-' && cp[1] != '\0' && cp[1] != ']') {
			cp++;
			last = (unsigned char)*cp++;
			if (last == '\\' && (~flags & FNM_NOESCAPE) &&
			    *cp != '\0')
				last = (unsigned char)*cp++;
		}
		if (flags & FNM_CASEFOLD) {
			first = tolower(first);
			last = tolower(last);
			ch = tolower(ch);
		}
		if (first <= ch && ch <= last)
			matched = 1;
	}
	*pattern = cp;
	return (negate ? !matched : matched);
}

static int
win32_fnmatch(const char *pattern, const char *string, int flags)
{
	int	pc, sc;

	for (;;) {
		pc = (unsigned char)*pattern++;
		switch (pc) {
		case '\0':
			return (*string == '\0');
		case '?':
			sc = (unsigned char)*string++;
			if (sc == '\0')
				return (0);
			if ((flags & FNM_PATHNAME) && (sc == '/' || sc == '\\'))
				return (0);
			break;
		case '*':
			while (*pattern == '*')
				pattern++;
			if (*pattern == '\0') {
				if (~flags & FNM_PATHNAME)
					return (1);
				return (strchr(string, '/') == NULL &&
				    strchr(string, '\\') == NULL);
			}
			do {
				if (win32_fnmatch(pattern, string, flags))
					return (1);
				sc = (unsigned char)*string++;
				if (sc == '\0')
					return (0);
				if ((flags & FNM_PATHNAME) &&
				    (sc == '/' || sc == '\\'))
					return (0);
			} while (1);
		case '[':
			sc = (unsigned char)*string++;
			if (sc == '\0')
				return (0);
			if ((flags & FNM_PATHNAME) && (sc == '/' || sc == '\\'))
				return (0);
			if (!win32_fnmatch_range(&pattern, sc, flags))
				return (0);
			break;
		case '\\':
			if ((~flags & FNM_NOESCAPE) && *pattern != '\0')
				pc = (unsigned char)*pattern++;
			/* FALLTHROUGH */
		default:
			sc = (unsigned char)*string++;
			if (flags & FNM_CASEFOLD) {
				pc = tolower(pc);
				sc = tolower(sc);
			}
			if (pc != sc)
				return (0);
			break;
		}
	}
}

int
fnmatch(const char *pattern, const char *string, int flags)
{
	if ((flags & FNM_PERIOD) && string[0] == '.' && pattern[0] != '.')
		return (FNM_NOMATCH);
	return (win32_fnmatch(pattern, string, flags) ? 0 : FNM_NOMATCH);
}

static int
win32_glob_compare(const void *a0, const void *b0)
{
	char	*const *a = a0, *const *b = b0;

	return (strcmp(*a, *b));
}

static int
win32_glob_magic(const char *pattern, int flags)
{
	const char	*cp;

	for (cp = pattern; *cp != '\0'; cp++) {
		if (~flags & GLOB_NOESCAPE && *cp == '\\') {
			if (cp[1] != '\0')
				cp++;
			continue;
		}
		if (*cp == '*' || *cp == '?' || *cp == '[')
			return (1);
	}
	return (0);
}

static char *
win32_glob_unescape(const char *pattern, int flags)
{
	char		*out, *dst;
	const char	*src;

	out = xmalloc(strlen(pattern) + 1);
	dst = out;
	for (src = pattern; *src != '\0'; src++) {
		if (~flags & GLOB_NOESCAPE && *src == '\\' &&
		    src[1] != '\0') {
			src++;
			*dst++ = *src;
			continue;
		}
		*dst++ = (*src == '/') ? '\\' : *src;
	}
	*dst = '\0';
	return (out);
}

static void
win32_glob_dirname(const char *path, char **dir)
{
	const char	*last1, *last2, *last;
	size_t		 len;

	last1 = strrchr(path, '\\');
	last2 = strrchr(path, '/');
	if (last1 == NULL || (last2 != NULL && last2 > last1))
		last = last2;
	else
		last = last1;

	if (last == NULL) {
		*dir = xstrdup("");
		return;
	}
	len = last - path + 1;
	*dir = xmalloc(len + 1);
	memcpy(*dir, path, len);
	(*dir)[len] = '\0';
}

static void
win32_glob_add(glob_t *g, const char *path)
{
	g->gl_pathv = xreallocarray(g->gl_pathv, g->gl_pathc + 2,
	    sizeof *g->gl_pathv);
	g->gl_pathv[g->gl_pathc++] = xstrdup(path);
	g->gl_pathv[g->gl_pathc] = NULL;
}

int
glob(const char *pattern, int flags, __unused int (*errfunc)(const char *, int),
    glob_t *g)
{
	WIN32_FIND_DATAW	 data;
	HANDLE			 find;
	DWORD			 attrs;
	wchar_t			*wpattern;
	char			*path, *dir, *name, *matched;
	int			 magic, result = GLOB_NOMATCH;
	size_t			 size;

	memset(g, 0, sizeof *g);

	magic = win32_glob_magic(pattern, flags);
	path = win32_glob_unescape(pattern, flags);
	wpattern = win32_utf8_to_wide(path);
	if (wpattern == NULL) {
		free(path);
		return (GLOB_NOSPACE);
	}

	if (!magic) {
		attrs = GetFileAttributesW(wpattern);
		if (attrs != INVALID_FILE_ATTRIBUTES) {
			win32_glob_add(g, path);
			result = 0;
		}
		free(wpattern);
		free(path);
		if (result != 0 && (flags & GLOB_NOCHECK)) {
			win32_glob_add(g, pattern);
			return (0);
		}
		return (result);
	}

	win32_glob_dirname(path, &dir);
	find = FindFirstFileW(wpattern, &data);
	free(wpattern);
	if (find == INVALID_HANDLE_VALUE) {
		free(dir);
		if (flags & GLOB_NOCHECK) {
			win32_glob_add(g, pattern);
			free(path);
			return (0);
		}
		free(path);
		return (GLOB_NOMATCH);
	}

	do {
		if (wcscmp(data.cFileName, L".") == 0 ||
		    wcscmp(data.cFileName, L"..") == 0)
			continue;
		name = win32_wide_to_utf8(data.cFileName);
		if (name == NULL) {
			result = GLOB_NOSPACE;
			break;
		}
		size = strlen(dir) + strlen(name) + 1;
		matched = xmalloc(size);
		xsnprintf(matched, size, "%s%s", dir, name);
		win32_glob_add(g, matched);
		free(matched);
		free(name);
		result = 0;
	} while (FindNextFileW(find, &data));

	FindClose(find);
	if (result == 0 && (~flags & GLOB_NOSORT))
		qsort(g->gl_pathv, g->gl_pathc, sizeof *g->gl_pathv,
		    win32_glob_compare);
	free(dir);
	free(path);
	return (result);
}

void
globfree(glob_t *g)
{
	size_t	i;

	if (g == NULL)
		return;
	for (i = 0; i < g->gl_pathc; i++)
		free(g->gl_pathv[i]);
	free(g->gl_pathv);
	memset(g, 0, sizeof *g);
}

#endif /* TMUX_WIN32 */
