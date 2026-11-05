# OpenMouse's test suite under QuickJS-ng

Runs every `*.test.ts` of an OpenMouse checkout, unchanged, in the same engine the dongle uses
(`qjs`, QuickJS-ng 0.17.0), with small stand-ins for `node:test` and `node:assert` and the
browser globals the drivers use (`prelude.js`). Used on 2026-10-27 to decide that OpenMouse can
run on the dongle as is: 185 of 188 runnable files passed (the rest need Node's `Buffer` in the
test itself, or format numbers with `toLocaleString`, which QuickJS does not localise).

    E=$(dirname "$(npx --yes -p esbuild@0.28.2 sh -c 'command -v esbuild')")/../esbuild
    zsh run_all.sh <openmouse checkout> "$E" <out dir>

The three files OpenMouse runs with real timers (`attackshark/hid`, `gwolves/magnetic`,
`pulsar/pulsar-areson-hid`) need `QJS_REAL_TIMERS=1`; `run_all.sh` collapses timers like
OpenMouse's own `fast-test-timers.mjs`. Files that read captures through `node:fs` are reported
as build failures.
