/*
 * win32-consoletap - measure what a Windows console delivers for one key,
 * in one console input mode.
 *
 * The probe runs in its own console window (hidden is fine, and required:
 * every measurement must start from a fresh console or the previous key's
 * state leaks into the next one). It writes the INPUT_RECORD sequence the
 * console host produces for a physical key press into its own input queue,
 * then reports two separate things:
 *
 *   bytes    what ReadFile() returns - the byte path a relay-style reader
 *            like tmux uses
 *   records  any INPUT_RECORD left in the queue afterwards - the record path
 *            PSReadLine and vim use
 *
 * Injecting into its own console is deliberate: AttachConsole to somebody
 * else's console fails with ERROR_ACCESS_DENIED for an unrelated console of
 * the same user, and even when it succeeds the records land in that process's
 * queue where two readers race for them.
 *
 *   consoletap probe <outfile> <mode|none> <key>
 *   consoletap keys
 *
 * <mode> is hex (0x0, 0x200, 0x3f0, 0x1f7) or "none" to leave the console
 * exactly as the console host created it.
 *
 * Every read is bounded by a watchdog thread, so a mode that delivers nothing
 * is reported as "bytes none" instead of hanging.
 *
 * Build (MSYS2 UCRT64):
 *   gcc -O2 -o win32-consoletap.exe win32-consoletap.c
 */

#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ keys */

struct keydef {
	const char	*name;
	WORD		 vk;
	WCHAR		 ch;
	DWORD		 ctrl;
	WORD		 scancode;
};

static const struct keydef keydefs[] = {
	{ "a",           'A',  L'a',  0,                  0x1e },
	{ "z",           'Z',  L'z',  0,                  0x2c },
	{ "space",       VK_SPACE, L' ', 0,               0x39 },
	{ "ctrl-a",      'A',  L'\x01', LEFT_CTRL_PRESSED, 0x1e },
	{ "ctrl-b",      'B',  L'\x02', LEFT_CTRL_PRESSED, 0x30 },
	{ "ctrl-c",      'C',  L'\x03', LEFT_CTRL_PRESSED, 0x2e },
	{ "ctrl-h",      'H',  L'\x08', LEFT_CTRL_PRESSED, 0x23 },
	{ "ctrl-i",      'I',  L'\x09', LEFT_CTRL_PRESSED, 0x17 },
	{ "ctrl-j",      'J',  L'\n',   LEFT_CTRL_PRESSED, 0x24 },
	{ "ctrl-l",      'L',  L'\x0c', LEFT_CTRL_PRESSED, 0x26 },
	{ "ctrl-m",      'M',  L'\r',   LEFT_CTRL_PRESSED, 0x32 },
	{ "ctrl-[",      'O',  L'\x1b', LEFT_CTRL_PRESSED, 0x18 },
	{ "ctrl-\\",     '\\', L'\x1c', LEFT_CTRL_PRESSED, 0x2b },
	{ "ctrl-]",      ']',  L'\x1d', LEFT_CTRL_PRESSED, 0x1b },
	{ "ctrl-space",  VK_SPACE, L' ', LEFT_CTRL_PRESSED, 0x39 },
	{ "enter",       VK_RETURN, L'\r', 0,             0x1c },
	{ "tab",         VK_TAB, L'\t',  0,               0x0f },
	{ "esc",         VK_ESCAPE, L'\x1b', 0,           0x01 },
	{ "backspace",   VK_BACK, L'\x08', 0,             0x0e },
	{ "shift-tab",   VK_TAB, L'\t', SHIFT_PRESSED,    0x0f },
	{ "up",          VK_UP, L'\0', 0,                 0x48 },
	{ "down",        VK_DOWN, L'\0', 0,               0x50 },
	{ "left",        VK_LEFT, L'\0', 0,               0x4b },
	{ "right",       VK_RIGHT, L'\0', 0,              0x4d },
	{ "home",        VK_HOME, L'\0', 0,               0x47 },
	{ "end",         VK_END, L'\0', 0,                0x4f },
	{ "insert",      VK_INSERT, L'\0', 0,             0x52 },
	{ "delete",      VK_DELETE, L'\0', 0,             0x53 },
	{ "pageup",      VK_PRIOR, L'\0', 0,              0x49 },
	{ "pagedown",    VK_NEXT, L'\0', 0,               0x51 },
	{ "f1",          VK_F1, L'\0', 0,                 0x3b },
	{ "f5",          VK_F5, L'\0', 0,                 0x3f },
	{ "alt-x",       'X',  L'x',  LEFT_ALT_PRESSED,    0x2d },
};

static const struct keydef *
find_key(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof keydefs / sizeof keydefs[0]; i++) {
		if (strcmp(keydefs[i].name, name) == 0)
			return (&keydefs[i]);
	}
	return (NULL);
}

/*
 * Build the record sequence a console host emits for this key: modifier
 * down, key down, key up, modifier up. Reproducing the modifiers matters
 * because a reader that inspects dwControlKeyState sees a different shape if
 * they are missing.
 */
static int
build_records(const struct keydef *k, INPUT_RECORD *r)
{
	int	n = 0;

#define ADD_RECORD(vk_, sc_, ch_, ctrl_, down_)				\
	do {								\
		ZeroMemory(&r[n], sizeof r[n]);				\
		r[n].EventType = KEY_EVENT;				\
		r[n].Event.KeyEvent.bKeyDown = (down_);			\
		r[n].Event.KeyEvent.wRepeatCount = 1;			\
		r[n].Event.KeyEvent.wVirtualKeyCode = (vk_);		\
		r[n].Event.KeyEvent.wVirtualScanCode = (sc_);		\
		r[n].Event.KeyEvent.uChar.UnicodeChar = (ch_);		\
		r[n].Event.KeyEvent.dwControlKeyState = (ctrl_);	\
		n++;							\
	} while (0)

	if (k->ctrl & (LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED))
		ADD_RECORD(VK_CONTROL, 0x1d, 0, k->ctrl, TRUE);
	if (k->ctrl & (LEFT_ALT_PRESSED|RIGHT_ALT_PRESSED))
		ADD_RECORD(VK_MENU, 0x38, 0, k->ctrl, TRUE);
	if (k->ctrl & SHIFT_PRESSED)
		ADD_RECORD(VK_SHIFT, 0x2a, 0, k->ctrl, TRUE);

	ADD_RECORD(k->vk, k->scancode, k->ch, k->ctrl, TRUE);
	ADD_RECORD(k->vk, k->scancode, k->ch, k->ctrl, FALSE);

	if (k->ctrl & SHIFT_PRESSED)
		ADD_RECORD(VK_SHIFT, 0x2a, 0, 0, FALSE);
	if (k->ctrl & (LEFT_ALT_PRESSED|RIGHT_ALT_PRESSED))
		ADD_RECORD(VK_MENU, 0x38, 0, 0, FALSE);
	if (k->ctrl & (LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED))
		ADD_RECORD(VK_CONTROL, 0x1d, 0, 0, FALSE);

#undef ADD_RECORD
	return (n);
}

/* ------------------------------------------------------- bounded reading */

struct reader {
	HANDLE			h;
	volatile LONG		done;
	DWORD			got;
	unsigned char		buf[256];
};

static DWORD WINAPI
reader_bytes_thread(void *arg)
{
	struct reader *r = arg;
	DWORD		 got = 0;

	if (ReadFile(r->h, r->buf, sizeof r->buf, &got, NULL))
		r->got = got;
	else
		r->got = 0;
	InterlockedExchange(&r->done, 1);
	return (0);
}

struct rec_reader {
	HANDLE			h;
	volatile LONG		done;
	INPUT_RECORD		rec;
	DWORD			got;
};

static DWORD WINAPI
reader_record_thread(void *arg)
{
	struct rec_reader *r = arg;
	DWORD		 got = 0;

	if (!ReadConsoleInputW(r->h, &r->rec, 1, &got))
		got = 0;
	r->got = got;
	InterlockedExchange(&r->done, 1);
	return (0);
}

/*
 * A console handle blocks forever when nothing is pending, so every read runs
 * on a thread we can abandon. The thread is deliberately leaked on timeout:
 * this is a short-lived probe, and closing a handle under a blocked ReadFile
 * would disturb the very console state being measured.
 */
static int
read_bytes_bounded(HANDLE h, unsigned char *out, DWORD cap, DWORD *gotp,
    DWORD timeout_ms)
{
	struct reader	r;
	HANDLE		t;
	int		done;

	memset(&r, 0, sizeof r);
	r.h = h;
	t = CreateThread(NULL, 0, reader_bytes_thread, &r, 0, NULL);
	if (t == NULL)
		return (-1);
	WaitForSingleObject(t, timeout_ms);
	done = (InterlockedCompareExchange(&r.done, 0, 0) != 0);
	if (!done)
		TerminateThread(t, 0);
	CloseHandle(t);
	if (!done) {
		*gotp = 0;
		return (1);
	}
	if (r.got > cap)
		r.got = cap;
	memcpy(out, r.buf, r.got);
	*gotp = r.got;
	return (0);
}

static int
read_record_bounded(HANDLE h, INPUT_RECORD *out, DWORD timeout_ms)
{
	struct rec_reader	r;
	HANDLE			t;
	int			done;

	memset(&r, 0, sizeof r);
	r.h = h;
	t = CreateThread(NULL, 0, reader_record_thread, &r, 0, NULL);
	if (t == NULL)
		return (-1);
	WaitForSingleObject(t, timeout_ms);
	done = (InterlockedCompareExchange(&r.done, 0, 0) != 0);
	if (!done)
		TerminateThread(t, 0);
	CloseHandle(t);
	if (!done)
		return (1);
	if (r.got != 1)
		return (2);
	*out = r.rec;
	return (0);
}

/* -------------------------------------------------------------- describe */

static void
describe_mode(FILE *f, const char *tag, DWORD m)
{
	fprintf(f, "%s=0x%03lx", tag, (unsigned long)m);
	if (m & ENABLE_PROCESSED_INPUT)
		fprintf(f, " PROCESSED");
	if (m & ENABLE_LINE_INPUT)
		fprintf(f, " LINE");
	if (m & ENABLE_ECHO_INPUT)
		fprintf(f, " ECHO");
	if (m & ENABLE_WINDOW_INPUT)
		fprintf(f, " WINDOW");
	if (m & ENABLE_MOUSE_INPUT)
		fprintf(f, " MOUSE");
	if (m & ENABLE_INSERT_MODE)
		fprintf(f, " INSERT");
#ifdef ENABLE_QUICK_EDIT_MODE
	if (m & ENABLE_QUICK_EDIT_MODE)
		fprintf(f, " QUICKEDIT");
#endif
	if (m & ENABLE_EXTENDED_FLAGS)
		fprintf(f, " EXTFLAGS");
#ifdef ENABLE_VIRTUAL_TERMINAL_INPUT
	if (m & ENABLE_VIRTUAL_TERMINAL_INPUT)
		fprintf(f, " VT_INPUT");
#endif
	fprintf(f, "\n");
}

static void
describe_record(FILE *f, const INPUT_RECORD *r)
{
	if (r->EventType == KEY_EVENT) {
		const KEY_EVENT_RECORD *k = &r->Event.KeyEvent;

		fprintf(f, "record key down=%d vk=0x%04x sc=0x%04x ch=0x%04x "
		    "ctrl=0x%08lx repeat=%u\n", (int)k->bKeyDown,
		    (unsigned)k->wVirtualKeyCode, (unsigned)k->wVirtualScanCode,
		    (unsigned)k->uChar.UnicodeChar,
		    (unsigned long)k->dwControlKeyState,
		    (unsigned)k->wRepeatCount);
	} else if (r->EventType == MOUSE_EVENT) {
		const MOUSE_EVENT_RECORD *m = &r->Event.MouseEvent;

		fprintf(f, "record mouse x=%d y=%d buttons=0x%08lx "
		    "ctrl=0x%08lx flags=0x%08lx\n", (int)m->dwMousePosition.X,
		    (int)m->dwMousePosition.Y, (unsigned long)m->dwButtonState,
		    (unsigned long)m->dwControlKeyState,
		    (unsigned long)m->dwEventFlags);
	} else if (r->EventType == WINDOW_BUFFER_SIZE_EVENT) {
		fprintf(f, "record resize %dx%d\n",
		    (int)r->Event.WindowBufferSizeEvent.dwSize.X,
		    (int)r->Event.WindowBufferSizeEvent.dwSize.Y);
	} else {
		fprintf(f, "record type=%u\n", (unsigned)r->EventType);
	}
}

/* ------------------------------------------------------------------ main */

static HANDLE
open_conin(void)
{
	return (CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
	    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL));
}

static int
probe_role(int argc, char **argv)
{
	const char	*outpath, *modearg, *keyname;
	const struct keydef *k;
	FILE		*f;
	HANDLE		 h;
	DWORD		 orig = 0, mode, got, written = 0;
	unsigned char	 bytes[256];
	INPUT_RECORD	 recs[8];
	INPUT_RECORD	 rec;
	int		 nrec, i, rc, available = 0, want_records = 1;
	char		 keybuf[64];

	if (argc < 5) {
		fprintf(stderr, "probe: need <outfile> <mode|none> <key>\n");
		return (2);
	}
	outpath = argv[2];
	modearg = argv[3];
	keyname = argv[4];

	/* "wait" means: do not inject, just report what a real user types. */
	if (strcmp(keyname, "wait") == 0) {
		want_records = 0;
		k = NULL;
	} else {
		k = find_key(keyname);
		if (k == NULL) {
			fprintf(stderr, "probe: unknown key '%s'\n", keyname);
			return (2);
		}
	}
	f = fopen(outpath, "wb");
	if (f == NULL) {
		fprintf(stderr, "probe: cannot open %s\n", outpath);
		return (2);
	}

	h = open_conin();
	if (h == INVALID_HANDLE_VALUE) {
		fprintf(f, "result error=no_console detail=CONIN\\$ err%lu\n",
		    (unsigned long)GetLastError());
		fclose(f);
		return (0);
	}
	if (!GetConsoleMode(h, &orig)) {
		fprintf(f, "result error=no_console detail=GetConsoleMode "
		    "err%lu\n", (unsigned long)GetLastError());
		fclose(f);
		CloseHandle(h);
		return (0);
	}

	fprintf(f, "pid=%lu\n", (unsigned long)GetCurrentProcessId());
	describe_mode(f, "host_default", orig);

	if (strcmp(modearg, "none") == 0) {
		describe_mode(f, "active", orig);
	} else {
		mode = (DWORD)strtoul(modearg, NULL, 16);
		if (!SetConsoleMode(h, mode)) {
			fprintf(f, "result error=set_mode_failed "
			    "mode=0x%03lx err=%lu\n", (unsigned long)mode,
			    (unsigned long)GetLastError());
			fclose(f);
			CloseHandle(h);
			return (0);
		}
		GetConsoleMode(h, &mode);
		describe_mode(f, "active", mode);
	}

	/* Start from a clean queue so leftovers cannot be mistaken for ours. */
	FlushConsoleInputBuffer(h);

	if (want_records) {
		/*
		 * Inject into our own console queue. The probe already owns
		 * this console, so no AttachConsole is needed - which is what
		 * makes the same binary work inside conhost, Windows Terminal
		 * and WezTerm alike.
		 */
		nrec = build_records(k, recs);
		if (!WriteConsoleInputW(h, recs, (DWORD)nrec, &written) ||
		    written != (DWORD)nrec) {
			fprintf(f, "result error=inject_failed wrote=%lu "
			    "want=%d err=%lu\n", (unsigned long)written, nrec,
			    (unsigned long)GetLastError());
			fclose(f);
			CloseHandle(h);
			return (0);
		}
		Sleep(120);
	} else {
		fprintf(f, "waiting=1\n");
		fflush(f);
	}

	/*
	 * Report bytes first: if the host converted the records into bytes, it
	 * already removed them from the queue, and this is the only chance to
	 * see the conversion.
	 */
	rc = read_bytes_bounded(h, bytes, sizeof bytes, &got, 800);
	if (rc < 0)
		fprintf(f, "bytes error=read_failed\n");
	else if (rc == 1)
		fprintf(f, "bytes none\n");
	else {
		fprintf(f, "bytes got=%lu [", (unsigned long)got);
		for (i = 0; i < (int)got; i++)
			fprintf(f, "%s%02x", i ? " " : "", bytes[i]);
		fprintf(f, "]\n");
	}

	/* Whatever still sits in the queue was not turned into bytes. */
	for (i = 0; i < 6; i++) {
		rc = read_record_bounded(h, &rec, 100);
		if (rc != 0)
			break;
		describe_record(f, &rec);
	}
	if (GetNumberOfConsoleInputEvents(h, &got))
		available = (int)got;
	fprintf(f, "queue_left=%d\n", available);
	(void)keybuf;

	fprintf(f, "result ok=1\n");
	fflush(f);
	fclose(f);
	CloseHandle(h);
	return (0);
}

static int
list_keys(void)
{
	size_t i;

	for (i = 0; i < sizeof keydefs / sizeof keydefs[0]; i++)
		printf("%s\n", keydefs[i].name);
	return (0);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: consoletap probe <outfile> "
		    "<mode|none> <key>\n"
		    "       consoletap keys\n");
		return (2);
	}
	if (strcmp(argv[1], "keys") == 0)
		return (list_keys());
	if (strcmp(argv[1], "probe") == 0)
		return (probe_role(argc, argv));
	fprintf(stderr, "consoletap: unknown verb '%s'\n", argv[1]);
	return (2);
}
