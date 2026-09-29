#!/bin/sh
set -eu

REPO_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_DIR"

failed=0

private_files=$(rg --files \
    -g '!managed_components/**' \
    -g '!components/**' \
    -g '!build*/**' |
    grep -Ei '(^|/)(docs/development|docs/ref|internal-notes|scratch)/|(^|/)(TODO|NOTES|.*AUDIT|.*FINDINGS|.*GAP)(_[^/]*)?\.md$|\.(local|draft|internal)\.md$' || true)
if [ -n "$private_files" ]; then
    echo "开发记录或私有资料不能留在正式仓库："
    echo "$private_files"
    failed=1
fi

forbidden_refs=$(rg -ni \
    '(/Users/[A-Za-z0-9._-]+/|file://|codex-clipboard|docs/development/|docs/ref/[^`[:space:]]+|NOTES\.local|\.(local|draft|internal)\.md)' \
    -g '!managed_components/**' \
    -g '!components/**' \
    -g '!build*/**' \
    -g '!scripts/check-repository-hygiene.sh' \
    -g '!scripts/check-public-privacy.py' \
    -g '!scripts/check-documentation.mjs' \
    . || true)
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
