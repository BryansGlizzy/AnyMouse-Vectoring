// Minimal node:assert/strict covering what OpenMouse's tests call.
class AssertionError extends Error {}

function fmt(v) {
  try { return JSON.stringify(v, (k, x) => (ArrayBuffer.isView(x) ? Array.from(x) : x)); } catch { return String(v); }
}

function deq(a, b) {
  if (Object.is(a, b)) return true;
  if (typeof a !== "object" || typeof b !== "object" || a === null || b === null) return false;
  if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
  if (ArrayBuffer.isView(a)) {
    if (a.byteLength !== b.byteLength) return false;
    const x = new Uint8Array(a.buffer, a.byteOffset, a.byteLength), y = new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
    for (let i = 0; i < x.length; i++) if (x[i] !== y[i]) return false;
    return true;
  }
  if (a instanceof Map) return a.size === b.size && [...a].every(([k, v]) => b.has(k) && deq(v, b.get(k)));
  if (a instanceof Set) return a.size === b.size && [...a].every((v) => b.has(v));
  const ka = Object.keys(a), kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  return ka.every((k) => Object.prototype.hasOwnProperty.call(b, k) && deq(a[k], b[k]));
}

function matches(err, exp) {
  if (exp === undefined) return true;
  if (typeof exp === "function") return exp.prototype !== undefined && err instanceof exp ? true : exp(err) === true;
  if (exp instanceof RegExp) return exp.test(String(err)) || exp.test(String(err && err.message));
  if (typeof exp === "object") return Object.keys(exp).every((k) => (exp[k] instanceof RegExp ? exp[k].test(err[k]) : deq(err[k], exp[k])));
  return true;
}

function fail(msg) { throw new AssertionError(msg); }

const assert = (v, m) => { if (!v) fail(m || `expected truthy, got ${fmt(v)}`); };
assert.ok = assert;
assert.AssertionError = AssertionError;
assert.fail = (m) => fail(m || "failed");
assert.equal = assert.strictEqual = (a, b, m) => { if (!Object.is(a, b)) fail(m || `${fmt(a)} !== ${fmt(b)}`); };
assert.notEqual = assert.notStrictEqual = (a, b, m) => { if (Object.is(a, b)) fail(m || `${fmt(a)} === ${fmt(b)}`); };
assert.deepEqual = assert.deepStrictEqual = (a, b, m) => { if (!deq(a, b)) fail(m || `not deep equal:\n      ${fmt(a)}\n      ${fmt(b)}`); };
assert.notDeepEqual = assert.notDeepStrictEqual = (a, b, m) => { if (deq(a, b)) fail(m || "deep equal"); };
assert.match = (s, re, m) => { if (!re.test(s)) fail(m || `${fmt(s)} !~ ${re}`); };
assert.doesNotMatch = (s, re, m) => { if (re.test(s)) fail(m || `${fmt(s)} =~ ${re}`); };
assert.throws = (fn, exp, m) => {
  try { fn(); } catch (e) { if (!matches(e, exp)) fail(m || `wrong error: ${e && e.message}`); return; }
  fail(m || "expected throw");
};
assert.doesNotThrow = (fn) => fn();
assert.rejects = async (p, exp, m) => {
  try { await (typeof p === "function" ? p() : p); } catch (e) { if (!matches(e, exp)) fail(m || `wrong rejection: ${e && e.message}`); return; }
  fail(m || "expected rejection");
};
assert.doesNotReject = async (p) => { await (typeof p === "function" ? p() : p); };

export default assert;
export const { ok, equal, strictEqual, deepEqual, deepStrictEqual, notEqual, match, throws, rejects } = assert;
export { AssertionError };
