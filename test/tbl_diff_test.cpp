/*************************************************************
 * test/tbl_diff_test.cpp
 *
 * Unit coverage for the .TBL parser and IMG-vs-table diff in
 * platform/tbl_diff.cpp. No UI/SDL dependencies.
 *
 * The fixture is a trimmed copy of mk2-main/src/MK1SKULL.TBL, the
 * hand-maintained table that has no build step to regenerate it — the exact
 * situation where an imgtool re-export that moved an anipoint would desync
 * art from table silently.
 *************************************************************/
#include "tbl_diff.h"

#include <cstdio>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static const char *kMk1Skull =
    "\t.DATA\n"
    "MK1FIRE1:\n"
    "\t.word   89,110,75,-64\n"
    "\t.long   03d27000H\n"
    "\t.word   06680H\n"
    "MK1FIRE6:\n"
    "\t.word   30,118,37,-56\n"
    "\t.long   03d57eb2H\n"
    "\t.word   06080H\n"
    "MK1SKEL1:\n"
    "\t.word   57,137,9,-23\n"
    "\t.long   03d5c80eH\n"
    "\t.word   06280H\n"
    "\t.long   BLOOD_P\n"
    "\t.TEXT\n";

static const TblDiffRow *find_row(const std::vector<TblDiffRow> &rows,
                                  const char *name)
{
    for (size_t i = 0; i < rows.size(); i++)
        if (rows[i].name == name) return &rows[i];
    return NULL;
}

static const TblFieldDiff *find_field(const TblDiffRow &row, const char *field)
{
    for (size_t i = 0; i < row.fields.size(); i++)
        if (row.fields[i].field == field) return &row.fields[i];
    return NULL;
}

static TblEntry make_img(const char *name, int w, int h, int ax, int ay)
{
    TblEntry e;
    e.name = name;
    e.w = w; e.h = h;
    e.anix = ax; e.aniy = ay;
    e.anix2 = e.aniy2 = e.aniz2 = 0;
    e.has_secondary = false;
    e.has_sag = false; e.sag = 0;
    e.has_flags = false; e.flags = 0;
    e.pal_explicit = false;
    e.line = 0;
    return e;
}

int main(void)
{
    /* ---- Parse ---- */
    std::vector<TblEntry> tbl;
    std::vector<std::string> warnings;
    std::string err;
    CHECK(ParseTblText(kMk1Skull, tbl, &warnings, &err) == true);
    CHECK(tbl.size() == 3);
    if (tbl.size() == 3) {
        CHECK(tbl[0].name == "MK1FIRE1");
        CHECK(tbl[0].w == 89 && tbl[0].h == 110);
        CHECK(tbl[0].anix == 75 && tbl[0].aniy == -64);
        CHECK(tbl[0].has_sag && tbl[0].sag == 0x3d27000u);
        CHECK(tbl[0].has_flags && tbl[0].flags == 0x6680u);
        CHECK(tbl[0].has_secondary == false);   /* four-word MK2 form */
        CHECK(tbl[2].name == "MK1SKEL1");
        CHECK(tbl[2].anix == 9 && tbl[2].aniy == -23);
        CHECK(tbl[2].pal_label == "BLOOD_P");
        CHECK(tbl[2].pal_explicit == true);
        /* The palette only appears on the last record, so earlier ones carry
           no label rather than inheriting a later one. */
        CHECK(tbl[0].pal_label.empty());
    }
    CHECK(warnings.empty());

    /* .DATA/.TEXT and comments are ignored; a comment must not eat a record. */
    {
        std::vector<TblEntry> t2;
        std::string e2;
        const char *src =
            "\t.DATA\n"
            "; leading comment\n"
            "FOO:\n"
            "\t.word   4,5,1,2   ; W,H,ANIX,ANIY\n"
            "\t.long   0100H\n"
            "\t.word   00080H\n"
            "\t.TEXT\n";
        CHECK(ParseTblText(src, t2, NULL, &e2) == true);
        CHECK(t2.size() == 1);
        if (t2.size() == 1) {
            CHECK(t2[0].name == "FOO");
            CHECK(t2[0].w == 4 && t2[0].h == 5 && t2[0].anix == 1 && t2[0].aniy == 2);
        }
    }

    /* Seven-word MK3 geometry carries the secondary anipoint. */
    {
        std::vector<TblEntry> t3;
        std::string e3;
        const char *src = "BAR:\n\t.word 10,20,3,4,5,6,7\n\t.long 0200H\n";
        CHECK(ParseTblText(src, t3, NULL, &e3) == true);
        CHECK(t3.size() == 1);
        if (t3.size() == 1) {
            CHECK(t3[0].has_secondary == true);
            CHECK(t3[0].anix2 == 5 && t3[0].aniy2 == 6 && t3[0].aniz2 == 7);
        }
    }

    /* A label with no geometry is warned about, not silently emitted. */
    {
        std::vector<TblEntry> t4;
        std::vector<std::string> w4;
        std::string e4;
        const char *src = "EMPTY:\nREAL:\n\t.word 1,2,3,4\n";
        CHECK(ParseTblText(src, t4, &w4, &e4) == true);
        CHECK(t4.size() == 1);
        CHECK(w4.size() == 1);
    }

    /* Real tables inline palette blobs between sprite records (MKOUTER's
       OHOUTBLP, MKBGANI's OH_TIMEFLR_p): a colour count, then rows of hex
       .word data. Those are normal, so they are skipped WITHOUT a warning,
       and the record that references them still binds the label. */
    {
        std::vector<TblEntry> t6;
        std::vector<std::string> w6;
        std::string e6;
        const char *src =
            "OHTEMPLE:\n"
            "\t.word   109,117,-2,-3\n"
            "\t.long   03e18008H\n"
            "\t.word   06a80H\n"
            "\t.long   OHOUTBLP\n"
            "OHOUTBLP:\n"
            "\t.word\t 45\n"
            "\t.word\t00H,00H,01H,02H,03H,04H,05H,06H\n"
            "\t.word\t022H,023H,024H,025H,0423H,026H,0424H,027H\n";
        CHECK(ParseTblText(src, t6, &w6, &e6) == true);
        CHECK(t6.size() == 1);
        CHECK(w6.empty());
        if (t6.size() == 1) {
            CHECK(t6[0].name == "OHTEMPLE");
            CHECK(t6[0].pal_label == "OHOUTBLP");
            CHECK(t6[0].anix == -2 && t6[0].aniy == -3);
        }
    }

    /* A .set symbol table (MKZIP.TBL, MKFLOORS.TBL) is not a sprite table;
       the error should say so rather than read like a parse bug. */
    {
        std::vector<TblEntry> t7;
        std::string e7;
        const char *src =
            "\t.DATA\n"
            "FL_ARM\t.set\t03000000h\n"
            "FL_TOW\t.set\t0306cfc0h\n"
            "\t.TEXT\n";
        CHECK(ParseTblText(src, t7, NULL, &e7) == false);
        CHECK(e7.find(".set") != std::string::npos ||
              e7.find("not a sprite table") != std::string::npos);
    }

    /* Non-table text fails with a reason rather than returning junk. */
    {
        std::vector<TblEntry> t5;
        std::string e5;
        CHECK(ParseTblText("just some prose\nwith no labels\n", t5, NULL, &e5) == false);
        CHECK(!e5.empty());
    }

    /* ---- Diff ---- */
    std::vector<TblEntry> img;
    /* MK1FIRE1 matches the table exactly. */
    img.push_back(make_img("MK1FIRE1", 89, 110, 75, -64));
    /* MK1FIRE6 carries the pre-fix anipoint: the exact drift this catches. */
    img.push_back(make_img("MK1FIRE6", 30, 118, -106, -56));
    /* MK1SKEL1 differs only in palette — the MKBLOOD-style divergence where
       the IMG binds BLOODNB_P and the shipped table says BLOOD_P. */
    {
        TblEntry e = make_img("MK1SKEL1", 57, 137, 9, -23);
        e.pal_label = "BLOODNB_P";
        img.push_back(e);
    }
    /* Present in the IMG, absent from the table. */
    img.push_back(make_img("MK1SKEL9", 20, 20, 5, 5));

    std::vector<TblDiffRow> rows;
    TblDiffSummary sum;
    DiffTblEntries(tbl, img, rows, &sum);

    CHECK(sum.matched == 1);
    CHECK(sum.differing == 2);
    CHECK(sum.only_in_tbl == 0);
    CHECK(sum.only_in_img == 1);

    {
        const TblDiffRow *r = find_row(rows, "MK1FIRE1");
        CHECK(r && r->kind == TblDiff_Match);
        CHECK(r && r->fields.empty());
    }
    {
        const TblDiffRow *r = find_row(rows, "MK1FIRE6");
        CHECK(r && r->kind == TblDiff_Differs);
        if (r) {
            const TblFieldDiff *f = find_field(*r, "ANIX");
            CHECK(f != NULL);
            CHECK(f && f->tbl_value == "37" && f->img_value == "-106");
            /* Size agrees, so it must not be reported. */
            CHECK(find_field(*r, "W") == NULL);
        }
    }
    {
        const TblDiffRow *r = find_row(rows, "MK1SKEL1");
        CHECK(r && r->kind == TblDiff_Differs);
        if (r) {
            const TblFieldDiff *f = find_field(*r, "PAL");
            CHECK(f != NULL);
            CHECK(f && f->tbl_value == "BLOOD_P" && f->img_value == "BLOODNB_P");
        }
    }
    {
        const TblDiffRow *r = find_row(rows, "MK1SKEL9");
        CHECK(r && r->kind == TblDiff_OnlyInImg);
    }

    /* SAG is not drift: the IMG side never claims to know a ROM bit address,
       so a table SAG must never show up as a difference. Same for FLAGS —
       a table's flags word is a DMA control word, while IMG.flags is the
       editor's mark/loaded/changed bitfield. Comparing them reported drift on
       all 42 rows of the real MK1SKULL pair whose geometry matched exactly. */
    for (size_t i = 0; i < rows.size(); i++) {
        CHECK(find_field(rows[i], "SAG") == NULL);
        CHECK(find_field(rows[i], "FLAGS") == NULL);
    }

    /* A sprite the table names but the library lacks is reported, not dropped. */
    {
        std::vector<TblEntry> only_one;
        only_one.push_back(make_img("MK1FIRE1", 89, 110, 75, -64));
        std::vector<TblDiffRow> r2;
        TblDiffSummary s2;
        DiffTblEntries(tbl, only_one, r2, &s2);
        CHECK(s2.only_in_tbl == 2);
        const TblDiffRow *row = find_row(r2, "MK1SKEL1");
        CHECK(row && row->kind == TblDiff_OnlyInTbl);
    }

    /* ---- Report ---- */
    {
        std::string report = FormatTblDiffReport("MK1SKULL.TBL", rows, sum);
        CHECK(report.find("MK1FIRE6") != std::string::npos);
        CHECK(report.find("ANIX") != std::string::npos);
        /* Matching rows stay out of the report so drift is what you read. */
        CHECK(report.find("MK1FIRE1") == std::string::npos);

        std::vector<TblDiffRow> clean;
        TblDiffSummary clean_sum = {3, 0, 0, 0};
        std::string ok = FormatTblDiffReport("MK1SKULL.TBL", clean, clean_sum);
        CHECK(ok.find("No drift.") != std::string::npos);
    }

    if (g_fails == 0) std::printf("tbl_diff_test: all checks passed\n");
    else std::fprintf(stderr, "tbl_diff_test: %d failure(s)\n", g_fails);
    return g_fails ? 1 : 0;
}
