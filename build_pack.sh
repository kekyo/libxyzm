#!/bin/sh

set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ARTIFACT_ROOT="$PROJECT_ROOT/artifacts"
PACKAGE_NAME=libxyzm
PACKAGE_DESCRIPTION="A portable implementation of X/Y/ZMODEM."
DEFAULT_MAINTAINER="libxyzm packager <packager@localhost>"
DEFAULT_PARALLEL_JOB_CAP=16
PACKAGE_CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Wformat=2 -Wundef -Wshadow -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wold-style-definition -Wconversion -Wsign-conversion -O3"
PACKAGE_CXXFLAGS="-std=c++20 -Wall -Wextra -Wpedantic -Wformat=2 -Wundef -Wcast-align -Wconversion -Wsign-conversion -O3 -pthread"

LINUX_MATRIX=$(cat <<'EOF'
debian bookworm amd64 linux/amd64
debian bookworm i686 linux/386
debian bookworm arm64 linux/arm64
debian bookworm armv7l linux/arm/v7
debian trixie amd64 linux/amd64
debian trixie i686 linux/386
debian trixie arm64 linux/arm64
debian trixie armv7l linux/arm/v7
debian trixie riscv64 linux/riscv64
ubuntu 22.04 amd64 linux/amd64
ubuntu 22.04 arm64 linux/arm64
ubuntu 22.04 armv7l linux/arm/v7
ubuntu 22.04 riscv64 linux/riscv64
ubuntu 24.04 amd64 linux/amd64
ubuntu 24.04 arm64 linux/arm64
ubuntu 24.04 armv7l linux/arm/v7
ubuntu 24.04 riscv64 linux/riscv64
EOF
)

WIN32_MATRIX=$(cat <<'EOF'
x86 i686-w64-mingw32-gcc i686-w64-mingw32-g++
x64 x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++
EOF
)

print_usage() {
	cat <<'EOF'
Usage: ./build_pack.sh [options]

Options:
  --version <version>  Package version. Defaults to a screw-up-derived version.
  --target <target>    all, deb, or win32. Defaults to all.
  --distro <list>      Comma-separated distro filter for deb builds.
  --release <list>     Comma-separated release filter for deb builds.
  --arch <list>        Comma-separated architecture filter.
  --jobs <count>       Maximum concurrent package jobs. Defaults to auto (up to 14).
  --print-version      Print the resolved package version and exit.
  --help               Show this help.
EOF
}

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || fail "Missing required command: $1"
}

validate_positive_integer() {
	value_name=$1
	value=$2

	case $value in
		'' | *[!0-9]*)
			fail "$value_name must be a positive integer: $value"
			;;
	esac

	[ "$value" -gt 0 ] || fail "$value_name must be a positive integer: $value"
}

detect_processor_count() {
	detected_count=''

	if command -v getconf >/dev/null 2>&1; then
		detected_count=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
	fi
	if [ -z "$detected_count" ] && command -v nproc >/dev/null 2>&1; then
		detected_count=$(nproc 2>/dev/null || true)
	fi
	if [ -z "$detected_count" ] && command -v sysctl >/dev/null 2>&1; then
		detected_count=$(sysctl -n hw.ncpu 2>/dev/null || true)
	fi

	case $detected_count in
		'' | *[!0-9]*)
			detected_count=1
			;;
	esac

	if [ "$detected_count" -lt 1 ]; then
		detected_count=1
	fi

	printf '%s\n' "$detected_count"
}

min_int() {
	left_value=$1
	right_value=$2

	if [ "$left_value" -le "$right_value" ]; then
		printf '%s\n' "$left_value"
	else
		printf '%s\n' "$right_value"
	fi
}

detect_version() {
	if ! command -v screw-up >/dev/null 2>&1; then
		fail 'Missing required command: screw-up. Pass --version to build packages without screw-up.'
	fi
	detected_version=$(printf '%s\n' '{version}' | screw-up format | tr -d '\r')
	[ -n "$detected_version" ] || fail 'screw-up did not return a version'
	printf '%s\n' "$detected_version"
}

validate_version() {
	case $1 in
		'' | *[!0-9A-Za-z.+:~\-]*)
			fail "Invalid package version: $1"
			;;
	esac
}

matches_filter() {
	filter_value=$1
	actual_value=$2
	if [ -z "$filter_value" ]; then
		return 0
	fi

	previous_ifs=$IFS
	IFS=','
	for allowed_value in $filter_value; do
		if [ "$allowed_value" = "$actual_value" ]; then
			IFS=$previous_ifs
			return 0
		fi
	done
	IFS=$previous_ifs
	return 1
}

pack_image_for_target() {
	distro=$1
	release=$2
	arch=$3

	printf 'localhost/libxyzm-pack-%s-%s-%s:latest\n' "$distro" "$release" "$arch"
}

count_deb_builds() {
	build_count=0

	while IFS=' ' read -r distro release arch platform; do
		[ -n "$distro" ] || continue
		matches_filter "$DISTRO_FILTER" "$distro" || continue
		matches_filter "$RELEASE_FILTER" "$release" || continue
		matches_filter "$ARCH_FILTER" "$arch" || continue
		build_count=$((build_count + 1))
	done <<EOF
$LINUX_MATRIX
EOF

	printf '%s\n' "$build_count"
}

count_win32_builds() {
	build_count=0

	while IFS=' ' read -r arch cc cxx; do
		[ -n "$arch" ] || continue
		matches_filter "$ARCH_FILTER" "$arch" || continue
		build_count=$((build_count + 1))
	done <<EOF
$WIN32_MATRIX
EOF

	printf '%s\n' "$build_count"
}

assert_prereq_image() {
	image=$1

	if ! podman image exists "$image"; then
		fail "Missing prerequisite image: $image. Run ./prereq.sh first."
	fi
}

build_deb_package() {
	distro=$1
	release=$2
	arch=$3
	platform=$4
	image=$(pack_image_for_target "$distro" "$release" "$arch")
	work_root="$TMP_ROOT/deb/$distro/$release/$arch"
	container_root="/workspace/artifacts/.tmp/$RUN_ID/deb/$distro/$release/$arch"
	work_dir="$container_root/work"
	meta_dir="$container_root/meta"
	package_dir="$ARTIFACT_ROOT/deb"

	assert_prereq_image "$image"

	printf '%s\n' "[deb] $distro $release $arch"
	mkdir -p "$package_dir"

	podman run --rm \
		--platform "$platform" \
		-v "$PROJECT_ROOT:/workspace" \
		-v "$CARDIO_DIR:/workspace-cardio:ro" \
		-w /workspace \
		-e LIBXYZM_WORK_DIR="$work_dir" \
		-e LIBXYZM_META_DIR="$meta_dir" \
		-e LIBXYZM_HOST_UID="$(id -u)" \
		-e LIBXYZM_HOST_GID="$(id -g)" \
		-e LIBXYZM_PACKAGE_VERSION="$VERSION" \
		-e LIBXYZM_PACKAGE_NAME="$PACKAGE_NAME" \
		-e LIBXYZM_PACKAGE_DESCRIPTION="$PACKAGE_DESCRIPTION" \
		-e LIBXYZM_PACKAGE_MAINTAINER="${DEB_MAINTAINER:-$DEFAULT_MAINTAINER}" \
		-e LIBXYZM_MAKE_JOBS="$MAKE_JOBS" \
		-e LIBXYZM_CARDIO_DIR=/workspace-cardio \
		-e LIBXYZM_PACKAGE_CFLAGS="$PACKAGE_CFLAGS" \
		-e LIBXYZM_PACKAGE_CXXFLAGS="$PACKAGE_CXXFLAGS" \
		"$image" \
		./scripts/build_linux_dist_container.sh

	deb_arch=$(cat "$work_root/meta/deb_arch")
	dpkg-deb --root-owner-group --build \
		"$work_root/work/stage/$PACKAGE_NAME" \
		"$package_dir/${PACKAGE_NAME}-${VERSION}-${distro}-${release}-${deb_arch}.deb" >/dev/null
}

build_win32_package() {
	label=$1
	cc=$2
	cxx=$3
	work_root="$TMP_ROOT/win32/$label"
	build_dir="$work_root/build"
	stage_root="$work_root/stage"
	top_dir_name="${PACKAGE_NAME}-${VERSION}-win32-$label"
	package_root="$stage_root/$top_dir_name"
	package_dir="$ARTIFACT_ROOT/win32"
	package_path="$package_dir/${top_dir_name}.zip"

	printf '%s\n' "[win32] $label"
	rm -rf "$work_root"
	mkdir -p "$package_root/bin" "$package_root/lib" "$package_root/include/libxyzm" "$package_dir"

	make -j "$MAKE_JOBS" all TARGET_OS=win32 BUILD_DIR="$build_dir" CC="$cc" CXX="$cxx" CARDIO_DIR="$CARDIO_DIR" CARDIO_CPPFLAGS="-isystem$CARDIO_DIR/include" CFLAGS="$PACKAGE_CFLAGS" CXXFLAGS="$PACKAGE_CXXFLAGS"

	cp "$build_dir/libxyzm.dll" "$package_root/bin/"
	cp "$build_dir/libxyzm_async.dll" "$package_root/bin/"
	cp "$build_dir/libxyzm.dll.a" "$package_root/lib/"
	cp "$build_dir/libxyzm_async.dll.a" "$package_root/lib/"
	cp "$build_dir/libxyzm.a" "$package_root/lib/"
	cp "$build_dir/libxyzm_async.a" "$package_root/lib/"
	cp include/libxyzm/*.h "$package_root/include/libxyzm/"
	cp LICENSE "$package_root/"
	cp README.md "$package_root/"
	cp README_ja.md "$package_root/"

	rm -f "$package_path"
	(
		cd "$stage_root"
		zip -qr "$package_path" "$top_dir_name"
	)
}

wait_for_oldest_job() {
	[ "$ACTIVE_JOB_COUNT" -gt 0 ] || return 0

	set -- $ACTIVE_JOB_PIDS
	wait_pid=$1
	shift

	if wait "$wait_pid"; then
		:
	else
		JOB_FAILURE=1
	fi

	ACTIVE_JOB_PIDS=$*
	ACTIVE_JOB_COUNT=$((ACTIVE_JOB_COUNT - 1))
}

run_parallel_job() {
	while [ "$ACTIVE_JOB_COUNT" -ge "$PARALLEL_JOBS" ]; do
		wait_for_oldest_job
	done

	[ "$JOB_FAILURE" -eq 0 ] || fail 'One or more package builds failed'

	"$@" &
	ACTIVE_JOB_PIDS="${ACTIVE_JOB_PIDS}${ACTIVE_JOB_PIDS:+ }$!"
	ACTIVE_JOB_COUNT=$((ACTIVE_JOB_COUNT + 1))
}

wait_for_all_jobs() {
	while [ "$ACTIVE_JOB_COUNT" -gt 0 ]; do
		wait_for_oldest_job
	done

	[ "$JOB_FAILURE" -eq 0 ] || fail 'One or more package builds failed'
}

schedule_deb_builds() {
	while IFS=' ' read -r distro release arch platform; do
		[ -n "$distro" ] || continue
		matches_filter "$DISTRO_FILTER" "$distro" || continue
		matches_filter "$RELEASE_FILTER" "$release" || continue
		matches_filter "$ARCH_FILTER" "$arch" || continue
		run_parallel_job build_deb_package "$distro" "$release" "$arch" "$platform"
	done <<EOF
$LINUX_MATRIX
EOF
}

schedule_win32_builds() {
	while IFS=' ' read -r arch cc cxx; do
		[ -n "$arch" ] || continue
		matches_filter "$ARCH_FILTER" "$arch" || continue
		run_parallel_job build_win32_package "$arch" "$cc" "$cxx"
	done <<EOF
$WIN32_MATRIX
EOF
}

VERSION=''
TARGET='all'
DISTRO_FILTER=''
RELEASE_FILTER=''
ARCH_FILTER=''
PARALLEL_JOBS=''
PRINT_VERSION='false'

while [ "$#" -gt 0 ]; do
	case $1 in
		--version)
			[ "$#" -ge 2 ] || fail 'Missing value for --version'
			VERSION=$2
			shift 2
			;;
		--target)
			[ "$#" -ge 2 ] || fail 'Missing value for --target'
			TARGET=$2
			shift 2
			;;
		--distro)
			[ "$#" -ge 2 ] || fail 'Missing value for --distro'
			DISTRO_FILTER=$2
			shift 2
			;;
		--release)
			[ "$#" -ge 2 ] || fail 'Missing value for --release'
			RELEASE_FILTER=$2
			shift 2
			;;
		--arch)
			[ "$#" -ge 2 ] || fail 'Missing value for --arch'
			ARCH_FILTER=$2
			shift 2
			;;
		--jobs)
			[ "$#" -ge 2 ] || fail 'Missing value for --jobs'
			PARALLEL_JOBS=$2
			shift 2
			;;
		--help)
			print_usage
			exit 0
			;;
		--print-version)
			PRINT_VERSION='true'
			shift
			;;
		*)
			fail "Unknown argument: $1"
			;;
	esac
done

if [ -z "$VERSION" ]; then
	VERSION=$(detect_version)
fi
validate_version "$VERSION"
if [ -n "$PARALLEL_JOBS" ]; then
	validate_positive_integer 'Parallel job count' "$PARALLEL_JOBS"
fi

if [ "$PRINT_VERSION" = 'true' ]; then
	printf '%s\n' "$VERSION"
	exit 0
fi

case $TARGET in
	all | deb | win32)
		;;
	*)
		fail "Unsupported target: $TARGET"
		;;
esac

CARDIO_DIR=${CARDIO_DIR:-"$PROJECT_ROOT/deps/cardio"}
CARDIO_DIR=$(CDPATH= cd -- "$CARDIO_DIR" && pwd) || fail "Missing cardio directory: ${CARDIO_DIR:-}"
[ -f "$CARDIO_DIR/include/cardio.h" ] || fail "Missing cardio header: $CARDIO_DIR/include/cardio.h"

require_command make

CPU_COUNT=$(detect_processor_count)
if [ -z "$PARALLEL_JOBS" ]; then
	PARALLEL_JOBS=$(min_int "$CPU_COUNT" "$DEFAULT_PARALLEL_JOB_CAP")
fi

BUILD_TASK_COUNT=0
if [ "$TARGET" = 'all' ] || [ "$TARGET" = 'deb' ]; then
	BUILD_TASK_COUNT=$((BUILD_TASK_COUNT + $(count_deb_builds)))
fi
if [ "$TARGET" = 'all' ] || [ "$TARGET" = 'win32' ]; then
	BUILD_TASK_COUNT=$((BUILD_TASK_COUNT + $(count_win32_builds)))
fi

if [ "$BUILD_TASK_COUNT" -gt 0 ]; then
	EFFECTIVE_BUILD_JOBS=$(min_int "$PARALLEL_JOBS" "$BUILD_TASK_COUNT")
else
	EFFECTIVE_BUILD_JOBS=1
fi

MAKE_JOBS=$((CPU_COUNT / EFFECTIVE_BUILD_JOBS))
if [ "$MAKE_JOBS" -lt 1 ]; then
	MAKE_JOBS=1
fi

RUN_ID="run-$(date +%Y%m%d%H%M%S)-$$"
TMP_ROOT="$ARTIFACT_ROOT/.tmp/$RUN_ID"
ACTIVE_JOB_PIDS=''
ACTIVE_JOB_COUNT=0
JOB_FAILURE=0

mkdir -p "$ARTIFACT_ROOT"
rm -rf "$ARTIFACT_ROOT/deb" "$ARTIFACT_ROOT/win32"
mkdir -p "$TMP_ROOT"

printf '%s\n' "Using up to $PARALLEL_JOBS package jobs with make -j$MAKE_JOBS"

if [ "$TARGET" = 'all' ] || [ "$TARGET" = 'deb' ]; then
	require_command podman
	require_command dpkg-deb
	schedule_deb_builds
fi

if [ "$TARGET" = 'all' ] || [ "$TARGET" = 'win32' ]; then
	require_command zip
	schedule_win32_builds
fi

wait_for_all_jobs

printf '%s\n' "Artifacts generated in $ARTIFACT_ROOT"
