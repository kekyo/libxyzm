#!/bin/sh

set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARTIFACT_ROOT="$PROJECT_ROOT/artifacts"
PACKAGE_NAME=libxyzm

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

assert_file() {
	[ -f "$1" ] || fail "Missing expected file: $1"
}

assert_not_file() {
	[ ! -f "$1" ] || fail "Unexpected file: $1"
}

assert_contains() {
	target_path=$1
	expected_text=$2
	grep -F "$expected_text" "$target_path" >/dev/null 2>&1 || fail "Missing expected text in $target_path: $expected_text"
}

expected_elf_class() {
	case $1 in
		amd64 | arm64 | riscv64)
			printf '%s\n' 'ELF64'
			;;
		i686 | armv7l)
			printf '%s\n' 'ELF32'
			;;
		*)
			fail "Unsupported ELF class lookup: $1"
			;;
	esac
}

expected_elf_machine() {
	case $1 in
		amd64)
			printf '%s\n' 'Advanced Micro Devices X86-64'
			;;
		i686)
			printf '%s\n' 'Intel 80386'
			;;
		arm64)
			printf '%s\n' 'AArch64'
			;;
		armv7l)
			printf '%s\n' 'ARM'
			;;
		riscv64)
			printf '%s\n' 'RISC-V'
			;;
		*)
			fail "Unsupported ELF machine lookup: $1"
			;;
	esac
}

deb_arch_name() {
	case $1 in
		amd64)
			printf '%s\n' 'amd64'
			;;
		i686)
			printf '%s\n' 'i386'
			;;
		arm64)
			printf '%s\n' 'arm64'
			;;
		armv7l)
			printf '%s\n' 'armhf'
			;;
		riscv64)
			printf '%s\n' 'riscv64'
			;;
		*)
			fail "Unsupported Debian architecture lookup: $1"
			;;
	esac
}

deb_artifact_path() {
	distro=$1
	release=$2
	arch=$3
	deb_arch=$(deb_arch_name "$arch")

	printf '%s\n' "$ARTIFACT_ROOT/deb/${PACKAGE_NAME}-${VERSION}-${distro}-${release}-${deb_arch}.deb"
}

validate_deb_package() {
	package_path=$1
	expected_arch=$2
	expected_deb_arch=$(deb_arch_name "$expected_arch")
	tmp_dir=$(mktemp -d)

	assert_file "$package_path"
	[ "$(dpkg-deb -f "$package_path" Package)" = "$PACKAGE_NAME" ] || fail "Unexpected Package field in $package_path"
	[ "$(dpkg-deb -f "$package_path" Architecture)" = "$expected_deb_arch" ] || fail "Unexpected Architecture field in $package_path"
	[ "$(dpkg-deb -f "$package_path" Version)" = "$VERSION" ] || fail "Unexpected Version field in $package_path"
	[ "$(dpkg-deb -f "$package_path" Section)" = 'libs' ] || fail "Unexpected Section field in $package_path"
	dpkg-deb -f "$package_path" Depends >"$tmp_dir/depends.txt"
	assert_contains "$tmp_dir/depends.txt" 'liburing2'

	dpkg-deb -x "$package_path" "$tmp_dir/root"

	for header_path in "$PROJECT_ROOT"/include/libxyzm/*.h; do
		header_name=$(basename "$header_path")
		assert_file "$tmp_dir/root/usr/include/libxyzm/$header_name"
	done
	assert_not_file "$tmp_dir/root/usr/include/cardio.h"
	assert_file "$tmp_dir/root/usr/share/doc/$PACKAGE_NAME/LICENSE"
	assert_file "$tmp_dir/root/usr/share/doc/$PACKAGE_NAME/README.md"
	assert_file "$tmp_dir/root/usr/share/doc/$PACKAGE_NAME/README_ja.md"

	for library_name in libxyzm.so libxyzm_async.so libxyzm.a libxyzm_async.a; do
		library_path=$(find "$tmp_dir/root/usr/lib" -type f -name "$library_name" | head -n 1)
		[ -n "$library_path" ] || fail "Missing $library_name in $package_path"
	done
	assert_not_file "$(find "$tmp_dir/root/usr/lib" -type f -name '*.so.a' | head -n 1)"

	for library_name in libxyzm.so libxyzm_async.so; do
		library_path=$(find "$tmp_dir/root/usr/lib" -type f -name "$library_name" | head -n 1)
		readelf -h "$library_path" >"$tmp_dir/readelf-$library_name.txt"
		assert_contains "$tmp_dir/readelf-$library_name.txt" "$(expected_elf_class "$expected_arch")"
		assert_contains "$tmp_dir/readelf-$library_name.txt" "$(expected_elf_machine "$expected_arch")"
	done

	rm -rf "$tmp_dir"
}

validate_win32_package() {
	package_path=$1
	label=$2
	tmp_dir=$(mktemp -d)
	top_dir="$PACKAGE_NAME-$VERSION-win32-$label"

	assert_file "$package_path"
	unzip -q "$package_path" -d "$tmp_dir"

	for header_path in "$PROJECT_ROOT"/include/libxyzm/*.h; do
		header_name=$(basename "$header_path")
		assert_file "$tmp_dir/$top_dir/include/libxyzm/$header_name"
	done
	assert_not_file "$tmp_dir/$top_dir/include/cardio.h"
	assert_file "$tmp_dir/$top_dir/LICENSE"
	assert_file "$tmp_dir/$top_dir/README.md"
	assert_file "$tmp_dir/$top_dir/README_ja.md"
	assert_file "$tmp_dir/$top_dir/bin/libxyzm.dll"
	assert_file "$tmp_dir/$top_dir/bin/libxyzm_async.dll"
	assert_file "$tmp_dir/$top_dir/lib/libxyzm.dll.a"
	assert_file "$tmp_dir/$top_dir/lib/libxyzm_async.dll.a"
	assert_file "$tmp_dir/$top_dir/lib/libxyzm.a"
	assert_file "$tmp_dir/$top_dir/lib/libxyzm_async.a"

	for dll_name in libxyzm.dll libxyzm_async.dll; do
		file "$tmp_dir/$top_dir/bin/$dll_name" >"$tmp_dir/file-$dll_name.txt"
		case $label in
			x86)
				assert_contains "$tmp_dir/file-$dll_name.txt" 'PE32 executable (DLL)'
				assert_contains "$tmp_dir/file-$dll_name.txt" 'Intel 80386'
				;;
			x64)
				assert_contains "$tmp_dir/file-$dll_name.txt" 'PE32+ executable (DLL)'
				assert_contains "$tmp_dir/file-$dll_name.txt" 'x86-64'
				;;
			*)
				fail "Unsupported Win32 label: $label"
				;;
		esac
	done

	rm -rf "$tmp_dir"
}

cd "$PROJECT_ROOT"

prereq_help_output=$(./prereq.sh --help)
printf '%s\n' "$prereq_help_output" | grep -F -- '--force' >/dev/null 2>&1 || fail 'Missing --force option in prereq help output'

pack_help_output=$(./build_pack.sh --help)
printf '%s\n' "$pack_help_output" | grep -F -- '--jobs <count>' >/dev/null 2>&1 || fail 'Missing --jobs option in build_pack help output'

VERSION=${LIBXYZM_PACK_TEST_VERSION:-0.0.0-test}
PACK_BUILD_JOBS=${LIBXYZM_PACK_TEST_JOBS:-4}

actual_version=$(./build_pack.sh --version "$VERSION" --print-version)
[ "$actual_version" = "$VERSION" ] || fail "Unexpected explicit version: $actual_version"

if command -v screw-up >/dev/null 2>&1; then
	expected_default_version=$(printf '%s\n' '{version}' | screw-up format | tr -d '\r')
	actual_default_version=$(./build_pack.sh --print-version)
	[ "$actual_default_version" = "$expected_default_version" ] || fail "Unexpected default version: $actual_default_version"
else
	if ./build_pack.sh --print-version >/tmp/libxyzm-pack-version.err 2>&1; then
		fail 'build_pack.sh --print-version unexpectedly succeeded without screw-up'
	fi
	assert_contains /tmp/libxyzm-pack-version.err 'screw-up'
fi

if ./build_pack.sh --version "$VERSION" --target invalid >/dev/null 2>&1; then
	fail 'Invalid build_pack target unexpectedly succeeded'
fi
if ./build_pack.sh --version "$VERSION" --jobs 0 --print-version >/dev/null 2>&1; then
	fail 'build_pack --jobs 0 unexpectedly succeeded'
fi
if ./prereq.sh --jobs 0 >/dev/null 2>&1; then
	fail 'prereq --jobs 0 unexpectedly succeeded'
fi

./build_pack.sh --version "$VERSION" --jobs "$PACK_BUILD_JOBS"

deb_count=$(find "$ARTIFACT_ROOT/deb" -type f -name '*.deb' | wc -l | tr -d ' ')
zip_count=$(find "$ARTIFACT_ROOT/win32" -type f -name '*.zip' | wc -l | tr -d ' ')
[ "$deb_count" = '17' ] || fail "Unexpected deb artifact count: $deb_count"
[ "$zip_count" = '2' ] || fail "Unexpected zip artifact count: $zip_count"

while IFS=' ' read -r distro release architectures; do
	[ -n "$distro" ] || continue
	for arch in $architectures; do
		validate_deb_package "$(deb_artifact_path "$distro" "$release" "$arch")" "$arch"
	done
done <<'EOF'
debian bookworm amd64 i686 arm64 armv7l
debian trixie amd64 i686 arm64 armv7l riscv64
ubuntu 22.04 amd64 arm64 armv7l riscv64
ubuntu 24.04 amd64 arm64 armv7l riscv64
EOF

validate_win32_package "$ARTIFACT_ROOT/win32/${PACKAGE_NAME}-$VERSION-win32-x86.zip" x86
validate_win32_package "$ARTIFACT_ROOT/win32/${PACKAGE_NAME}-$VERSION-win32-x64.zip" x64
