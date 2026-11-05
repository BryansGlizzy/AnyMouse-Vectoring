import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath, pathToFileURL } from "node:url";
const ROOT = process.env.OPENMOUSE_DIR;
const paths = JSON.parse(readFileSync(ROOT + "/tsconfig.json", "utf8").replace(/\/\/.*$/gm, "")).compilerOptions.paths;
const pkg = JSON.parse(readFileSync(ROOT + "/package.json", "utf8"));
export async function resolve(spec, ctx, next) {
  if (spec.startsWith("@openmouse/protocol")) {
    let t = paths[spec]?.[0];
    if (!t) {
      const e = pkg.exports["./" + spec.slice("@openmouse/protocol/".length)];
      if (e) t = e.import.replace("./dist/", "./src/").replace(/\.js$/, ".ts");
    }
    if (t) return next(pathToFileURL(ROOT + "/" + t.replace(/^\.\//, "")).href, ctx);
  }
  if (spec.startsWith(".") && spec.endsWith(".js") && ctx.parentURL) {
    const p = fileURLToPath(new URL(spec, ctx.parentURL));
    if (!existsSync(p)) { try { return await next(spec.replace(/\.js$/, ".ts"), ctx); } catch {} }
  }
  return next(spec, ctx);
}
