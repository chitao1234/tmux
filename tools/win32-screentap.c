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
 * Corpora.
 *
 * Every line brackets the character under test with ASCII markers, so the
 * reported grid shows directly how many cells it occupied: anything after the
 * marker shifts by the character's width, and a host that disagrees produces a
 * visibly different column rather than a subtle rendering difference.
 *
 * Length is a hard constraint, not a style choice. Each host has a fixed
 * screen buffer - 40 rows for a system ConPTY, 30 for Windows Terminal and
 * WezTerm - and once the corpus is taller than the buffer the first lines
 * scroll away and cannot be read back at all. Every corpus here therefore
 * stays well under 24 lines.
 *
 * The width material is split in two so it can be covered completely:
 *
 *   width    the common cases: ambiguous width, CJK/Hangul/Kana, halfwidth
 *            and fullwidth, plane 2, surrogate pairs, wide-next-to-narrow
 *   width2   the awkward cases: zero-width and formatting characters,
 *            combining marks on wide bases, variation selectors, private use,
 *            emoji sequences
 */
static const wchar_t *corpus_width[] = {
	/* ambiguity: 1 cell in most terminals, 2 in CJK locales */
	L"01 amb boxdraw \x2500|\x2502|",
	L"02 amb arrow \x2190|\x2192|",
	L"03 amb punct \x2018|\x2019|",
	L"04 amb degree \x00b0|\x00b1|",

	/* definitely wide */
	L"05 cjk han \x4e2d|\x6587|",
	L"06 cjk hira \x3042|\x3044|",
	L"07 cjk kata \x30a2|\x30a4|",
	L"08 hangul \xac00|\xd55c|",
	L"09 hangul jamo \x1100|\x1160|",
	L"10 cjk compat \xf900|\xfaff|",
	L"11 fullwidth \xff21|\xff41|",
	L"12 halfwidth kata \xff66|\xff9d|",
	L"13 cjk ext b \U00020000|\U0002a6df|",

	/* surrogate pairs: one character in two UTF-16 units */
	L"14 emoji face \U0001f600|\U0001f601|",
	L"15 emoji symbol \U0001f4a9|\U0001f525|",
	L"16 emoji flag \U0001f1e8\U0001f1f3|",

	/* wide next to narrow, which is where off-by-one shows up */
	L"17 wide+ascii \x4e2d" L"AB" L"\x6587" L"CD|",
	L"18 ascii+wide " L"AB" L"\x4e2d" L"CD" L"\x6587|",
	L"19 wide only \x4e2d\x6587\x4e2d\x6587|",
	L"20 many wide \x4e2d\x4e2d\x4e2d\x4e2d\x4e2d\x4e2d\x4e2d|",

	/* control behaviour */
	L"21 tab a\tb\tc",
	L"22 cursor \x1b[4GX",
	NULL
};

static const wchar_t *corpus_width2[] = {
	/* zero width and formatting characters */
	L"01 zwsp a\x200b" L"b|",
	L"02 zwj a\x200d" L"b|",
	L"03 soft hyphen a\x00ad" L"b|",
	L"04 word joiner a\x2060" L"b|",
	L"05 bidi mark a\x200e" L"b|",
	L"06 hangul filler \x115f|",

	/* combining marks, including on a double-width base */
	L"07 combining e\x0301|",
	L"08 combining multi a\x0300\x0301\x0302|",
	L"09 mark on wide \x4e2d\x0301|",

	/* variation selectors */
	L"10 vs on heart \x2764\xfe0f|",
	L"11 vs on wide \x4e2d\xfe0f|",
	L"12 keycap 1\xfe0f\x20e3|",

	/* emoji sequences */
	L"13 skin tone \U0001f44d\U0001f3fd|",
	L"14 zwj family \U0001f468\x200d\U0001f469|",
	L"15 rainbow \U0001f3f3\xfe0f\x200d\U0001f308|",

	/* private use area */
	L"16 pua \ue000|\uf8ff|",

	/* wide followed by a mark then ascii */
	L"17 wide+mark+ascii \x4e2d\x0301" L"AB|",

	/* 4-byte character at end of line */
	L"18 ext b at eol \U00020000\U00020001",
	NULL
};

/*
 * Attribute corpus: colours and text attributes. Separate from the width
 * corpora because a failure here means something completely different from a
 * width mismatch.
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
	NULL
};

/* ---------------------------------------------------------------- helpers */

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
	SHORT			 x, y, width, height, first;
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
	 * Read the whole buffer, then report only the rows that contain
	 * something.
	 *
	 * The window is however tall the host happens to be - 24 rows under
	 * ConEmu, 30 under conhost, 40 under a fresh ConPTY - and once the
	 * corpus is taller than the window the earliest lines scroll out of
	 * view. Reading only the window therefore made a correct renderer look
	 * like it had lost rows, and made otherwise-identical reports differ by
	 * host purely because of their default window height.
	 *
	 * Reading buffer rows from 0 and trimming blank rows at both ends
	 * removes the window size from the comparison entirely.
	 */
	width = csbi.dwSize.X;
	height = csbi.dwSize.Y;
	size.X = width;
	size.Y = height;
	origin.X = 0;
	origin.Y = 0;
	region.Left = 0;
	region.Top = 0;
	region.Right = (SHORT)(width - 1);
	region.Bottom = (SHORT)(height - 1);

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
	fprintf(f, "cursor=%d,%d\n", (int)csbi.dwCursorPosition.X,
	    (int)csbi.dwCursorPosition.Y);

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
	/*
	 * Row numbers are relative to the first non-blank buffer row, so the
	 * report starts at row 00 no matter how far the corpus scrolled or how
	 * tall the host's default window is.
	 */
	first = -1;
	for (y = 0; y < height && first < 0; y++) {
		for (x = 0; x < width; x++) {
			CHAR_INFO *ci = &buf[(size_t)y * width + x];
			WCHAR ch = ci->Char.UnicodeChar;

			if (ch != 0 && ch != L' ') {
				first = y;
				break;
			}
		}
	}
	if (first < 0)
		first = 0;
	fprintf(f, "first=%d\n", (int)first);

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
		fprintf(f, "row %02d |%s|\n", (int)(y - first), line);
	}

	/*
	 * Attribute grid: only cells whose colour or attribute differs from the
	 * default, so a clean run produces no attr lines at all.
	 *
	 * The LVB bits are masked out of the comparison. They mark the leading
	 * and trailing halves of a double-width character, and different hosts
	 * set them on different cells; treating that as a colour difference
	 * would drown the real signal in noise. They are still counted as a
	 * width fact, by the text grid above.
	 */
	for (y = 0; y < height; y++) {
		int any = 0;
		used = 0;
		for (x = 0; x < width; x++) {
			CHAR_INFO *ci = &buf[(size_t)y * width + x];
			WORD plain = ci->Attributes &
			    (WORD)~(COMMON_LVB_LEADING_BYTE |
			    COMMON_LVB_TRAILING_BYTE);
			if (plain == csbi.wAttributes)
				continue;
			if (!any) {
				used = (size_t)snprintf(line, sizeof line,
				    "attr %02d |", (int)(y - first));
				any = 1;
			}
			used += (size_t)snprintf(line + used,
			    sizeof line - used, "%d:%04x ", (int)x,
			    (unsigned)plain);
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
	else if (strcmp(argv[2], "width2") == 0)
		corpus = corpus_width2;
	else if (strcmp(argv[2], "attr") == 0)
		corpus = corpus_attr;
	else {
		fprintf(stderr, "screentap: unknown corpus '%s'\n", argv[2]);
		return (2);
	}

	if (strcmp(argv[1], "emit") == 0) {
		return (emit_corpus(corpus));
	}
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
