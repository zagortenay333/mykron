// =============================================================================
// @todo
//
// - We do not signal to the caller that a glyph got evicted from
//   the atlas. If the caller is batching render calls, they might
//   hold a glyph to be rendered so long in the batch that it gets
//   evicted before being rendered.
//
// - The font size is set just once when calling glyph_cache_new().
//   One solution is to index into the glyph cache using font size
//   as well. Then we would internally maintain multiple atlases,
//   one for a certain range of font sizes.
// =============================================================================

#include "vendor/glad/glad.h"
#include <freetype/freetype.h>
#include <freetype/ftmodapi.h>
#include "vendor/plutosvg/src/plutosvg.h"
#include "vendor/stb/stb_image_write.h"
#include "ui/font.h"
#include "base/array.h"
#include "base/string.h"
#include "base/log.h"
#include "base/map.h"
#include "os/fs.h"
#include "os/time.h"
#include "ui/ui.h"

#define LOG_HEADER "Glyph cache"

istruct (ScriptRange) {
    hb_script_t script;
    U64 start;
    U64 end; // Inclusive
};

array_typedef(ScriptRange, ScriptRange);

static GlyphId info_to_id (GlyphInfo *info) {
    return cast(U64, info->glyph_index) | (cast(U64, info->font_slot) << 32);
}

static GlyphSlot **cache_get_slot (GlyphCache *cache, GlyphId id) {
    return &cache->map[hash_u64(id) % cache->atlas_size];
}

static GlyphSlot *cache_get (GlyphCache *cache, GlyphId id) {
    GlyphSlot *s = *cache_get_slot(cache, id);
    while (s && (s->id != id)) s = s->map_next;
    return s;
}

static Void cache_add (GlyphCache *cache, GlyphSlot *slot) {
    GlyphSlot **s = cache_get_slot(cache, slot->id);
    slot->map_next = *s;
    *s = slot;
}

static Void cache_remove (GlyphCache *cache, GlyphSlot *slot) {
    GlyphSlot **s = cache_get_slot(cache, slot->id);
    while (*s != slot) s = &(*s)->map_next;
    *s = slot->map_next;
}

GlyphSlot *glyph_cache_get (GlyphCache *cache, GlyphInfo *info) {
    GlyphId id = info_to_id(info);
    GlyphSlot *slot = cache_get(cache, id);

    Bool atlas_update_needed = true;

    // Remove from lru chain.
    if (slot) {
        slot->lru_next->lru_prev = slot->lru_prev;
        slot->lru_prev->lru_next = slot->lru_next;
        atlas_update_needed = false;
    }

    // See if we have a free slot.
    if (!slot && cache->sentinel.map_next) {
        slot = cache->sentinel.map_next;
        cache->sentinel.map_next = slot->map_next;
        slot->id = id;
        slot->font_slot = info->font_slot;
        slot->glyph_index = info->glyph_index;
        cache_add(cache, slot);
    }

    // Evict the lru slot.
    if (! slot) {
        assert_dbg(cache->sentinel.lru_prev != &cache->sentinel);
        slot = cache->sentinel.lru_prev;
        cache_remove(cache, slot);
        slot->id = id;
        slot->font_slot = info->font_slot;
        slot->glyph_index = info->glyph_index;
        cache_add(cache, slot);
    }

    // Mark as mru slot
    slot->lru_next = cache->sentinel.lru_next;
    slot->lru_prev = &cache->sentinel;
    cache->sentinel.lru_next->lru_prev = slot;
    cache->sentinel.lru_next = slot;

    if (atlas_update_needed) {
        Font *font = array_ref(&cache->font_slots, slot->font_slot);

        if (FT_Load_Glyph(font->ft_face, slot->glyph_index, FT_LOAD_RENDER | (FT_HAS_COLOR(font->ft_face) ? FT_LOAD_COLOR : 0))) {
            log_msg_fmt(LOG_ERROR, LOG_HEADER, 0, "Couldn't load/render font glyph.");
            goto done;
        }

        Auto ft_glyph = font->ft_face->glyph;
        Auto ft_bitmap = ft_glyph->bitmap;
        Auto w = ft_bitmap.width;
        Auto h = ft_bitmap.rows;

        slot->width = w;
        slot->height = h;
        slot->bearing_x = ft_glyph->bitmap_left;
        slot->bearing_y = ft_glyph->bitmap_top;
        slot->pixel_mode = ft_bitmap.pixel_mode;

        if ((w > cache->atlas_slot_size) || (h > cache->atlas_slot_size)) {
            log_msg_fmt(LOG_ERROR, LOG_HEADER, 0, "Font glyph too big to fit into atlas slot.");
            goto done;
        }

        if ((w == 0) || (h == 0)) {
            goto done;
        }

        tmem_new(tm);
        U8 *buf = mem_alloc(tm, U8, .zeroed=true, .size=(cache->atlas_slot_size * cache->atlas_slot_size * 4));

        switch (ft_bitmap.pixel_mode) {
        case FT_PIXEL_MODE_GRAY: {
            for (U32 y = 0; y < h; ++y) {
                U8 *src = ft_bitmap.buffer + y * abs(ft_bitmap.pitch);
                for (U32 x = 0; x < w; ++x) {
                    U8 value = src[x];
                    U32 i = (y * cache->atlas_slot_size + x) * 4;
                    buf[i + 0] = 255;
                    buf[i + 1] = 255;
                    buf[i + 2] = 255;
                    buf[i + 3] = value;
                }
            }
        } break;

        case FT_PIXEL_MODE_BGRA: {
            for (U32 y = 0; y < h; ++y) {
                U8 *src = ft_bitmap.buffer + y * abs(ft_bitmap.pitch);
                for (U32 x = 0; x < w; ++x) {
                    U32 i = (y * cache->atlas_slot_size + x) * 4;
                    buf[i + 0] = src[x * 4 + 2];
                    buf[i + 1] = src[x * 4 + 1];
                    buf[i + 2] = src[x * 4 + 0];
                    buf[i + 3] = src[x * 4 + 3];
                }
            }
        } break;

        default: badpath;
        }

        glBindTexture(GL_TEXTURE_2D, cache->atlas_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, slot->x, slot->y, cache->atlas_slot_size, cache->atlas_slot_size, GL_RGBA, GL_UNSIGNED_BYTE, buf);

        done:;
    }

    return slot;
}

static Void font_init (GlyphCache *cache, Font *font, String font_binary) {
    FT_Open_Args args = {
        .flags       = FT_OPEN_MEMORY,
        .memory_base = cast(U8*, font_binary.data),
        .memory_size = font_binary.count,
    };
    if (FT_Open_Face(cache->ft_lib, &args, 0, &font->ft_face)) log_msg_fmt(LOG_ERROR, LOG_HEADER, 0, "Couldn't open freetype face.");
    FT_Set_Pixel_Sizes(font->ft_face, cache->font_size * cache->dpr, cache->font_size * cache->dpr);

    font->hb_face = hb_ft_face_create_referenced(font->ft_face);
    font->hb_font = hb_font_create(font->hb_face);

    I32 hb_font_size = cache->font_size * cache->dpr * 64;
    hb_font_set_scale(font->hb_font, hb_font_size, hb_font_size);
}

GlyphCache *glyph_cache_new (Mem *mem, U16 atlas_size, U32 font_size) {
    Auto cache = mem_new(mem, GlyphCache);
    cache->mem = mem;
    cache->dpr = 1;
    cache->font_size = font_size;
    cache->atlas_size = atlas_size;
    cache->atlas_slot_size = cache->font_size * cache->dpr * 2;
    cache->slots = mem_alloc(mem, GlyphSlot, .size=(atlas_size * atlas_size * sizeof(GlyphSlot)));
    cache->map = mem_alloc(mem, GlyphSlot*, .zeroed=true, .size=(atlas_size * sizeof(GlyphSlot*)));
    cache->sentinel.lru_next = &cache->sentinel;
    cache->sentinel.lru_prev = &cache->sentinel;

    U32 x = 0;
    U32 y = 0;
    for (U32 i = 0; i < cast(U32, atlas_size) * atlas_size; ++i) {
        GlyphSlot *slot = &cache->slots[i];
        slot->x = x*cache->atlas_slot_size;
        slot->y = y*cache->atlas_slot_size;
        slot->map_next = cache->sentinel.map_next;
        cache->sentinel.map_next = slot;
        x++;
        if (x == atlas_size) { x = 0; y++; }
    }

    if (FT_Init_FreeType(&cache->ft_lib)) log_msg_fmt(LOG_ERROR, LOG_HEADER, 0, "Couldn't init freetype.");
    array_init(&cache->font_slots, mem);
    array_ensure_count(&cache->font_slots, FONT_COUNT, false);
    font_init(cache, array_ref(&cache->font_slots, FONT_LATIN), fs_read_entire_file(mem, str("./data/fonts/NotoSans-Regular.ttf"), 0));
    font_init(cache, array_ref(&cache->font_slots, FONT_ARABIC), fs_read_entire_file(mem, str("./data/fonts/NotoSansArabic-Regular.ttf"), 0));
    font_init(cache, array_ref(&cache->font_slots, FONT_JAPANESE), fs_read_entire_file(mem, str("./data/fonts/NotoSansJP-Regular.ttf"), 0));
    font_init(cache, array_ref(&cache->font_slots, FONT_EMOJI), fs_read_entire_file(mem, str("./data/fonts/NotoColorEmoji-COLRv1.ttf"), 0));

    Auto hooks = plutosvg_ft_svg_hooks();
    if (FT_Property_Set(cache->ft_lib, "ot-svg", "svg-hooks", hooks)) log_msg_fmt(LOG_ERROR, LOG_HEADER, 0, "Couldn't set pluto svg hooks.");

    glGenTextures(1, &cache->atlas_texture);
    glBindTexture(GL_TEXTURE_2D, cache->atlas_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlas_size*cache->atlas_slot_size, atlas_size*cache->atlas_slot_size, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    return cache;
}

Void glyph_cache_destroy (GlyphCache *cache) {
    if (FT_Done_FreeType(cache->ft_lib)) log_msg_fmt(LOG_ERROR, LOG_HEADER, 0, "Couldn't free freetype face.");

    array_iter (font, &cache->font_slots, *) {
        if (FT_Done_Face(font->ft_face)) log_msg_fmt(LOG_ERROR, LOG_HEADER, 0, "Couldn't free freetype face.");
        hb_font_destroy(font->hb_font);
    }
}

static hb_direction_t script_to_direction (hb_script_t script) {
    switch (script) {
    case HB_SCRIPT_ARABIC:
    case HB_SCRIPT_HEBREW:
        return HB_DIRECTION_RTL;
    default:
        return HB_DIRECTION_LTR;
    }
}

static FontSlot script_to_font (hb_script_t script) {
    switch (script) {
    case HB_SCRIPT_LATIN:      return FONT_LATIN;
    case HB_SCRIPT_CYRILLIC:   return FONT_LATIN;
    case HB_SCRIPT_DEVANAGARI: return FONT_LATIN;
    case HB_SCRIPT_ARABIC:     return FONT_ARABIC;
    case HB_SCRIPT_COMMON:     return FONT_EMOJI;
    case HB_SCRIPT_HIRAGANA:   return FONT_JAPANESE;
    case HB_SCRIPT_KATAKANA:   return FONT_JAPANESE;
    default:                   return FONT_NONE;
    }
}

static hb_script_t codepoint_to_script (U32 codepoint) {
    switch (codepoint) {
    case 0x0020 ... 0x007F: case 0x00A0 ... 0x00FF: case 0x0100 ... 0x017F: case 0x0180 ... 0x024F: return HB_SCRIPT_LATIN;
    case 0x0400 ... 0x04FF: return HB_SCRIPT_CYRILLIC;
    case 0x0900 ... 0x097F: return HB_SCRIPT_DEVANAGARI;
    case 0x0600 ... 0x06FF: return HB_SCRIPT_ARABIC;
    case 0x3041 ... 0x3096: return HB_SCRIPT_HIRAGANA;
    case 0x30A0 ... 0x30FF: return HB_SCRIPT_KATAKANA;
    default:                return HB_SCRIPT_COMMON;
    }
}

static SliceScriptRange get_ranges (Mem *mem, String data) {
    ArrayScriptRange ranges;
    array_init(&ranges, mem);

    ScriptRange current_range = {};
    Bool have_current_range = false;
    U64 byte_index = 0;

    str_utf8_iter (c, data) {
        Auto script = codepoint_to_script(c.decode.codepoint);

        if (have_current_range) {
            if (current_range.script == script) {
                current_range.end = byte_index + c.decode.inc - 1;
            } else {
                array_push(&ranges, current_range);
                current_range = (ScriptRange){
                    .script = script,
                    .start = byte_index,
                    .end = byte_index + c.decode.inc - 1,
                };
            }
        } else {
            have_current_range = true;
            current_range = (ScriptRange){
                .script = script,
                .start = byte_index,
                .end = byte_index + c.decode.inc - 1,
            };
        }

        byte_index += c.decode.inc;
    }

    if (have_current_range) array_push(&ranges, current_range);

    return ranges.as_slice;
}

SliceGlyphInfo get_glyph_infos (GlyphCache *cache, Mem *mem, String data) {
    tmem_new(tm);
    Auto ranges = get_ranges(tm, data);

    ArrayGlyphInfo infos;
    array_init(&infos, mem);

    I32 cursor_x = 0;
    I32 cursor_y = 0;

    array_iter (range, &ranges, *) {
        Auto buffer = hb_buffer_create();
        if (! hb_buffer_allocation_successful(buffer)) continue;

        hb_buffer_set_direction(buffer, script_to_direction(range->script));
        hb_buffer_set_script(buffer, range->script);

        String slice = str_slice(data, range->start, range->end - range->start + 1);
        hb_buffer_add_utf8(buffer, slice.data, slice.count, 0, slice.count);

        Auto font_slot = script_to_font(range->script);
        if (font_slot == FONT_NONE) {
            hb_buffer_destroy(buffer);
            continue;
        }

        Auto font = array_ref(&cache->font_slots, font_slot);
        hb_shape(font->hb_font, buffer, 0, 0);

        Slice(hb_glyph_info_t) hb_infos;
        Slice(hb_glyph_position_t) hb_positions;

        U32 info_count;
        hb_infos.data = hb_buffer_get_glyph_infos(buffer, &info_count);
        hb_infos.count = info_count;

        U32 position_count;
        hb_positions.data = hb_buffer_get_glyph_positions(buffer, &position_count);
        hb_positions.count = position_count;

        array_iter (info, &hb_infos) {
            Auto pos = array_get(&hb_positions, ARRAY_IDX);
            UtfDecode codepoint = str_utf8_decode(str_suffix_from(slice, info.cluster));

            array_push_lit(&infos,
                .x = cursor_x + (pos.x_offset >> 6),
                .y = cursor_y + (pos.y_offset >> 6),
                .x_advance = pos.x_advance >> 6,
                .y_advance = pos.y_advance >> 6,
                .glyph_index = info.codepoint, // After shaping harfbuzz sets this field to the glyph index.
                .codepoint = codepoint.codepoint,
                .font_slot = font_slot,
            );

            cursor_x += pos.x_advance >> 6;
            cursor_y += pos.y_advance >> 6;
        }

        hb_buffer_destroy(buffer);
    }

    return infos.as_slice;
}
