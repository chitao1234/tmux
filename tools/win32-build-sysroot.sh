#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
base_dir=$(CDPATH= cd -- "$repo_dir/.." && pwd)

prefix=${SYSROOT:-"$base_dir/sysroot"}
build_dir=${BUILD_DIR:-"$base_dir/build"}
host=${MINGW_HOST:-x86_64-w64-mingw32}
build=${BUILD:-x86_64-pc-linux-gnu}
ucrt_prefix=${UCRT_PREFIX:-/ucrt64}

if [ -d "$ucrt_prefix/bin" ]; then
	PATH="$ucrt_prefix/bin:$PATH"
	export PATH
fi

cc=${CC:-"$host-gcc"}
cxx=${CXX:-"$host-g++"}
ar=${AR:-"$host-ar"}
ranlib=${RANLIB:-"$host-ranlib"}
windres=${WINDRES:-"$host-windres"}
pkg_config=${PKG_CONFIG:-"$host-pkg-config --static"}

cppflags=${CPPFLAGS:-"-I$prefix/include -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000006"}
cflags=${CFLAGS:-"$cppflags"}
ldflags=${LDFLAGS:-"-L$prefix/lib -L$prefix/lib64"}

libevent_src=${LIBEVENT_SRC:-"$base_dir/libevent-2.1.12-stable"}
ncurses_src=${NCURSES_SRC:-"$base_dir/ncurses-6.6"}
openssl_src=${OPENSSL_SRC:-"$base_dir/openssl-3.5.6"}

mkdir -p "$prefix" "$build_dir"

run()
{
	printf '+'
	printf ' %s' "$@"
	printf '\n'
	"$@"
}

require_dir()
{
	if [ ! -d "$2" ]; then
		printf '%s: missing directory: %s\n' "$0" "$2" >&2
		exit 1
	fi
}

make_jobs()
{
	nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || printf 1
}

write_env()
{
	env_file=${1:-"$base_dir/sysroot-env.sh"}

	cat >"$env_file" <<EOF
#!/bin/sh
prefix=$prefix
export SYSROOT="\$prefix"
export MINGW_PREFIX="\$prefix"
export CC=$cc
export CXX=$cxx
export AR=$ar
export RANLIB=$ranlib
export STRIP=$host-strip
export WINDRES=$windres
export PKG_CONFIG="$pkg_config"
export PKG_CONFIG_LIBDIR="\$prefix/lib/pkgconfig:\$prefix/lib64/pkgconfig:\$prefix/share/pkgconfig"
export PKG_CONFIG_PATH="\$PKG_CONFIG_LIBDIR"
export CPPFLAGS="$cppflags"
export CFLAGS="\$CPPFLAGS"
export LDFLAGS="$ldflags"
export PATH_SEPARATOR=';'
EOF
	chmod +x "$env_file"
	printf 'wrote %s\n' "$env_file"
}

build_libevent()
{
	require_dir libevent "$libevent_src"
	mkdir -p "$build_dir/libevent-2.1.12-mingw"
	(
		cd "$build_dir/libevent-2.1.12-mingw"
		run "$libevent_src/configure" \
			--host="$host" \
			--build="$build" \
			--prefix="$prefix" \
			--disable-shared \
			--enable-static \
			--disable-samples \
			--disable-libevent-regress \
			--disable-openssl \
			CC="$cc" \
			CFLAGS="$cflags" \
			CPPFLAGS="$cppflags" \
			LDFLAGS="$ldflags"
		run make -j"$(make_jobs)"
		run make install
	)
}

build_ncurses()
{
	require_dir ncurses "$ncurses_src"
	mkdir -p "$build_dir/ncurses-6.6-mingw"
	(
		cd "$build_dir/ncurses-6.6-mingw"
		run "$ncurses_src/configure" \
			--host="$host" \
			--build="$build" \
			--prefix="$prefix" \
			--without-cxx \
			--without-ada \
			--without-tests \
			--without-manpages \
			--without-libtool \
			--disable-home-terminfo \
			--disable-termcap \
			--enable-database \
			--enable-sp-funcs \
			--enable-term-driver \
			--enable-interop \
			--enable-ext-funcs \
			--enable-pc-files \
			--with-pkg-config-libdir="$prefix/lib/pkgconfig" \
			--with-normal \
			--without-shared \
			--without-debug \
			--without-progs \
			CC="$cc" \
			CXX="$cxx" \
			CFLAGS="$cflags" \
			CPPFLAGS="$cppflags" \
			LDFLAGS="$ldflags"
		run make -j"$(make_jobs)"
		run make install
	)
}

build_openssl()
{
	require_dir openssl "$openssl_src"
	(
		cd "$openssl_src"
		run ./Configure \
			mingw64 \
			--prefix="$prefix" \
			--openssldir="$prefix/ssl" \
			no-shared \
			no-tests \
			no-docs \
			no-demos \
			CC="$cc" \
			CXX="$cxx" \
			AR="$ar" \
			RANLIB="$ranlib" \
			WINDRES="$windres" \
			CPPFLAGS="$cppflags" \
			CFLAGS="$cflags" \
			LDFLAGS="$ldflags"
		run make -j"$(make_jobs)"
		run make install_sw
	)
}

copy_regex()
{
	require_dir ucrt64 "$ucrt_prefix"

	mkdir -p "$prefix/include" "$prefix/include/tre" "$prefix/lib" \
		"$prefix/lib/pkgconfig" "$prefix/bin"
	run cp "$ucrt_prefix/include/regex.h" "$prefix/include/regex.h"
	run cp "$ucrt_prefix/include/tre/regex.h" "$prefix/include/tre/regex.h"
	run cp "$ucrt_prefix/include/tre/tre-config.h" \
		"$prefix/include/tre/tre-config.h"
	run cp "$ucrt_prefix/include/tre/tre.h" "$prefix/include/tre/tre.h"

	run cp "$ucrt_prefix/lib/libregex.a" "$prefix/lib/libregex.a"
	run cp "$ucrt_prefix/lib/libregex.dll.a" "$prefix/lib/libregex.dll.a"
	run cp "$ucrt_prefix/lib/libsystre.a" "$prefix/lib/libsystre.a"
	run cp "$ucrt_prefix/lib/libsystre.dll.a" "$prefix/lib/libsystre.dll.a"
	run cp "$ucrt_prefix/lib/libtre.a" "$prefix/lib/libtre.a"
	run cp "$ucrt_prefix/lib/libtre.dll.a" "$prefix/lib/libtre.dll.a"
	run cp "$ucrt_prefix/lib/libintl.a" "$prefix/lib/libintl.a"
	run cp "$ucrt_prefix/lib/libintl.dll.a" "$prefix/lib/libintl.dll.a"
	for pc in regex tre; do
		sed "s|^prefix=.*|prefix=$prefix|;s|-I$ucrt_prefix/include|-I\${includedir}|g" \
			"$ucrt_prefix/lib/pkgconfig/$pc.pc" \
			>"$prefix/lib/pkgconfig/$pc.pc"
	done

	run cp "$ucrt_prefix/bin/libsystre-0.dll" "$prefix/bin/libsystre-0.dll"
	run cp "$ucrt_prefix/bin/libtre-5.dll" "$prefix/bin/libtre-5.dll"
	run cp "$ucrt_prefix/bin/libintl-8.dll" "$prefix/bin/libintl-8.dll"
}

verify_sysroot()
{
	tmp=${TMPDIR:-/tmp}/tmux-win32-sysroot.$$
	mkdir -p "$tmp"
	trap 'rm -rf "$tmp"' EXIT HUP INT TERM

	cat >"$tmp/probe.c" <<'EOF'
#include <event.h>
#include <curses.h>
#include <regex.h>

int
main(void)
{
	regex_t re;

	event_init();
	if (regcomp(&re, "tmux", REG_EXTENDED) != 0)
		return (1);
	regfree(&re);
	return (OK == 0 ? 0 : 0);
}
EOF

	export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$prefix/lib64/pkgconfig:$prefix/share/pkgconfig"
	export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
	run $cc $cflags -o "$tmp/probe.exe" "$tmp/probe.c" \
		$($pkg_config --cflags --libs libevent_core ncursesw regex)
	file "$tmp/probe.exe"
}

verify_openssl()
{
	tmp=${TMPDIR:-/tmp}/tmux-win32-openssl.$$
	mkdir -p "$tmp"
	trap 'rm -rf "$tmp"' EXIT HUP INT TERM

	cat >"$tmp/probe.c" <<'EOF'
#include <openssl/ssl.h>

int
main(void)
{
	SSL_library_init();
	return (0);
}
EOF

	export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$prefix/lib64/pkgconfig:$prefix/share/pkgconfig"
	export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
	run $cc $cflags -o "$tmp/probe.exe" "$tmp/probe.c" \
		$($pkg_config --cflags --libs openssl)
	file "$tmp/probe.exe"
}

usage()
{
	cat <<EOF
usage: $0 [all|env|libevent|ncurses|openssl|regex|verify|verify-openssl]

Build or verify the MinGW sysroot used by the native Win32 tmux port.

Environment overrides:
  SYSROOT       install prefix, default: $base_dir/sysroot
  BUILD_DIR     out-of-tree dependency builds, default: $base_dir/build
  MINGW_HOST    target triplet, default: x86_64-w64-mingw32
  LIBEVENT_SRC  libevent source directory
  NCURSES_SRC   ncurses source directory
  OPENSSL_SRC   OpenSSL source directory
  UCRT_PREFIX   MSYS2 UCRT prefix used for regex/TRE, default: /ucrt64
EOF
}

cmd=${1:-all}
case "$cmd" in
all)
	write_env
	build_openssl
	build_libevent
	build_ncurses
	copy_regex
	verify_sysroot
	;;
env)
	write_env
	;;
libevent)
	build_libevent
	;;
ncurses)
	build_ncurses
	;;
openssl)
	build_openssl
	;;
regex)
	copy_regex
	;;
verify)
	verify_sysroot
	;;
verify-openssl)
	verify_openssl
	;;
*)
	usage >&2
	exit 1
	;;
esac
