#!/usr/bin/env bash
# Configure + build the slicer core and the linked test binaries (GUI off).
# Run after chain_a.sh and chain_b.sh have completed.
set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

PREFIX=$ROOT/OrcaSlicer_dep/usr/local
REPO=$ROOT/../..                    # /home/user/OrcaSlicer (repo root)
BUILD=$REPO/build                   # /home/user/OrcaSlicer/build (gitignored)
# The test binaries are executed at build time (Catch2 test discovery) and at
# test time; make their shared libs (libpython, shared deps) resolvable.
export LD_LIBRARY_PATH="$PREFIX/lib:$PREFIX/libpython/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

for dep in zlib openssl curl libpng jpeg freetype expat boost tbb eigen m4 gmp mpfr cgal opencv assimp draco libnoise nlopt cereal openexr c-blosc openvdb occt stubs; do
    [ -f "$LOG/$dep.done" ] || { log "missing dependency: $dep (run chain_a.sh)"; exit 1; }
done
[ -f "$LOG/python.done" ] || { log "missing dependency: python (run chain_b.sh)"; exit 1; }

log "=== configure main ==="
"$CMAKE" -S "$REPO" -B "$BUILD" -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=ON -DSLIC3R_GUI=OFF -DSLIC3R_PCH=ON \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DPKG_CONFIG_EXECUTABLE=/home/user/fakebin/pkg-config \
    -DDBUS_INCLUDE_DIR="$PREFIX/include/dbus-1.0" \
    -DDBUS_ARCH_INCLUDE_DIR="$PREFIX/include/dbus-1.0" \
    -DDBUS_LIBRARIES="$PREFIX/lib/libdbus-1.a" \
    -DOPENGL_INCLUDE_DIR="$PREFIX/include" \
    -DOPENGL_gl_LIBRARY="$PREFIX/lib/libGL.a" \
    -Dglfw3_DIR="$PREFIX/lib/cmake/glfw3" \
    -DCMAKE_EXE_LINKER_FLAGS="-L$PREFIX/lib" \
    -DCMAKE_SHARED_LINKER_FLAGS="-L$PREFIX/lib" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O1 -DNDEBUG" \
    -DCMAKE_C_FLAGS_RELEASE="-O1 -DNDEBUG" \
    > "$LOG/main_configure.log" 2>&1 || { log "CONFIGURE FAILED main"; tail -30 "$LOG/main_configure.log"; exit 1; }

log "=== build libslic3r + tests ==="
"$CMAKE" --build "$BUILD" --target libslic3r fff_print_tests libslic3r_tests -j"$JOBS" \
    > "$LOG/main_build.log" 2>&1 || { log "BUILD FAILED main"; tail -40 "$LOG/main_build.log"; exit 1; }

log "main build complete"
