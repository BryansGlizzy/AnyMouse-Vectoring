// Registers the module-resolution hooks that let the OpenMouse TypeScript sources load unbuilt
// (".js" specifiers that mean ".ts" files, and the package's own "@openmouse/protocol/*" imports).
import { register } from "node:module";
register("./om_resolve_hooks.mjs", import.meta.url);
