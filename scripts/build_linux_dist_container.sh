#!/bin/sh

set -eu

require_env() {
	var_name=$1
	eval "var_value=\${$var_name:-}"
	[ -n "$var_value" ] || {
		printf '%s\n' "Missing required environment variable: $var_name" >&2
		exit 1
	}
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || {
		printf '%s\n' "Missing required command: $1" >&2
		exit 1
	}
}

validate_positive_integer() {
	value_name=$1
	value=$2

	case $value in
		'' | *[!0-9]*)
			printf '%s\n' "$value_name must be a positive integer: $value" >&2
			exit 1
			;;
	esac

	[ "$value" -gt 0 ] || {
		printf '%s\n' "$value_name must be a positive integer: $value" >&2
		exit 1
	}
}

require_env LIBXYZM_WORK_DIR
require_env LIBXYZM_META_DIR
require_env LIBXYZM_HOST_UID
require_env LIBXYZM_HOST_GID
require_env LIBXYZM_PACKAGE_VERSION
require_env LIBXYZM_PACKAGE_NAME
require_env LIBXYZM_PACKAGE_DESCRIPTION
require_env LIBXYZM_PACKAGE_MAINTAINER
require_env LIBXYZM_CARDIO_DIR
require_env LIBXYZM_PACKAGE_CFLAGS
require_env LIBXYZM_PACKAGE_CXXFLAGS

LIBXYZM_MAKE_JOBS=${LIBXYZM_MAKE_JOBS:-1}
validate_positive_integer 'LIBXYZM_MAKE_JOBS' "$LIBXYZM_MAKE_JOBS"

require_command make
require_command dpkg-architecture
require_command pkg-config

work_dir=$LIBXYZM_WORK_DIR
meta_dir=$LIBXYZM_META_DIR
build_dir="$work_dir/build"
stage_dir="$work_dir/stage/$LIBXYZM_PACKAGE_NAME"

rm -rf "$work_dir" "$meta_dir"
mkdir -p "$build_dir" "$meta_dir"

make -j "$LIBXYZM_MAKE_JOBS" all BUILD_DIR="$build_dir" CARDIO_DIR="$LIBXYZM_CARDIO_DIR" CARDIO_CPPFLAGS="-isystem$LIBXYZM_CARDIO_DIR/include" CFLAGS="$LIBXYZM_PACKAGE_CFLAGS" CXXFLAGS="$LIBXYZM_PACKAGE_CXXFLAGS"

deb_arch=$(dpkg-architecture -qDEB_HOST_ARCH)
multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)

lib_dir="$stage_dir/usr/lib/$multiarch"
include_dir="$stage_dir/usr/include/libxyzm"
doc_dir="$stage_dir/usr/share/doc/$LIBXYZM_PACKAGE_NAME"
control_dir="$stage_dir/DEBIAN"
control_path="$control_dir/control"

rm -rf "$stage_dir"
mkdir -p "$lib_dir" "$include_dir" "$doc_dir" "$control_dir"

cp "$build_dir/libxyzm.so" "$lib_dir/"
cp "$build_dir/libxyzm.a" "$lib_dir/"
cp "$build_dir/libxyzm_async.so" "$lib_dir/"
cp "$build_dir/libxyzm_async.a" "$lib_dir/"
cp include/libxyzm/*.h "$include_dir/"
cp LICENSE "$doc_dir/"
cp README.md "$doc_dir/"
cp README_ja.md "$doc_dir/"

{
	printf 'Package: %s\n' "$LIBXYZM_PACKAGE_NAME"
	printf 'Version: %s\n' "$LIBXYZM_PACKAGE_VERSION"
	printf 'Section: libs\n'
	printf 'Priority: optional\n'
	printf 'Architecture: %s\n' "$deb_arch"
	printf 'Maintainer: %s\n' "$LIBXYZM_PACKAGE_MAINTAINER"
	printf 'Depends: libc6, libstdc++6, libgcc-s1, liburing2\n'
	printf 'Description: %s\n' "$LIBXYZM_PACKAGE_DESCRIPTION"
} >"$control_path"

printf '%s\n' "$deb_arch" >"$meta_dir/deb_arch"
printf '%s\n' "$multiarch" >"$meta_dir/multiarch"

chown -R "$LIBXYZM_HOST_UID:$LIBXYZM_HOST_GID" "$work_dir" "$meta_dir"
