#include "swift_probe.h"
#include "snap.h"
#include <string.h>

static Snap g_snap;
static SPtr g_sptr;
static Snap g_scratch;

// What the pointers of SPtr point AT. Static, so they outlive every read.
static const int16_t g_vals[3] = { 11, -12, 13 };
static const SItem g_items[2] = {
    { .len = 2, .text = { 'o', 'k' }, .score = 101 },
    { .len = 5, .text = { 'h', (char)0xC3, (char)0xA9, 'l', 'o' }, .score = -202 },
};
static const char g_name[5] = { (char)0xC3, (char)0x9C, 'n', (char)0xC3, (char)0xAF };   // "Ünï"

void probe_fill(void) {
    memset(&g_snap, 0, sizeof g_snap);
    memset(&g_scratch, 0, sizeof g_scratch);
    g_snap.n_pairs = 2;
    g_snap.n_items = 2;
    g_snap.n_text = 6;                 // "héllo" is 6 UTF-8 bytes
    g_snap.flag = true;
    g_snap.w = -1234;
    g_snap.u = 4000000000u;            // past INT32_MAX: an unsigned field is not a signed one
    g_snap.big = -1234567890123LL;
    g_snap.d = 2.5;
    g_snap.bits = 5;                   // unsigned:3
    g_snap.sbits = -7;                 // signed:5, and its sign must survive
    g_snap.pairs[0].a.s = 1;  g_snap.pairs[0].a.v = 2;
    g_snap.pairs[0].b.s = -2; g_snap.pairs[0].b.v = -3;
    g_snap.pairs[1].a.s = 3;  g_snap.pairs[1].a.v = 13;
    g_snap.pairs[1].b.s = 0;  g_snap.pairs[1].b.v = 1;
    g_snap.pairs[2].a.s = -4; g_snap.pairs[2].a.v = -15;   // past n_pairs: must NOT be copied
    memcpy(&g_snap.items[0], &g_items[0], sizeof(SItem));
    memcpy(&g_snap.items[1], &g_items[1], sizeof(SItem));
    g_snap.items[2].len = 1; g_snap.items[2].text[0] = 'z'; g_snap.items[2].score = 999;
    memcpy(g_snap.text, "h\xC3\xA9llo", 6);
    memcpy(g_snap.cstr, "ab", 3);
    g_snap.nums[0] = 7; g_snap.nums[1] = -8; g_snap.nums[2] = 9;
    g_snap.card.s = 3; g_snap.card.v = -4;

    memset(&g_sptr, 0, sizeof g_sptr);
    g_sptr.vals = g_vals;      g_sptr.n_vals = 3;
    g_sptr.items = g_items;    g_sptr.n_items = 2;
    g_sptr.name = g_name;      g_sptr.name_len = 5;
    g_sptr.none = 0;           g_sptr.n_none = 0;   // NULL with a count of 0 is an empty array
    g_sptr.tail = 4242;
}

const void *probe_snap(void) { return &g_snap; }
const void *probe_sptr(void) { return &g_sptr; }

int probe_sizeof_snap(void) { return (int)sizeof(Snap); }
int probe_sizeof_sptr(void) { return (int)sizeof(SPtr); }
int probe_sizeof_item(void) { return (int)sizeof(SItem); }
int probe_sizeof_pair(void) { return (int)sizeof(SPair); }
int probe_sizeof_card(void) { return (int)sizeof(KCard); }

void probe_set_n_pairs(int n) { g_snap.n_pairs = (int8_t)n; }
void probe_set_n_text(int n) { g_snap.n_text = (uint16_t)n; }
void probe_set_ptr_none(int n) { g_sptr.n_none = (uint8_t)n; }
void probe_set_ptr_name_len(int n) { g_sptr.name_len = (uint16_t)n; }
void probe_set_ptr_items(int n) { g_sptr.n_items = n; }

void *probe_scratch(void) { return &g_scratch; }
int probe_scratch_w(void) { return g_scratch.w; }
int probe_scratch_item_score(int i) { return g_scratch.items[i].score; }
int probe_scratch_item_len(int i) { return g_scratch.items[i].len; }
int probe_scratch_text_byte(int i) { return (unsigned char)g_scratch.text[i]; }
int probe_scratch_n_items(void) { return g_scratch.n_items; }
int probe_scratch_cstr_byte(int i) { return (unsigned char)g_scratch.cstr[i]; }
int probe_scratch_card_v(void) { return g_scratch.card.v; }
