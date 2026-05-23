/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include <sys/types.h>

#include <glob.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#ifdef TMUX_WIN32

static int	win32_fnmatch(const wchar_t *, const wchar_t *, int, int);

static int
win32_glob_is_separator(int ch)
{
	return (ch == '/' || ch == '\\');
}

static int
win32_glob_is_wseparator(wchar_t ch)
{
	return (ch == L'/' || ch == L'\\');
}

static wchar_t
win32_fnmatch_fold(wchar_t ch)
{
	WCHAR	wch = ch;

	CharLowerBuffW(&wch, 1);
	return (wch);
}

static int
win32_fnmatch_equal(wchar_t pc, wchar_t sc, int flags)
{
	if (win32_glob_is_wseparator(pc) && win32_glob_is_wseparator(sc))
		return (1);
	if (flags & FNM_CASEFOLD) {
		pc = win32_fnmatch_fold(pc);
		sc = win32_fnmatch_fold(sc);
	}
	return (pc == sc);
}

static int
win32_fnmatch_period(const wchar_t *pattern, const wchar_t *string, int flags,
    int component_start)
{
	if ((flags & FNM_PERIOD) && component_start && string[0] == L'.' &&
	    pattern[0] != L'.')
		return (0);
	return (1);
}

static int
win32_fnmatch_range(const wchar_t **pattern, wchar_t ch, int flags)
{
	const wchar_t	*cp = *pattern, *start;
	int		 negate = 0, matched = 0;
	wchar_t		 c, first, last;

	if (*cp == L'!' || *cp == L'^') {
		negate = 1;
		cp++;
	}
	start = cp;
	for (;;) {
		c = *cp++;
		if (c == L'\0') {
			*pattern = start;
			return (0);
		}
		if (c == L']' && cp - 1 != start)
			break;
		if (c == L'\\' && (~flags & FNM_NOESCAPE) &&
		    *cp != L'\0')
			c = *cp++;
		first = c;
		last = c;
		if (*cp == L'-' && cp[1] != L'\0' && cp[1] != L']') {
			cp++;
			last = *cp++;
			if (last == L'\\' && (~flags & FNM_NOESCAPE) &&
			    *cp != L'\0')
				last = *cp++;
		}
		if (flags & FNM_CASEFOLD) {
			first = win32_fnmatch_fold(first);
			last = win32_fnmatch_fold(last);
			ch = win32_fnmatch_fold(ch);
		}
		if (first <= ch && ch <= last)
			matched = 1;
	}
	*pattern = cp;
	return (negate ? !matched : matched);
}

static int
win32_fnmatch(const wchar_t *pattern, const wchar_t *string, int flags,
    int component_start)
{
	wchar_t	pc, sc;

	for (;;) {
		pc = *pattern++;
		switch (pc) {
		case L'\0':
			return (*string == L'\0');
		case L'?':
			if (!win32_fnmatch_period(pattern - 1, string, flags,
			    component_start))
				return (0);
			sc = *string++;
			if (sc == L'\0')
				return (0);
			if ((flags & FNM_PATHNAME) &&
			    win32_glob_is_wseparator(sc))
				return (0);
			component_start = 0;
			break;
		case L'*':
			while (*pattern == L'*')
				pattern++;
			if (component_start && (flags & FNM_PERIOD) &&
			    string[0] == L'.')
				return (0);
			if (*pattern == L'\0') {
				if (~flags & FNM_PATHNAME)
					return (1);
				while (*string != L'\0') {
					if (win32_glob_is_wseparator(*string))
						return (0);
					string++;
				}
				return (1);
			}
			do {
				if (win32_fnmatch(pattern, string, flags,
				    component_start))
					return (1);
				sc = *string++;
				if (sc == L'\0')
					return (0);
				if ((flags & FNM_PATHNAME) &&
				    win32_glob_is_wseparator(sc))
					return (0);
				component_start = 0;
			} while (1);
		case L'[':
			if (!win32_fnmatch_period(pattern - 1, string, flags,
			    component_start))
				return (0);
			sc = *string++;
			if (sc == L'\0')
				return (0);
			if ((flags & FNM_PATHNAME) &&
			    win32_glob_is_wseparator(sc))
				return (0);
			if (!win32_fnmatch_range(&pattern, sc, flags))
				return (0);
			component_start = 0;
			break;
		case L'\\':
			if ((~flags & FNM_NOESCAPE) && *pattern != L'\0')
				pc = *pattern++;
			/* FALLTHROUGH */
		default:
			sc = *string++;
			if (!win32_fnmatch_equal(pc, sc, flags))
				return (0);
			if ((flags & FNM_PATHNAME) &&
			    win32_glob_is_wseparator(sc))
				component_start = 1;
			else
				component_start = 0;
			break;
		}
	}
}

int
fnmatch(const char *pattern, const char *string, int flags)
{
	wchar_t	*wpattern, *wstring;
	int	 matched;

	wpattern = win32_utf8_to_wide(pattern);
	wstring = win32_utf8_to_wide(string);
	if (wpattern == NULL || wstring == NULL) {
		free(wpattern);
		free(wstring);
		return (FNM_NOMATCH);
	}
	matched = win32_fnmatch(wpattern, wstring, flags, 1);
	free(wpattern);
	free(wstring);
	return (matched ? 0 : FNM_NOMATCH);
}

static int
win32_glob_compare(const void *a0, const void *b0)
{
	char	*const *a = a0, *const *b = b0;

	return (strcmp(*a, *b));
}

static size_t
win32_glob_root_length(const char *path)
{
	const char	*p;

	if (path == NULL || *path == '\0')
		return (0);
	if (((path[0] >= 'A' && path[0] <= 'Z') ||
	    (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') {
		if (path[2] == '/' || path[2] == '\\')
			return (3);
		return (2);
	}
	if (win32_glob_is_separator(path[0]) &&
	    win32_glob_is_separator(path[1])) {
		p = path + 2;
		while (*p != '\0' && !win32_glob_is_separator(*p))
			p++;
		if (*p == '\0')
			return (2);
		p++;
		while (*p != '\0' && !win32_glob_is_separator(*p))
			p++;
		if (*p == '\0')
			return ((size_t)(p - path));
		return ((size_t)(p - path + 1));
	}
	if (win32_glob_is_separator(path[0]))
		return (1);
	return (0);
}

static char *
win32_glob_unescape(const char *pattern, int flags, char **escapedp)
{
	char		*out, *escaped, *dst, *edst;
	const char	*src;

	out = xmalloc(strlen(pattern) + 1);
	escaped = xmalloc(strlen(pattern) + 1);
	dst = out;
	edst = escaped;
	for (src = pattern; *src != '\0'; src++) {
		if (~flags & GLOB_NOESCAPE && *src == '\\' &&
		    src[1] != '\0') {
			src++;
			*dst++ = *src;
			*edst++ = 1;
			continue;
		}
		*dst++ = (*src == '/') ? '\\' : *src;
		*edst++ = 0;
	}
	*dst = '\0';
	*edst = '\0';
	*escapedp = escaped;
	return (out);
}

static int
win32_glob_magic(const char *pattern, const char *escaped)
{
	for (; *pattern != '\0'; pattern++, escaped++) {
		if (!*escaped &&
		    (*pattern == '*' || *pattern == '?' || *pattern == '['))
			return (1);
	}
	return (0);
}

static char *
win32_glob_memdup0(const char *src, size_t len)
{
	char	*dst;

	dst = xmalloc(len + 1);
	memcpy(dst, src, len);
	dst[len] = '\0';
	return (dst);
}

static int
win32_glob_next_segment(const char **path, const char **escaped,
    char **segment, char **segment_escaped, int *last)
{
	const char	*start;
	const char	*estart;

	while (win32_glob_is_separator(**path)) {
		(*path)++;
		(*escaped)++;
	}
	if (**path == '\0')
		return (0);
	start = *path;
	estart = *escaped;
	while (**path != '\0' && !win32_glob_is_separator(**path)) {
		(*path)++;
		(*escaped)++;
	}
	*segment = xstrndup(start, *path - start);
	*segment_escaped = win32_glob_memdup0(estart, *path - start);
	while (win32_glob_is_separator(**path)) {
		(*path)++;
		(*escaped)++;
	}
	*last = (**path == '\0');
	return (1);
}

static char *
win32_glob_join(const char *prefix, const char *name)
{
	size_t	len;
	char	*joined;

	if (prefix == NULL || *prefix == '\0')
		return (xstrdup(name));
	len = strlen(prefix);
	if (win32_glob_is_separator(prefix[len - 1]))
		xasprintf(&joined, "%s%s", prefix, name);
	else
		xasprintf(&joined, "%s\\%s", prefix, name);
	return (joined);
}

static void
win32_glob_add(glob_t *g, const char *path, int flags, DWORD attrs)
{
	char	*marked;
	size_t	 len;

	g->gl_pathv = xreallocarray(g->gl_pathv, g->gl_pathc + 2,
	    sizeof *g->gl_pathv);
	if ((flags & GLOB_MARK) && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
		len = strlen(path);
		if (len == 0 || !win32_glob_is_separator(path[len - 1])) {
			xasprintf(&marked, "%s\\", path);
			g->gl_pathv[g->gl_pathc++] = marked;
		} else
			g->gl_pathv[g->gl_pathc++] = xstrdup(path);
	} else
		g->gl_pathv[g->gl_pathc++] = xstrdup(path);
	g->gl_pathv[g->gl_pathc] = NULL;
}

static int
win32_glob_get_attributes(const char *path, DWORD *attrp)
{
	DWORD	 attrs;
	wchar_t	*wpath;

	wpath = win32_utf8_to_wide(path);
	if (wpath == NULL)
		return (-1);
	attrs = GetFileAttributesW(wpath);
	free(wpath);
	if (attrs == INVALID_FILE_ATTRIBUTES)
		return (1);
	*attrp = attrs;
	return (0);
}

static char *
win32_glob_match_pattern(const char *segment, const char *escaped)
{
	char		*pattern, *dst;
	const char	*src, *esc;

	pattern = xmalloc((2 * strlen(segment)) + 1);
	dst = pattern;
	for (src = segment, esc = escaped; *src != '\0'; src++, esc++) {
		if (*esc)
			*dst++ = '\\';
		*dst++ = *src;
	}
	*dst = '\0';
	return (pattern);
}

static void
win32_glob_expand(glob_t *g, const char *prefix, const char *rest,
    const char *rest_escaped, int flags, int *result)
{
	WIN32_FIND_DATAW	 data;
	HANDLE			 find;
	DWORD			 attrs;
	wchar_t			*wsearch;
	char			*segment, *segment_escaped, *search, *name;
	char			*path, *match_pattern;
	int			 last, magic, matchflags = FNM_CASEFOLD;

	if (*result == GLOB_NOSPACE)
		return;
	if (!win32_glob_next_segment(&rest, &rest_escaped, &segment,
	    &segment_escaped, &last)) {
		if (win32_glob_get_attributes(prefix, &attrs) == 0) {
			win32_glob_add(g, prefix, flags, attrs);
			*result = 0;
		}
		return;
	}

	magic = win32_glob_magic(segment, segment_escaped);
	if (!magic) {
		path = win32_glob_join(prefix, segment);
		free(segment);
		free(segment_escaped);
		if (win32_glob_get_attributes(path, &attrs) == 0) {
			if (last) {
				win32_glob_add(g, path, flags, attrs);
				*result = 0;
			} else if (attrs & FILE_ATTRIBUTE_DIRECTORY)
				win32_glob_expand(g, path, rest, rest_escaped,
				    flags, result);
		}
		free(path);
		return;
	}

	search = win32_glob_join(prefix, "*");
	wsearch = win32_utf8_to_wide(search);
	free(search);
	if (wsearch == NULL) {
		free(segment);
		free(segment_escaped);
		*result = GLOB_NOSPACE;
		return;
	}
	find = FindFirstFileW(wsearch, &data);
	free(wsearch);
	if (find == INVALID_HANDLE_VALUE) {
		free(segment);
		free(segment_escaped);
		return;
	}

	if (flags & GLOB_NOESCAPE)
		matchflags |= FNM_NOESCAPE;
	match_pattern = win32_glob_match_pattern(segment, segment_escaped);
	do {
		if (wcscmp(data.cFileName, L".") == 0 ||
		    wcscmp(data.cFileName, L"..") == 0)
			continue;
		name = win32_wide_to_utf8(data.cFileName);
		if (name == NULL) {
			*result = GLOB_NOSPACE;
			break;
		}
		if (fnmatch(match_pattern, name, matchflags) != 0) {
			free(name);
			continue;
		}
		path = win32_glob_join(prefix, name);
		free(name);
		if (last) {
			win32_glob_add(g, path, flags, data.dwFileAttributes);
			*result = 0;
		} else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			win32_glob_expand(g, path, rest, rest_escaped, flags,
			    result);
		free(path);
	} while (*result != GLOB_NOSPACE && FindNextFileW(find, &data));

	FindClose(find);
	free(match_pattern);
	free(segment);
	free(segment_escaped);
}

int
glob(const char *pattern, int flags, __unused int (*errfunc)(const char *, int),
    glob_t *g)
{
	DWORD			 attrs;
	wchar_t			*wpattern;
	char			*path, *escaped, *root;
	int			 magic, result = GLOB_NOMATCH;
	size_t			 rootlen;

	memset(g, 0, sizeof *g);

	path = win32_glob_unescape(pattern, flags, &escaped);
	magic = win32_glob_magic(path, escaped);
	wpattern = win32_utf8_to_wide(path);
	if (wpattern == NULL) {
		free(escaped);
		free(path);
		return (GLOB_NOSPACE);
	}

	if (!magic) {
		attrs = GetFileAttributesW(wpattern);
		if (attrs != INVALID_FILE_ATTRIBUTES) {
			win32_glob_add(g, path, flags, attrs);
			result = 0;
		}
		free(wpattern);
		free(escaped);
		free(path);
		if (result != 0 && (flags & GLOB_NOCHECK)) {
			win32_glob_add(g, pattern, flags, 0);
			return (0);
		}
		return (result);
	}
	free(wpattern);

	rootlen = win32_glob_root_length(path);
	root = xstrndup(path, rootlen);
	win32_glob_expand(g, root, path + rootlen, escaped + rootlen, flags,
	    &result);
	if (result == 0 && (~flags & GLOB_NOSORT))
		qsort(g->gl_pathv, g->gl_pathc, sizeof *g->gl_pathv,
		    win32_glob_compare);
	free(root);
	free(escaped);
	free(path);
	if (result != 0 && (flags & GLOB_NOCHECK)) {
		win32_glob_add(g, pattern, flags, 0);
		return (0);
	}
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
