#!/bin/sh

set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEFAULT_PARALLEL_JOB_CAP=4

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

print_usage() {
	cat <<'EOF'
Usage: ./prereq.sh [options]

Options:
  --distro <list>   Comma-separated distro filter.
  --release <list>  Comma-separated release filter.
  --arch <list>     Comma-separated architecture filter.
  --jobs <count>    Maximum concurrent image builds. Defaults to auto (up to 4).
  --force           Rebuild images even when a tagged image already exists.
  --help            Show this help.
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

base_image_for_target() {
	distro=$1
	release=$2

	printf 'docker.io/library/%s:%s\n' "$distro" "$release"
}

pack_image_for_target() {
	distro=$1
	release=$2
	arch=$3

	printf 'localhost/libxyzm-pack-%s-%s-%s:latest\n' "$distro" "$release" "$arch"
}

build_image() {
	distro=$1
	release=$2
	arch=$3
	platform=$4
	base_image=$(base_image_for_target "$distro" "$release")
	pack_image=$(pack_image_for_target "$distro" "$release" "$arch")
	work_dir="$TMP_ROOT/$distro/$release/$arch"
	containerfile="$work_dir/Containerfile"

	if [ "$FORCE" = 'false' ] && podman image exists "$pack_image"; then
		printf '%s\n' "[prereq] exists $pack_image"
		return 0
	fi

	printf '%s\n' "[prereq] build $pack_image ($platform)"
	rm -rf "$work_dir"
	mkdir -p "$work_dir"

	cat >"$containerfile" <<'EOF'
ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      dpkg-dev \
      liburing-dev \
      pkg-config && \
    rm -rf /var/lib/apt/lists/*
EOF

	build_args=''
	if [ "$FORCE" = 'true' ]; then
		build_args='--no-cache'
	fi

	# shellcheck disable=SC2086
	podman build $build_args \
		--platform "$platform" \
		--pull=missing \
		--build-arg "BASE_IMAGE=$base_image" \
		-t "$pack_image" \
		-f "$containerfile" \
		"$work_dir"
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

	[ "$JOB_FAILURE" -eq 0 ] || fail 'One or more prerequisite image builds failed'

	"$@" &
	ACTIVE_JOB_PIDS="${ACTIVE_JOB_PIDS}${ACTIVE_JOB_PIDS:+ }$!"
	ACTIVE_JOB_COUNT=$((ACTIVE_JOB_COUNT + 1))
}

wait_for_all_jobs() {
	while [ "$ACTIVE_JOB_COUNT" -gt 0 ]; do
		wait_for_oldest_job
	done

	[ "$JOB_FAILURE" -eq 0 ] || fail 'One or more prerequisite image builds failed'
}

schedule_image_builds() {
	while IFS=' ' read -r distro release arch platform; do
		[ -n "$distro" ] || continue
		matches_filter "$DISTRO_FILTER" "$distro" || continue
		matches_filter "$RELEASE_FILTER" "$release" || continue
		matches_filter "$ARCH_FILTER" "$arch" || continue
		run_parallel_job build_image "$distro" "$release" "$arch" "$platform"
	done <<EOF
$LINUX_MATRIX
EOF
}

DISTRO_FILTER=''
RELEASE_FILTER=''
ARCH_FILTER=''
PARALLEL_JOBS=''
FORCE='false'

while [ "$#" -gt 0 ]; do
	case $1 in
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
		--force)
			FORCE='true'
			shift
			;;
		--help)
			print_usage
			exit 0
			;;
		*)
			fail "Unknown argument: $1"
			;;
	esac
done

if [ -n "$PARALLEL_JOBS" ]; then
	validate_positive_integer 'Parallel job count' "$PARALLEL_JOBS"
else
	PARALLEL_JOBS=$(min_int "$(detect_processor_count)" "$DEFAULT_PARALLEL_JOB_CAP")
fi

require_command podman

RUN_ID="prereq-$(date +%Y%m%d%H%M%S)-$$"
TMP_ROOT="$PROJECT_ROOT/artifacts/.tmp/$RUN_ID"
ACTIVE_JOB_PIDS=''
ACTIVE_JOB_COUNT=0
JOB_FAILURE=0

mkdir -p "$TMP_ROOT"

printf '%s\n' "Using up to $PARALLEL_JOBS prerequisite image jobs"
schedule_image_builds
wait_for_all_jobs

printf '%s\n' 'Prerequisite images are ready.'
