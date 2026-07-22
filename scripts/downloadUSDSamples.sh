#!/usr/bin/env bash
# Download USD sample scenes into Examples/USDViewer/assets/models.
# Currently Pixar's Kitchen_set (~30MB) — the reference/payload composition test
# the importer flattens on load. Kept out of the repo; source: openusd.org.
set -euo pipefail
cd "$(dirname "$0")/.."
DEST="Examples/USDViewer/assets/models/kitchen"
mkdir -p "$DEST"
URL="https://graphics.pixar.com/usd/files/Kitchen_set.zip"
echo "Fetching Kitchen_set from $URL ..."
curl -L -o "$DEST/Kitchen_set.zip" "$URL"
unzip -o -q "$DEST/Kitchen_set.zip" -d "$DEST"
# The zip contains a Kitchen_set/ folder; flatten so the .usd sits at $DEST.
if [ -d "$DEST/Kitchen_set" ]; then
    mv "$DEST"/Kitchen_set/* "$DEST"/
    rmdir "$DEST/Kitchen_set"
fi
rm -f "$DEST/Kitchen_set.zip"
echo "Done: $DEST/Kitchen_set.usd"
