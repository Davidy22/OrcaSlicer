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

Some dependencies are stubs (OpenGL, glfw3, DBus) — safe only because
`SLIC3R_GUI=OFF`. fontconfig is the real system runtime library reached through
a symlink plus a minimal public header. OCCT is a REAL static build (libslic3r
compiles Format/STEP.cpp and links the TK toolkits); Draw and Visualization
modules are off (no Tcl/Tk, no X11/GLX headers in the sandbox) and an empty
libTKV3d.a satisfies the linker because nothing in libslic3r references it.

## Sandbox lifecycle — read this before running anything

The sandbox is torn down when a turn ends: background processes die and
everything outside the git workspace diff (tracked modifications + small
untracked files) is dropped — `deps/build/`, the swapfile, venvs, local git
commits. Only pushes to the remote branch survive externally. Therefore:

- After a turn boundary: `bash deps/sandbox/recover.sh` (git + swap + venv;
  add `--no-chains` when build_all.sh will launch the chains itself).
- The whole build+test pipeline MUST run inside ONE turn:

      bash deps/sandbox/build_all.sh

  launches both dependency chains in parallel, then configures/builds
  `libslic3r` + `fff_print_tests` + `libslic3r_tests`, then runs the purge
  acceptance suite and regressions. Poll `deps/build/log/PIPELINE_RESULT.txt`
  until it ends with `PIPELINE OK` or `FAILED: ...`. Expected duration on 2
  cores: ~5 hours.
