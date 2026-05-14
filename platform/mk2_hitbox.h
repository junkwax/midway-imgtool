/*************************************************************
 * platform/mk2_hitbox.h
 * MK2 strike-table editor (MKSTK.ASM).
 *
 * MK2 hitboxes are NOT inside the IMG container; they live in a
 * separate hand-written assembly file (src/MKSTK.ASM in the mk2
 * source tree). Each move ("stk_*") is 8 fields: x/y/w/h of the
 * collision box, the strike routine token, packed hit/block
 * damage, score (long), and a sound-flag token. Character tables
 * ("xx_strikes") list the moves per fighter.
 *
 * imgtool parses MKSTK.ASM as the source of truth, edits in place,
 * and writes back preserving comments and symbolic literals.
 *************************************************************/
#ifndef MK2_HITBOX_H
#define MK2_HITBOX_H

#include <string>
#include <vector>
#include <cstdint>

namespace mk2 {

struct StrikeField {
    /* The raw token as it appeared in the ASM (e.g. "059h", "-00ah",
       "sf_squeeze"). Preserved verbatim on save unless the user edits
       the numeric value. */
    std::string raw;
    /* Parsed numeric value, valid when has_value is true. Symbolic
       tokens (like "sf_squeeze") have has_value=false. */
    int32_t     value = 0;
    bool        has_value = false;
    /* 1-based line number in MKSTK.ASM where this field lives. */
    int         line = 0;
};

struct StrikeRecord {
    std::string label;        /* "stk_lkhikick" */
    int         label_line = 0;
    /* 8 fields in the canonical MKSTK order:
       0 x_offset, 1 y_offset, 2 x_size, 3 y_size,
       4 strike_routine, 5 damage (hi=hit, lo=block),
       6 score (32-bit), 7 sound. */
    StrikeField fields[8];
};

struct CharTable {
    std::string             name;   /* "lk", "jc", ... */
    int                     line = 0;
    std::vector<std::string> moves; /* labels in slot order */
};

struct Document {
    std::string                source_path;     /* absolute path to MKSTK.ASM */
    std::vector<std::string>   raw_lines;       /* file contents, 1-based via raw_lines[i-1] */
    std::vector<StrikeRecord>  records;
    std::vector<CharTable>     char_tables;
    bool                       dirty = false;
};

/* Field accessors — index into StrikeRecord::fields. */
enum FieldIdx {
    F_X_OFFSET = 0,
    F_Y_OFFSET = 1,
    F_X_SIZE   = 2,
    F_Y_SIZE   = 3,
    F_STRIKE   = 4,
    F_DAMAGE   = 5,
    F_SCORE    = 6,
    F_SOUND    = 7,
    F_COUNT    = 8
};
extern const char *kFieldNames[F_COUNT];
/* true for the 32-bit score field; the rest are 16-bit words. */
bool field_is_long(int idx);

/* Parse MKSTK.ASM. Returns true on success. doc->source_path is set
   to the resolved path. Errors are returned via err (may be NULL). */
bool load(Document *doc, const char *path, std::string *err);

/* Write doc back to source_path, applying any edits made via set_value().
   Lines that weren't touched are written verbatim. Returns true on success. */
bool save(Document *doc, std::string *err);

/* Update a numeric field. The on-disk literal is reformatted in the same
   style as the original (hex `0XXh` for hex values, plain decimal otherwise).
   Symbolic fields (strike_routine / sound) can be edited as text via
   set_raw(). Pushes doc->dirty true if anything changed. */
void set_value(Document *doc, int record_idx, int field_idx, int32_t value);
void set_raw  (Document *doc, int record_idx, int field_idx, const char *raw);

/* Character-table helpers. Returns -1 if not found. */
int find_char_table(const Document *doc, const char *name);
int find_record    (const Document *doc, const char *label);

/* Decompose / pack the damage word. */
inline int damage_hit  (int dmg) { return (dmg >> 8) & 0xFF; }
inline int damage_block(int dmg) { return dmg & 0xFF; }
inline int pack_damage (int hit, int block) {
    return (int)(((hit & 0xFF) << 8) | (block & 0xFF));
}

} // namespace mk2

#endif /* MK2_HITBOX_H */
