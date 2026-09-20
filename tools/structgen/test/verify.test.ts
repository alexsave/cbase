// Generator genericity check: accessors generated from kinds.h (every field kind)
// and from the real anim_plan.h / legal.h, checked against a wasm32 module whose
// C code reads, writes and offsetof()s the same structs (test/verify.c).
// Build first: bash tools/structgen/gen.sh (writes gen/kinds.ts, gen/snap.ts,
// sdk/ts/gen/*.ts and the wasm below). The wasm is a build output, not a
// committed fixture: its bytes are whichever clang linked it, so it is made on
// demand and lives in build/ with the generator's own binary.
// Run: node --import tsx --test tools/structgen/test/verify.test.ts
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as K from '../gen/kinds.ts';
import * as S from '../gen/snap.ts';
import * as A from '../../../sdk/ts/gen/anim.bots.ts';

const wasmAt = new URL('../build/verify.wasm', import.meta.url);
let wasm: Buffer;
try {
    wasm = readFileSync(wasmAt);
} catch {
    throw new Error(`structgen: no ${wasmAt.pathname} - build it with \`bash tools/structgen/gen.sh\``);
}
const ex = new WebAssembly.Instance(new WebAssembly.Module(wasm), {}).exports as Record<string, any>;
const m = K.memOf(ex.memory.buffer);

test('kinds: generated getters read what C initialized', () => {
    const p = ex.k_ptr();
    assert.equal(K.Kinds_get_tag(m, p), -5);
    assert.equal(K.Kinds_get_big(m, p), -1234567890123n);
    assert.equal(K.Kinds_get_ubig(m, p), 0xF123456789ABCDEFn);
    assert.equal(K.Kinds_get_f(m, p), 1.5);
    assert.equal(K.Kinds_get_d(m, p), -2.25);
    assert.equal(K.Kinds_get_e(m, p), -1);
    assert.equal(K.Kinds_get_p(m, p), 1);
    assert.equal(K.Kinds_name_ptr(m, p), 0x1234);
    const card = (i: number, j: number) => { const c = K.Kinds_cards_at(p, i, j); return [K.KCard_get_s(m, c), K.KCard_get_v(m, c)]; };
    assert.deepEqual([card(0, 0), card(0, 1), card(1, 0), card(1, 1), card(2, 0), card(2, 1)],
        [[-1, -1], [3, 13], [0, 1], [2, 7], [-4, 15], [1, -16]]);
    assert.equal(K.KNamed_get_a(m, K.Kinds_n_at(p)), -300);
    assert.equal(K.KNamed_get_d(m, K.Kinds_narr_at(p, 1)), 9.75);
    assert.equal(K.Kinds_u_get_i(m, K.Kinds_u_at(p)), -42);
    assert.equal(K.Kinds_get_w(m, p), 0x7abc);
    assert.equal(K.Kinds_pt_get_x(m, K.Kinds_pt_at(p, 1)), 250);
    assert.equal(K.Kinds_get_flag(m, p), true);
    assert.equal(K.Kinds_get_bits(m, p), 5);
    assert.equal(K.Kinds_get_wide(m, p), 0xABCDE);
    assert.equal(K.Kinds_get_bflag(m, p), true);
    assert.equal(K.Kinds_get_text(m, p, 3), 'd'.charCodeAt(0));
    assert.equal(K.Kinds_get_u16s(m, p, 1), 65535);
    assert.equal(K.Kinds_get_i32(m, p), -99);
});

test('kinds: generated setters write what C reads back', () => {
    const p = ex.k_ptr();
    K.Kinds_set_tag(m, p, 17); K.Kinds_set_big(m, p, 77n); K.Kinds_set_ubig(m, p, 0x8000000000000001n);
    K.Kinds_set_f(m, p, -0.125); K.Kinds_set_d(m, p, 1e300); K.Kinds_set_e(m, p, 7);
    const c21 = K.Kinds_cards_at(p, 2, 1);
    K.KCard_set_s(m, c21, -3); K.KCard_set_v(m, c21, 9);       // must not disturb cards[2][0]
    const n1 = K.Kinds_narr_at(p, 1);
    K.KNamed_set_a(m, n1, 12345); K.KNamed_set_d(m, n1, -0.5);
    K.Kinds_u_set_b(m, K.Kinds_u_at(p), 3, 0x80);
    K.Kinds_pt_set_y(m, K.Kinds_pt_at(p, 1), 7);
    K.Kinds_set_flag(m, p, false);
    K.Kinds_set_bits(m, p, 2); K.Kinds_set_wide(m, p, 0xFFFFF); K.Kinds_set_bflag(m, p, false);
    K.Kinds_set_text(m, p, 4, 'z'.charCodeAt(0));
    K.Kinds_set_u16s(m, p, 2, 40000); K.Kinds_set_i32(m, p, -2147483648);
    const kp = K.Kinds_packed_at(p);   // all-bitfield record: read by field, by raw + unpack, set by raw + pack
    assert.deepEqual([K.KPacked_get_lo(m, kp), K.KPacked_get_mid(m, kp), K.KPacked_get_on(m, kp), K.KPacked_get_top(m, kp)], [4000, -1000, true, 200]);
    const r = K.KPacked_raw_get(m, kp);
    assert.deepEqual([K.KPacked_unpack_lo(r), K.KPacked_unpack_mid(r), K.KPacked_unpack_on(r), K.KPacked_unpack_top(r)], [4000, -1000, true, 200]);
    K.KPacked_raw_set(m, kp, K.KPacked_pack(1, -1024, false, 255));
    assert.equal(ex.k_check(), 0, `C saw wrong fields, bitmask ${ex.k_check().toString(2)}`);
});

test('pointers: an address getter, 0 for NULL, and never a setter', () => {
    const p = ex.k_ptr();
    ex.k_ptrs_fill();
    const exported = Object.keys(K);
    assert.deepEqual(exported.filter(k => /^Kinds_(get|set)_(name|hand|vals|opaque|fn|handle|list)$/.test(k)), [],
        'a pointer field is not a plain u32: no getter or setter');
    assert.deepEqual(exported.filter(k => /^KNode_(get|set)_next$/.test(k)), []);
    assert.equal(K.Kinds_hand_ptr(m, p), ex.k_hand_addr());
    assert.equal(K.Kinds_vals_ptr(m, p), ex.k_vals_addr());
    assert.equal(K.Kinds_opaque_ptr(m, p), 0x5678);
    assert.equal(K.Kinds_fn_ptr(m, p), ex.k_fn_addr());
    assert.equal(K.Kinds_handle_ptr(m, p), 0);
    assert.equal(K.Kinds_list_ptr(m, p), ex.k_node_addr(0));
    assert.deepEqual(exported.filter(k => /_deref_at$/.test(k)).sort(),
        ['KNode_next_deref_at', 'Kinds_hand_deref_at', 'Kinds_list_deref_at', 'Kinds_name_deref_at', 'Kinds_vals_deref_at'],
        'void *, a function pointer and an incomplete pointee have nothing to follow');
});

test('pointers: deref_at gives element addresses that read what C wrote', () => {
    const p = ex.k_ptr();
    ex.k_ptrs_fill();
    const cards = [0, 1, 2].map(i => { const c = K.Kinds_hand_deref_at(m, p, i); return [K.KCard_get_s(m, c), K.KCard_get_v(m, c)]; });
    assert.deepEqual(cards, [[1, 6], [-2, 11], [3, -7]]);
    assert.equal(K.Kinds_hand_deref_at(m, p, 2), ex.k_hand_addr() + 2 * K.KCard_SIZE);
    assert.deepEqual([0, 1, 2, 3].map(i => m.dv.getInt16(K.Kinds_vals_deref_at(m, p, i), true)), [-1, 2, -30000, 4]);
    const n0 = K.Kinds_list_deref_at(m, p, 0);
    assert.equal(K.KNode_get_v(m, n0), 41);
    const n1 = K.KNode_next_deref_at(m, n0, 0);
    assert.equal(n1, ex.k_node_addr(1));
    assert.equal(K.KNode_get_v(m, n1), -42);
    assert.equal(K.KNode_next_ptr(m, n1), 0);
    assert.throws(() => K.KNode_next_deref_at(m, n1, 0), (e: Error) => e instanceof RangeError && /KNode\.next: NULL/.test(e.message));
});

test('pointers: deref_at throws on NULL, a negative index, and past the end of memory', () => {
    const p = ex.k_ptr();
    ex.k_ptrs_fill();
    assert.throws(() => K.Kinds_vals_deref_at(m, p, -1), (e: Error) => e instanceof RangeError && /Kinds\.vals: element -1/.test(e.message));
    ex.k_vals_null();
    assert.throws(() => K.Kinds_vals_deref_at(m, p, 0), (e: Error) => e instanceof RangeError && /Kinds\.vals: NULL/.test(e.message));
    const end = ex.k_vals_at_end();
    assert.equal(end + 3, ex.mem_bytes());
    assert.equal(K.Kinds_vals_deref_at(m, p, 0), end, 'element 0 ends one byte before the end of memory');
    assert.throws(() => K.Kinds_vals_deref_at(m, p, 1), (e: Error) => e instanceof RangeError && /Kinds\.vals: element 1 at \d+ is outside wasm memory/.test(e.message));
    ex.k_ptrs_fill();
});

test('--snapshot: a counted pointer is followed and copied; NULL with a count, or past memory, throws', () => {
    ex.snap_fill(2, 3, 3);
    const s = S.readSPtr(m, ex.sptr_fill(0));
    assert.deepEqual(s, {
        vals: [5, -6, 7], items: [{ text: 'p', score: -150 }, { text: 'qr', score: -50 }],
        name: `h${String.fromCharCode(0xe9)}llo`, none: [], tail: -77,
    });
    ex.snap_fill(0, 0, 0);
    assert.deepEqual(s.items[1], { text: 'qr', score: -50 }, 'the copy holds no view of wasm memory');
    assert.throws(() => S.readSPtr(m, ex.sptr_fill(1)), (e: Error) => e instanceof RangeError && /SPtr\.vals: NULL with a count of 3/.test(e.message));
    assert.throws(() => S.readSPtr(m, ex.sptr_fill(2)), (e: Error) => e instanceof RangeError && /SPtr\.vals: 3 elements at \d+ are outside wasm memory/.test(e.message));
    assert.equal(Object.keys(S).some(k => /^writeSPtr$/.test(k)), false);
});

test('anim_plan.h / legal.h: generated addresses equal compiler offsetof', () => {
    const got = [
        A.AnimPlan_SIZE, A.AnimPlanStep_hand_at(A.AnimPlan_steps_at(0, 3), 2), A.AnimCounts_flipped_at(A.AnimPlan_pre_at(0)),
        A.AnimPlan_veil_ids_at(0, 7), A.AnimPlan_total_ms_at(0),
        A.AnimBeats_SIZE, A.AnimBeat_placed_ids_at(A.AnimBeats_beats_at(0, 5)), A.AnimBeats_first_good_mask_at(0),
        A.AnimEvent_cards_at(0), A.AnimEvent_mask_cards_at(0),
        A.LegalMoves_SIZE, A.LegalMove_attack_cards_at(A.LegalMoves_moves_at(0, 4095), 27), A.LegalMove_cards_at(A.LegalMoves_moves_at(0, 1), 3),
    ];
    assert.deepEqual(got, got.map((_, i) => ex.off_at(i)));
});

test('anim_plan.h: values C wrote read back through nested generated accessors', () => {
    const b = ex.beats_fill();
    assert.equal(A.AnimBeat_get_placed_ids(m, A.AnimBeats_beats_at(b, 5)), 0x0123456789ABCDEFn);
    assert.equal(A.AnimBeat_get_outs_mask(m, A.AnimBeats_beats_at(b, 127)), 0x80000001);
    assert.equal(A.AnimBeats_get_first_good_mask(m, b), -7);
    const pl = ex.plan_fill();
    const f = A.AnimCounts_flipped_at(A.AnimPlan_pre_at(pl));
    assert.deepEqual([A.Card_get_suit(m, f), A.Card_get_value(m, f)], [-1, 12]);
    assert.equal(A.AnimPlanStep_get_hand(m, A.AnimPlan_steps_at(pl, 3), 2), -123456);
});

test('--const: enum constants and integer #defines carry the values C compiled', () => {
    assert.deepEqual([K.K_NEG, K.K_POS, K.KFLAG_LOW, K.KFLAG_HIGH, K.KFLAG_NEG],
        [0, 1, 2, 3, 4].map(i => Number(ex.const_at(i))));
});

test('--snapshot: a plain object of what C wrote, arrays cut to their counts, strings by count or NUL', () => {
    const s = S.readSnap(m, ex.snap_fill(2, 3, 3));
    assert.deepEqual(s, {
        flag: true, w: -12345, u: 0xF0000001, big: -9876543210n, d: 0.625, bits: 6, sbits: -9,
        pairs: [{ a: { s: -1, v: 10 }, b: { s: -2, v: -2 } }, { a: { s: 0, v: 11 }, b: { s: -2, v: -2 } }],
        items: [{ text: 'p', score: -150 }, { text: 'qr', score: -50 }, { text: 'rst', score: 50 }],
        text: `a${String.fromCharCode(0xe9)}`, cstr: 'hi', nums: [7, -8, 9], card: { s: 3, v: 13 },
    }, 'the counts are not copied: they are the arrays\' lengths');
});

test('--snapshot: the copy holds no view of wasm memory', () => {
    const s = S.readSnap(m, ex.snap_fill(4, 1, 8));
    ex.snap_fill(0, 0, 0);
    assert.equal(s.pairs.length, 4);
    assert.equal(s.items[0].text, 'p');
    assert.equal(s.text.length, 7, 'eight bytes of UTF-8, seven characters');
});

test('--snapshot: a count outside its array throws rather than reading past it', () => {
    assert.throws(() => S.readSnap(m, ex.snap_fill(5, 0, 0)), /Snap\.pairs: count 5 is outside 0\.\.4/);
    assert.throws(() => S.readSnap(m, ex.snap_fill(-1, 0, 0)), /Snap\.pairs: count -1 is outside 0\.\.4/);
    assert.throws(() => S.readSnap(m, ex.snap_fill(0, 4, 0)), /Snap\.items: count 4 is outside 0\.\.3/);
    assert.throws(() => S.readSnap(m, ex.snap_fill(0, 0, 9)), /Snap\.text: count 9 is outside 0\.\.8/);
});

test('--writer: C reads back what the writer wrote, arrays to their counts, strings by count or NUL', () => {
    for (const [pairs, items, text] of [[2, 3, 3], [0, 0, 0], [4, 1, 8]]) {
        const s = S.readSnap(m, ex.snap_fill(pairs, items, text));
        const q = ex.snap_scratch();
        S.writeSnap(m, q, s);
        assert.equal(ex.snap_written_equals(), 1, `C sees the same Snap (${pairs}, ${items}, ${text})`);
        assert.deepEqual(S.readSnap(m, q), s, 'and it reads back as the snapshot written');
    }
});

test('--writer: a value that does not fit throws rather than truncate', () => {
    const s = S.readSnap(m, ex.snap_fill(2, 3, 3));
    const q = ex.snap_scratch();
    const pair = s.pairs[0];
    assert.throws(() => S.writeSnap(m, q, { ...s, pairs: [pair, pair, pair, pair, pair] }), /Snap\.pairs: 5 elements do not fit in 4/);
    assert.throws(() => S.writeSnap(m, q, { ...s, text: 'abcdefghi' }), /Snap\.text: 9 UTF-8 bytes do not fit in 8/);
    assert.throws(() => S.writeSnap(m, q, { ...s, text: `abcdefg${String.fromCharCode(0xe9)}` }), /Snap\.text: 9 UTF-8 bytes do not fit in 8/);
    assert.throws(() => S.writeSnap(m, q, { ...s, nums: [1, 2] }), /Snap\.nums: 2 elements, not 3/);
    assert.throws(() => S.writeSnap(m, q, { ...s, items: [{ text: 'sevenss', score: 1 }] }), /SItem\.text: 7 UTF-8 bytes do not fit in 6/);
    assert.throws(() => S.writeSnap(m, q, { ...s, cstr: 'sixsix' }), /Snap\.cstr: 6 UTF-8 bytes do not fit in 5/);
});

test('char[N]: UTF-8, NUL-terminated, at most N-1 bytes; the setter throws rather than truncate', () => {
    const p = ex.k_ptr();
    const bytes = () => [0, 1, 2, 3, 4].map(i => ex.k_text_byte(i));
    const eacute = String.fromCharCode(0xe9), nul = String.fromCharCode(0);
    ex.k_set_text_utf8();                                 // C wrote e-acute, 't', NUL, junk
    assert.equal(K.Kinds_get_text_str(m, p), `${eacute}t`);
    K.Kinds_set_text_str(m, p, 'ab');
    assert.deepEqual(bytes(), [97, 98, 0, 0, 0]);
    K.Kinds_set_text_str(m, p, eacute + eacute);          // 4 UTF-8 bytes: exactly fits
    assert.deepEqual(bytes(), [0xc3, 0xa9, 0xc3, 0xa9, 0]);
    assert.equal(K.Kinds_get_text_str(m, p), eacute + eacute);
    K.Kinds_set_text_str(m, p, 'abcd');
    assert.equal(K.Kinds_get_text_str(m, p), 'abcd');
    assert.throws(() => K.Kinds_set_text_str(m, p, 'abcde'), /5 UTF-8 bytes do not fit in 4/);
    assert.throws(() => K.Kinds_set_text_str(m, p, `ab${eacute}e`), /5 UTF-8 bytes do not fit in 4/);
    assert.throws(() => K.Kinds_set_text_str(m, p, `a${nul}b`), /NUL/);
    assert.equal(K.Kinds_get_text_str(m, p), 'abcd', 'a rejected write leaves the field untouched');
});
