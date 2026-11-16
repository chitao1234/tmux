/*
 * win32-conhost-ident - report what is actually hosting this console.
 *
 * Guessing from the ancestor chain is not enough, and getting it wrong is
 * expensive: an earlier version of this probe reported ConEmu as ConPTY and a
 * whole comparison run was labelled with the wrong host.
 *
 * The three signals that actually separate the cases:
 *
 *   conhost ancestor      a classic console has conhost.exe above us in the
 *                         parent chain
 *   window owner          GetConsoleWindow() belongs to some other process
 *                         when an emulator (ConEmu and friends) draws the
 *                         console itself
 *   conhost existence     a pseudo console still has a conhost.exe, but as a
 *                         sibling created by the terminal, never as our
 *                         ancestor
 *
 * Which gives:
 *
 *   emulated          window owned by another process (ConEmu)
 *   conhost           conhost.exe in our ancestor chain (classic)
 *   conpty            conhost.exe exists but is not our ancestor (ConPTY)
 *   conpty-openconsole no conhost.exe at all (Windows Terminal, WezTerm)
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

struct proc {
	DWORD	pid;
	DWORD	ppid;
	char	name[128];
};

static struct proc *
snapshot(size_t *countp)
{
	HANDLE			 snap;
	PROCESSENTRY32W		 pe;
	struct proc		*list = NULL;
	size_t			 n = 0, cap = 0;

	snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return (NULL);
	pe.dwSize = sizeof pe;
	if (Process32FirstW(snap, &pe)) {
		do {
			if (n == cap) {
				cap = cap == 0 ? 256 : cap * 2;
				list = realloc(list, cap * sizeof *list);
				if (list == NULL) {
					CloseHandle(snap);
					return (NULL);
				}
			}
			list[n].pid = pe.th32ProcessID;
			list[n].ppid = pe.th32ParentProcessID;
			list[n].name[0] = '\0';
			WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1,
			    list[n].name, sizeof list[n].name, NULL, NULL);
			n++;
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	*countp = n;
	return (list);
}

static const char *
name_of(struct proc *list, size_t n, DWORD pid)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (list[i].pid == pid)
			return (list[i].name);
	}
	return (NULL);
}

static DWORD
parent_of(struct proc *list, size_t n, DWORD pid)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (list[i].pid == pid)
			return (list[i].ppid);
	}
	return (0);
}

static int
is_conhost(const char *name)
{
	return (name != NULL && _stricmp(name, "conhost.exe") == 0);
}

/* Is there a conhost.exe that is our ancestor, within a sane depth? */
static DWORD
conhost_ancestor(struct proc *list, size_t n, DWORD me)
{
	DWORD	chain = me;
	int	depth;

	for (depth = 0; depth < 16; depth++) {
		DWORD pp = parent_of(list, n, chain);

		if (pp == 0 || pp == chain)
			break;
		if (is_conhost(name_of(list, n, pp)))
			return (pp);
		chain = pp;
	}
	return (0);
}

/*
 * Is a conhost.exe our sibling?
 *
 * A pseudo console created by the system has its conhost.exe spawned by the
 * same parent that spawned us, so it appears beside us rather than above us.
 * Counting conhost.exe processes machine-wide is useless - services keep their
 * own - so the only meaningful evidence is a conhost sharing our parent, or
 * sharing our grandparent when the terminal inserted one more level.
 */
static DWORD
conhost_sibling(struct proc *list, size_t n, DWORD me)
{
	DWORD	mypp = parent_of(list, n, me);
	DWORD	grand = mypp != 0 ? parent_of(list, n, mypp) : 0;
	size_t	i;

	for (i = 0; i < n; i++) {
		if (!is_conhost(list[i].name))
			continue;
		if (mypp != 0 && list[i].ppid == mypp)
			return (list[i].pid);
		if (grand != 0 && list[i].ppid == grand)
			return (list[i].pid);
	}
	return (0);
}

int
main(int argc, char **argv)
{
	FILE		*f;
	struct proc	*list;
	size_t		 n;
	DWORD		 me = GetCurrentProcessId();
	DWORD		 ancestor, sibling, wpid = 0;
	HWND		 w;
	const char	*host;
	int		 owned_by_other = 0;

	if (argc < 2)
		return (2);
	f = fopen(argv[1], "wb");
	if (f == NULL)
		return (2);

	list = snapshot(&n);
	if (list == NULL) {
		fprintf(f, "error=snapshot_failed\n");
		fclose(f);
		return (1);
	}

	fprintf(f, "pid=%lu\n", (unsigned long)me);
	fprintf(f, "parent=%lu\n", (unsigned long)parent_of(list, n, me));
	if (name_of(list, n, parent_of(list, n, me)) != NULL) {
		fprintf(f, "parent_name=%s\n",
		    name_of(list, n, parent_of(list, n, me)));
	}

	/* Ancestor chain, for the record. */
	{
		DWORD	chain = me;
		int	depth;

		for (depth = 0; depth < 6; depth++) {
			DWORD		pp = parent_of(list, n, chain);
			const char	*nm;

			if (pp == 0 || pp == chain)
				break;
			nm = name_of(list, n, pp);
			fprintf(f, "ancestor[%lu]=%s\n", (unsigned long)pp,
			    nm != NULL ? nm : "?");
			chain = pp;
		}
	}

	ancestor = conhost_ancestor(list, n, me);
	sibling = conhost_sibling(list, n, me);
	fprintf(f, "conhost_ancestor=%lu\n", (unsigned long)ancestor);
	fprintf(f, "conhost_sibling=%lu\n", (unsigned long)sibling);

	/* Console process list, which shows injected helpers. */
	{
		DWORD	clist[128], cn, j;

		cn = GetConsoleProcessList(clist, 128);
		fprintf(f, "console_processes=%lu\n", (unsigned long)cn);
		for (j = 0; j < cn && j < 128; j++) {
			const char *nm = name_of(list, n, clist[j]);

			fprintf(f, "console_pid[%lu]=%s\n",
			    (unsigned long)clist[j], nm != NULL ? nm : "?");
		}
	}

	/*
	 * The console window's owner is the decisive signal for an emulator: a
	 * real console is drawn by whoever owns the window, and when that is a
	 * different process the console is being rendered for us rather than by
	 * the system host.
	 */
	w = GetConsoleWindow();
	if (w != NULL) {
		GetWindowThreadProcessId(w, &wpid);
		fprintf(f, "window_pid=%lu\n", (unsigned long)wpid);
		if (name_of(list, n, wpid) != NULL) {
			fprintf(f, "window_owner=%s\n",
			    name_of(list, n, wpid));
		}
		fprintf(f, "window_visible=%d\n", IsWindowVisible(w) ? 1 : 0);
		if (wpid != 0 && wpid != me)
			owned_by_other = 1;
	}
	fprintf(f, "window_owned_by_other=%d\n", owned_by_other);

	if (owned_by_other)
		host = "emulated";
	else if (ancestor != 0)
		host = "conhost";
	else if (sibling != 0)
		host = "conpty";
	else
		host = "conpty-openconsole";

	fprintf(f, "host=%s\n", host);
	fflush(f);
	fclose(f);
	free(list);
	return (0);
}
