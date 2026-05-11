#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <conio.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE output;
static int width = 100;
static int rows = 16;
static int delay_ms = 120;
static int auto_steps = -1;
static int selected;
static int split_writes;
static int full_redraw;
static int hold_after_auto;
static int phased_updates;
static int phase_delay_ms = 16;

static const char *names[] = {
	"cmd.exe",
	"msedge.exe",
	"ntop.exe",
	"svchost.exe",
	"OpenConsole.exe",
	"tmux.exe",
	"powershell.exe",
	"conhost.exe",
	"SearchHost.exe",
	"RuntimeBroker.exe",
	"explorer.exe",
	"WindowsTerminal.exe",
	"pwsh.exe",
	"TextInputHost.exe",
	"msedgewebview2.exe",
	"ApplicationFrameHost.exe"
};

static void
write_bytes(const char *data, size_t size)
{
	DWORD	written;

	while (size != 0) {
		if (!WriteFile(output, data, (DWORD)size, &written, NULL))
			ExitProcess(2);
		if (written == 0)
			ExitProcess(2);
		data += written;
		size -= written;
	}
}

static void
write_string(const char *s)
{
	write_bytes(s, strlen(s));
}

static void
write_printf(const char *fmt, ...)
{
	va_list	ap;
	char	buf[4096];
	int	n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	if (n < 0)
		ExitProcess(2);
	if ((size_t)n >= sizeof buf)
		n = (int)sizeof buf - 1;
	write_bytes(buf, (size_t)n);
}

static void
pad_line(char *out, size_t outsize, const char *in)
{
	size_t	len;

	snprintf(out, outsize, "%-*.*s", width, width, in);
	len = strlen(out);
	if (len > (size_t)width)
		out[width] = '\0';
}

static void
draw_row(int index, int is_selected)
{
	char	text[512], line[512];
	int	pid = 24000 - index * 137;

	snprintf(text, sizeof text,
	    "%6d  %-8s  %3d  %05.1f%%  %8.1f MB  %4d  %7.1f MB/s  "
	    "00:%02d:%02d:%02d  %s",
	    pid, (index % 3) == 0 ? "SYSTEM" : "chi",
	    4 + (index % 6), (double)((index * 7) % 100) / 10.0,
	    2.0 + (double)(index * 17) / 10.0, 1 + (index % 28),
	    (double)(index % 5) / 10.0, index, (index * 3) % 60,
	    (index * 7) % 60, names[index % (int)(sizeof names / sizeof *names)]);
	pad_line(line, sizeof line, text);

	write_printf("\033[%d;1H", 7 + index);
	if (is_selected)
		write_string("\033[30m\033[106m");
	else
		write_string("\033[39m\033[49m");

	if (split_writes) {
		size_t half = (size_t)width / 2;

		write_bytes(line, half);
		FlushFileBuffers(output);
		Sleep(2);
		write_bytes(line + half, (size_t)width - half);
	} else
		write_bytes(line, (size_t)width);

	write_string("\033[39m\033[49m");
}

static void
draw_all(void)
{
	int	i;

	write_string("\033[?25l\033[2J\033[H");
	write_printf("\033[44m%-*s\033[49m", width,
	    "tmux win32 highlight repro");
	write_printf("\033[3;1H\033[96mRows: \033[39m%d  "
	    "\033[96mWidth: \033[39m%d  "
	    "\033[96mMode: \033[39m%s%s%s",
	    rows, width, full_redraw ? "full" : "partial",
	    split_writes ? "+split" : "",
	    phased_updates ? "+phase" : "");
	write_printf("\033[5;1H\033[30m\033[106m%-6s  %-8s  %-3s  %-6s  "
	    "%8s  %-4s  %-11s  %-11s  %s\033[39m\033[49m",
	    "PID", "USER", "PRI", "CPU%", "MEM", "THRD", "DISK", "TIME",
	    "PROCESS");
	for (i = 0; i < rows; i++)
		draw_row(i, i == selected);
	write_printf("\033[%d;1H\033[39m\033[49m", 8 + rows);
	write_string("Use Up/Down, q quits. --auto N runs without input.");
}

static void
move_selection(int delta)
{
	int	old = selected;

	selected += delta;
	if (selected < 0)
		selected = 0;
	if (selected >= rows)
		selected = rows - 1;
	if (selected == old)
		return;

	if (full_redraw)
		draw_all();
	else if (phased_updates) {
		draw_row(selected, 1);
		FlushFileBuffers(output);
		if (phase_delay_ms != 0)
			Sleep((DWORD)phase_delay_ms);
		draw_row(selected, 1);
		draw_row(old, 0);
		write_printf("\033[%d;1H", 8 + rows);
	}
	else {
		draw_row(old, 0);
		draw_row(selected, 1);
		write_printf("\033[%d;1H", 8 + rows);
	}
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: win32-highlight-repro [--auto N] [--delay MS] [--rows N]\n"
	    "                             [--width N] [--full] [--split]\n"
	    "                             [--phase] [--phase-delay MS] [--hold]\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	DWORD	mode;
	int	i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--auto") == 0) {
			if (++i == argc)
				usage();
			auto_steps = atoi(argv[i]);
		} else if (strcmp(argv[i], "--delay") == 0) {
			if (++i == argc)
				usage();
			delay_ms = atoi(argv[i]);
		} else if (strcmp(argv[i], "--rows") == 0) {
			if (++i == argc)
				usage();
			rows = atoi(argv[i]);
		} else if (strcmp(argv[i], "--width") == 0) {
			if (++i == argc)
				usage();
			width = atoi(argv[i]);
		} else if (strcmp(argv[i], "--full") == 0)
			full_redraw = 1;
		else if (strcmp(argv[i], "--split") == 0)
			split_writes = 1;
		else if (strcmp(argv[i], "--phase") == 0)
			phased_updates = 1;
		else if (strcmp(argv[i], "--phase-delay") == 0) {
			if (++i == argc)
				usage();
			phase_delay_ms = atoi(argv[i]);
			phased_updates = 1;
		}
		else if (strcmp(argv[i], "--hold") == 0)
			hold_after_auto = 1;
		else
			usage();
	}
	if (rows < 2 || rows > (int)(sizeof names / sizeof *names))
		usage();
	if (width < 40 || width > 240)
		usage();
	if (delay_ms < 0)
		usage();
	if (phase_delay_ms < 0)
		usage();

	_setmode(_fileno(stdout), _O_BINARY);
	SetConsoleOutputCP(CP_UTF8);
	output = GetStdHandle(STD_OUTPUT_HANDLE);
	if (GetConsoleMode(output, &mode))
		SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

	draw_all();
	if (auto_steps >= 0) {
		for (i = 0; i < auto_steps; i++) {
			Sleep((DWORD)delay_ms);
			move_selection(1);
		}
		if (hold_after_auto) {
			for (;;) {
				int ch = _getch();

				if (ch == 'q' || ch == 'Q')
					break;
			}
		}
		Sleep((DWORD)delay_ms);
		write_string("\033[?25h\033[39m\033[49m\n");
		return (0);
	}

	for (;;) {
		int	ch = _getch();

		if (ch == 'q' || ch == 'Q')
			break;
		if (ch == 0 || ch == 224) {
			ch = _getch();
			if (ch == 72)
				move_selection(-1);
			else if (ch == 80)
				move_selection(1);
		} else if (ch == 'j')
			move_selection(1);
		else if (ch == 'k')
			move_selection(-1);
	}
	write_string("\033[?25h\033[39m\033[49m\n");
	return (0);
}
