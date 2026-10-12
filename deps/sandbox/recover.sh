#!/usr/bin/env bash
# One-command recovery after a sandbox restore.
#
# Sandbox restores wipe ALL non-git-tracked files (deps/build, swapfile, venvs)
# and may roll the local branch back to the session base commit. Everything
# durable lives on the remote branch, so recovery is:
#   1. make the local branch match origin (only when the working tree already
#      matches origin — nothing uncommitted is ever discarded),
#   2. recreate the 8G swapfile,
#   3. recreate deps/build/venv3 (cmake + ninja),
#   4. (re)launch the dependency chains (idempotent via .done markers).
#
# After the chains finish, run configure_main.sh, then run_tests.sh.
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO" || exit 1
BRANCH=arena/5c7be923-orcaslicer
B=$REPO/deps/build

echo "== git =="
git fetch origin "$BRANCH" || { echo "fetch failed"; exit 1; }
if git rev-parse --verify -q "origin/$BRANCH" >/dev/null; then
    if git diff --quiet "origin/$BRANCH" 2>/dev/null; then
        git reset --hard "origin/$BRANCH" >/dev/null
        echo "local branch fast-forwarded to origin/$BRANCH ($(git rev-parse --short HEAD))"
    else
        echo "WARNING: working tree differs from origin/$BRANCH — NOT resetting."
        echo "Commit and push your changes first, then re-run recover.sh."
        exit 1
    fi
fi

echo "== swap =="
if ! sudo -n /sbin/swapon --show 2>/dev/null | grep -q swapfile; then
    if [ ! -f /home/user/swapfile ]; then
        dd if=/dev/zero of=/home/user/swapfile bs=1M count=8192 status=none \
            && chmod 600 /home/user/swapfile && /sbin/mkswap /home/user/swapfile >/dev/null
    fi
    sudo -n /sbin/swapon /home/user/swapfile || echo "WARNING: swapon failed"
fi
sudo -n /sbin/swapon --show 2>/dev/null | tail -1

echo "== venv3 =="
if [ ! -x "$B/venv3/bin/cmake" ]; then
    python3 -m venv "$B/venv3" || exit 1
    "$B/venv3/bin/pip" install -q cmake==3.31.6 ninja || exit 1
fi
"$B/venv3/bin/cmake" --version | head -1

mkdir -p "$B/log"
echo "== chains =="
if ! pgrep -f chain_a.sh >/dev/null; then
    setsid nohup bash deps/sandbox/chain_a.sh < /dev/null > "$B/log/chain_a.out" 2>&1 &
    echo "launched chain_a"
else
    echo "chain_a already running"
fi
if ! pgrep -f chain_b.sh >/dev/null; then
    setsid nohup bash deps/sandbox/chain_b.sh < /dev/null > "$B/log/chain_b.out" 2>&1 &
    echo "launched chain_b"
else
    echo "chain_b already running"
fi
echo "recovery complete"
