#!/usr/bin/env bash
# Chain B: CPython 3.12.13 (matches deps/python3), installed into
# <prefix>/libpython — the top-level CMakeLists forces Python3_ROOT_DIR there
# and requires find_package(Python3 3.12.13 EXACT REQUIRED COMPONENTS
# Interpreter Development.Embed).
set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

if ! is_done python; then
    log "=== python ==="
    dl python.tar.gz "https://codeload.github.com/python/cpython/tar.gz/refs/tags/v3.12.13" || exit 1
    S=$(ex python.tar.gz "$SRC/python") || exit 1
    wait_file "$LOG/expat.done" || exit 1
    ( cd "$S" && ./configure --prefix="$PREFIX/libpython" --enable-shared \
        --with-ensurepip=no --disable-test-modules --with-system-expat \
        CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib -Wl,-rpath,$PREFIX/lib" ) >>"$LOG/python.log" 2>&1 \
        || { log "CONFIGURE FAILED python"; exit 1; }
    # The sandbox has no bzip2/xz/sqlite3/libffi/readline/gdbm/tk/uuid/soundcard
    # headers and no package mirror; disable those extension modules (nothing in
    # the slicer build imports them — pybind11 needs only the core interpreter).
    cat >> "$S/Modules/Setup.local" << 'EOS'
*disabled*
_blake2
_bz2
_lzma
_sqlite3
_ctypes
_tkinter
_gdbm
_dbm
readline
nis
ossaudiodev
spwd
_uuid
EOS
    ( cd "$S" && make -j"$JOBS" && make install ) >>"$LOG/python.log" 2>&1 || { log "BUILD FAILED python"; exit 1; }
    done_marker python
fi

log "chain_b complete"
