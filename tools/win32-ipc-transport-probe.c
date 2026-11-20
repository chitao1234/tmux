/* $OpenBSD$ */

/*
 * Copyright (c) 2026 tmux Windows port contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

/* Exercise a pipe write that remains pending while its peer stops reading. */
#define _WIN32_WINNT 0x0A00
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tmux-protocol.h"

struct header {
	uint32_t type, len, version, pid;
};

struct sample {
	uint32_t hash, suffix;
};

static int
compare_sample(const void *a, const void *b)
{
	const struct sample *x = a, *y = b;

	return ((x->hash > y->hash) - (x->hash < y->hash));
}

static int
find_collision(const char *prefix)
{
	struct sample *samples;
	const unsigned char *p;
	char path[4096];
	uint32_t i, hash, x = 0x12345678, count = 400000;
	unsigned char c;

	if (strlen(prefix) + 9 > sizeof path)
		return (2);
	samples = calloc(count, sizeof *samples);
	if (samples == NULL)
		return (2);
	for (i = 0; i < count; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		snprintf(path, sizeof path, "%s%08x", prefix, (unsigned)x);
		hash = 2166136261u;
		for (p = (unsigned char *)path; *p != '\0'; p++) {
			c = *p;
			if (c >= 'A' && c <= 'Z')
				c += 'a' - 'A';
			else if (c == '\\')
				c = '/';
			hash = (hash ^ c) * 16777619u;
		}
		samples[i].hash = hash;
		samples[i].suffix = x;
	}
	qsort(samples, count, sizeof *samples, compare_sample);
	for (i = 1; i < count; i++) {
		if (samples[i - 1].hash == samples[i].hash) {
			printf("%s%08x\n%s%08x\n", prefix,
			    (unsigned)samples[i - 1].suffix,
			    prefix, (unsigned)samples[i].suffix);
			free(samples);
			return (0);
		}
	}
	free(samples);
	return (1);
}

static int
transfer(HANDLE pipe, void *data, DWORD size, int writing)
{
	OVERLAPPED ov = {0};
	DWORD done, error;
	BOOL ok;

	ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (ov.hEvent == NULL)
		return (-1);
	while (size != 0) {
		ResetEvent(ov.hEvent);
		if (writing)
			ok = WriteFile(pipe, data, size, &done, &ov);
		else
			ok = ReadFile(pipe, data, size, &done, &ov);
		if (!ok) {
			error = GetLastError();
			if (error != ERROR_IO_PENDING)
				goto fail;
			if (WaitForSingleObject(ov.hEvent, 3000) != WAIT_OBJECT_0) {
				CancelIoEx(pipe, &ov);
				GetOverlappedResult(pipe, &ov, &done, TRUE);
				goto fail;
			}
			if (!GetOverlappedResult(pipe, &ov, &done, FALSE))
				goto fail;
		}
		if (done == 0)
			goto fail;
		size -= done;
		data = (char *)data + done;
	}
	CloseHandle(ov.hEvent);
	return (0);
fail:
	CloseHandle(ov.hEvent);
	return (-1);
}

static int
send_message(HANDLE pipe, uint32_t type, void *body, DWORD size)
{
	struct header header = {type, sizeof header + size,
	    PROTOCOL_VERSION, GetCurrentProcessId()};

	if (transfer(pipe, &header, sizeof header, 1) != 0)
		return (-1);
	return (transfer(pipe, body, size, 1));
}

static int
read_message(HANDLE pipe, struct header *header, void *body, DWORD capacity)
{
	if (transfer(pipe, header, sizeof *header, 0) != 0 ||
	    header->len < sizeof *header || header->len - sizeof *header > capacity)
		return (-1);
	return (transfer(pipe, body, header->len - sizeof *header, 0));
}

int
main(int argc, char **argv)
{
	HANDLE pipe = INVALID_HANDLE_VALUE, server = NULL, remote = NULL;
	DWORD server_pid;
	wchar_t name[128];
	struct header header;
	struct msg_win32_auth_bind bind;
	struct msg_write_ready ready;
	char body[16384];
	/* Packed argc followed by argv strings. */
	struct {
		int argc;
		char argv[sizeof "save-buffer\0-b\0ipc-test\0-"];
	} command = {4, "save-buffer\0-b\0ipc-test\0-"};
	int result = 1, reject = 0;

	if (argc == 3 && strcmp(argv[1], "--collision") == 0)
		return (find_collision(argv[2]));
	if (argc == 3 && strcmp(argv[1], "--reject") == 0) {
		reject = 1;
		argc--;
		argv++;
	}
	if (argc != 2 || strlen(argv[1]) > 90)
		return (2);
	swprintf(name, 128, L"\\\\.\\pipe\\%hs", argv[1]);
	pipe = CreateFileW(name, GENERIC_READ|GENERIC_WRITE, 0, NULL,
	    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (pipe == INVALID_HANDLE_VALUE ||
	    !GetNamedPipeServerProcessId(pipe, &server_pid))
		goto out;
	server = OpenProcess(PROCESS_DUP_HANDLE, FALSE, server_pid);
	if (server == NULL || !DuplicateHandle(GetCurrentProcess(),
	    GetCurrentProcess(), server, &remote,
	    PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0))
		goto out;
	bind.process_handle = (uint64_t)(uintptr_t)remote;
	if (send_message(pipe, MSG_WIN32_AUTH_BIND, &bind, sizeof bind) != 0)
		goto out;
	remote = NULL; /* The server now owns this handle. */
	if (read_message(pipe, &header, body, sizeof body) != 0 ||
	    header.type != MSG_WIN32_AUTH_RESULT)
		goto out;
	if (send_message(pipe, MSG_IDENTIFY_DONE, NULL, 0) != 0 ||
	    send_message(pipe, MSG_COMMAND, &command,
	    sizeof command.argc + sizeof command.argv) != 0)
		goto out;
	do {
		if (read_message(pipe, &header, body, sizeof body) != 0)
			goto out;
	} while (header.type != MSG_WRITE_OPEN);
	memcpy(&ready.stream, body, sizeof ready.stream);
	ready.error = 0;
	if (send_message(pipe, MSG_WRITE_READY, &ready, sizeof ready) != 0)
		goto out;
	if (reject) {
		header.type = MSG_IDENTIFY_FLAGS;
		header.len = sizeof header;
		header.version = PROTOCOL_VERSION - 1;
		header.pid = GetCurrentProcessId();
		if (transfer(pipe, &header, sizeof header, 1) != 0)
			goto out;
	}
	puts("Holding bulk output unread");
	fflush(stdout);
	/* The smoke runner checks another client while this pipe is full. */
	Sleep(reject ? 6000 : 2000);
	if (reject && send_message(pipe, MSG_EXITING, NULL, 0) == 0) {
		fprintf(stderr, "bad peer was not closed at its deadline\n");
		goto out;
	}
	result = 0;
out:
	if (result != 0)
		fprintf(stderr, "transport probe failed: %lu\n", GetLastError());
	if (remote != NULL)
		DuplicateHandle(server, remote, NULL, NULL, 0, FALSE,
		    DUPLICATE_CLOSE_SOURCE);
	if (server != NULL)
		CloseHandle(server);
	if (pipe != INVALID_HANDLE_VALUE)
		CloseHandle(pipe);
	return (result);
}
