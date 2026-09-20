// The C half of tools/structgen/test/swift.sh: the fixture structs, laid out by
// the compiler that compiles this file, and filled through THEIR OWN FIELD
// NAMES. The Swift half reads them through the generated readers, so the two
// agreeing is the same statement as "every offset the generator emitted is the
// offset C's offsetof would give" - and it is checked against the real headers,
// not against a copy of them.
#ifndef STRUCTGEN_SWIFT_PROBE_H
#define STRUCTGEN_SWIFT_PROBE_H

// The structs (snap.h) filled with the values the Swift driver expects.
void probe_fill(void);
const void *probe_snap(void);
const void *probe_sptr(void);

// Sizes as the compiler makes them, against the snapshot types' own cSize.
int probe_sizeof_snap(void);
int probe_sizeof_sptr(void);
int probe_sizeof_item(void);
int probe_sizeof_pair(void);
int probe_sizeof_card(void);

// The counts a reader must refuse, written straight into the filled struct.
void probe_set_n_pairs(int n);
void probe_set_n_text(int n);
void probe_set_ptr_none(int n);
void probe_set_ptr_name_len(int n);
void probe_set_ptr_items(int n);

// A zeroed Snap for the writer to write into, and what C then reads out of it
// through its own field names.
void *probe_scratch(void);
int probe_scratch_w(void);
int probe_scratch_item_score(int i);
int probe_scratch_item_len(int i);
int probe_scratch_text_byte(int i);
int probe_scratch_n_items(void);
int probe_scratch_cstr_byte(int i);
int probe_scratch_card_v(void);

#endif
