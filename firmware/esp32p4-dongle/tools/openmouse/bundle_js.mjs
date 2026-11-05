// Bundles the dongle's JS bridge with OpenMouse's drivers into one minified ES module.
//   node bundle_js.mjs <esbuild package dir> <bridge.js> <openmouse dir> <openmouse commit> <out.js>
// The package imports itself ("@openmouse/protocol/<sub>"); those resolve through its
// package.json exports to the TypeScript sources, as om_resolve_hooks.mjs does for the catalog.
import { createRequire } from "node:module";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const [esbuildDir, entry, omDir, commit, out] = process.argv.slice(2);
const esbuild = createRequire(join(esbuildDir, "package.json"))("esbuild");
const pkg = JSON.parse(readFileSync(join(omDir, "package.json"), "utf8"));

const selfRef = {
  name: "openmouse-self",
  setup(b) {
    b.onResolve({ filter: /^@openmouse\/protocol(\/.*)?$/ }, (a) => {
      const sub = "." + (a.path.slice("@openmouse/protocol".length) || "");
      const exp = pkg.exports[sub];
      if (!exp) return { errors: [{ text: "no package export " + sub }] };
      return { path: join(omDir, exp.import.replace(/^\.\/dist\//, "src/").replace(/\.js$/, ".ts")) };
    });
  },
};

const r = await esbuild.build({
  entryPoints: [entry],
  bundle: true,
  format: "esm",
  target: "es2022",
  minify: true,
  legalComments: "none",
  define: { __OPENMOUSE_COMMIT__: JSON.stringify(commit) },
  outfile: out,
  plugins: [selfRef],
  logLevel: "warning",
  metafile: true,
});
console.log(`bundled ${Object.keys(r.metafile.inputs).length} modules`);
