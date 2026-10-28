// Minimal node:test: queue every test, run them in order after the module loads.
const queue = [];
let scheduled = false;

function schedule() {
  if (scheduled) return;
  scheduled = true;
  setTimeout(async () => {
    let pass = 0, fail = 0, skip = 0;
    for (const t of queue) {
      if (t.skip) { skip++; continue; }
      try {
        await t.fn({ skip() {}, todo() {}, diagnostic() {}, mock: undefined });
        pass++;
      } catch (e) {
        fail++;
        print(`not ok - ${t.name}\n    ${String(e && e.message || e).split("\n")[0]}\n${(e && e.stack || "").split("\n").slice(0,3).join("\n")}`);
      }
    }
    print(`RESULT pass=${pass} fail=${fail} skip=${skip}`);
  }, 0);
}

export default function test(name, opts, fn) {
  if (typeof opts === "function") { fn = opts; opts = {}; }
  queue.push({ name, fn, skip: !!(opts && (opts.skip || opts.todo)) });
  schedule();
}
test.skip = (name, fn) => test(name, { skip: true }, fn);
test.todo = (name, fn) => test(name, { todo: true }, fn);
export { test };
export const describe = (name, fn) => fn();
export const it = test;
