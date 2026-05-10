#!/bin/sh
set -eu

cc=${MINGW_CC:-x86_64-w64-mingw32-gcc}
out=${1:-tools/win32-conpty-probe.exe}

"$cc" -Wall -Wextra -Werror -std=gnu99 \
	-o "$out" tools/win32-conpty-probe.c \
	-lkernel32

printf '%s\n' "built $out"
