/*
 * win32-screentap - render a fixed corpus, then report the resulting screen.
 *
 * The point is to compare renderers by what ends up on screen, not by the
 * bytes that were sent. tmux rewrites escape sequences as it redraws, so a
 * byte comparison would report differences that do not exist; the character
 * grid is the thing a user actually sees.
 *
 * Two modes:
 *
 *   screentap self <outfile> [cols rows]
 *        Write the corpus to this console, read the screen buffer back with
 *        ReadConsoleOutputW, and dump the grid. Use directly in conhost or in
 *        a ConPTY-backed console.
 *
 *   screentap emit
 *        Only write the corpus, then exit. Use inside a tmux pane and read the
 *        result with capture-pane.
 *
 * The corpus covers the cases where renderers realistically diverge: wide
 * characters, combining marks, tabs, erase, cursor addressing, wrapping, and
 * scrolling regions. Every line is fixed text so the expected grid can be
 * written down by hand.
 */

#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- corpus */

/*
 * Each entry is one screen line. Keeping them one-per-line means a diff points
 * straight at the feature that broke.
 */
/*
 * Width corpus.
 *
 * Each line brackets the character under test with ASCII markers, so the
 * reported grid directly shows how many cells it occupied: anything after the
 * marker shifts by the character's width, and a host that disagrees produces a
 * visibly different column rather than a subtle rendering difference.
 *
 * The ranges cover every width class renderers actually disagree about:
 * ambiguous-width symbols, the CJK/Hangul/Kana blocks, halfwidth and
 * fullwidth forms, plane-2 ideographs, zero-width characters, combining
 * marks, variation selectors, and emoji including ZWJ sequences.
 *
 * Kept to 28 lines so the whole corpus fits a stock 30-row console window
 * without scrolling; a corpus that silently loses its first lines to
 * scrollback is worse than a shorter one.
 */
static const wchar_t *corpus_width[] = {
	/* ambiguity: 1 cell in most terminals, 2 in CJK locales */
	L"20 amb boxdraw \x2500|\x2502|",
	L"21 amb arrow \x2190|\x2192|",
	L"22 amb punct \x2018|\x2019|",
	L"23 amb degree \x00b0|\x00b1|",

	/* definitely wide */
	L"30 cjk han \x4e2d|\x6587|",
	L"31 cjk hira \x3042|\x3044|",
	L"32 cjk kata \x30a2|\x30a4|",
	L"33 hangul \xac00|\xd55c|",
	L"34 hangul jamo \x1100|\x1160|",
	L"35 cjk compat \xf900|\xfaff|",
	L"36 fullwidth \xff21|\xff41|",
	L"37 halfwidth kata \xff66|\xff9d|",
	L"38 cjk ext b \U00020000|\U0002a6df|",

	/* zero width and combining */
	L"40 zwsp a\x200b" L"b|",
	L"41 zwj a\x200d" L"b|",
	L"42 combining e\x0301|",
	L"43 combining multi a\x0300\x0301\x0302|",
	L"44 variation sel \x2764\xfe0f|",

	/* surrogate pairs: one character in two UTF-16 units */
	L"50 emoji face \U0001f600|\U0001f601|",
	L"51 emoji symbol \U0001f4a9|\U0001f525|",
	L"52 emoji flag \U0001f1e8\U0001f1f3|",
	L"53 emoji zwj \U0001f468\x200d\U0001f469|",

	/* wide next to narrow, which is where off-by-one shows up */
	L"60 wide+ascii \x4e2d" L"AB" L"\x6587" L"CD|",
	L"61 ascii+wide " L"AB" L"\x4e2d" L"CD" L"\x6587|",
	L"62 wide only \x4e2d\x6587\x4e2d\x6587|",
	L"63 wide at eol \x4e2d\x6587",

	/* control behaviour */
	L"70 tab a\tb\tc",
	L"72 cursor \x1b[4GX",
	NULL
};

/*
 * Attribute corpus: colours and text attributes. Separate from the width
 * corpus because it needs a different window shape and because a failure here
 * means something completely different from a width mismatch.
 */
static const wchar_t *corpus_attr[] = {
	L"01 plain ascii",
	L"02 bold \x1b[1mBOLD\x1b[0m",
	L"03 dim \x1b[2mDIM\x1b[0m",
	L"04 italic \x1b[3mITALIC\x1b[0m",
	L"05 underline \x1b[4mUNDER\x1b[0m",
	L"06 blink \x1b[5mBLINK\x1b[0m",
	L"07 reverse \x1b[7mREV\x1b[0m",
	L"08 hidden \x1b[8mHIDDEN\x1b[0m",
	L"09 strike \x1b[9mSTRIKE\x1b[0m",
	L"10 fg black \x1b[30mBLACK\x1b[0m",
	L"11 fg red \x1b[31mRED\x1b[0m",
	L"12 fg green \x1b[32mGREEN\x1b[0m",
	L"13 fg yellow \x1b[33mYELLOW\x1b[0m",
	L"14 fg blue \x1b[34mBLUE\x1b[0m",
	L"15 fg magenta \x1b[35mMAGENTA\x1b[0m",
	L"16 fg cyan \x1b[36mCYAN\x1b[0m",
	L"17 fg white \x1b[37mWHITE\x1b[0m",
	L"18 bg red \x1b[41mBGRED\x1b[0m",
	L"19 bg bright \x1b[101mBGBRIGHT\x1b[0m",
	L"20 fg bright \x1b[91mBRIGHT\x1b[0m",
	L"21 fg 256 \x1b[38;5;196mC256\x1b[0m",
	L"22 bg 256 \x1b[48;5;21mB256\x1b[0m",
	L"23 fg rgb \x1b[38;2;255;0;0mRGB\x1b[0m",
	L"24 bg rgb \x1b[48;2;0;128;255mBRGB\x1b[0m",
	L"25 attr mix \x1b[1;4;31mBUR\x1b[0m",
	L"26 all off AA\x1b[1;4;7mBB\x1b[0mCC",
	NULL
};

/* ---------------------------------------------------------------- helpers */

/* ---------------------------------------------------------------- helpers */

static int
set_vt_output(HANDLE out)
{
	DWORD	mode;

	if (!GetConsoleMode(out, &mode))
		return (-1);
	mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
#ifdef DISABLE_NEWLINE_AUTO_RETURN
	mode |= DISABLE_NEWLINE_AUTO_RETURN;
#endif
	if (!SetConsoleMode(out, mode)) {
#ifdef DISABLE_NEWLINE_AUTO_RETURN
		mode &= ~DISABLE_NEWLINE_AUTO_RETURN;
		if (!SetConsoleMode(out, mode))
#endif
			return (-1);
	}
	return (0);
}

/*
 * Console buffer conventions for double-width characters.
 *
 * A wide character occupies two cells. conhost marks them so a reader does not
 * have to guess: the leading cell carries COMMON_LVB_LEADING_BYTE and the
 * trailing cell carries COMMON_LVB_TRAILING_BYTE. Using those bits means the
 * reported width is the host's own answer, not a table compiled into the
 * probe, which is exactly what makes the comparison meaningful.
 */
#ifndef COMMON_LVB_LEADING_BYTE
#define COMMON_LVB_LEADING_BYTE		0x0100
#endif
#ifndef COMMON_LVB_TRAILING_BYTE
#define COMMON_LVB_TRAILING_BYTE	0x0200
#endif

/*
 * Render one cell as text. Non-ASCII characters are written as \uXXXX so a
 * comparison never depends on the codepage of whatever terminal the report is
 * being read in.
 */
static int
cell_text(char *dst, size_t cap, size_t *used, WCHAR ch)
{
	int	n;

	if (ch >= 0x20 && ch < 0x7f) {
		if (*used + 1 >= cap)
			return (-1);
		dst[(*used)++] = (char)ch;
		return (0);
	}
	if (ch == 0) {
		if (*used + 1 >= cap)
			return (-1);
		dst[(*used)++] = ' ';
		return (0);
	}
	n = snprintf(dst + *used, cap - *used, "\\u%04x", (unsigned)ch);
	if (n <= 0 || (size_t)n >= cap - *used)
		return (-1);
	*used += (size_t)n;
	return (0);
}


/* -------------------------------------------------------------- emit mode */

static int
emit_corpus(const wchar_t **corpus)
{
	HANDLE	out;
	DWORD	written;
	int	i;

	out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (out == INVALID_HANDLE_VALUE)
		return (1);
	(void)set_vt_output(out);

	for (i = 0; corpus[i] != NULL; i++) {
		if (!WriteConsoleW(out, corpus[i], (DWORD)wcslen(corpus[i]),
		    &written, NULL)) {
			fprintf(stderr, "emit: WriteConsoleW failed %lu\n",
			    (unsigned long)GetLastError());
			return (1);
		}
		if (corpus[i + 1] != NULL) {
			if (!WriteConsoleW(out, L"\r\n", 2, &written, NULL))
				return (1);
		}
	}
	return (0);
}

/* -------------------------------------------------------------- self mode */

static int
dump_screen(const char *path)
{
	FILE			*f;
	HANDLE			 out;
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	CHAR_INFO		*buf = NULL;
	COORD			 size, origin;
	SMALL_RECT		 region;
	char			 line[8192];
	size_t			 used;
	SHORT			 x, y, width, height;
	int			 retval = 1;

	f = fopen(path, "wb");
	if (f == NULL)
		return (1);

	out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
	    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (out == INVALID_HANDLE_VALUE) {
		fprintf(f, "result error=no_console\n");
		fclose(f);
		return (1);
	}
	if (!GetConsoleScreenBufferInfo(out, &csbi)) {
		fprintf(f, "result error=no_info\n");
		CloseHandle(out);
		fclose(f);
		return (1);
	}

	/*
	 * Read only the visible window. A conhost buffer is thousands of rows
	 * tall, and dumping all of it buries the answer in blank rows.
	 */
	width = (SHORT)(csbi.srWindow.Right - csbi.srWindow.Left + 1);
	height = (SHORT)(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
	size.X = width;
	size.Y = height;
	origin.X = 0;
	origin.Y = 0;
	region.Left = csbi.srWindow.Left;
	region.Top = csbi.srWindow.Top;
	region.Right = csbi.srWindow.Right;
	region.Bottom = csbi.srWindow.Bottom;

	buf = calloc((size_t)width * (size_t)height, sizeof *buf);
	if (buf == NULL) {
		CloseHandle(out);
		fclose(f);
		return (1);
	}
	if (!ReadConsoleOutputW(out, buf, size, origin, &region)) {
		fprintf(f, "result error=read_failed err=%lu\n",
		    (unsigned long)GetLastError());
		goto out;
	}

	fprintf(f, "buffer=%dx%d\n", (int)csbi.dwSize.X,
	    (int)csbi.dwSize.Y);
	fprintf(f, "window=%dx%d\n", (int)width, (int)height);
	fprintf(f, "cursor=%d,%d\n",
	    (int)(csbi.dwCursorPosition.X - csbi.srWindow.Left),
	    (int)(csbi.dwCursorPosition.Y - csbi.srWindow.Top));

	/*
	 * Text grid relative to the window, trailing blanks trimmed.
	 *
	 * Every character is encoded as its own code unit, so a surrogate pair
	 * shows as two escapes and a double-width character as one.
	 *
	 * Width comes from the buffer itself: conhost tags the leading cell of a
	 * wide character with COMMON_LVB_LEADING_BYTE and the trailing cell with
	 * COMMON_LVB_TRAILING_BYTE. Trusting those bits rather than a built-in
	 * width table means the report states what the host actually did, which
	 * is the whole point of comparing hosts. A cell tagged as a trailing
	 * byte is not a character of its own and is skipped.
	 */
	for (y = 0; y < height; y++) {
		used = 0;
		line[0] = '\0';
		for (x = 0; x < width; x++) {
			CHAR_INFO *ci = &buf[(size_t)y * width + x];
			WCHAR ch = ci->Char.UnicodeChar;

			if (ci->Attributes & COMMON_LVB_TRAILING_BYTE)
				continue;
			if (cell_text(line, sizeof line, &used, ch) != 0)
				break;
		}
		while (used > 0 && line[used - 1] == ' ')
			used--;
		line[used] = '\0';
		if (used == 0)
			continue;
		fprintf(f, "row %02d |%s|\n", (int)y, line);
	}

	/*
	 * Attribute grid: only cells that differ from the default attribute,
	 * so a clean run produces no attr lines at all.
	 */
	for (y = 0; y < height; y++) {
		int any = 0;
		used = 0;
		for (x = 0; x < width; x++) {
			CHAR_INFO *ci = &buf[(size_t)y * width + x];
			if (ci->Attributes == csbi.wAttributes)
				continue;
			if (!any) {
				used = (size_t)snprintf(line, sizeof line,
				    "attr %02d |", (int)y);
				any = 1;
			}
			used += (size_t)snprintf(line + used,
			    sizeof line - used, "%d:%04x ", (int)x,
			    (unsigned)ci->Attributes);
		}
		if (any) {
			line[used++] = '|';
			line[used] = '\0';
			fprintf(f, "%s\n", line);
		}
	}

	fprintf(f, "result ok=1\n");
	retval = 0;

out:
	free(buf);
	CloseHandle(out);
	fclose(f);
	return (retval);
}

/* ------------------------------------------------------------------ main */

int
main(int argc, char **argv)
{
	const wchar_t	**corpus;
	HANDLE		 out;
	DWORD		 written;

	if (argc < 2) {
		fprintf(stderr, "usage: screentap <self|emit> <width|attr> "
		    "[outfile]\n");
		return (2);
	}
	if (argc < 3) {
		fprintf(stderr, "screentap: need a corpus name\n");
		return (2);
	}
	if (strcmp(argv[2], "width") == 0)
		corpus = corpus_width;
	else if (strcmp(argv[2], "attr") == 0)
		corpus = corpus_attr;
	else {
		fprintf(stderr, "screentap: unknown corpus '%s'\n", argv[2]);
		return (2);
	}

	if (strcmp(argv[1], "emit") == 0)
		return (emit_corpus(corpus));
	if (strcmp(argv[1], "self") == 0 && argc >= 4) {
		if (emit_corpus(corpus) != 0)
			return (1);
		/*
		 * Give the console a moment to finish processing the last
		 * sequences before reading the buffer back.
		 */
		out = GetStdHandle(STD_OUTPUT_HANDLE);
		if (out != INVALID_HANDLE_VALUE)
			(void)WriteConsoleW(out, L"", 0, &written, NULL);
		Sleep(250);
		return (dump_screen(argv[3]));
	}
	fprintf(stderr, "screentap: unknown verb '%s'\n", argv[1]);
	return (2);
}
