#!/bin/sh
set -eu

REPO_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_DIR"

failed=0

# Prefer ripgrep; otherwise use git's view of tracked and untracked,
# non-ignored files. A missing scanner must fail rather than pass silently.
if command -v rg >/dev/null 2>&1; then
    list_files() {
        rg --files -g '!managed_components/**' -g '!components/**' -g '!build*/**'
    }
    search_files() {
        rg -ni "$1" -g '!managed_components/**' -g '!components/**' -g '!build*/**' \
            -g '!scripts/check-repository-hygiene.sh' -g '!scripts/check-public-privacy.py' \
            -g '!scripts/check-documentation.mjs' .
    }
elif git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    # Match ripgrep's defaults: skip hidden files and directories.
    set -- ':(glob,exclude)**/managed_components/**' ':(glob,exclude)**/components/**' \
        ':(glob,exclude)**/build*/**' ':(glob,exclude)**/.*' ':(glob,exclude)**/.*/**'
    list_files() {
        git ls-files --cached --others --exclude-standard -- . "$@"
    }
    search_files() {
        pattern=$1
        shift
        git grep --untracked -nIi -E "$pattern" -- . "$@" \
            ':(exclude)scripts/check-repository-hygiene.sh' ':(exclude)scripts/check-public-privacy.py' \
            ':(exclude)scripts/check-documentation.mjs'
    }
else
    echo "repository hygiene requires ripgrep (rg) or a git work tree" >&2
    exit 1
fi

private_files=$(list_files "$@" |
    grep -Ei '(^|/)(docs/development|docs/ref|internal-notes|scratch)/|(^|/)(TODO|NOTES|.*AUDIT|.*FINDINGS|.*GAP)(_[^/]*)?\.md$|\.(local|draft|internal)\.md$' || true)
if [ -n "$private_files" ]; then
    echo "开发记录或私有资料不能留在正式仓库："
    echo "$private_files"
    failed=1
fi

forbidden_refs=$(search_files \
    '(/Users/[A-Za-z0-9._-]+/|file://|codex-clipboard|docs/development/|docs/ref/[^`[:space:]]+|NOTES\.local|\.(local|draft|internal)\.md)' \
    "$@" || true)
if [ -n "$forbidden_refs" ]; then
    echo "正式仓库包含本机路径或开发记录引用："
    echo "$forbidden_refs"
    failed=1
fi

if ! node scripts/check-documentation.mjs "$REPO_DIR"; then
    failed=1
fi

if ! python3 scripts/check-public-privacy.py "$REPO_DIR"; then
    failed=1
fi

if [ "$failed" -ne 0 ]; then
    exit 1
fi

echo "repository hygiene: ok"
