#!/usr/bin/env bash
# Shared environment + helpers for the sandbox dependency build.
# Sourced by chain_a.sh / chain_b.sh. Idempotent via .done markers in $LOG.

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../build" && pwd)"
PREFIX=$ROOT/OrcaSlicer_dep/usr/local
SRC=$ROOT/src
BLD=$ROOT/b
LOG=$ROOT/log
DL=$ROOT/src_dl
CMAKE=$ROOT/venv3/bin/cmake
NINJA=$ROOT/venv3/bin/ninja
JOBS=${JOBS:-2}
export PATH="$ROOT/venv3/bin:/home/user/fakebin:$PREFIX/bin:$PATH"
export CC=gcc CXX=g++
export CFLAGS="-O2 -fno-omit-frame-pointer"
export CXXFLAGS="-O2 -fno-omit-frame-pointer"
mkdir -p "$PREFIX" "$DL" "$SRC" "$BLD" "$LOG" /home/user/fakebin

log() { echo "[$(date +%H:%M:%S)] $*"; }

dl() { # dl <file-name> <url>
    local name=$1 url=$2
    if [ ! -s "$DL/$name" ]; then
        log "downloading $name"
        curl -sL --retry 3 --max-time 1800 -o "$DL/$name" "$url" || { log "DOWNLOAD FAILED: $name"; return 1; }
    fi
    [ -s "$DL/$name" ] || { log "EMPTY DOWNLOAD: $name"; return 1; }
    return 0
}

ex() { # ex <tarball> <destdir> ; prints top-level dir (read from the tarball itself)
    local tb=$1 dest=$2
    mkdir -p "$dest"
    tar xf "$DL/$tb" -C "$dest" || return 1
    local top
    top=$(tar tzf "$DL/$tb" 2>/dev/null | head -1 | cut -d/ -f1)
    [ -n "$top" ] || { ls -d "$dest"/*/ 2>/dev/null | head -1; return 0; }
    echo "$dest/$top/"
}

done_marker() { touch "$LOG/$1.done"; }
is_done() { [ -f "$LOG/$1.done" ]; }

wait_file() { # wait_file <path> [timeout_sec]
    local t=${2:-7200}
    while [ ! -e "$1" ]; do
        sleep 10
        t=$((t - 10))
        [ $t -le 0 ] && { log "TIMEOUT waiting for $1"; return 1; }
    done
    return 0
}

cmake_build() { # cmake_build <name> <srcdir> <builddir> [extra cmake args...]
    local name=$1 src=$2 bld=$3; shift 3
    log "=== $name ==="
    rm -rf "$bld"
    "$CMAKE" -S "$src" -B "$bld" -GNinja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_PREFIX_PATH="$PREFIX" -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
        "$@" >>"$LOG/$name.log" 2>&1 || { log "CONFIGURE FAILED: $name"; return 1; }
    "$CMAKE" --build "$bld" -j"$JOBS" >>"$LOG/$name.log" 2>&1 || { log "BUILD FAILED: $name"; return 1; }
    "$CMAKE" --install "$bld" >>"$LOG/$name.log" 2>&1 || { log "INSTALL FAILED: $name"; return 1; }
    rm -rf "$bld"
    done_marker "$name"
}
