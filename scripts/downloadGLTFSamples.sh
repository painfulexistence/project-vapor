#!/usr/bin/env bash
# Download a few Khronos glTF-Sample-Assets (GLB, self-contained) into
# Examples/GLTFViewer/assets/models. Kept out of the repo; GLTFViewer lists them
# in its model picker alongside the committed cube.
#   DamagedHelmet     — the canonical PBR test model
#   DragonAttenuation — KHR_materials_transmission/_volume/_ior showcase
#   BoomBox           — small, high-detail PBR (metallic-roughness + normal/AO/emissive)
set -euo pipefail
cd "$(dirname "$0")/.."
DEST="Examples/GLTFViewer/assets/models"
mkdir -p "$DEST"
BASE="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models"

fetch() { # <ModelName>
    local name="$1"
    local url="$BASE/$name/glTF-Binary/$name.glb"
    echo "Fetching $name.glb ..."
    curl -fL -o "$DEST/$name.glb" "$url" || { echo "  (skipped $name — $url unavailable)"; rm -f "$DEST/$name.glb"; }
}

fetch DamagedHelmet
fetch DragonAttenuation
fetch BoomBox
echo "Done. Models in $DEST/ (pick them in GLTFViewer's model list)."
