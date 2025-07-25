#include <errno.h>
#define XXH_STATIC_LINKING_ONLY
#include "vendor/xxhash/xxhash.h"
#include "base/string.h"
#include "os/fs.h"

// =============================================================================
// String:
// =============================================================================
Bool    is_whitespace (Char c)               { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
CString cstr          (Mem *mem, String s)   { return astr_fmt(mem, "%.*s%c", STR(s), 0).data; }
String  str           (CString s)            { return (String){ .data=s, .count=cast(U64, strlen(s)) }; }
U64     str_hash_seed (String str, U64 seed) { return XXH3_64bits_withSeed(str.data, str.count, seed); }
U64     str_hash      (String str)           { return str_hash_seed(str, 5381); }
Bool    str_match     (String s1, String s2) { return (s1.count == s2.count) && (! strncmp(s1.data, s2.data, s1.count)); }
U64     istr_hash     (IString *i)           { return str_hash(*i); }
U64     cstr_hash     (CString s)            { return str_hash(str(s)); }
Bool    cstr_match    (CString a, CString b) { return str_match(str(a), str(b)); }
Void    str_clear     (String s, U8 b)       { memset(s.data, b, s.count); }

Bool str_starts_with (String str, String prefix) {
    if (str.count < prefix.count) return false;
    str.count = prefix.count;
    return str_match(str, prefix);
}

Bool str_ends_with (String str, String suffix) {
    if (str.count < suffix.count) return false;
    str = str_suffix_from(str, str.count - suffix.count);
    return str_match(str, suffix);
}

// Returns ARRAY_NIL_IDX if not found.
U64 str_index_of_first (String str, U8 byte) {
    return array_find(&str, IT == byte);
}

// Returns ARRAY_NIL_IDX if not found.
U64 str_index_of_last (String str, U8 byte) {
    array_iter_back (c, &str) if (c == byte) return ARRAY_IDX;
    return ARRAY_NIL_IDX;
}

String str_slice (String str, U64 offset, U64 count) {
    offset = min(offset, str.count);
    count  = min(count, str.count - offset);
    return (String){ .data=(str.data + offset), .count=count };
}

// Gets rid of whitespace at the beggining and end.
String str_trim (String str) {
    U64 start = 0;
    U64 end   = 0;
    array_iter (c, &str)      if (! is_whitespace(c)) { start = ARRAY_IDX; break; }
    array_iter_back (c, &str) if (! is_whitespace(c)) { end = ARRAY_IDX + 1; break; }
    return str_slice(str, start, end - start);
}

String str_cut_prefix (String str, String prefix) {
    if (str_starts_with(str, prefix)) {
        str.data  += prefix.count;
        str.count -= prefix.count;
    }
    return str;
}

String str_cut_suffix (String str, String suffix) {
    if (str_ends_with(str, suffix)) str.count -= suffix.count;
    return str;
}

// Non-inclusive.
String str_prefix_to (String str, U64 to_idx) {
    str.count = min(to_idx, str.count);
    return str;
}

// Inclusive.
String str_suffix_from (String str, U64 from_idx) {
    U64 idx    = min(from_idx, str.count);
    str.data  += idx;
    str.count -= idx;
    return str;
}

// Non-inclusive.
String str_prefix_to_first (String str, U8 byte) {
    array_iter (c, &str) if (c == byte) return str_prefix_to(str, ARRAY_IDX);
    return (String){};
}

// Non-inclusive.
String str_prefix_to_last (String str, U8 byte) {
    array_iter_back (c, &str) if (c == byte) return str_prefix_to(str, ARRAY_IDX);
    return (String){};
}

// Non-inclusive.
String str_suffix_from_last (String str, U8 byte) {
    array_iter_back (c, &str) if (c == byte) return str_suffix_from(str, ARRAY_IDX + 1);
    return (String){};
}

// Non-inclusive.
String str_suffix_from_first (String str, U8 byte) {
    array_iter (c, &str) if (c == byte) return str_suffix_from(str, ARRAY_IDX + 1);
    return (String){};
}

Bool str_to_u64 (CString str, U64 *out, U64 base) {
    errno = 0;
    Char *endptr = 0;
    *out = cast(U64, strtoul(str, &endptr, base));
    return (errno == 0) && (endptr != str);
}

Bool str_to_f64 (CString str, F64 *out) {
    errno = 0;
    Char *endptr = 0;
    *out = strtod(str, &endptr);
    return (errno == 0) && (endptr != str);
}

String str_copy (Mem *mem, String str) {
    if (! str.count) return (String){};
    Auto p = mem_alloc(mem, Char, .size=str.count);
    memcpy(p, str.data, str.count);
    return (String){.data=p, .count=str.count};
}

// This function splits the given 'str' into tokens separated by
// bytes that appear in the 'separators' string. For example, for
// the inputs:
//
//     str = "/a/b|c//foobar/"
//     separators = "/|"
//
// ... there are 4 possible outputs depending on the values of the
// 'keep_separators' and 'keep_empties' arguments:
//
//     1. [a] [b] [c] [foobar]
//     2. [] [a] [b] [c] [] [foobar] []
//     3. [/] [a] [/] [b] [|] [c] [/] [/] [foobar] [/]
//     4. [] [/] [a] [/] [b] [|] [c] [/] [] [/] [foobar] [/] []
//
Void str_split (String str, String separators, Bool keep_separators, Bool keep_empties, ArrayString *out) {
    U64 prev_pos = 0;

    array_iter (c, &str) {
        if (! array_has(&separators, c)) continue;
        if (keep_empties || (ARRAY_IDX > prev_pos)) array_push(out, str_slice(str, prev_pos, ARRAY_IDX - prev_pos));
        if (keep_separators) array_push(out, str_slice(str, ARRAY_IDX, 1));
        prev_pos = ARRAY_IDX + 1;
    }

    if (keep_empties || (str.count > prev_pos)) array_push(out, str_slice(str, prev_pos, str.count - prev_pos));
}

// This functions searches the haystack for the needle in a fuzzy way.
// If the needle is *not* found it returns INT64_MIN; otherwise, the
// returned val indicates how close of a match it is (higher is better).
//
// If the 'tokens' argument is not NULL, this function will emit into it
// slices into the haystack where the matches were found. The last token
// emitted is special: it is the remainder of the haystack (from the end
// of the last match to the end of the haystack). This is useful if you
// wish to call this function on the remainder of the haystack over and
// over again until you exhaust it.
//
// The algorithm is a fairly simple O(n) search. First we look ahead to
// see if all chars in the needle appear in the haystack in the exact
// order and separated any number of chars. We then search in reverse
// in hopes of finding a shorter match:
//
//     a b c d e abcdef
//     -------------->|
//               |<----
//
// This algorithm does not try to find the optimal match:
//
//     a b c d e ab c def abcdef
//     ---------------->|
//               |<------
//
// The score is computed based on how many consecutive letters in the
// text were found, whether letters appear at word beginnings, number
// of gaps between letters, ...
I64 str_fuzzy_search (String needle, String haystack, ArrayString *tokens) {
    if (needle.count == 0) return INT64_MIN;
    if (needle.count > haystack.count) return INT64_MIN;

    U64 needle_cursor = 0;
    U64 haystack_end  = 0;

    { // 1. Search forwards to find the initial match:
        array_iter (b, &haystack) {
            if (b == needle.data[needle_cursor]) {
                needle_cursor++;
                if (needle_cursor == needle.count) { haystack_end = ARRAY_IDX; break; }
            }
        }

        if (needle_cursor != needle.count) return INT64_MIN;
        needle_cursor--;
    }

    tmem_new(tm);
    ArrayU64 indices; // Map from needle idx to haystack idx.
    if (tokens) { array_init(&indices, tm); array_ensure_count(&indices, needle.count, 0); }

    I64 gaps            = 0;
    I64 consecutives    = 0;
    I64 word_beginnings = 0;

    { // 2. Compute score while searching again in reverse:
        U64 prev_match_idx = ARRAY_NIL_IDX;

        array_iter_back_from (b, &haystack, haystack_end) {
            if (b != needle.data[needle_cursor]) {
                gaps++;
            } else {
                if (tokens) array_set(&indices, needle_cursor, ARRAY_IDX);
                if ((ARRAY_IDX + 1) == prev_match_idx) consecutives++;
                if ((ARRAY_IDX > 1) && is_whitespace(haystack.data[ARRAY_IDX - 1])) word_beginnings++;
                if (needle_cursor == 0) break;
                needle_cursor--;
                prev_match_idx = ARRAY_IDX;
            }
        }

        assert_dbg(needle_cursor == 0);
    }

    if (tokens) { // 3. Emit tokens:
        String token = str_slice(haystack, indices.data[0], 1);

        array_iter_from (i, &indices, 1) {
            if (i == indices.data[ARRAY_IDX - 1] + 1) {
                token.count++;
            } else {
                array_push(tokens, token);
                token = str_slice(haystack, i, 1);
            }
        }

        array_push(tokens, token);
        array_push(tokens, str_slice(haystack, array_get_last(&indices) + 1, haystack.count));
    }

    return max(INT64_MIN+1, (consecutives * 4) + (word_beginnings * 3) - gaps);
}

static U8 utf8_class [32] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,2,2,2,2,3,3,4,5,
};

UtfDecode str_utf8_decode (String str) {
    UtfDecode result = {1, UINT32_MAX};

    U8 byte = array_get(&str, 0);
    U8 byte_class = utf8_class[byte >> 3];

    switch (byte_class) {
    case 1: {
        result.codepoint = byte;
        result.inc = 1;
    } break;

    case 2: {
        if (str.count >= 2) {
            U8 b = str.data[1];
            if (utf8_class[b >> 3] == 0) {
                result.codepoint  = (byte & 0b11111) << 6;
                result.codepoint |= (b & 0b111111);
                result.inc = 2;
            }
        }
    } break;

    case 3: {
        if (str.count >= 3) {
            U8 b[2] = {str.data[1], str.data[2]};
            if (utf8_class[b[0] >> 3] == 0 &&
                utf8_class[b[1] >> 3] == 0
            ) {
                result.codepoint  = (byte & 0b1111) << 12;
                result.codepoint |= ((b[0] & 0b111111) << 6);
                result.codepoint |= (b[1] & 0b111111);
                result.inc = 3;
            }
        }
    } break;

    case 4: {
        if (str.count >= 4) {
            U8 b[3] = {str.data[1], str.data[2], str.data[3]};
            if (utf8_class[b[0] >> 3] == 0 &&
                utf8_class[b[1] >> 3] == 0 &&
                utf8_class[b[2] >> 3] == 0
            ) {
                result.codepoint  = (byte & 0b111) << 18;
                result.codepoint |= ((b[0] & 0b111111) << 12);
                result.codepoint |= ((b[1] & 0b111111) << 6);
                result.codepoint |= (b[2]  & 0b111111);
                result.inc = 4;
            }
        }
    }
    }

    return result;
}

UtfIter str_utf8_iter_new (String str) {
    return (UtfIter){ .str=str };
}

Bool str_utf8_iter_next (UtfIter *it) {
    if (it->str.count == 0) return false;
    UtfDecode d = str_utf8_decode(it->str);
    it->decode = d;
    it->str.data += d.inc;
    it->str.count -= d.inc;
    return true;
}

// =============================================================================
// AString:
// =============================================================================
Void    astr_print         (AString *a)                           { if (a->count) printf("%.*s", STR(*a)); }
Void    astr_println       (AString *a)                           { if (a->count) printf("%.*s\n", STR(*a)); }
CString astr_to_cstr       (AString *a)                           { astr_push_byte(a, 0); return a->data; }
String  astr_to_str        (AString *a)                           { return a->as_slice; }
Void    astr_push_u8       (AString *a, U8 v)                     { array_push_n(a, v); }
Void    astr_push_2u8      (AString *a, U8 x, U8 y)               { array_push_n(a, x, y); }
Void    astr_push_3u8      (AString *a, U8 x, U8 y, U8 z)         { array_push_n(a, x, y, z); }
Void    astr_push_u16      (AString *a, U16 v)                    { array_push_n(a, v>>0, v>>8); }
Void    astr_push_u32      (AString *a, U64 v)                    { array_push_n(a, v>>0, v>>8, v>>16, v>>24); }
Void    astr_push_u64      (AString *a, U64 v)                    { array_push_n(a, v>>0, v>>8, v>>16, v>>24, v>>32, v>>40, v>>48, v>>56); }
Void    astr_push_byte     (AString *a, U8 b)                     { array_push(a, b); }
Void    astr_push_bytes    (AString *a, U8 b, U64 n)              { SliceU8 s; array_increase_count_o(a, n, false, &s); if (n) memset(s.data, b, n); }
Void    astr_push_str      (AString *a, String s)                 { array_push_many(a, &s); }
Void    astr_push_cstr     (AString *a, CString s)                { astr_push_str(a, (String){ .data=s, .count=(U64)(strlen(s)) }); }
Void    astr_push_cstr_nul (AString *a, CString s)                { astr_push_str(a, (String){ .data=s, .count=(U64)(strlen(s) + 1) }); }
Void    astr_push_2cstr    (AString *a, CString s1, CString s2)   { astr_push_cstr(a, s1); astr_push_cstr(a, s2); }
Void    astr_push_fmt      Fmt(2, 3) (AString *a, CString f, ...) { astr_push_fmt_vam(a, f); }

Void astr_push_fmt_va Fmt(2, 0) (AString *astr, CString fmt, VaList va) {
    VaList va2;
    va_copy(va2, va);
    Int fmt_len = vsnprintf(0, 0, fmt, va);
    assert_always(fmt_len >= 0);
    array_ensure_capacity(astr, fmt_len + 1);
    vsnprintf(astr->data + astr->count, fmt_len + 1, fmt, va2);
    astr->count += fmt_len;
    va_end(va2);
}

// Append the str argument wrapped in double quotes with
// any double quotes within str escaped with a backslash:
//
//     (foo "bar" baz)  ->  ("foo \"bar\" baz")
//
Void astr_push_str_quoted (AString *astr, String str) {
    astr_push_byte(astr, '"');

    Bool escaped = false;
    String chunk = { .data = str.data };

    array_iter (c, &str) {
        if (escaped) {
            escaped = false;
            chunk.count++;
        } else if (c == '"') {
            astr_push_str(astr, chunk);
            astr_push_byte(astr, '\\');
            astr_push_byte(astr, '"');
            chunk.count = 0;
            chunk.data = array_try_ref(&str, ARRAY_IDX + 1);
        } else if (c == '\\') {
            escaped = true;
            chunk.count++;
        } else {
            chunk.count++;
        }
    }

    astr_push_str(astr, chunk);
    astr_push_byte(astr, '"');
}

// =============================================================================
// Gap Buffer:
// =============================================================================
istruct (GapBuf) {
    AString str;
    U64 gap_min;
    U64 gap_idx;
    U64 gap_count;
};

static Void print_state (GapBuf *gb) {
    String before_gap = { gb->str.data, gb->gap_idx };
    U64 n = gb->gap_idx + gb->gap_count;
    String after_gap  = { gb->str.data + n, gb->str.count - n };
    printf(
        "%.*s" TERM_CYAN("[ i=%lu n=%lu ]") "%.*s" TERM_RED("[ n=%lu cap=%lu ]\n"),
        STR(before_gap),
        gb->gap_idx,
        gb->gap_count,
        STR(after_gap),
        gb->str.count,
        gb->str.capacity
    );
}

static Void move_gap (GapBuf *gb, U64 idx) {
    if (idx <= gb->gap_idx) {
        Auto p = gb->str.data + idx;
        memmove(p + gb->gap_count, p, gb->gap_idx - idx);
    } else {
        Auto i = idx + gb->gap_count;
        Auto p = gb->str.data + gb->gap_idx + gb->gap_count;
        memmove(p - gb->gap_count, p, i - gb->gap_idx - gb->gap_count);
    }

    gb->gap_idx = idx;
}

static Void move_gap_to_end (GapBuf *gb) {
    move_gap(gb, gb_count(gb));
}

// Call this to increase the size of the gap region to min
// 'cap' when you have a good estimate about the amount of
// text you are about to insert.
//
// Note that calling gb_delete() after this function can
// undo the effect of this function by shrinking the gap.
Void gb_set_gap_size (GapBuf *gb, U64 cap) {
    if (gb->gap_count >= cap) return;
    U64 inc = gb->gap_min + (cap - gb->gap_count);
    U64 to_move = gb->str.count - gb->gap_count - gb->gap_idx;
    array_ensure_count(&gb->str, gb->str.count + inc, false);
    Char *p = gb->str.data + gb->gap_idx + gb->gap_count;
    memmove(p + inc, p, to_move);
    gb->gap_count += inc;
}

// The idx parameter does not include the gap region.
// After the insert the first char of str is at idx.
Void gb_insert (GapBuf *gb, String str, U64 idx) {
    idx = min(idx, gb->str.count - gb->gap_count);
    gb_set_gap_size(gb, str.count);
    move_gap(gb, idx);
    memcpy(gb->str.data + gb->gap_idx, str.data, str.count);
    gb->gap_idx   += str.count;
    gb->gap_count -= str.count;
}

// The idx parameter does not include the gap region.
Void gb_delete (GapBuf *gb, U64 count, U64 idx) {
    idx   = min(idx, gb->str.count - gb->gap_count);
    count = min(count, gb->str.count - gb->gap_count - idx);
    move_gap(gb, idx + count);
    gb->gap_idx   -= count;
    gb->gap_count += count;

    if (gb->gap_count > (4 * gb->gap_min)) {
        move_gap_to_end(gb);
        U64 n = gb->gap_count - gb->gap_min;
        gb->gap_count -= n;
        gb->str.count -= n;
        gb->str.capacity += n;
        array_maybe_decrease_capacity(&gb->str);
    }
}

U64 gb_count (GapBuf *gb) {
    return gb->str.count - gb->gap_count;
}

String gb_str (GapBuf *gb) {
    move_gap_to_end(gb);
    return (String){ .data=gb->str.data, .count=gb_count(gb) };
}

// The line is 1-indexed and the offset is 0-indexed.
U64 gb_line_to_offset (GapBuf *gb, U64 line) {
    if (line == 1) return 0;
    String s = gb_str(gb);
    U64 l = 1;
    array_iter (c, &s) if ((c == '\n') && (++l == line)) return ARRAY_IDX + 1;
    return 0;
}

GapBuf *gb_new (Mem *mem, U64 gap_size) {
    Auto gb     = mem_new(mem, GapBuf);
    gb->str     = astr_new(mem);
    gb->gap_min = max(1*KB, gap_size);
    return gb;
}

// This is a more efficient way to init than gb_new() if the
// initial text is to be loaded from a file.
GapBuf *gb_new_from_file (Mem *mem, String filepath, U64 gap_size) {
    gap_size         = max(1*KB, gap_size);
    Auto gb          = mem_new(mem, GapBuf);
    String file      = fs_read_entire_file(mem, filepath, gap_size);
    gb->str.mem      = mem;
    gb->str.data     = file.data;
    gb->str.count    = file.count + gap_size + 1;
    gb->str.capacity = file.count + gap_size + 1;
    gb->gap_min      = 1*KB;
    gb->gap_count    = gap_size + 1;
    gb->gap_idx      = file.count;
    return gb;
}
