# Sandbox build scripts (NOT part of the product build)

These scripts build the third-party dependencies of OrcaSlicer inside a
restricted sandbox where:

- only github.com / codeload.github.com / api.github.com / registry.npmjs.org /
  pypi.org / files.pythonhosted.org are reachable (no apt, no GNU mirrors, no
  GitHub release assets — those redirect to objects.githubusercontent.com);
- there is no wxWidgets/FFMPEG toolchain, so the GUI is built OFF and only
  `libslic3r` + the linked test binaries (`fff_print_tests`, `libslic3r_tests`)
  are produced.

Layout (all outputs under the gitignored `deps/build/`):

- `deps/build/venv3/`          — python venv with cmake 3.31.6 + ninja
- `deps/build/OrcaSlicer_dep/usr/local` — install PREFIX for every dependency
- `deps/build/src_dl/`         — downloaded tarballs (cached)
- `deps/build/src/`            — extracted sources
- `deps/build/b/`              — per-dependency build trees (deleted after install)
- `deps/build/log/`            — per-dependency logs + `.done` markers

Run:

    bash deps/sandbox/chain_a.sh &   # stubs + most C/C++ dependencies
    bash deps/sandbox/chain_b.sh &   # CPython (must finish before the main build)

Both scripts are idempotent (`.done` markers) and safe to re-run after a
sandbox restore. GNU m4 (needed by GMP) is taken from the `cmeel-m4` manylinux
wheel on PyPI because no m4/autoconf exists in the sandbox and GNU git mirrors
do not ship a generated `configure`.

Some dependencies are stubs (OpenGL, glfw3, DBus, fontconfig) — safe only
because `SLIC3R_GUI=OFF`. OCCT is a headers+empty-archives stub; its STEP
importer is only called from the GUI.
