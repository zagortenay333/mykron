#pragma once

#include "base/array.h"

// =============================================================================
// String:
// =============================================================================
typedef SliceChar String;
array_typedef(String, String);

// Interned string.
typedef SliceChar IString;
array_typedef(IString, IString);

istruct (UtfDecode) {
    U32 codepoint;
    U32 inc;
};

istruct (UtfIter) {
    String str;
    UtfDecode decode;
};

// Loop over codepoints in a UTF-8 encoded string.
#define str_utf8_iter(X, S)\
    for (UtfIter X = str_utf8_iter_new(S); str_utf8_iter_next(&X);)\

#define STR(X) cast(Int, (X).count), (X).data

Bool      is_whitespace         (Char);
CString   cstr                  (Mem *, String);
String    str                   (CString);
U64       istr_hash             (IString *);
U64       cstr_hash             (CString);
U64       str_hash              (String);
U64       str_hash_seed         (String str, U64 seed);
Bool      cstr_match            (CString, CString);
Bool      str_match             (String, String);
Bool      str_starts_with       (String, String prefix);
Bool      str_ends_with         (String, String suffix);
String    str_slice             (String, U64 offset, U64 count);
String    str_trim              (String);
U64       str_index_of_first    (String, U8 byte);
U64       str_index_of_last     (String, U8 byte);
String    str_cut_prefix        (String, String prefix);
String    str_cut_suffix        (String, String suffix);
String    str_prefix_to         (String, U64);
String    str_suffix_from       (String, U64);
String    str_prefix_to_first   (String, U8 byte);
String    str_prefix_to_last    (String, U8 byte);
String    str_suffix_from_first (String, U8 byte);
String    str_suffix_from_last  (String, U8 byte);
Void      str_clear             (String, U8 byte);
Bool      str_to_u64            (CString, U64 *out, U64 base);
Bool      str_to_f64            (CString, F64 *out);
Void      str_split             (String, String seps, Bool keep_seps, Bool keep_empties, ArrayString *);
I64       str_fuzzy_search      (String needle, String haystack, ArrayString *);
String    str_copy              (Mem *, String);
UtfDecode str_utf8_decode       (String str);
UtfIter   str_utf8_iter_new     (String str);
Bool      str_utf8_iter_next    (UtfIter *it);

// =============================================================================
// AString: Wrapper around Array for string building.
// =============================================================================
typedef ArrayChar AString;

#define astr_new(MEM)             ({ AString astr; array_init(&astr, MEM); astr; })
#define astr_new_cap(MEM, CAP)    ({ AString astr; array_init_cap(&astr, MEM, CAP); astr; })
#define astr_fmt(MEM, ...)        ({ AString astr = astr_new(MEM); astr_push_fmt(&astr, __VA_ARGS__); astr_to_str(&astr); })
#define astr_push_fmt_vam(A, FMT) ({ def1(a, A); VaList va; va_start(va, FMT); astr_push_fmt_va(a, FMT, va); va_end(va); })

Void    astr_print           (AString *);
Void    astr_println         (AString *);
CString astr_to_cstr         (AString *);
String  astr_to_str          (AString *);
Void    astr_push_u8         (AString *, U8);
Void    astr_push_2u8        (AString *, U8, U8);
Void    astr_push_3u8        (AString *, U8, U8, U8);
Void    astr_push_u16        (AString *, U16);
Void    astr_push_u32        (AString *, U64);
Void    astr_push_u64        (AString *, U64);
Void    astr_push_byte       (AString *, U8);
Void    astr_push_bytes      (AString *, U8 byte, U64 n_times);
Void    astr_push_str        (AString *, String);
Void    astr_push_cstr       (AString *, CString);
Void    astr_push_2cstr      (AString *, CString, CString);
Void    astr_push_cstr_nul   (AString *, CString);
Void    astr_push_str_quoted (AString *, String);
Void    astr_push_fmt_va     Fmt(2, 0) (AString *, CString fmt, VaList);
Void    astr_push_fmt        Fmt(2, 3) (AString *, CString fmt, ...);

// =============================================================================
// Gap Buffer:
// -----------
//
// The is a data structure for representing strings that allows for
// efficient insert/delete operations at arbitrary locations.
//
// This is a flat buffer with gap region within into which insertion
// is possible. This gap moves around (via memmove), so the best case
// scenario is that multiple edits are nerby so that we don't have to
// memmove a lot.
//
// Usage example:
// --------------
//
//     tmem_new(tm);
//     GapBuf *gb = gb_new(tm, 0);
//
//     gb_insert(gb, str("Fire... walk with me."), 0);
//     gb_insert(gb, str("One chants out between two worlds... "), 0);
//     gb_insert(gb, str("The magician longs to see. "), 0);
//     gb_insert(gb, str("Through the darkness of future's past, "), 0);
//
//     String str = gb_str(gb);
//     printf("%.*s\n", STR(str));
//
// =============================================================================
istruct (GapBuf);

GapBuf *gb_new            (Mem *, U64 gap_size);
GapBuf *gb_new_from_file  (Mem *, String filepath, U64 gap_size);
Void    gb_insert         (GapBuf *, String str, U64 idx);
Void    gb_delete         (GapBuf *, U64 count, U64 idx);
U64     gb_count          (GapBuf *);
String  gb_str            (GapBuf *);
U64     gb_line_to_offset (GapBuf *, U64 line);
Void    gb_set_gap_size   (GapBuf *, U64 cap);
