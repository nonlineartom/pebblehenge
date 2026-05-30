#!/usr/bin/env bash
# Stamp the current git short hash into package.json's versionLabel,
# run pebble build, then copy the bundle to a versioned filename so
# every .pbw is uniquely identifiable - both by its filename and by
# what the Pebble phone app shows under the installed version.
#
# Usage:  scripts/build.sh
#
# The base version (everything before the hash) lives in package.json's
# pebble.versionLabel. Bump it manually when you want to mark a phase
# boundary; the script only stamps the build identifier after it.

set -euo pipefail
cd "$(dirname "$0")/.."

# Detect whether the working tree has uncommitted changes, before we
# touch package.json. That gives a "+dirty" marker so we can tell
# WIP builds apart from a build of a clean commit.
DIRTY=""
if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
    DIRTY="+dirty"
fi

HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "untracked")
BASE=$(python3 -c "
import json
d = json.load(open('package.json'))
v = d['pebble'].get('versionLabel', '0.1')
print(v.split('-')[0].split('+')[0])
")
LABEL="${BASE}-${HASH}${DIRTY}"

# Revert package.json on exit so the working tree stays clean. The
# version stamp is captured in the built artifact, not in source.
trap 'git checkout -- package.json 2>/dev/null || true' EXIT

python3 - "$LABEL" <<'PY'
import json, sys
# Only stamp into versionLabel - it's a free-form display string.
# `version` must be strict semver (each component int 0..255) or the
# SDK's wscript rejects the project at configure time.
label = sys.argv[1]
with open('package.json') as f:
    d = json.load(f)
d['pebble']['versionLabel'] = label
with open('package.json', 'w') as f:
    json.dump(d, f, indent=2)
    f.write('\n')
PY

echo "stamping versionLabel=$LABEL"

pebble clean >/dev/null
pebble build

TARGET="build/pebblehenge-${LABEL}.pbw"
cp build/pebblehenge.pbw "$TARGET"

echo
echo "build OK"
echo "  artifact:     $TARGET"
echo "  installed-as: $LABEL  (visible in the Pebble phone app)"
