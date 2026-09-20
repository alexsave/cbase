// structgen verification module (wasm32). Independent of the generator: C code
// fills and checks structs through the COMPILER's own field access and offsetof,
// and the TS test reads/writes the same bytes through the generated accessors.
#include "kinds.h"
#include "anim_plan.h"
#include "legal.h"

static Kinds k = {
    .tag = -5, .big = -1234567890123LL, .ubig = 0xF123456789ABCDEFull, .f = 1.5f, .d = -2.25,
    .e = K_NEG, .p = KP_B, .name = (const char *)0x1234,
    .cards = { { { -1, -1 }, { 3, 13 } }, { { 0, 1 }, { 2, 7 } }, { { -4, 15 }, { 1, -16 } } },
    .n = { -300, 3.5 }, .narr = { { 1, 0.5 }, { -2, 9.75 } },
    .u = { .i = -42 }, .w = 0x7abc, .pt = { { 1, 2 }, { 250, 251 } },
    .flag = 1, .bits = 5, .wide = 0xABCDE, .bflag = 1, .text = "abcd", .u16s = { 1, 65535, 300 }, .i32 = -99,
    .packed = { 4000, -1000, 1, 200 },
};
Kinds *k_ptr(void) { return &k; }

// Pointer fields: what they point at, and where, as C sees it.
static KCard hand_cards[3] = { { 1, 6 }, { -2, 11 }, { 3, -7 } };
static int16_t vals[4] = { -1, 2, -30000, 4 };
static struct KNode nodes[2];
static void k_fn(void) {}
void k_ptrs_fill(void) {
    k.hand = hand_cards; k.vals = vals; k.opaque = (void *)0x5678; k.fn = k_fn; k.handle = 0;
    nodes[0].v = 41; nodes[0].next = &nodes[1];
    nodes[1].v = -42; nodes[1].next = 0;
    k.list = &nodes[0];
}
unsigned k_hand_addr(void) { return (unsigned)(uintptr_t)hand_cards; }
unsigned k_vals_addr(void) { return (unsigned)(uintptr_t)vals; }
unsigned k_fn_addr(void) { return (unsigned)(uintptr_t)k_fn; }
unsigned k_node_addr(int i) { return (unsigned)(uintptr_t)&nodes[i]; }
void k_vals_null(void) { k.vals = 0; }
// vals points three bytes before the end of memory: element 0 fits, element 1 does not.
unsigned k_vals_at_end(void) { k.vals = (int16_t *)(uintptr_t)(__builtin_wasm_memory_size(0) * 65536 - 3); return (unsigned)(uintptr_t)k.vals; }
unsigned mem_bytes(void) { return (unsigned)(__builtin_wasm_memory_size(0) * 65536); }

// After TS wrote the "second" values, report each mismatching field as a bit.
int k_check(void) {
    int bad = 0, b = 0;
#define CHK(cond) do { if (!(cond)) bad |= 1 << b; b++; } while (0)
    CHK(k.tag == 17);            CHK(k.big == 77LL);          CHK(k.ubig == 0x8000000000000001ull);
    CHK(k.f == -0.125f);         CHK(k.d == 1e300);           CHK(k.e == K_POS);
    CHK(k.cards[2][1].s == -3 && k.cards[2][1].v == 9 && k.cards[2][0].s == -4 && k.cards[2][0].v == 15);
    CHK(k.narr[1].a == 12345 && k.narr[1].d == -0.5);         CHK(k.u.b[3] == 0x80);
    CHK(k.pt[1].y == 7 && k.pt[1].x == 250);                  CHK(k.flag == 0);
    CHK(k.bits == 2 && k.wide == 0xFFFFF && k.bflag == 0);    CHK(k.text[4] == 'z');
    CHK(k.u16s[2] == 40000);     CHK(k.i32 == -2147483647 - 1);
    CHK(k.packed.lo == 1 && k.packed.mid == -1024 && k.packed.on == 0 && k.packed.top == 255);
#undef CHK
    return bad;
}

// Constants as C sees them, for the --const check.
static const long long consts[] = { K_NEG, K_POS, KFLAG_LOW, KFLAG_HIGH, KFLAG_NEG };
long long const_at(int i) { return consts[i]; }
// A string C wrote, for the char[N] reader.
void k_set_text_utf8(void) { const char t[5] = { (char)0xc3, (char)0xa9, 't', 0, 'x' }; for (int i = 0; i < 5; i++) k.text[i] = t[i]; }
int k_text_byte(int i) { return (unsigned char)k.text[i]; }

// Independent offsetof probe for the real anim_plan.h / legal.h structs.
static const unsigned offs[] = {
    sizeof(AnimPlan), __builtin_offsetof(AnimPlan, steps[3].hand[2]), __builtin_offsetof(AnimPlan, pre.flipped),
    __builtin_offsetof(AnimPlan, veil_ids[7]), __builtin_offsetof(AnimPlan, total_ms),
    sizeof(AnimBeats), __builtin_offsetof(AnimBeats, beats[5].placed_ids), __builtin_offsetof(AnimBeats, first_good_mask),
    __builtin_offsetof(AnimEvent, cards), __builtin_offsetof(AnimEvent, mask_cards),
    sizeof(LegalMoves), __builtin_offsetof(LegalMoves, moves[4095].attack_cards[27]), __builtin_offsetof(LegalMoves, moves[1].cards[3]),
};
unsigned off_at(int i) { return offs[i]; }

static AnimBeats beats;
AnimBeats *beats_fill(void) {
    beats.beats[5].placed_ids = 0x0123456789ABCDEFull;
    beats.beats[127].outs_mask = 0x80000001u;
    beats.first_good_mask = -7;
    return &beats;
}
static AnimPlan plan;
AnimPlan *plan_fill(void) {
    plan.pre.flipped.suit = -1; plan.pre.flipped.value = 12;
    plan.steps[3].hand[2] = -123456;
    return &plan;
}

// --snapshot (gen/snap.ts): a record C filled, for the snapshot reader.
#include "snap.h"
static Snap snap;
Snap *snap_fill(int n_pairs, int n_items, int n_text) {
    static const char text[8] = { 'a', (char)0xc3, (char)0xa9, 'z', 'y', 'x', 'w', 'v' };
    snap.n_pairs = (int8_t)n_pairs; snap.n_items = (uint8_t)n_items; snap.n_text = (uint16_t)n_text;
    snap.flag = 1; snap.w = -12345; snap.u = 0xF0000001u; snap.big = -9876543210LL; snap.d = 0.625;
    snap.bits = 6; snap.sbits = -9;
    for (int i = 0; i < 4; i++) { snap.pairs[i].a.s = (int8_t)(i - 1); snap.pairs[i].a.v = (int8_t)(i + 10); snap.pairs[i].b.s = -2; snap.pairs[i].b.v = -2; }
    for (int i = 0; i < 3; i++) { snap.items[i].len = (uint8_t)(i + 1); for (int j = 0; j < 6; j++) snap.items[i].text[j] = (char)('p' + i + j); snap.items[i].score = (int16_t)(100 * i - 150); }
    for (int i = 0; i < 8; i++) snap.text[i] = text[i];
    snap.cstr[0] = 'h'; snap.cstr[1] = 'i'; snap.cstr[2] = 0; snap.cstr[3] = 'X';
    snap.nums[0] = 7; snap.nums[1] = -8; snap.nums[2] = 9;
    snap.card.s = 3; snap.card.v = 13;
    return &snap;
}

// --snapshot with pointers (gen/snap.ts readSPtr): mode 0 is a good record,
// 1 a NULL vals with a nonzero count, 2 a vals running past the end of memory.
static SPtr sptr;
static const int16_t sptr_vals[3] = { 5, -6, 7 };
SPtr *sptr_fill(int mode) {
    sptr.vals = sptr_vals; sptr.n_vals = 3;
    sptr.items = snap.items; sptr.n_items = 2;
    sptr.name = "h\xc3\xa9llo!"; sptr.name_len = 6;   // not the '!': a counted string is exactly its bytes
    sptr.none = 0; sptr.n_none = 0;
    sptr.tail = -77;
    if (mode == 1) sptr.vals = 0;
    if (mode == 2) sptr.vals = (const int16_t *)(uintptr_t)(__builtin_wasm_memory_size(0) * 65536 - 4);
    return &sptr;
}

// --writer (gen/snap.ts writeSnap): a second record, filled with junk, that the
// generated writer writes; snap_written_equals() then compares it with `snap`
// the way C reads a Snap - every array only to its count, strings by count or NUL.
static Snap written;
Snap *snap_scratch(void) {
    for (unsigned i = 0; i < sizeof written; i++) ((unsigned char *)&written)[i] = (unsigned char)(0xA5 ^ i);
    return &written;
}
static int card_eq_k(KCard a, KCard b) { return a.s == b.s && a.v == b.v; }
int snap_written_equals(void) {
    const Snap *a = &snap, *b = &written;
    if (a->n_pairs != b->n_pairs || a->n_items != b->n_items || a->n_text != b->n_text) return 0;
    if (a->flag != b->flag || a->w != b->w || a->u != b->u || a->big != b->big || a->d != b->d) return 0;
    if (a->bits != b->bits || a->sbits != b->sbits) return 0;
    for (int i = 0; i < a->n_pairs; i++)
        if (!card_eq_k(a->pairs[i].a, b->pairs[i].a) || !card_eq_k(a->pairs[i].b, b->pairs[i].b)) return 0;
    for (int i = 0; i < a->n_items; i++) {
        if (a->items[i].len != b->items[i].len || a->items[i].score != b->items[i].score) return 0;
        for (int j = 0; j < a->items[i].len; j++) if (a->items[i].text[j] != b->items[i].text[j]) return 0;
    }
    for (int i = 0; i < a->n_text; i++) if (a->text[i] != b->text[i]) return 0;
    for (int i = 0; i < 6; i++) { if (a->cstr[i] != b->cstr[i]) return 0; if (!a->cstr[i]) break; }
    for (int i = 0; i < 3; i++) if (a->nums[i] != b->nums[i]) return 0;
    return card_eq_k(a->card, b->card);
}
