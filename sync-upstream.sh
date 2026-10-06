#!/bin/bash
# Pull the latest changes from hrydgard/ppsspp into this fork.
#
#   master - our version: upstream plus our own changes. Upstream is merged into it.
#
# Usage: ./sync-upstream.sh [--push]
#   --push  push master to origin (the fork) afterwards

set -e

PUSH=0
for arg in "$@"; do
	case "$arg" in
		--push) PUSH=1 ;;
		*) echo "Unknown option: $arg"; exit 1 ;;
	esac
done

cd "$(dirname "$0")"

if [ -n "$(git status --porcelain --ignore-submodules)" ]; then
	echo "Working tree has uncommitted changes. Commit or stash them first."
	exit 1
fi

if [ "$(git remote get-url --push upstream)" != "DISABLE" ]; then
	echo "Safety check failed: pushing to upstream is not disabled."
	echo "Run: git remote set-url --push upstream DISABLE"
	exit 1
fi

START_BRANCH=$(git rev-parse --abbrev-ref HEAD)

echo "== Fetching upstream"
git fetch upstream --tags

echo "== Merging upstream/master into master"
git checkout master
git merge --no-edit upstream/master

echo "== Updating submodules"
git submodule sync --recursive
git submodule update --init --recursive

if [ "$PUSH" = 1 ]; then
	echo "== Pushing to origin"
	git push origin master
fi

if [ "$START_BRANCH" != master ]; then
	git checkout "$START_BRANCH"
fi

echo "== Done. master now includes upstream $(git rev-parse --short upstream/master)"
