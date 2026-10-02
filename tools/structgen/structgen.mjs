#!/usr/bin/env node
// structgen - C structs -> TypeScript accessors over wasm32 linear memory.
//
// Every offset, size, bitfield position and array stride comes from clang
// (-fdump-record-layouts for --target=wasm32, once per build flag set); this
// script never computes a layout. It only discovers which types to ask about:
// each type is probed as `struct __sgN { __typeof__(EXPR) v; char end; }`, so
// offset(end) is sizeof(type) and v's dump is the type's own field list.
//
// Output:
//   --ts  accessors: <T>_get_<f>/<T>_set_<f> for scalars and scalar arrays,
//         <T>_<f>_at(p, i...) addresses for everything, <T>_<f>_LEN, <T>_SIZE,
//         plus LAYOUT_HASH. Only facts that agree across EVERY --build are
//         emitted; a disagreeing fact is omitted with a comment.
//   --c   header with SG_LAYOUT_HASH and a _Static_assert per emitted fact, so
//         any build that includes it proves at compile time that the TS agrees.
//
// Usage:
//   node structgen.mjs --cwd c --header game.h --root Game [--root T ...]
//        --build rules="-Isrc -DMAX_LOGS=128 ..." [--build bots="..."]
//        --ts out.ts --c out.h [--clang /path/to/clang]
import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

const opt = { header: [], root: [], build: [] };
for (let i = 2; i < process.argv.length; i += 2) {
    const k = process.argv[i].replace(/^--/, ''), v = process.argv[i + 1];
    if (Array.isArray(opt[k])) opt[k].push(v); else opt[k] = v;
}
const CLANG = opt.clang || process.env.WASM_CC || 'clang';
const CWD = resolve(opt.cwd || '.');
if (!opt.build.length) opt.build.push('default=');
const TMP = mkdtempSync(join(tmpdir(), 'structgen-'));

// Flags that shape a layout; everything else (-O, -W, -Wl, -c) is irrelevant.
const layoutFlags = (s) => s.split(/\s+/).filter(Boolean).filter((f, i, a) =>
    /^-(D|U|I|isystem|include|f(un)?signed-char|fshort-enums|fpack-struct|m)/.test(f) || a[i - 1] === '-isystem');

// ---- clang: dump the layouts of a batch of probe wrappers ------------------
function probe(flags, wrappers) {
    const src = join(TMP, 'probe.c');
    writeFileSync(src, opt.header.map(h => `#include "${h}"`).join('\n') + '\n' +
        wrappers.map((w, i) => `struct __sg${i} { __typeof__(${w.expr}) v; char end; };`).join('\n') + '\n');
    const out = execFileSync(CLANG, ['--target=wasm32', '-ffreestanding', '-fsyntax-only', '-iquote', CWD, ...layoutFlags(flags),
        '-Xclang', '-fdump-record-layouts-complete', '-Xclang', '-fdump-record-layouts-canonical', src],
        { cwd: CWD, encoding: 'utf8', maxBuffer: 1 << 28 });
    const trees = [];
    for (const block of out.split('*** Dumping AST Record Layout')) {
        const lines = block.split('\n').filter(l => l.includes('|'));
        const head = lines[0] && lines[0].match(/\| struct __sg(\d+)$/);
        if (!head) continue;
        const root = { children: [] }, stack = [root];
        for (const l of lines.slice(1)) {
            const m = l.match(/^\s*(\d+)(?::(\d+)-(\d+))? \| (\s*)(.*)$/);
            if (!m) continue;
            const depth = m[4].length / 2, sp = m[5].lastIndexOf(' ');
            const node = { off: +m[1], lo: m[2] && +m[2], hi: m[3] && +m[3],
                type: m[5].slice(0, sp).replace(/\b(const|volatile) /g, ''), name: m[5].slice(sp + 1), children: [] };
            stack.length = depth;
            stack[depth - 1].children.push(node);
            stack[depth] = node;
        }
        const [v, end] = root.children;
        trees[+head[1]] = { v, size: end.off };
    }
    return trees;
}

// ---- discovery: roots and everything they contain, for one build -----------
const ident = (t) => t.replace(/^(struct|union|enum) /, '');
function scalarKind(t) {
    if (t.includes('*') || t.includes('(')) return 'u';          // pointer: an address
    if (t === '_Bool') return 'b';
    if (t === 'float' || t === 'double') return 'f';
    if (/^unsigned\b/.test(t)) return 'u';
    return 'i';                                                   // char/short/int/long/enum
}
function discover(flags) {
    const types = new Map();                                      // canonical type -> info
    let pending = opt.root.map(r => ({ key: r, expr: `*(${r} *)0`, name: r }));
    while (pending.length) {
        const trees = probe(flags, pending), next = [];
        const want = (key, expr, name) => {
            if (!types.has(key) && !next.some(n => n.key === key)) next.push({ key, expr, name });
            return key;
        };
        pending.forEach((w, i) => {
            const { v, size } = trees[i];
            if (!v.children.length) { types.set(w.key, { name: w.name, size, kind: scalarKind(w.key) }); return; }
            const fields = [];
            const walk = (nodes, base, expr) => {
                for (const n of nodes) {
                    const off = n.off - base;
                    if (n.name === '') { walk(n.children, base, expr); continue; }   // anonymous member
                    if (n.lo !== undefined) { fields.push({ name: n.name, off, lo: n.lo, width: n.hi - n.lo + 1, kind: scalarKind(n.type) }); continue; }
                    const dims = [...n.type.matchAll(/\[(\d+)\]/g)].map(d => +d[1]);
                    const elem = n.type.includes('(') ? n.type : n.type.replace(/(\[\d+\])+$/, '');
                    const ename = /^[A-Za-z_]\w*$/.test(ident(elem)) ? ident(elem) : `${w.name}_${n.name}`;
                    fields.push({ name: n.name, off, dims, type: want(elem, `(${expr}).${n.name}` + '[0]'.repeat(dims.length), ename) });
                }
            };
            walk(v.children, 0, w.expr);
            types.set(w.key, { name: w.name, size, fields, expr: w.expr });
        });
        pending = next.filter(n => !types.has(n.key));
    }
    return types;
}

// ---- merge builds: keep only facts every build agrees on -------------------
const builds = opt.build.map(b => { const i = b.indexOf('='); return { name: b.slice(0, i), types: discover(b.slice(i + 1)) }; });
const [first, ...rest] = builds;
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const notes = [];
const model = [];
for (const [key, t] of first.types) {
    const others = rest.map(b => b.types.get(key));
    if (others.some(o => !o)) { notes.push(`${t.name}: missing in some build`); continue; }
    const rec = { key, name: t.name, expr: t.expr, kind: t.kind };
    rec.size = others.every(o => o.size === t.size) ? t.size : (notes.push(`${t.name}_SIZE differs across builds`), undefined);
    if (t.fields) rec.fields = t.fields.flatMap(f => {
        const of = others.map(o => o.fields && o.fields.find(g => g.name === f.name));
        const stride = (x, b) => b.types.get(x.type)?.size;
        if (of.some((g, j) => !g || g.off !== f.off || g.type !== f.type || g.lo !== f.lo || g.width !== f.width
            || (f.type && stride(g, rest[j]) !== stride(f, first)))) {
            notes.push(`${t.name}.${f.name} differs across builds: omitted`); return [];
        }
        const dimsAgree = of.every(g => same(g.dims, f.dims));
        if (!dimsAgree) notes.push(`${t.name}.${f.name} length differs across builds: no _LEN`);
        return [{ ...f, lens: dimsAgree }];
    });
    model.push(rec);
}
const byKey = new Map(model.map(r => [r.key, r]));

// ---- emit TypeScript --------------------------------------------------------
const RD = { i1: 'm.i8[A]', u1: 'm.u8[A]', b1: 'm.u8[A] !== 0', i2: 'm.dv.getInt16(A, true)', u2: 'm.dv.getUint16(A, true)',
    i4: 'm.dv.getInt32(A, true)', u4: 'm.dv.getUint32(A, true)', f4: 'm.dv.getFloat32(A, true)', f8: 'm.dv.getFloat64(A, true)',
    i8: 'm.dv.getBigInt64(A, true)', u8: 'm.dv.getBigUint64(A, true)' };
const WR = { i1: 'm.i8[A] = v', u1: 'm.u8[A] = v', b1: 'm.u8[A] = v ? 1 : 0', i2: 'm.dv.setInt16(A, v, true)', u2: 'm.dv.setUint16(A, v, true)',
    i4: 'm.dv.setInt32(A, v, true)', u4: 'm.dv.setUint32(A, v, true)', f4: 'm.dv.setFloat32(A, v, true)', f8: 'm.dv.setFloat64(A, v, true)',
    i8: 'm.dv.setBigInt64(A, v, true)', u8: 'm.dv.setBigUint64(A, v, true)' };
const ts = [];                                                    // accessor body; hashed below
for (const r of model) {
    if (!r.fields) continue;
    ts.push(`// ${r.name}`);
    if (r.size !== undefined) ts.push(`export const ${r.name}_SIZE = ${r.size};`);
    for (const f of r.fields) {
        const P = `${r.name}_${f.name}`;
        if (f.width) {                                            // bitfield
            const wide = f.lo + f.width > 8, W = wide ? 'm.dv.getUint32(p + OFF, true)' : 'm.u8[p + OFF]';
            const mask = (((2 ** f.width) - 1) * 2 ** f.lo) >>> 0, sh = 32 - f.lo - f.width;
            const get = `(${W} << ${sh}) ${f.kind === 'i' ? '>>' : '>>>'} ${32 - f.width}`;
            const put = `(${W} & ${~mask}) | ((${f.kind === 'b' ? '+v' : 'v'} << ${f.lo}) & ${mask})`;
            ts.push(`export const ${r.name}_get_${f.name} = (m: Mem, p: number) => ${f.kind === 'b' ? `(${get}) !== 0` : get};`.replace(/OFF/g, f.off));
            ts.push((wide ? `export const ${r.name}_set_${f.name} = (m: Mem, p: number, v: number) => { m.dv.setUint32(p + OFF, ${put} >>> 0, true); };`
                : `export const ${r.name}_set_${f.name} = (m: Mem, p: number, v: ${f.kind === 'b' ? 'boolean' : 'number'}) => { m.u8[p + OFF] = ${put}; };`).replace(/OFF/g, f.off));
            continue;
        }
        const t = byKey.get(f.type), idx = f.dims.map((_, j) => `, i${j}: number`).join('');
        let addr = `p + ${f.off}`, stride = t.size;
        for (let j = f.dims.length - 1; j >= 0; j--) { addr += ` + i${j} * ${stride}`; stride *= f.dims[j]; }
        ts.push(`export const ${P}_at = (p: number${idx}) => ${addr};`);
        if (f.lens) f.dims.forEach((d, j) => ts.push(`export const ${P}_LEN${j || ''} = ${d};`));
        const code = t.kind && `${t.kind}${t.size}`;
        if (code && RD[code]) {
            const vt = t.kind === 'b' ? 'boolean' : t.size === 8 && t.kind !== 'f' ? 'bigint' : 'number';
            ts.push(`export const ${r.name}_get_${f.name} = (m: Mem, p: number${idx}) => ${RD[code].replace('A', addr)};`);
            ts.push(`export const ${r.name}_set_${f.name} = (m: Mem, p: number${idx}, v: ${vt}) => { ${WR[code].replace('A', addr)}; };`);
        }
    }
}
// The layout hash is FNV-1a over the emitted accessor text: every offset, size,
// width and kind the TS relies on is in it, and nothing tool-specific is.
let hash = 0x811c9dc5;
for (const ch of ts.map(l => l + '\n').join('')) hash = Math.imul(hash ^ ch.charCodeAt(0), 16777619) >>> 0;
const HASH = '0x' + hash.toString(16).padStart(8, '0');
if (opt.ts) writeFileSync(opt.ts, [`// GENERATED by tools/structgen - do not edit.`,
    `// builds: ${opt.build.map(b => b.slice(0, b.indexOf('='))).join(', ')}; roots: ${opt.root.join(', ')}`,
    ...notes.map(n => `// note: ${n}`),
    `export const LAYOUT_HASH = ${HASH};`,
    `export interface Mem { u8: Uint8Array; i8: Int8Array; dv: DataView }`,
    `export const memOf = (b: ArrayBuffer): Mem => ({ u8: new Uint8Array(b), i8: new Int8Array(b), dv: new DataView(b) });`,
    ...ts].join('\n') + '\n');

// ---- emit C: the same facts as compile-time assertions ---------------------
const c = [`// GENERATED by tools/structgen - do not edit.`, `#ifndef STRUCTGEN_${HASH}`, `#define STRUCTGEN_${HASH}`,
    ...opt.header.map(h => `#include "${h}"`), `#define SG_LAYOUT_HASH_${opt.root[0]} ${HASH}u`];
for (const r of model) {
    if (!r.fields) continue;
    const T = `__typeof__(${r.expr})`;
    if (r.size !== undefined) c.push(`_Static_assert(sizeof(${T}) == ${r.size}, "${r.name} size");`);
    for (const f of r.fields) if (!f.width) {
        c.push(`_Static_assert(__builtin_offsetof(${T}, ${f.name}) == ${f.off}, "${r.name}.${f.name}");`);
        const es = byKey.get(f.type).size;
        if (f.lens && es !== undefined)
            c.push(`_Static_assert(sizeof(((${T} *)0)->${f.name}) == ${f.dims.reduce((a, d) => a * d, es)}, "${r.name}.${f.name} size");`);
    }
}
c.push('#endif');
if (opt.c) writeFileSync(opt.c, c.join('\n') + '\n');
process.stderr.write(`structgen: ${model.filter(r => r.fields).length} records, hash ${HASH}${notes.length ? `, ${notes.length} notes` : ''}\n`);
