#!/bin/bash

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 'Your commit message'"
    exit 1
fi

commit_message="$1"

git add -A
git commit -m "$commit_message"
git push --force
