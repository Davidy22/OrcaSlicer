#!/usr/bin/env bash
# Chain A: stubs + the C/C++ third-party dependencies of libslic3r (GUI off).
set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

###############################################################################
# Stubs (safe only with SLIC3R_GUI=OFF; see README.md)
if ! is_done stubs; then
    log "=== stubs ==="
    mkdir -p /home/user/fakebin "$PREFIX/include/GL" "$PREFIX/lib/cmake/glfw3" \
             "$PREFIX/include/dbus-1.0/dbus" "$PREFIX/include/dbus" "$PREFIX/lib"
    # fake pkg-config (the real one is absent; the main build only probes with it)
    cat > /home/user/fakebin/pkg-config << 'EOS'
#!/usr/bin/env bash
case "$1" in
  --version) echo "0.29.2" ;;
  --exists) exit 0 ;;
  --cflags|--libs) echo "" ;;
  *) echo "" ;;
esac
EOS
    chmod +x /home/user/fakebin/pkg-config
    # OpenGL stub
    printf '#ifndef SANDBOX_GL_H\n#define SANDBOX_GL_H\ntypedef unsigned int GLenum;\ntypedef unsigned int GLuint;\ntypedef int GLint;\ntypedef int GLsizei;\ntypedef unsigned char GLboolean;\ntypedef float GLfloat;\ntypedef void GLvoid;\n#endif\n' > "$PREFIX/include/GL/gl.h"
    ar rcs "$PREFIX/lib/libGL.a"
    # glfw3 stub config (INTERFACE target)
    cat > "$PREFIX/lib/cmake/glfw3/glfw3Config.cmake" << 'EOS'
if(NOT TARGET glfw)
    add_library(glfw INTERFACE)
endif()
set(glfw3_FOUND TRUE)
EOS
    # DBus stub headers + empty archive
    printf '#ifndef SANDBOX_DBUS_H\n#define SANDBOX_DBUS_H\ntypedef struct DBusConnection DBusConnection;\ntypedef struct DBusMessage DBusMessage;\ntypedef struct DBusError DBusError;\ntypedef struct DBusWatch DBusWatch;\ntypedef struct DBusTimeout DBusTimeout;\n#endif\n' > "$PREFIX/include/dbus-1.0/dbus/dbus.h"
    cp "$PREFIX/include/dbus-1.0/dbus/dbus.h" "$PREFIX/include/dbus/dbus.h"
    printf '#ifndef SANDBOX_DBUS_ARCH_DEPS_H\n#define SANDBOX_DBUS_ARCH_DEPS_H\n#define DBUS_ARCHITECTURE "x86_64"\n#endif\n' > "$PREFIX/include/dbus-1.0/dbus/dbus-arch-deps.h"
    cp "$PREFIX/include/dbus-1.0/dbus/dbus-arch-deps.h" "$PREFIX/include/dbus/dbus-arch-deps.h"
    ar rcs "$PREFIX/lib/libdbus-1.a"
    # fontconfig stub (libslic3r links bare -lfontconfig)
    ar rcs "$PREFIX/lib/libfontconfig.a"
    # real libudev.h (for deps_src/hidapi compile) from the systemd repo via gh api
    gh api "repos/systemd/systemd/contents/src/libudev/libudev.h?ref=v255.5" --jq .content 2>/dev/null \
        | tr -d '\n' | base64 -d > "$PREFIX/include/libudev.h" 2>/dev/null
    [ -s "$PREFIX/include/libudev.h" ] || printf '#ifndef SANDBOX_LIBUDEV_H\n#define SANDBOX_LIBUDEV_H\nstruct udev;\nstruct udev_device;\nstruct udev_enumerate;\nstruct udev_list_entry;\nstruct udev_monitor;\n#endif\n' > "$PREFIX/include/libudev.h"
    done_marker stubs
fi

###############################################################################
# zlib 1.2.13
if ! is_done zlib; then
    dl zlib.tar.gz "https://codeload.github.com/madler/zlib/tar.gz/refs/tags/v1.2.13" || exit 1
    S=$(ex zlib.tar.gz "$SRC/zlib") || exit 1
    cmake_build zlib "$S" "$BLD/zlib" || exit 1
fi

# OpenSSL 1.1.1w (no perl-doc target: this version has no no-docs option)
if ! is_done openssl; then
    log "=== openssl ==="
    dl openssl.tar.gz "https://codeload.github.com/openssl/openssl/tar.gz/refs/tags/OpenSSL_1_1_1w" || exit 1
    S=$(ex openssl.tar.gz "$SRC/openssl") || exit 1
    ( cd "$S" && ./Configure linux-x86_64 no-shared no-tests --prefix="$PREFIX" --openssldir="$PREFIX/ssl" ) >>"$LOG/openssl.log" 2>&1 \
        || { log "CONFIGURE FAILED openssl"; exit 1; }
    ( cd "$S" && make -j"$JOBS" && make install_sw ) >>"$LOG/openssl.log" 2>&1 || { log "BUILD FAILED openssl"; exit 1; }
    done_marker openssl
fi

# curl 7.75.0 (static, OpenSSL + zlib)
if ! is_done curl; then
    dl curl.tar.gz "https://codeload.github.com/curl/curl/tar.gz/refs/tags/curl-7_75_0" || exit 1
    S=$(ex curl.tar.gz "$SRC/curl") || exit 1
    wait_file "$LOG/openssl.done" || exit 1
    cmake_build curl "$S" "$BLD/curl" \
        -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DCURL_USE_OPENSSL=ON \
        -DCURL_USE_LIBSSH2=OFF -DCURL_ZLIB=ON -DCURL_DISABLE_LDAP=ON \
        -DCMAKE_USE_LIBSSH2=OFF || exit 1
fi

# libpng 1.6.35
if ! is_done libpng; then
    dl libpng.tar.gz "https://codeload.github.com/pnggroup/libpng/tar.gz/refs/tags/v1.6.35" || exit 1
    S=$(ex libpng.tar.gz "$SRC/libpng") || exit 1
    wait_file "$LOG/zlib.done" || exit 1
    cmake_build libpng "$S" "$BLD/libpng" -DPNG_TESTS=OFF -DPNG_SHARED=OFF -DPNG_TOOLS=OFF || exit 1
fi

# libjpeg-turbo 3.0.1 (no SIMD: no nasm in the sandbox)
if ! is_done jpeg; then
    dl jpeg.tar.gz "https://codeload.github.com/libjpeg-turbo/libjpeg-turbo/tar.gz/refs/tags/3.0.1" || exit 1
    S=$(ex jpeg.tar.gz "$SRC/jpeg") || exit 1
    cmake_build jpeg "$S" "$BLD/jpeg" -DENABLE_SHARED=OFF -DWITH_SIMD=OFF || exit 1
fi

# freetype 2.12.1
if ! is_done freetype; then
    dl freetype.tar.gz "https://codeload.github.com/freetype/freetype/tar.gz/refs/tags/VER-2-12-1" || exit 1
    S=$(ex freetype.tar.gz "$SRC/freetype") || exit 1
    wait_file "$LOG/libpng.done" || exit 1
    cmake_build freetype "$S" "$BLD/freetype" \
        -DFT_DISABLE_ZLIB=OFF -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=OFF \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON || exit 1
fi

# expat 2.6.2 (real build: the main CMake does a top-level find_package(EXPAT);
# the tarball's CMakeLists.txt lives in the expat/ subdirectory)
if ! is_done expat; then
    dl expat.tar.gz "https://codeload.github.com/libexpat/libexpat/tar.gz/refs/tags/R_2_6_2" || exit 1
    S=$(ex expat.tar.gz "$SRC/expat") || exit 1
    cmake_build expat "$S/expat" "$BLD/expat" -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_EXAMPLES=OFF \
        -DEXPAT_BUILD_FUZZERS=OFF -DEXPAT_BUILD_DOCS=OFF -DEXPAT_SHARED_LIBS=OFF || exit 1
fi

# Boost 1.84.0 (reduced library set, native CMake so BoostConfig.cmake exists
# for CMP0167 NEW; ICU off)
if ! is_done boost; then
    log "=== boost ==="
    if [ ! -d "$SRC/boost/.git" ]; then
        rm -rf "$SRC/boost"
        git clone --depth 1 --recursive --shallow-submodules --branch boost-1.84.0 \
            https://github.com/boostorg/boost.git "$SRC/boost" >>"$LOG/boost.log" 2>&1 || { log "CLONE FAILED boost"; exit 1; }
    fi
    cmake_build boost "$SRC/boost" "$BLD/boost" \
        -DBOOST_INCLUDE_LIBRARIES="system;filesystem;thread;log;locale;regex;chrono;atomic;date_time;iostreams;program_options;nowide" \
        -DBOOST_EXCLUDE_LIBRARIES="contract;fiber;numpy;stacktrace;wave;test" \
        -DBOOST_LOCALE_ENABLE_ICU=OFF -DBUILD_TESTING=OFF || exit 1
fi

# oneTBB 2021.5.0 (static; tbbbind needs hwloc which is absent -> patched out)
if ! is_done tbb; then
    dl tbb.tar.gz "https://codeload.github.com/oneapi-src/oneTBB/tar.gz/refs/tags/v2021.5.0" || exit 1
    S=$(ex tbb.tar.gz "$SRC/tbb") || exit 1
    sed -i 's/^add_subdirectory(tbbbind)/# add_subdirectory(tbbbind)/' "$S/CMakeLists.txt" 2>/dev/null
    cmake_build tbb "$S" "$BLD/tbb" -DTBB_TEST=OFF -DTBB_STRICT=OFF || exit 1
fi

# Eigen 5.0.1 (eigen-mirror; gitlab is unreachable)
if ! is_done eigen; then
    dl eigen.tar.gz "https://codeload.github.com/eigen-mirror/eigen/tar.gz/refs/tags/5.0.1" || exit 1
    S=$(ex eigen.tar.gz "$SRC/eigen") || exit 1
    cmake_build eigen "$S" "$BLD/eigen" -DBUILD_TESTING=OFF -DEIGEN_BUILD_DOC=OFF || exit 1
fi

# GNU m4 1.4.19 (GMP's configure requires m4; no autoconf/m4 in the sandbox and
# GNU git mirrors ship no generated configure, so take the binary from the
# cmeel-m4 manylinux wheel on PyPI)
if ! is_done m4; then
    log "=== m4 ==="
    dl cmeel_m4.whl "https://files.pythonhosted.org/packages/py3/c/cmeel-m4/cmeel_m4-1.4.19.1-0-py3-none-manylinux_2_28_x86_64.whl" || exit 1
    mkdir -p "$ROOT/m4wheel"
    ( cd "$ROOT/m4wheel" && python3 -m zipfile -e "$DL/cmeel_m4.whl" . ) >>"$LOG/m4.log" 2>&1 || { log "WHEEL FAILED m4"; exit 1; }
    mkdir -p "$PREFIX/bin"
    cp "$ROOT/m4wheel/cmeel.prefix/bin/m4" "$PREFIX/bin/m4" && chmod +x "$PREFIX/bin/m4"
    "$PREFIX/bin/m4" --version >>"$LOG/m4.log" 2>&1 || { log "M4 FAILED"; exit 1; }
    done_marker m4
fi

# GMP 6.2.1 (alisw release mirror: configure + pre-generated mpn sources are
# shipped, but configure still requires m4 -> provided above)
if ! is_done gmp; then
    log "=== gmp ==="
    dl gmp-alisw.tar.gz "https://codeload.github.com/alisw/GMP/tar.gz/refs/tags/v6.2.1" || exit 1
    S=$(ex gmp-alisw.tar.gz "$SRC/gmp") || exit 1
    wait_file "$LOG/m4.done" || exit 1
    ( cd "$S" && ./configure --prefix="$PREFIX" --disable-shared --enable-static ) >>"$LOG/gmp.log" 2>&1 || { log "CONFIGURE FAILED gmp"; exit 1; }
    ( cd "$S" && make -j"$JOBS" && make install ) >>"$LOG/gmp.log" 2>&1 || { log "BUILD FAILED gmp"; exit 1; }
    done_marker gmp
fi

# MPFR 4.0.2 (vendored release tree inside stegos/gmp-mpfr-sys; MPFR only needs
# m4 in maintainer mode)
if ! is_done mpfr; then
    log "=== mpfr ==="
    wait_file "$LOG/gmp.done" || exit 1
    dl gmpmpfr.tar.gz "https://codeload.github.com/stegos/gmp-mpfr-sys/tar.gz/refs/heads/master" || exit 1
    S=$(ex gmpmpfr.tar.gz "$SRC/gmpmpfr") || exit 1
    M="$S/mpfr-4.0.2-p1-c"
    [ -d "$M" ] || { log "mpfr source missing"; exit 1; }
    ( cd "$M" && ./configure --prefix="$PREFIX" --disable-shared --enable-static --with-gmp="$PREFIX" ) >>"$LOG/mpfr.log" 2>&1 || { log "CONFIGURE FAILED mpfr"; exit 1; }
    ( cd "$M" && make -j"$JOBS" && make install ) >>"$LOG/mpfr.log" 2>&1 || { log "BUILD FAILED mpfr"; exit 1; }
    done_marker mpfr
fi

# CGAL 5.6.3
if ! is_done cgal; then
    dl cgal.tar.gz "https://codeload.github.com/CGAL/cgal/tar.gz/refs/tags/v5.6.3" || exit 1
    S=$(ex cgal.tar.gz "$SRC/cgal") || exit 1
    wait_file "$LOG/gmp.done" || exit 1
    wait_file "$LOG/mpfr.done" || exit 1
    wait_file "$LOG/boost.done" || exit 1
    cmake_build cgal "$S" "$BLD/cgal" -DBUILD_SHARED_LIBS=OFF -DCGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE=ON || exit 1
fi

# OpenCV 4.6.0 (reduced module set; no world amalgamation because it needs
# highgui — an opencv_world INTERFACE target is synthesized after install)
if ! is_done opencv; then
    dl opencv.tar.gz "https://codeload.github.com/opencv/opencv/tar.gz/refs/tags/4.6.0" || exit 1
    S=$(ex opencv.tar.gz "$SRC/opencv") || exit 1
    cmake_build opencv "$S" "$BLD/opencv" \
        -DBUILD_LIST=core,imgproc,imgcodecs -DBUILD_opencv_world=OFF \
        -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_opencv_apps=OFF \
        -DBUILD_JAVA=OFF -DBUILD_opencv_python2=OFF -DBUILD_opencv_python3=OFF -DBUILD_opencv_python_tests=OFF \
        -DBUILD_JPEG=ON -DBUILD_PNG=ON -DBUILD_ZLIB=ON -DBUILD_TIFF=OFF -DBUILD_WEBP=OFF \
        -DBUILD_OPENJPEG=OFF -DBUILD_JASPER=OFF -DBUILD_OPENEXR=OFF -DBUILD_PROTOBUF=OFF \
        -DWITH_JPEG=ON -DWITH_PNG=ON -DWITH_TIFF=OFF -DWITH_WEBP=OFF -DWITH_OPENEXR=OFF \
        -DWITH_OPENJPEG=OFF -DWITH_JASPER=OFF -DWITH_FFMPEG=OFF -DWITH_GTK=OFF -DWITH_GTK_2_X=OFF \
        -DWITH_VTK=OFF -DWITH_EIGEN=OFF -DWITH_TBB=OFF -DWITH_OPENCL=OFF -DWITH_ITT=OFF -DWITH_IPP=OFF \
        -DWITH_CUDA=OFF -DWITH_LAPACK=OFF -DWITH_QUIRC=OFF -DWITH_ADE=OFF -DWITH_GSTREAMER=OFF \
        -DWITH_1394=OFF -DWITH_MFX=OFF -DWITH_OPENCLAMDBLAS=OFF -DWITH_OPENCLAMDFFT=OFF \
        -DWITH_OPENVINO=OFF -DWITH_INF_ENGINE=OFF -DWITH_NVCUVID=OFF -DWITH_QT=OFF -DWITH_GPHOTO2=OFF \
        -DWITH_MATLAB=OFF -DWITH_VA=OFF -DWITH_VA_INTEL=OFF -DWITH_HALIDE=OFF \
        -DWITH_MSMF=OFF -DWITH_DSHOW=OFF -DWITH_AVFOUNDATION=OFF \
        -DOPENCV_ENABLE_NONFREE=OFF -DENABLE_PRECOMPILED_HEADERS=OFF -DOPENCV_GENERATE_SETUPVARS=OFF \
        -DINSTALL_TESTS=OFF -DINSTALL_C_EXAMPLES=OFF -DINSTALL_PYTHON_EXAMPLES=OFF || exit 1
    cat >> "$PREFIX/lib/cmake/opencv4/OpenCVConfig.cmake" << 'EOC'
if(NOT TARGET opencv_world)
    add_library(opencv_world INTERFACE)
    target_link_libraries(opencv_world INTERFACE opencv_core opencv_imgproc opencv_imgcodecs)
    target_include_directories(opencv_world INTERFACE ${OpenCV_INCLUDE_DIRS})
endif()
EOC
fi

# assimp 5.4.3 (GLTF/OBJ/FBX importers on, no export, bundled zlib)
if ! is_done assimp; then
    dl assimp.tar.gz "https://codeload.github.com/assimp/assimp/tar.gz/refs/tags/v5.4.3" || exit 1
    S=$(ex assimp.tar.gz "$SRC/assimp") || exit 1
    cmake_build assimp "$S" "$BLD/assimp" \
        -DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_ASSIMP_TOOLS=OFF -DASSIMP_NO_EXPORT=ON \
        -DASSIMP_BUILD_ZLIB=ON -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF \
        -DASSIMP_BUILD_GLTF_IMPORTER=ON -DASSIMP_BUILD_OBJ_IMPORTER=ON -DASSIMP_BUILD_FBX_IMPORTER=ON \
        -DASSIMP_BUILD_STL_IMPORTER=ON -DASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT=OFF || exit 1
fi

# draco 1.5.7 (+ hand-written draco-config.cmake defining draco::draco if absent)
if ! is_done draco; then
    dl draco.tar.gz "https://codeload.github.com/google/draco/tar.gz/refs/tags/1.5.7" || exit 1
    S=$(ex draco.tar.gz "$SRC/draco") || exit 1
    cmake_build draco "$S" "$BLD/draco" -DDRACO_TESTS=OFF -DDRACO_WERROR=OFF || exit 1
    if [ ! -f "$PREFIX/lib/cmake/draco/draco-config.cmake" ]; then
        mkdir -p "$PREFIX/lib/cmake/draco"
        cat > "$PREFIX/lib/cmake/draco/draco-config.cmake" << 'EOC'
if(NOT TARGET draco::draco)
    add_library(draco::draco STATIC IMPORTED)
    set_target_properties(draco::draco PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/../../../lib/libdraco.a"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../../include")
endif()
EOC
    fi
fi

# libnoise (SoftFever fork used by the official deps)
if ! is_done libnoise; then
    dl libnoise.tar.gz "https://codeload.github.com/SoftFever/Orca-deps-libnoise/tar.gz/refs/tags/1.0" || exit 1
    S=$(ex libnoise.tar.gz "$SRC/libnoise") || exit 1
    cmake_build libnoise "$S" "$BLD/libnoise" || exit 1
fi

# nlopt 2.5.0 (+ hand-written NLoptConfig.cmake if absent)
if ! is_done nlopt; then
    dl nlopt.tar.gz "https://codeload.github.com/stevengj/nlopt/tar.gz/refs/tags/v2.5.0" || exit 1
    S=$(ex nlopt.tar.gz "$SRC/nlopt") || exit 1
    cmake_build nlopt "$S" "$BLD/nlopt" -DNLOPT_PYTHON=OFF -DNLOPT_JAVA=OFF -DNLOPT_GUILE=OFF \
        -DNLOPT_TESTS=OFF -DBUILD_SHARED_LIBS=OFF || exit 1
    if ! find "$PREFIX" -name "NLoptConfig.cmake" -o -name "nlopt-config.cmake" 2>/dev/null | grep -q .; then
        mkdir -p "$PREFIX/lib/cmake/nlopt"
        cat > "$PREFIX/lib/cmake/nlopt/NLoptConfig.cmake" << 'EOC'
if(NOT TARGET NLopt::nlopt)
    add_library(NLopt::nlopt STATIC IMPORTED)
    set_target_properties(NLopt::nlopt PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/../../../lib/libnlopt.a"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../../include")
endif()
set(NLOPT_LIBRARIES NLopt::nlopt)
EOC
    fi
fi

# cereal 1.3.0 (header-only install)
if ! is_done cereal; then
    dl cereal.tar.gz "https://codeload.github.com/USCiLab/cereal/tar.gz/refs/tags/v1.3.0" || exit 1
    S=$(ex cereal.tar.gz "$SRC/cereal") || exit 1
    cmake_build cereal "$S" "$BLD/cereal" -DJUST_INSTALL_CEREAL=ON -DBUILD_TESTING=OFF || exit 1
fi

# OpenEXR 2.5.5
if ! is_done openexr; then
    dl openexr.tar.gz "https://codeload.github.com/AcademySoftwareFoundation/openexr/tar.gz/refs/tags/v2.5.5" || exit 1
    S=$(ex openexr.tar.gz "$SRC/openexr") || exit 1
    cmake_build openexr "$S" "$BLD/openexr" -DBUILD_TESTING=OFF -DOPENEXR_BUILD_TOOLS=OFF \
        -DOPENEXR_BUILD_EXAMPLES=OFF -DOPENEXR_INSTALL_PKG_CONFIG=OFF || exit 1
fi

# c-blosc 1.17.0
if ! is_done c-blosc; then
    dl c-blosc.tar.gz "https://codeload.github.com/Blosc/c-blosc/tar.gz/refs/tags/v1.17.0" || exit 1
    S=$(ex c-blosc.tar.gz "$SRC/c-blosc") || exit 1
    cmake_build c-blosc "$S" "$BLD/c-blosc" -DBUILD_TESTS=OFF -DBUILD_BENCHMARKS=OFF \
        -DBUILD_EXAMPLES=OFF -DBUILD_FUZZERS=OFF -DPREFER_EXTERNAL_ZLIB=ON || exit 1
fi

# OpenVDB 8.2.x (fork with the version line the official deps pin; needed for
# TreeSupport3D.cpp's <openvdb/tools/VolumeToSpheres.h> and test_tree_support)
if ! is_done openvdb; then
    log "=== openvdb ==="
    if [ ! -d "$SRC/openvdb/.git" ]; then
        rm -rf "$SRC/openvdb"
        git clone --filter=blob:none https://github.com/tamasmeszaros/openvdb.git "$SRC/openvdb" >>"$LOG/openvdb.log" 2>&1 || { log "CLONE FAILED openvdb"; exit 1; }
        ( cd "$SRC/openvdb" && git checkout a68fd58 ) >>"$LOG/openvdb.log" 2>&1 || { log "CHECKOUT FAILED openvdb"; exit 1; }
    fi
    wait_file "$LOG/c-blosc.done" || exit 1
    cmake_build openvdb "$SRC/openvdb" "$BLD/openvdb" \
        -DUSE_BLOSC=ON -DUSE_ZLIB=ON -DOPENVDB_BUILD_CORE=ON -DOPENVDB_BUILD_BINARIES=OFF \
        -DOPENVDB_BUILD_UNITTESTS=OFF -DOPENVDB_BUILD_DOCS=OFF -DOPENVDB_BUILD_PYTHON_MODULE=OFF \
        -DOPENVDB_BUILD_HOUDINI_PLUGIN=OFF -DUSE_HOUDINI=OFF -DOPENVDB_BUILD_AX=OFF \
        -DOPENVDB_BUILD_AX_UNITTESTS=OFF || exit 1
fi

# OCCT V7_6_0 headers + fake config (headers-only use; the STEP importer is
# only called from the GUI which is built OFF)
if ! is_done occt; then
    log "=== occt ==="
    dl occt.tar.gz "https://codeload.github.com/Open-Cascade-SAS/OCCT/tar.gz/refs/tags/V7_6_0" || exit 1
    S=$(ex occt.tar.gz "$SRC/occt") || exit 1
    INC=""
    for d in "$S"/src/*/; do INC="$INC -DOpenCASCADE_${d%/} "; done
    mkdir -p "$PREFIX/lib/cmake/occt"
    {
        echo "set(OpenCASCADE_FOUND TRUE)"
        echo "set(OpenCASCADE_INCLUDE_DIR \"$S/src\")"
        for d in "$S"/src/*/; do echo "list(APPEND OpenCASCADE_INCLUDE_DIR \"${d%/}\")"; done
        for lib in TKXDESTEP TKSTEP TKSTEP209 TKSTEPAttr TKSTEPBase TKXCAF TKXSBase TKVCAF TKCAF TKLCAF TKCDF TKV3d TKService TKMesh TKBO TKPrim TKHLR TKShHealing TKTopAlgo TKGeomAlgo TKBRep TKGeomBase TKG3d TKG2d TKMath TKernel; do
            echo "if(NOT TARGET $lib)"
            echo "    add_library($lib STATIC IMPORTED)"
            echo "    set_target_properties($lib PROPERTIES IMPORTED_LOCATION \"$PREFIX/lib/lib${lib}.a\" INTERFACE_INCLUDE_DIRECTORIES \"\${OpenCASCADE_INCLUDE_DIR}\")"
            echo "endif()"
            ar rcs "$PREFIX/lib/lib${lib}.a" 2>/dev/null
        done
        echo "set(OpenCASCADE_LIBRARIES TKXDESTEP TKSTEP TKSTEP209 TKSTEPAttr TKSTEPBase TKXCAF TKXSBase TKVCAF TKCAF TKLCAF TKCDF TKV3d TKService TKMesh TKBO TKPrim TKHLR TKShHealing TKTopAlgo TKGeomAlgo TKBRep TKGeomBase TKG3d TKG2d TKMath TKernel)"
    } > "$PREFIX/lib/cmake/occt/OpenCASCADEConfig.cmake"
    done_marker occt
fi

log "chain_a complete"
