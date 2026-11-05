#!/bin/sh
# Regenerates components/amv_catalog/amv_catalog.generated.h from an OpenMouse checkout.
#   OPENMOUSE_DIR=/path/to/openmouse-protocol tools/openmouse/gen_catalog.sh
# Needs Node >= 20 and network access the first time (npx fetches the tsx TypeScript runner).
set -e
here=$(cd "$(dirname "$0")" && pwd)
: "${OPENMOUSE_DIR:?set OPENMOUSE_DIR to the OpenMouse checkout}"
export OPENMOUSE_DIR
cd "$here"
exec npx --yes tsx@4.23.15 --no-warnings --import ./om_register.mjs gen_catalog.mjs \
    "$here/../../components/amv_catalog/amv_catalog.generated.h"
