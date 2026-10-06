#!/bin/bash
# Update this fork to the latest stable PPSSPP release from hrydgard/ppsspp.
#
#   master - the latest stable release (a vX.Y.Z tag) plus our own changes.
#            We follow releases, not upstream master: development code has had
#            regressions (e.g. slower fast-forward on iOS) that releases don't.
#
# Usage: ./sync-upstream.sh [--push] [--tag vX.Y.Z]
#   --push        push master to origin (the fork) afterwards
#   --tag vX.Y.Z  merge this release instead of the newest one

set -e

PUSH=0
TAG=""
while [ $# -gt 0 ]; do
	case "$1" in
		--push) PUSH=1 ;;
		--tag) shift; TAG="$1" ;;
		*) echo "Unknown option: $1"; exit 1 ;;
	esac
	shift
done

cd "$(dirname "$0")"

if [ -n "$(git status --porcelain --ignore-submodules --untracked-files=no)" ]; then
	echo "Working tree has uncommitted changes. Commit or stash them first."
	exit 1
fi

if [ "$(git remote get-url --push upstream)" != "DISABLE" ]; then
	echo "Safety check failed: pushing to upstream is not disabled."
	echo "Run: git remote set-url --push upstream DISABLE"
	exit 1
fi

START_BRANCH=$(git rev-parse --abbrev-ref HEAD)

echo "== Fetching upstream releases"
git fetch upstream --tags

if [ -z "$TAG" ]; then
	# Stable releases only: v1.20.4 yes, v1.20.3-beta1 no.
	TAG=$(git tag -l 'v*' --sort=-v:refname | grep -E '^v[0-9]+\.[0-9]+(\.[0-9]+)?$' | head -1)
fi
echo "== Latest stable release: $TAG"

git checkout master
if git merge-base --is-ancestor "$TAG" master; then
	echo "== master already includes $TAG"
else
	echo "== Merging $TAG into master"
	git merge --no-edit "$TAG"
fi

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

echo "== Done. master is based on $TAG"
