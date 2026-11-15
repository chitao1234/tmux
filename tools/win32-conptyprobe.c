/*
 * win32-conptyprobe - measure what a program inside a pseudo console reads.
 *
 * This is the tmux pane geometry: one process owns the ConPTY and plays the
 * role tmux plays (it writes bytes at the device side), the other is the
 * child that tmux would have spawned (it sets a console input mode and
 * reports the bytes it gets back).
 *
 *   conptyprobe host <child-exe> <outfile> <mode|none> <byte-hex>...
 *   conptyprobe child <outfile> [mode-hex]
 *
 * Two rules matter and are easy to get wrong:
 *
 * 1. The child must not write anything to its console. Writing to stdout
 *    puts bytes through the pseudo console, and the pseudo console feeds
 *    some of them back into the input stream, which shows up as a phantom
 *    0x00 in front of the first key. Readiness is signalled through the
 *    result file instead.
 *
 * 2. Bytes must not be written before the child has attached and set its
 *    mode, or they are delivered under the default mode and the measurement
 *    is of the wrong mode. The host polls the result file for READY.
 */

#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *
find_bytes(const char *hay, size_t haylen, const char *needle)
{
	size_t nlen = strlen(needle), i;

	if (nlen == 0 || haylen < nlen)
		return (NULL);
	for (i = 0; i + nlen <= haylen; i++) {
		if (memcmp(hay + i, needle, nlen) == 0)
			return (hay + i);
	}
	return (NULL);
}

/* ---------------------------------------------------------------- child */

static int
child_mode(int argc, char **argv)
{
	HANDLE		 h;
	DWORD		 m = 0, want, n, i;
	unsigned char	 b[256];
	FILE		*f;

	if (argc < 2)
		return (2);
	f = fopen(argv[1], "wb");
	if (f == NULL)
		return (2);

	h = GetStdHandle(STD_INPUT_HANDLE);
	fprintf(f, "child_pid=%lu\n", (unsigned long)GetCurrentProcessId());
	if (GetConsoleMode(h, &m))
		fprintf(f, "default_mode=0x%lx\n", (unsigned long)m);
	else
		fprintf(f, "default_mode_failed err=%lu\n",
		    (unsigned long)GetLastError());

	if (argc > 2 && strcmp(argv[2], "none") != 0) {
		want = (DWORD)strtoul(argv[2], NULL, 16);
		if (!SetConsoleMode(h, want)) {
			fprintf(f, "set_mode_0x%lx_failed err=%lu\n",
			    (unsigned long)want,
			    (unsigned long)GetLastError());
			fflush(f);
			fclose(f);
			return (0);
		}
		if (GetConsoleMode(h, &m))
			fprintf(f, "active_mode=0x%lx\n", (unsigned long)m);
	}
	fprintf(f, "READY\n");
	fflush(f);

	for (;;) {
		if (!ReadFile(h, b, sizeof b, &n, NULL) || n == 0)
			break;
		fprintf(f, "read %lu:", (unsigned long)n);
		for (i = 0; i < n; i++)
			fprintf(f, " %02x", b[i]);
		fputc('\n', f);
		fflush(f);
	}

	fprintf(f, "END\n");
	fflush(f);
	fclose(f);
	return (0);
}

/* ----------------------------------------------------------------- host */

static void
drain_output(HANDLE out, int rounds, int sleep_ms)
{
	char	buf[4096];
	DWORD	avail, n;
	int	i;

	for (i = 0; i < rounds; i++) {
		avail = 0;
		if (!PeekNamedPipe(out, NULL, 0, NULL, &avail, NULL))
			return;
		if (avail == 0) {
			if (sleep_ms > 0)
				Sleep((DWORD)sleep_ms);
			continue;
		}
		if (!ReadFile(out, buf,
		    avail > sizeof buf ? (DWORD)sizeof buf : avail, &n, NULL))
			return;
		if (getenv("TMUX_CONPTYPROBE_LOG") != NULL) {
			FILE *log = fopen(getenv("TMUX_CONPTYPROBE_LOG"), "ab");

			if (log != NULL) {
				fwrite(buf, 1, n, log);
				fclose(log);
			}
		}
	}
}

/* Wait until the child's result file contains READY. */
static int
wait_ready(const char *path, DWORD timeout_ms)
{
	DWORD	waited = 0;
	int	found = 0;

	while (waited < timeout_ms) {
		FILE	*f = fopen(path, "rb");

		if (f != NULL) {
			char	buf[512];
			size_t	n = fread(buf, 1, sizeof buf - 1, f);

			fclose(f);
			buf[n] = '\0';
			if (find_bytes(buf, n, "READY") != NULL) {
				found = 1;
				break;
			}
		}
		Sleep(40);
		waited += 40;
	}
	return (found);
}

static int
host_mode(int argc, char **argv)
{
	HANDLE		 inR = NULL, inW = NULL, outR = NULL, outW = NULL;
	HPCON		 hpc = NULL;
	COORD		 size = { 80, 24 };
	STARTUPINFOEXW	 si;
	PROCESS_INFORMATION pi;
	SIZE_T		 attrs = 0;
	SECURITY_ATTRIBUTES sa;
	wchar_t		 cmd[2048];
	HRESULT		 hr;
	DWORD		 written;
	int		 i, n, ready;

	if (argc < 5)
		return (2);

	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = NULL;
	sa.bInheritHandle = FALSE;

	if (!CreatePipe(&inR, &inW, &sa, 0) ||
	    !CreatePipe(&outR, &outW, &sa, 0)) {
		fprintf(stderr, "host: CreatePipe failed %lu\n",
		    (unsigned long)GetLastError());
		return (1);
	}
	hr = CreatePseudoConsole(size, inR, outW, 0, &hpc);
	if (FAILED(hr)) {
		fprintf(stderr, "host: CreatePseudoConsole failed 0x%08lx\n",
		    (unsigned long)hr);
		return (1);
	}

	n = swprintf(cmd, 2048, L"\"%hs\" child \"%hs\"", argv[2], argv[3]);
	if (argc > 4)
		n += swprintf(cmd + n, 2048 - n, L" \"%hs\"", argv[4]);

	ZeroMemory(&si, sizeof si);
	si.StartupInfo.cb = sizeof si;
	/* NULL for all three: the child gets the pseudo console's handles,
	 * not ours. Without STARTF_USESTDHANDLES it inherits our pipes and
	 * GetStdHandle(STD_INPUT_HANDLE) is not a console handle. */
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = NULL;
	si.StartupInfo.hStdOutput = NULL;
	si.StartupInfo.hStdError = NULL;

	InitializeProcThreadAttributeList(NULL, 1, 0, &attrs);
	si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, attrs);
	if (si.lpAttributeList == NULL)
		return (1);
	InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrs);
	UpdateProcThreadAttribute(si.lpAttributeList, 0,
	    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, sizeof hpc, NULL, NULL);

	ZeroMemory(&pi, sizeof pi);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
	    EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) {
		fprintf(stderr, "host: CreateProcess failed %lu\n",
		    (unsigned long)GetLastError());
		return (1);
	}
	CloseHandle(inR);
	CloseHandle(outW);

	ready = wait_ready(argv[3], 8000);
	if (!ready)
		fprintf(stderr, "host: child never signalled READY\n");
	drain_output(outR, 4, 40);

	for (i = 5; i < argc; i++) {
		unsigned char byte = (unsigned char)strtoul(argv[i], NULL, 16);

		if (!WriteFile(inW, &byte, 1, &written, NULL) ||
		    written != 1) {
			fprintf(stderr, "host: WriteFile failed %lu\n",
			    (unsigned long)GetLastError());
			break;
		}
		drain_output(outR, 2, 60);
	}
	drain_output(outR, 12, 60);

	TerminateProcess(pi.hProcess, 0);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	ClosePseudoConsole(hpc);
	CloseHandle(inW);
	CloseHandle(outR);
	return (ready ? 0 : 3);
}

/* ---------------------------------------------------------------- usage */

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: conptyprobe host <child> <out> "
		    "<mode|none> <byte-hex>...\n"
		    "       conptyprobe child <out> [mode-hex]\n");
		return (2);
	}
	if (strcmp(argv[1], "child") == 0)
		return (child_mode(argc - 1, argv + 1));
	if (strcmp(argv[1], "host") == 0)
		return (host_mode(argc, argv));
	fprintf(stderr, "unknown verb '%s'\n", argv[1]);
	return (2);
}
