#!/usr/bin/env bash
# =============================================================================
# build.sh — Cross-compile Civilization V Hybrid LuaJIT 2.1 + Native C DLL
#            (lua51_Win32.dll) on Linux using MinGW-w64 (i686-w64-mingw32-gcc)
#
# Usage:
#   ./build.sh              # Build dist/lua51_Win32.dll & dist/civ5_bench_harness.exe
#   ./build.sh --amalg      # Amalgamated single-TU build (maximum inlining)
#   ./build.sh --package    # Also package release zip into dist/civ5turbo-win32.zip
#   ./build.sh --clean      # Clean all intermediate build artifacts
#
# Prerequisites (Ubuntu / Debian / GitHub Actions ubuntu-latest):
#   sudo apt-get update && sudo apt-get install -y \
#     build-essential gcc-multilib gcc-mingw-w64-i686 binutils-mingw-w64-i686 zip
# =============================================================================

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LUAJIT_SRC="${ROOT_DIR}/luajit/src"
BENCH_DIR="${ROOT_DIR}/benchmark"
DIST_DIR="${ROOT_DIR}/dist"

CROSS_PREFIX="${CROSS_PREFIX:-i686-w64-mingw32-}"
HOST_CC="${HOST_CC:-gcc -m32}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

USE_AMALG=0
DO_PACKAGE=0
DO_CLEAN_ONLY=0

for arg in "$@"; do
  case "$arg" in
    --amalg)   USE_AMALG=1 ;;
    --package) DO_PACKAGE=1 ;;
    --clean)   DO_CLEAN_ONLY=1 ;;
    -h|--help)
      sed -n '2,15p' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *)
      echo "Unknown option: $arg" >&2
      exit 1
      ;;
  esac
done

clean_luajit() {
  echo "[*] Cleaning LuaJIT intermediate build artifacts..."
  make -C "${LUAJIT_SRC}" clean TARGET_SYS=Windows >/dev/null 2>&1 || true
  rm -f "${LUAJIT_SRC}"/*.o "${LUAJIT_SRC}"/*.obj "${LUAJIT_SRC}"/*.dll \
        "${LUAJIT_SRC}"/*.exe "${LUAJIT_SRC}"/*.lib "${LUAJIT_SRC}"/*.exp \
        "${LUAJIT_SRC}"/host/*.o \
        "${LUAJIT_SRC}"/host/minilua "${LUAJIT_SRC}"/host/minilua.exe \
        "${LUAJIT_SRC}"/host/buildvm "${LUAJIT_SRC}"/host/buildvm.exe \
        "${LUAJIT_SRC}"/host/buildvm_arch.h
}

if [[ "${DO_CLEAN_ONLY}" -eq 1 ]]; then
  clean_luajit
  echo "[+] Clean complete."
  exit 0
fi

# Verify required cross-compilation toolchain
if ! command -v "${CROSS_PREFIX}gcc" >/dev/null 2>&1; then
  echo "[-] Error: ${CROSS_PREFIX}gcc not found." >&2
  echo "    Install on Ubuntu/Debian with:" >&2
  echo "      sudo apt-get install -y gcc-multilib gcc-mingw-w64-i686 binutils-mingw-w64-i686" >&2
  exit 1
fi

mkdir -p "${DIST_DIR}"
clean_luajit

echo "[*] Building 32-bit Windows LuaJIT 2.1 + Civ 5 Native C Accelerator..."
echo "    HOST_CC      = ${HOST_CC}"
echo "    CROSS_PREFIX = ${CROSS_PREFIX}"
echo "    AMALGAMATED  = ${USE_AMALG}"

MAKE_TARGET="all"
if [[ "${USE_AMALG}" -eq 1 ]]; then
  MAKE_TARGET="amalg"
fi

make -C "${LUAJIT_SRC}" -j"${JOBS}" "${MAKE_TARGET}" \
  HOST_CC="${HOST_CC}" \
  CROSS="${CROSS_PREFIX}" \
  TARGET_SYS=Windows \
  BUILDMODE=dynamic \
  TARGET_XCFLAGS="-DLUA_BUILD_AS_DLL -O2 -march=i686 -msse2 -mfpmath=sse" \
  TARGET_STRIP="${CROSS_PREFIX}strip --strip-unneeded"

if [[ ! -f "${LUAJIT_SRC}/lua51.dll" ]]; then
  echo "[-] Error: ${LUAJIT_SRC}/lua51.dll was not produced by make." >&2
  exit 1
fi

cp -f "${LUAJIT_SRC}/lua51.dll" "${DIST_DIR}/lua51_Win32.dll"
echo "[+] Built ${DIST_DIR}/lua51_Win32.dll ($(wc -c < "${DIST_DIR}/lua51_Win32.dll") bytes)"

# Verify that critical Lua 5.1.4 + LuaJIT exports exist in the PE export table
if command -v "${CROSS_PREFIX}objdump" >/dev/null 2>&1; then
  echo "[*] Verifying PE export table in lua51_Win32.dll..."
  EXPORTS_OUT="$("${CROSS_PREFIX}objdump" -p "${DIST_DIR}/lua51_Win32.dll")"
  for sym in luaL_newstate lua_newstate lua_pcall lua_setlevel luaL_loadbuffer luaJIT_profile_start; do
    if ! grep -q "${sym}" <<<"${EXPORTS_OUT}"; then
      echo "[-] Error: Missing required exported symbol '${sym}' in lua51_Win32.dll!" >&2
      exit 1
    fi
  done
  echo "[+] Verified required Lua 5.1.4 ABI exports (including lua_setlevel)."
fi

# Cross-compile standalone equivalence & benchmark harness
echo "[*] Cross-compiling standalone benchmark harness (civ5_bench_harness.exe)..."
"${CROSS_PREFIX}gcc" -O2 -Wall -Wextra -Wno-unused-parameter \
  "${BENCH_DIR}/civ5_bench_harness.c" \
  -o "${DIST_DIR}/civ5_bench_harness.exe"
"${CROSS_PREFIX}strip" --strip-unneeded "${DIST_DIR}/civ5_bench_harness.exe"
cp -f "${DIST_DIR}/civ5_bench_harness.exe" "${BENCH_DIR}/civ5_bench_harness.exe"

# Clean intermediate objects from luajit/src so the source tree remains clean
clean_luajit

# Generate SHA-256 checksums
(
  cd "${DIST_DIR}"
  sha256sum lua51_Win32.dll civ5_bench_harness.exe > SHA256SUMS.txt
)
echo "[+] SHA-256 checksums:"
cat "${DIST_DIR}/SHA256SUMS.txt"

# Optional release archive packaging
if [[ "${DO_PACKAGE}" -eq 1 ]]; then
  PKG_NAME="civ5turbo-win32.zip"
  STAGE_DIR="$(mktemp -d)"
  trap 'rm -rf "${STAGE_DIR}"' EXIT

  mkdir -p "${STAGE_DIR}/civ5turbo/scripts"
  cp -f "${DIST_DIR}/lua51_Win32.dll"          "${STAGE_DIR}/civ5turbo/"
  cp -f "${DIST_DIR}/civ5_bench_harness.exe"   "${STAGE_DIR}/civ5turbo/"
  cp -f "${DIST_DIR}/SHA256SUMS.txt"           "${STAGE_DIR}/civ5turbo/"
  cp -f "${ROOT_DIR}/README.md"                "${STAGE_DIR}/civ5turbo/"
  cp -f "${ROOT_DIR}/LICENSE"                  "${STAGE_DIR}/civ5turbo/"
  cp -f "${ROOT_DIR}/install_playtest.ps1"     "${STAGE_DIR}/civ5turbo/scripts/"
  cp -f "${ROOT_DIR}/summarize_profile.ps1"    "${STAGE_DIR}/civ5turbo/scripts/"

  (
    cd "${STAGE_DIR}"
    zip -r -9 "${DIST_DIR}/${PKG_NAME}" civ5turbo >/dev/null
  )
  echo "[+] Packaged release archive: ${DIST_DIR}/${PKG_NAME}"
fi

echo "[+] Build completed successfully."
