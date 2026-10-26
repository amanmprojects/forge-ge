#!/usr/bin/env bash
# Publish web/dist to the gh-pages branch of origin; GitHub Pages serves it
# as-is (static files, wasm as application/wasm, no headers needed).
#
#   ./web/build.sh && ./web/record.sh && ./web/deploy.sh
#
# The branch is regenerated as a single commit on every deploy and
# force-pushed, so old clips and wasm binaries never pile up in history.
set -euo pipefail

cd "$(dirname "$0")/.."

DIST=web/dist
GAMES="platformer breakout snake chess lantern"

[[ -f "$DIST/index.html" ]] || { echo "error: no $DIST/index.html - run ./web/build.sh" >&2; exit 1; }
for game in $GAMES; do
    [[ -f "$DIST/$game/index.wasm" ]] || { echo "error: $DIST/$game is not built" >&2; exit 1; }
    [[ -f "$DIST/clips/$game.mp4" ]] || echo "warning: no clip for $game - run ./web/record.sh" >&2
done

remote="$(git remote get-url origin)"
rev="$(git rev-parse --short HEAD)"
name="$(git config user.name || echo forge)"
email="$(git config user.email || echo forge@localhost)"

site="$(mktemp -d)"
trap 'rm -rf "$site"' EXIT
cp -R "$DIST"/. "$site"/
# Local-only dotfiles stay out (`vercel link` leaves .vercel/ and a .gitignore).
rm -rf "$site"/.[!.]*
touch "$site/.nojekyll" # no Jekyll pass: serve the files exactly as built

git -C "$site" init -q -b gh-pages
git -C "$site" add -A
git -C "$site" -c user.name="$name" -c user.email="$email" \
    commit -q -m "Deploy web build of $rev"
git -C "$site" push -f "$remote" gh-pages
echo "Deployed $rev to gh-pages on $remote"
