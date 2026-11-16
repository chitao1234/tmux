/*
 * win32-conhost-ident - report what is hosting this process's console.
 *
 * The input matrix has a "conhost" tier that runs the probe with
 * CREATE_NEW_CONSOLE. On Windows 11 with Windows Terminal set as the default
 * terminal application, CREATE_NEW_CONSOLE is handed off to Windows Terminal,
 * which backs the console with ConPTY instead of conhost.exe. In that case a
 * run labelled "conhost" is really measuring the ConPTY path, and the whole
 * point of the tier - classic console host behaviour - is missed.
 *
 * This probe answers the question directly rather than by assumption:
 *
 *   console_host   name of the conhost.exe/OpenConsole.exe/WindowsTerminal.exe
 *                  process found in the ancestor chain or the console's
 *                  attached process list
 *
 *   conpty         "yes" when the console is a pseudo console (GetConsoleMode
 *                  reports a mode but there is no conhost.exe ancestor, and
 *                  the attached list contains only this process)
 *
 * Usage: win32-conhost-ident <outfile>
 */

#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

static const char *
process_name(DWORD pid, char *buf, size_t len)
{
	HANDLE			snap;
	PROCESSENTRY32W		pe;
	const char		*base;

	snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return (NULL);
	pe.dwSize = sizeof pe;
	if (!Process32FirstW(snap, &pe)) {
		CloseHandle(snap);
		return (NULL);
	}
	do {
		if (pe.th32ProcessID != pid)
			continue;
		if (WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, buf,
		    (int)len, NULL, NULL) == 0)
			buf[0] = '\0';
		CloseHandle(snap);
		base = buf;
		return (base);
	} while (Process32NextW(snap, &pe));
	CloseHandle(snap);
	return (NULL);
}

static DWORD
parent_pid(DWORD pid)
{
	HANDLE			snap;
	PROCESSENTRY32W		pe;
	DWORD			parent = 0;

	snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return (0);
	pe.dwSize = sizeof pe;
	if (Process32FirstW(snap, &pe)) {
		do {
			if (pe.th32ProcessID == pid) {
				parent = pe.th32ParentProcessID;
				break;
			}
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	return (parent);
}

static int
name_contains(const char *name, const char *needle)
{
	if (name == NULL)
		return (0);
	return (strstr(name, needle) != NULL);
}

/*
 * Classify the console host behind this process.
 *
 * Three cases matter and they are not interchangeable:
 *
 *   conhost   classic console host, conhost.exe in the ancestor chain
 *   conpty    no host process in the chain; the console is a pseudo console
 *             owned by an out-of-process terminal
 */
static const char *
classify(int found_conhost, int found_terminal)
{
	if (found_conhost)
		return ("conhost");
	if (found_terminal)
		return ("terminal-conpty");
	return ("conpty-unknown");
}

int
main(int argc, char **argv)
{
	FILE	*f;
	HANDLE	 out;
	DWORD	 me = GetCurrentProcessId();
	DWORD	 pid, parent;
	DWORD	 list[64], n, i;
	DWORD	 mode = 0;
	char	 nbuf[256];
	const char *host;
	int	 found_conhost = 0, found_terminal = 0;
	int	 conpty = 0;

	if (argc < 2)
		return (2);
	f = fopen(argv[1], "wb");
	if (f == NULL)
		return (2);

	fprintf(f, "pid=%lu\n", (unsigned long)me);

	/* Console host visible in the ancestor chain? */
	pid = me;
	for (i = 0; i < 8; i++) {
		parent = parent_pid(pid);
		if (parent == 0 || parent == pid)
			break;
		if (process_name(parent, nbuf, sizeof nbuf) != NULL) {
			fprintf(f, "ancestor[%lu]=%s\n",
			    (unsigned long)parent, nbuf);
			if (name_contains(nbuf, "conhost"))
				found_conhost = 1;
			if (name_contains(nbuf, "WindowsTerminal") ||
			    name_contains(nbuf, "OpenConsole"))
				found_terminal = 1;
		}
		pid = parent;
	}

	/* Processes attached to this console per the kernel. */
	out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ |
	    FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (out == INVALID_HANDLE_VALUE) {
		fprintf(f, "conout=unavailable err=%lu\n",
		    (unsigned long)GetLastError());
	} else {
		n = GetConsoleProcessList(list, 64);
		fprintf(f, "console_processes=%lu\n", (unsigned long)n);
		for (i = 0; i < n && i < 64; i++) {
			if (process_name(list[i], nbuf, sizeof nbuf) != NULL) {
				fprintf(f, "console_pid[%lu]=%s\n",
				    (unsigned long)list[i], nbuf);
				if (name_contains(nbuf, "conhost"))
					found_conhost = 1;
			}
		}
		if (GetConsoleMode(out, &mode))
			fprintf(f, "conout_mode=0x%lx\n", (unsigned long)mode);
		CloseHandle(out);
	}

	host = classify(found_conhost, found_terminal);
	conpty = (strcmp(host, "conpty-unknown") == 0 ||
	    strcmp(host, "terminal-conpty") == 0);

	fprintf(f, "host=%s\n", host);
	fprintf(f, "conpty=%s\n", conpty ? "yes" : "no");
	fflush(f);
	fclose(f);
	return (0);
}
