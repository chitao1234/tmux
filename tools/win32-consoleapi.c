/*
 * win32-consoleapi - exercise the classic Console API and report the screen.
 *
 * tmux itself only ever writes VT sequences (WriteConsoleW / WriteFile), so a
 * VT-only comparison says nothing about the other half of the surface: the
 * programs running inside a pane, which use the Console API directly. ConPTY
 * has to translate those calls into VT for the terminal, conhost implements
 * them on the screen buffer natively, and that translation is where the two
 * can diverge.
 *
 * Each case is a fixed sequence of Console API calls with a deterministic
 * expected result, and every case runs in a fresh process so no case can
 * inherit state from another.
 *
 *   consoleapi <case> <outfile>
 *   consoleapi list
 *
 * The dump format matches win32-screentap so the reports can be diffed
 * directly: non-ASCII as \uXXXX, attributes only where they differ from the
 * default, wide characters counted once.
 */

#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE	out;
static FILE	*rep;

/* --------------------------------------------------------------- helpers */

static int
cell_width(WCHAR ch)
{
	if (ch < 0x1100)
		return (1);
	if (ch <= 0x115f)
		return (2);
	if (ch >= 0x2e80 && ch <= 0xa4cf)
		return (2);
	if (ch >= 0xac00 && ch <= 0xd7a3)
		return (2);
	if (ch >= 0xf900 && ch <= 0xfaff)
		return (2);
	if (ch >= 0xfe30 && ch <= 0xfe6f)
		return (2);
	if (ch >= 0xff00 && ch <= 0xff60)
		return (2);
	if (ch >= 0xffe0 && ch <= 0xffe6)
		return (2);
	return (1);
}

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

static void
dump(const char *label)
{
	CONSOLE_SCREEN_BUFFER_INFO	 csbi;
	CHAR_INFO			*buf;
	COORD				 size, origin;
	SMALL_RECT			 region;
	char				 line[16384];
	size_t				 used;
	SHORT				 x, y, width, height;

	fprintf(rep, "--- %s ---\n", label);
	if (!GetConsoleScreenBufferInfo(out, &csbi)) {
		fprintf(rep, "error=no_info\n");
		return;
	}
	width = (SHORT)(csbi.srWindow.Right - csbi.srWindow.Left + 1);
	height = (SHORT)(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
	fprintf(rep, "buffer=%dx%d window=%dx%d cursor=%d,%d\n",
	    (int)csbi.dwSize.X, (int)csbi.dwSize.Y, (int)width, (int)height,
	    (int)(csbi.dwCursorPosition.X - csbi.srWindow.Left),
	    (int)(csbi.dwCursorPosition.Y - csbi.srWindow.Top));

	size.X = width;
	size.Y = height;
	origin.X = 0;
	origin.Y = 0;
	region = csbi.srWindow;
	buf = calloc((size_t)width * (size_t)height, sizeof *buf);
	if (buf == NULL)
		return;
	if (!ReadConsoleOutputW(out, buf, size, origin, &region)) {
		fprintf(rep, "error=read_failed err=%lu\n",
		    (unsigned long)GetLastError());
		free(buf);
		return;
	}

	for (y = 0; y < height; y++) {
		used = 0;
		line[0] = '\0';
		for (x = 0; x < width; x++) {
			CHAR_INFO *ci = &buf[(size_t)y * width + x];

			if (cell_text(line, sizeof line, &used,
			    ci->Char.UnicodeChar) != 0)
				break;
			if (cell_width(ci->Char.UnicodeChar) == 2 &&
			    x + 1 < width)
				x++;
		}
		while (used > 0 && line[used - 1] == ' ')
			used--;
		line[used] = '\0';
		if (used == 0)
			continue;
		fprintf(rep, "row %02d |%s|\n", (int)y, line);
	}

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
			fprintf(rep, "%s\n", line);
		}
	}
	free(buf);
}

/* Clear the whole buffer so each case starts from a known state. */
static void
reset_screen(void)
{
	CONSOLE_SCREEN_BUFFER_INFO	 csbi;
	DWORD				 written;
	DWORD				 cells;
	COORD				 home = { 0, 0 };

	if (!GetConsoleScreenBufferInfo(out, &csbi))
		return;
	cells = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
	FillConsoleOutputCharacterW(out, L' ', cells, home, &written);
	FillConsoleOutputAttribute(out, csbi.wAttributes, cells, home,
	    &written);
	SetConsoleCursorPosition(out, home);
}

static void
write_at(SHORT x, SHORT y, const wchar_t *s)
{
	COORD	pos;
	DWORD	written;

	pos.X = x;
	pos.Y = y;
	SetConsoleCursorPosition(out, pos);
	WriteConsoleW(out, s, (DWORD)wcslen(s), &written, NULL);
}

/* ---------------------------------------------------------------- cases */

/*
 * Case attr16: every legacy 4-bit foreground and background combination, so
 * the 16-colour to VT SGR mapping is exercised exhaustively.
 */
static void
case_attr16(void)
{
	WORD	 fg, bg;
	SHORT	 i;

	reset_screen();
	for (fg = 1; fg <= 15; fg++) {
		i = 0;
		for (bg = 0; bg <= 15; bg++) {
			COORD	pos;
			DWORD	written;

			pos.X = (SHORT)(i * 3);
			pos.Y = (SHORT)(fg - 1);
			SetConsoleTextAttribute(out,
			    (WORD)(fg | (bg << 4)));
			FillConsoleOutputCharacterW(out, L'#', 2, pos,
			    &written);
			FillConsoleOutputAttribute(out,
			    (WORD)(fg | (bg << 4)), 2, pos, &written);
			i++;
		}
	}
	SetConsoleTextAttribute(out, 0x07);
	dump("attr16");
}

/*
 * Case fill: partial-rectangle fill. ConPTY has no primitive for this, so it
 * must redraw cell by cell; only the rectangle may change.
 */
static void
case_fill(void)
{
	COORD	pos = { 5, 5 };
	DWORD	written;

	reset_screen();
	write_at(0, 4, L"AAAAABBBBBCCCCC");
	write_at(0, 5, L"AAAAABBBBBCCCCC");
	write_at(0, 6, L"AAAAABBBBBCCCCC");

	/* Fill only columns 5..9 on row 5 with a distinct colour. */
	SetConsoleTextAttribute(out, FOREGROUND_GREEN |
	    BACKGROUND_BLUE);
	pos.X = 5;
	pos.Y = 5;
	FillConsoleOutputCharacterW(out, L'.', 5, pos, &written);
	FillConsoleOutputAttribute(out, FOREGROUND_GREEN |
	    BACKGROUND_BLUE, 5, pos, &written);
	SetConsoleTextAttribute(out, 0x07);
	dump("fill");
}

/*
 * Case blo: WriteConsoleOutput of a small block at an offset. This is the
 * case most likely to differ, because it carries characters and attributes
 * together in one call and targets an arbitrary origin.
 */
static void
case_blo(void)
{
	CHAR_INFO	block[9];
	COORD		size = { 3, 3 };
	COORD		origin = { 0, 0 };
	SMALL_RECT	region = { 10, 10, 12, 12 };
	int		i;

	reset_screen();
	write_at(8, 9, L".........");
	write_at(8, 10, L".........");
	write_at(8, 11, L".........");
	write_at(8, 12, L".........");

	for (i = 0; i < 9; i++) {
		block[i].Char.UnicodeChar = (WCHAR)(L'a' + i);
		block[i].Attributes = (WORD)((i % 2) ? 0x1f : 0x2e);
	}
	if (!WriteConsoleOutputW(out, block, size, origin, &region))
		fprintf(rep, "blo: WriteConsoleOutputW failed %lu\n",
		    (unsigned long)GetLastError());
	SetConsoleTextAttribute(out, 0x07);
	dump("blo");
}

/*
 * Case scroll: scroll a sub-rectangle only. The lines outside the rectangle
 * must stay put.
 */
static void
case_scroll(void)
{
	SMALL_RECT	scroll = { 5, 5, 15, 10 };
	SMALL_RECT	clip = { 5, 5, 15, 10 };
	CHAR_INFO	fill;
	int		y;

	reset_screen();
	for (y = 0; y < 12; y++) {
		wchar_t	line[64];

		swprintf(line, 64, L"R%02d........................", y);
		write_at(0, (SHORT)y, line);
	}
	fill.Char.UnicodeChar = L' ';
	fill.Attributes = 0x07;
	if (!ScrollConsoleScreenBufferW(out, &scroll, &clip,
	    (COORD){ 5, 3 }, &fill))
		fprintf(rep, "scroll: ScrollConsoleScreenBufferW failed %lu\n",
		    (unsigned long)GetLastError());
	dump("scroll");
}

/*
 * Case resize: grow the buffer. A VT-only terminal would not be told to
 * reallocate anything, so the reported buffer size is the interesting part.
 */
static void
case_resize(void)
{
	reset_screen();
	write_at(0, 0, L"before resize");
	if (!SetConsoleScreenBufferSize(out, (COORD){ 100, 40 }))
		fprintf(rep, "resize: SetConsoleScreenBufferSize failed %lu\n",
		    (unsigned long)GetLastError());
	else
		write_at(0, 1, L"after resize");
	dump("resize");
}

/*
 * Case cursor: cursor shape and visibility.
 */
static void
case_cursor(void)
{
	CONSOLE_CURSOR_INFO	ci;

	reset_screen();
	write_at(0, 0, L"cursor shape");
	ci.dwSize = 100;
	ci.bVisible = TRUE;
	SetConsoleCursorInfo(out, &ci);
	dump("cursor");
}

/* ------------------------------------------------------------------ main */

struct {
	const char	*name;
	void		(*fn)(void);
} cases[] = {
	{ "attr16",	case_attr16 },
	{ "fill",	case_fill },
	{ "blo",	case_blo },
	{ "scroll",	case_scroll },
	{ "resize",	case_resize },
	{ "cursor",	case_cursor },
	{ NULL,		NULL }
};

int
main(int argc, char **argv)
{
	int	i;

	if (argc >= 2 && strcmp(argv[1], "list") == 0) {
		for (i = 0; cases[i].name != NULL; i++)
			printf("%s\n", cases[i].name);
		return (0);
	}
	if (argc < 3) {
		fprintf(stderr, "usage: consoleapi <case> <outfile>\n");
		return (2);
	}

	out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
	    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (out == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "consoleapi: no CONOUT$\n");
		return (1);
	}
	rep = fopen(argv[2], "wb");
	if (rep == NULL) {
		CloseHandle(out);
		return (1);
	}

	for (i = 0; cases[i].name != NULL; i++) {
		if (strcmp(argv[1], cases[i].name) != 0)
			continue;
		fprintf(rep, "case=%s\n", cases[i].name);
		cases[i].fn();
		fprintf(rep, "result ok=1\n");
		break;
	}
	if (cases[i].name == NULL)
		fprintf(rep, "result error=unknown_case\n");

	fflush(rep);
	fclose(rep);
	CloseHandle(out);
	return (0);
}
