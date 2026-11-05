// Bundle one OpenMouse *.test.ts into a single JS file QuickJS can run.
// Usage: node bundle_test.mjs <esbuild pkg dir> <openmouse dir> <test.ts> <out.js>
import { createRequire } from "node:module";
import { readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const [esbuildDir, omDir, entry, out] = process.argv.slice(2);
const here = dirname(fileURLToPath(import.meta.url));
const esbuild = createRequire(join(esbuildDir, "package.json"))("esbuild");
const pkg = JSON.parse(readFileSync(join(omDir, "package.json"), "utf8"));

const shims = {
  name: "shims",
  setup(b) {
    b.onResolve({ filter: /^node:test$/ }, () => ({ path: join(here, "node_test.js") }));
    b.onResolve({ filter: /^node:assert(\/strict)?$/ }, () => ({ path: join(here, "node_assert.js") }));
    b.onResolve({ filter: /^@openmouse\/protocol(\/.*)?$/ }, (a) => {
      const sub = "." + (a.path.slice("@openmouse/protocol".length) || "");
      const exp = pkg.exports[sub];
      if (!exp) return { errors: [{ text: "no export " + sub }] };
      return { path: join(omDir, exp.import.replace(/^\.\/dist\//, "src/").replace(/\.js$/, ".ts")) };
    });
  },
};

await esbuild.build({
  entryPoints: [entry], bundle: true, format: "esm", target: "es2022",
  outfile: out, plugins: [shims], logLevel: "error",
  inject: [join(here, "prelude.js")],
});
