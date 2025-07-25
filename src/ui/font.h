#include <hb.h>
#include <hb-ft.h>
#include "base/core.h"
#include "base/map.h"

ienum (FontSlot, U8) {
    FONT_LATIN,
    FONT_ARABIC,
    FONT_JAPANESE,
    FONT_EMOJI,
    FONT_COUNT,
    FONT_NONE,
};

istruct (GlyphInfo) {
    U32 x;
    U32 y;
    U32 x_advance;
    U32 y_advance;
    U32 codepoint;
    U32 glyph_index;
    FontSlot font_slot;
};

typedef U64 GlyphId;

istruct (GlyphSlot) {
    U16 x;
    U16 y;
    U32 width;
    U32 height;
    I32 bearing_x;
    I32 bearing_y;
    FT_Pixel_Mode pixel_mode;
    FontSlot font_slot;
    U32 glyph_index;

    // Private:
    GlyphId id;
    GlyphSlot *lru_next;
    GlyphSlot *lru_prev;
    GlyphSlot *map_next;
};

istruct (Font) {
    FT_Face ft_face;
    hb_face_t *hb_face;
    hb_font_t *hb_font;
};

istruct (GlyphCache) {
    Mem *mem;
    U16 atlas_size;
    U16 atlas_slot_size;
    U32 atlas_texture;
    U32 font_size;
    U32 dpr; // Device pixel ratio.
    Array(Font) font_slots;
    FT_Library ft_lib;
    GlyphSlot **map;
    GlyphSlot *slots;
    GlyphSlot sentinel;
};

array_typedef(Font, Font);
array_typedef(GlyphInfo, GlyphInfo);

GlyphCache    *glyph_cache_new     (Mem *, U16 atlas_size, U32 font_size);
Void           glyph_cache_destroy (GlyphCache *);
GlyphSlot     *glyph_cache_get     (GlyphCache *, GlyphInfo *);
SliceGlyphInfo get_glyph_infos     (GlyphCache *, Mem *, String);
