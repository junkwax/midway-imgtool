/*************************************************************
 * platform/tbl_diff.h
 * Parse a Midway sprite .TBL and diff it field-by-field against what an IMG
 * would export.
 *
 * Motivation: MK2's src/*.TBL files are hand-maintained — no build step
 * regenerates them — so an imgtool re-export that changed anipoints would
 * silently desync art from table. This gives the tool the same check
 * mk2-main's build.py Phase D runs (img_anipoint.py sync-tbl --check), so
 * drift is visible on both ends.
 *
 * Pure logic — text in, rows out. No g_doc / ImGui / SDL coupling.
 *
 * Format accepted (matching e.g. mk2-main/src/MK1SKULL.TBL):
 *
 *       .DATA
 *   MK1FIRE1:
 *       .word   89,110,75,-64              ; W,H,ANIX,ANIY   (MK2 form)
 *       .long   03d27000H                  ; SAG (ROM bit address)
 *       .word   06680H                     ; flags
 *       .long   BLOOD_P                    ; palette label, only when it
 *                                          ; changes from the previous entry
 *       .TEXT
 *
 * The MK3 form carries seven geometry words (W,H,ANIX,ANIY,ANIX2,ANIY2,ANIZ2).
 * Comments (';') and blank lines are ignored, as are .DATA/.TEXT and any other
 * directive the parser doesn't recognize.
 *************************************************************/
#pragma once
#include <string>
#include <vector>

struct TblEntry {
    std::string  name;
    int          w, h;
    int          anix, aniy;
    int          anix2, aniy2, aniz2;
    bool         has_secondary;   /* seven-word (MK3) geometry line */
    bool         has_sag;
    unsigned int sag;
    bool         has_flags;
    unsigned int flags;
    /* Palette label in force for this entry. A .TBL only emits .long <PAL>
       when the palette changes, so the parser carries the last one forward;
       that is what makes a per-entry palette comparison meaningful. */
    std::string  pal_label;
    bool         pal_explicit;    /* this entry is where the label changed */
    int          line;            /* 1-based source line of the label */
};

/* Parse .TBL text. Returns false only when no labelled entry was found at all;
   `err` then explains why. Malformed individual records are skipped and
   reported through `warnings` rather than failing the whole parse. */
bool ParseTblText(const std::string &text,
                  std::vector<TblEntry> &out,
                  std::vector<std::string> *warnings,
                  std::string *err);

enum TblDiffKind {
    TblDiff_Match = 0,     /* every compared field agrees */
    TblDiff_Differs,       /* present on both sides, at least one field drifts */
    TblDiff_OnlyInTbl,     /* the table names a sprite the IMG doesn't have */
    TblDiff_OnlyInImg      /* the IMG has a marked sprite the table doesn't */
};

struct TblFieldDiff {
    std::string field;
    std::string tbl_value;
    std::string img_value;
};

struct TblDiffRow {
    std::string               name;
    TblDiffKind               kind;
    std::vector<TblFieldDiff> fields;
    int                       tbl_index;  /* index into the tbl vector, or -1 */
    int                       img_index;  /* index into the img vector, or -1 */
};

struct TblDiffSummary {
    int matched;
    int differing;
    int only_in_tbl;
    int only_in_img;
};

/* Compare a parsed table against IMG-derived entries, matching on name.
 *
 * SAG is compared only when both sides set has_sag. imgtool cannot know a
 * sprite's ROM bit address from the .IMG alone — LOAD2 assigns it when it
 * builds the IRW — so the IMG side normally leaves has_sag false and the
 * field is reported as informational rather than as drift.
 *
 * Secondary anipoints are compared only when the table carries them
 * (seven-word MK3 geometry); an MK2-form table simply doesn't encode them.
 *
 * Rows come back in table order, with IMG-only rows appended. */
void DiffTblEntries(const std::vector<TblEntry> &tbl,
                    const std::vector<TblEntry> &img,
                    std::vector<TblDiffRow> &out,
                    TblDiffSummary *summary);

/* Render the diff as a plain-text report suitable for the clipboard or a log. */
std::string FormatTblDiffReport(const std::string &tbl_path,
                                const std::vector<TblDiffRow> &rows,
                                const TblDiffSummary &summary);
