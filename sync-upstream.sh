#!/bin/bash
# Pull the latest changes from hrydgard/ppsspp into this fork.
#
#   master  - mirror of upstream/master, never commit here
#   custom  - our own changes, merged on top of master
#
# Usage: ./sync-upstream.sh [--rebase] [--push]
#   --rebase  rebase custom onto master instead of merging
#   --push    push master and custom to origin (the fork) afterwards

set -e

MODE=merge
PUSH=0
for arg in "$@"; do
	case "$arg" in
		--rebase) MODE=rebase ;;
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

echo "== Updating master"
git checkout master
git merge --ff-only upstream/master

echo "== Updating custom ($MODE)"
git checkout custom
if [ "$MODE" = rebase ]; then
	git rebase master
else
	git merge --no-edit master
fi

echo "== Updating submodules"
git submodule sync --recursive
git submodule update --init --recursive

if [ "$PUSH" = 1 ]; then
	echo "== Pushing to origin"
	git push origin master
	if [ "$MODE" = rebase ]; then
		git push --force-with-lease origin custom
	else
		git push origin custom
	fi
fi

if [ "$START_BRANCH" != custom ] && [ "$START_BRANCH" != master ]; then
	git checkout "$START_BRANCH"
fi

echo "== Done. custom is now based on $(git rev-parse --short master)"
