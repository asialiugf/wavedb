#!/bin/bash
set -e
cd "$(dirname "$0")/.."
MSG="${1:-.}"
git add -A
git commit -m "$MSG"
git push
