#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [ "$#" -lt 1 ]; then
  echo "usage: $0 XNU_WORKTREE [PATCH_DIR ...]" >&2
  exit 2
fi
WORKTREE=$(cd "$1" && pwd)
shift

if ! git -C "$WORKTREE" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "error: XNU worktree is not a Git repository: $WORKTREE" >&2
  exit 1
fi

if [ "$#" -gt 0 ]; then
  PATCH_DIRS=("$@")
else
  PATCH_DIRS=("$ROOT/patches/xnu" "$ROOT/kits/NativePrelink/patches")
fi

shopt -s nullglob
PATCHES=()
for directory in "${PATCH_DIRS[@]}"; do
  PATCHES+=("$directory"/*.patch)
done

if [ "${#PATCHES[@]}" -eq 0 ]; then
  exit 0
fi

TEMP_INDEX=$(mktemp "${TMPDIR:-/tmp}/unixnu-xnu-index.XXXXXX")
TEMP_PATCH=$(mktemp "${TMPDIR:-/tmp}/unixnu-xnu-patch.XXXXXX")
rm -f "$TEMP_INDEX"
trap 'rm -f "$TEMP_INDEX" "$TEMP_PATCH"' EXIT

# Simulate the full patch replay in an alternate index. This lets us reverse
# overlapping patches in stack order without mutating the source tree before
# the complete forward stack has passed its conflict checks.
GIT_INDEX_FILE="$TEMP_INDEX" git -C "$WORKTREE" read-tree HEAD
GIT_INDEX_FILE="$TEMP_INDEX" git -C "$WORKTREE" add -A -- .
INITIAL_TREE=$(GIT_INDEX_FILE="$TEMP_INDEX" git -C "$WORKTREE" write-tree)

for ((index=${#PATCHES[@]} - 1; index >= 0; index--)); do
  patch=${PATCHES[index]}
  if GIT_INDEX_FILE="$TEMP_INDEX" git -C "$WORKTREE" apply --cached --reverse --check "$patch" >/dev/null 2>&1; then
    printf '== reverse XNU patch %s\n' "${patch##*/}"
    GIT_INDEX_FILE="$TEMP_INDEX" git -C "$WORKTREE" apply --cached --reverse "$patch"
  fi
done

for patch in "${PATCHES[@]}"; do
  printf '== apply XNU patch %s\n' "${patch##*/}"
  GIT_INDEX_FILE="$TEMP_INDEX" git -C "$WORKTREE" apply --cached --check "$patch"
  GIT_INDEX_FILE="$TEMP_INDEX" git -C "$WORKTREE" apply --cached "$patch"
done

GIT_INDEX_FILE="$TEMP_INDEX" git -C "$WORKTREE" diff --cached --binary "$INITIAL_TREE" > "$TEMP_PATCH"
if [ -s "$TEMP_PATCH" ]; then
  git -C "$WORKTREE" apply --check "$TEMP_PATCH"
  git -C "$WORKTREE" apply "$TEMP_PATCH"
fi
