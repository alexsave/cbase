// The Swift half of tools/structgen/test/swift.sh: the generated readers and
// writers against the C fixture that shares their headers (swift_probe.c).
//
// Every field kind the emitter claims to handle is read here, plus the counts
// that bound an array, the strings, and every refusal. A snapshot is a VALUE:
// what it holds must survive the struct it was copied from being overwritten,
// which is the last case in this file.

import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if ok { print("ok   \(what)") } else { print("FAIL \(what)"); failures += 1 }
}
func eq<T: Equatable>(_ got: T, _ want: T, _ what: String) {
    if got == want { print("ok   \(what)") } else { print("FAIL \(what): got \(got), want \(want)"); failures += 1 }
}
// Six pairs whatever the reader gave back, so the writer's refusal is tested
// even when the reader under test hands back nothing.
func refusalPairs(_ got: [SPairSnap]) -> [SPairSnap] {
    let one = got.first ?? SPairSnap(a: KCardSnap(s: 0, v: 0), b: KCardSnap(s: 0, v: 0))
    return Array(repeating: one, count: 6)
}
func throwsLayout(_ what: String, _ want: SGLayoutError, _ body: () throws -> Void) {
    do {
        try body()
        print("FAIL \(what): nothing was thrown")
        failures += 1
    } catch let e as SGLayoutError {
        eq(e, want, what)
    } catch {
        print("FAIL \(what): threw \(error)")
        failures += 1
    }
}

// Top level statements live in main.swift only, and this file is named for what
// it is; @main is the same entry point under its own name.
@main struct SwiftEmitterTest {
    static func main() {
        setvbuf(stdout, nil, _IONBF, 0)   // a crash must not swallow the line before it
        probe_fill()
        let p = probe_snap()!

        // ---- sizes: the generator's own arithmetic against the compiler's ----------
        eq(SnapSnap.cSize, Int(probe_sizeof_snap()), "sizeof Snap")
        eq(SPtrSnap.cSize, Int(probe_sizeof_sptr()), "sizeof SPtr")
        eq(SItemSnap.cSize, Int(probe_sizeof_item()), "sizeof SItem")
        eq(SPairSnap.cSize, Int(probe_sizeof_pair()), "sizeof SPair")
        eq(KCardSnap.cSize, Int(probe_sizeof_card()), "sizeof KCard")

        // ---- constants -------------------------------------------------------------
        eq(K_NEG, -1, "K_NEG")
        eq(K_POS, 7, "K_POS")
        eq(KFLAG_LOW, 3, "KFLAG_LOW")
        eq(KFLAG_HIGH, 1 << 30, "KFLAG_HIGH")
        eq(KFLAG_NEG, -(1 << 30) - 5, "KFLAG_NEG")

        // ---- every field kind ------------------------------------------------------
        do {
            let s = try readSnap(p)
            eq(s.flag, true, "bool")
            eq(s.w, -1234, "int16")
            eq(s.u, 4000000000, "uint32 past INT32_MAX")
            eq(s.big, -1234567890123, "int64")
            eq(s.d, 2.5, "double")
            eq(s.bits, 5, "unsigned bitfield")
            eq(s.sbits, -7, "signed bitfield keeps its sign")
            eq(s.card.s, 3, "nested record, first bitfield")
            eq(s.card.v, -4, "nested record, second bitfield")
            eq(s.nums, [7, -8, 9], "an uncounted array is all of it")
            eq(s.cstr, "ab", "a NUL-terminated char[N]")
            eq(s.text, "héllo", "a counted char[N] is exactly its count of UTF-8 bytes")

            eq(s.pairs.count, 2, "a counted array is its count long")
            if s.pairs.count == 2 {
                eq(s.pairs[0].a.s, 1, "record array element 0")
                eq(s.pairs[0].b.v, -3, "record array element 0, second member")
                eq(s.pairs[1].a.v, 13, "record array element 1")
            }

            eq(s.items.count, 2, "a second counted array")
            if s.items.count == 2 {
                eq(s.items[0].text, "ok", "a counted string inside an array element")
                eq(s.items[0].score, 101, "a field after a counted string")
                eq(s.items[1].text, "hélo", "element 1's string is read at ITS offset")
                eq(s.items[1].score, -202, "element 1's score")
            }
        } catch {
            check(false, "readSnap on the filled fixture threw \(error)")
        }

        // ---- pointers a count follows ---------------------------------------------
        do {
            let s = try readSPtr(probe_sptr()!)
            eq(s.vals, [11, -12, 13], "a counted pointer to a scalar")
            eq(s.items.count, 2, "a counted pointer to a record")
            if s.items.count == 2 { eq(s.items[1].text, "hélo", "…read as its own record") }
            eq(s.name, "Ünï", "a counted pointer to char is UTF-8")
            eq(s.none, [], "NULL with a count of 0 is an empty array")
            eq(s.tail, 4242, "the field after the pointers")
        } catch {
            check(false, "readSPtr on the filled fixture threw \(error)")
        }

        // ---- refusals: a count outside its array is never read ---------------------
        probe_set_n_pairs(5)
        throwsLayout("a count over the capacity is refused",
                     .count(field: "Snap.pairs", got: 5, capacity: 4)) { _ = try readSnap(p) }
        probe_set_n_pairs(-1)
        throwsLayout("a negative count is refused",
                     .count(field: "Snap.pairs", got: -1, capacity: 4)) { _ = try readSnap(p) }
        probe_set_n_pairs(2)
        probe_set_n_text(9)
        throwsLayout("a string count over the capacity is refused",
                     .count(field: "Snap.text", got: 9, capacity: 8)) { _ = try readSnap(p) }
        probe_set_n_text(6)
        probe_set_ptr_none(1)
        throwsLayout("NULL with a count is refused",
                     .null(field: "SPtr.none", count: 1)) { _ = try readSPtr(probe_sptr()!) }
        probe_set_ptr_none(0)
        probe_set_ptr_items(-3)
        throwsLayout("a negative count behind a pointer is refused",
                     .count(field: "SPtr.items", got: -3, capacity: Int.max)) { _ = try readSPtr(probe_sptr()!) }
        probe_set_ptr_items(2)

        // A count of 0 is not a refusal: an empty array reads.
        probe_set_ptr_name_len(0)
        do { eq(try readSPtr(probe_sptr()!).name, "", "a count of 0 reads as empty") }
        catch { check(false, "a count of 0 threw \(error)") }
        probe_set_ptr_name_len(5)

        // ---- the writer ------------------------------------------------------------
        let w = probe_scratch()!
        do {
            let s = try readSnap(p)
            try writeSnap(w, s)
            eq(probe_scratch_w(), -1234, "the writer put int16 where C reads it")
            eq(probe_scratch_n_items(), 2, "…and wrote the count from the array's length")
            eq(probe_scratch_item_score(1), -202, "…and element 1 at its own offset")
            eq(probe_scratch_item_len(1), 5, "…with its own counted string's length")
            eq(probe_scratch_text_byte(1), 0xC3, "…and the string's UTF-8 bytes")
            eq(probe_scratch_cstr_byte(2), 0, "…and a NUL-terminated string is terminated")
            eq(probe_scratch_card_v(), -4, "…and a nested record's bitfield")
            let back = try readSnap(UnsafeRawPointer(w))
            check(back == s, "a written snapshot reads back equal")
        } catch {
            check(false, "the writer threw \(error)")
        }

        // A value that does not fit is refused rather than truncated.
        do {
            let s = try readSnap(p)
            let tooManyPairs = SnapSnap(flag: s.flag, w: s.w, u: s.u, big: s.big, d: s.d, bits: s.bits,
                                        sbits: s.sbits, pairs: refusalPairs(s.pairs),
                                        items: s.items, text: s.text, cstr: s.cstr, nums: s.nums, card: s.card)
            throwsLayout("a counted array longer than its field is refused",
                         .tooLong(field: "Snap.pairs", got: 6, capacity: 4)) { try writeSnap(w, tooManyPairs) }

            let wrongNums = SnapSnap(flag: s.flag, w: s.w, u: s.u, big: s.big, d: s.d, bits: s.bits,
                                     sbits: s.sbits, pairs: s.pairs, items: s.items, text: s.text,
                                     cstr: s.cstr, nums: [1, 2], card: s.card)
            throwsLayout("an uncounted array of the wrong length is refused",
                         .tooLong(field: "Snap.nums", got: 2, capacity: 3)) { try writeSnap(w, wrongNums) }

            let longText = SnapSnap(flag: s.flag, w: s.w, u: s.u, big: s.big, d: s.d, bits: s.bits,
                                    sbits: s.sbits, pairs: s.pairs, items: s.items, text: "123456789",
                                    cstr: s.cstr, nums: s.nums, card: s.card)
            throwsLayout("a counted string longer than its field is refused",
                         .tooLong(field: "Snap.text", got: 9, capacity: 8)) { try writeSnap(w, longText) }

            let longCStr = SnapSnap(flag: s.flag, w: s.w, u: s.u, big: s.big, d: s.d, bits: s.bits,
                                    sbits: s.sbits, pairs: s.pairs, items: s.items, text: s.text,
                                    cstr: "123456", nums: s.nums, card: s.card)
            throwsLayout("a NUL-terminated string with no room for its NUL is refused",
                         .tooLong(field: "Snap.cstr", got: 6, capacity: 5)) { try writeSnap(w, longCStr) }
        } catch {
            check(false, "building the refusal cases threw \(error)")
        }

        // ---- a snapshot is a value -------------------------------------------------
        do {
            let before = try readSnap(p)
            probe_set_n_text(0)
            probe_set_n_pairs(0)
            let after = try readSnap(p)
            eq(after.pairs.count, 0, "the fixture really changed under it")
            eq(before.pairs.count, 2, "the snapshot taken first still holds its own copy")
            eq(before.text, "héllo", "…strings included")
        } catch {
            check(false, "the value test threw \(error)")
        }

        if failures == 0 {
            print("swift: all pass")
        } else {
            print("swift: \(failures) failed")
            exit(1)
        }

    }
}