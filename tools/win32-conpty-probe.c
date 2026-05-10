#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int
main(void)
{
	HANDLE in_read = NULL, in_write = NULL;
	HANDLE out_read = NULL, out_write = NULL;
	HPCON hpcon = NULL;
	COORD size;
	HRESULT hr;

	if (!CreatePipe(&in_read, &in_write, NULL, 0)) {
		fprintf(stderr, "CreatePipe input failed: %lu\n", GetLastError());
		return (1);
	}
	if (!CreatePipe(&out_read, &out_write, NULL, 0)) {
		fprintf(stderr, "CreatePipe output failed: %lu\n", GetLastError());
		return (1);
	}

	size.X = 80;
	size.Y = 24;
	hr = CreatePseudoConsole(size, in_read, out_write, 0, &hpcon);
	if (FAILED(hr)) {
		fprintf(stderr, "CreatePseudoConsole failed: 0x%08lx\n",
		    (unsigned long)hr);
		return (1);
	}

	hr = ResizePseudoConsole(hpcon, size);
	if (FAILED(hr)) {
		fprintf(stderr, "ResizePseudoConsole failed: 0x%08lx\n",
		    (unsigned long)hr);
		return (1);
	}

	ClosePseudoConsole(hpcon);
	CloseHandle(in_read);
	CloseHandle(in_write);
	CloseHandle(out_read);
	CloseHandle(out_write);
	puts("ConPTY compile probe OK");
	return (0);
}
