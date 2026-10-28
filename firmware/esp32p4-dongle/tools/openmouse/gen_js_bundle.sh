#!/bin/sh
# Builds components/amv_js/generated/openmouse.bc: the AnyMouse bridge (components/amv_js/js/)
# plus OpenMouse's driver registry, unchanged, bundled to one ES module and compiled to QuickJS
# bytecode. The firmware embeds the file and runs it in amv_js.
#   OPENMOUSE_DIR=/path/to/openmouse-protocol tools/openmouse/gen_js_bundle.sh
# Needs Node >= 20 (npx fetches esbuild the first time) and qjsc of the vendored QuickJS-ng
# version (brew install quickjs-ng): bytecode only loads into the engine version that wrote it.
set -e
here=$(cd "$(dirname "$0")" && pwd)
: "${OPENMOUSE_DIR:?set OPENMOUSE_DIR to the OpenMouse checkout}"
comp="$here/../../components/amv_js"
out="$comp/generated"
want=$(sed -n 's/^QuickJS-ng \([0-9.]*\) .*/\1/p' "$comp/quickjs-ng/VENDORED.txt")
have=$(qjsc 2>&1 | sed -n 's/^QuickJS-ng Compiler version \([0-9.]*\).*/\1/p')
if [ "$want" != "$have" ]; then
    echo "qjsc is version '$have', the vendored engine is '$want'" >&2
    exit 1
fi
commit=$(git -C "$OPENMOUSE_DIR" rev-parse --short HEAD)
mkdir -p "$out"
cd "$here"
esbuild_bin=$(npx --yes -p esbuild@0.28.2 sh -c 'command -v esbuild')
node bundle_js.mjs "$(dirname "$esbuild_bin")/../esbuild" "$comp/js/bridge.js" "$OPENMOUSE_DIR" "$commit" "$out/openmouse.js"
qjsc -ss -b -m -N openmouse -o "$out/openmouse.bc" "$out/openmouse.js"
printf 'OpenMouse %s, QuickJS-ng %s\njs %s bytes, bytecode %s bytes\n' "$commit" "$have" \
    "$(wc -c < "$out/openmouse.js" | tr -d ' ')" "$(wc -c < "$out/openmouse.bc" | tr -d ' ')" > "$out/openmouse.txt"
cat "$out/openmouse.txt"
