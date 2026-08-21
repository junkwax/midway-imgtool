/*************************************************************
 * test/strike_bind_test.cpp
 *
 * Coverage for platform/strike_bind.cpp. Every label here is copied out of
 * the MK2 sources — MKSTK.ASM's jc_strikes/lk_strikes and the anitabs in
 * MKJC.ASM / MKLK.ASM — because the module is a bet on how those two files
 * spell the same move. The cases that matter are the ones that do NOT match
 * letter for letter, and the ones that must refuse to guess.
 *************************************************************/
#include "strike_bind.h"

#include <cstdio>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

/* Cage's animation labels, as they appear in MKJC.ASM's anitab. */
static std::vector<std::string> CageAnims(void)
{
    return {
        "a_jcstance", "a_jcwalkf", "a_jcwalkb", "a_jcduck", "a_jcjumpup",
        "a_jcfflip", "a_jcbflip", "a_jcturn", "a_jcblockhi", "a_jcduckblock",
        "a_jchikick", "a_jclokick", "a_jchipunch", "a_jclopunch",
        "a_jcsweep", "a_jcsweepfall", "a_jcsweepup",
        "a_jcduckpunch", "a_jcduckkickh", "a_jcduckkickl",
        "a_jcuppercut", "a_jcjumpupkick", "a_jcflipkick", "a_jcflippunch",
        "a_jcroundh", "a_jcknee", "a_jcelbow", "a_jcshadow", "a_jczap",
        "a_shoruken", "a_jc_pp", "a_splits"
    };
}

static void ExpectAnim(const char *strike, const char *want_anim)
{
    std::vector<std::string> anims = CageAnims();
    int idx = strike_bind::MatchAnimForStrike(strike, anims);
    const char *got = (idx >= 0) ? anims[(size_t)idx].c_str() : "(none)";
    bool ok = want_anim ? (idx >= 0 && anims[(size_t)idx] == want_anim)
                        : (idx < 0);
    if (!ok) {
        std::fprintf(stderr, "FAIL %s: %s -> %s, wanted %s\n", __FILE__,
                     strike, got, want_anim ? want_anim : "(none)");
        g_fails++;
    }
}

int main(void)
{
    /* ---- The 14 that already agree letter for letter ---- */
    ExpectAnim("stk_jchikick",    "a_jchikick");
    ExpectAnim("stk_jclokick",    "a_jclokick");
    ExpectAnim("stk_jchipunch",   "a_jchipunch");
    ExpectAnim("stk_jclopunch",   "a_jclopunch");
    ExpectAnim("stk_jcduckpunch", "a_jcduckpunch");
    ExpectAnim("stk_jcduckkickh", "a_jcduckkickh");
    ExpectAnim("stk_jcduckkickl", "a_jcduckkickl");
    ExpectAnim("stk_jcuppercut",  "a_jcuppercut");
    ExpectAnim("stk_jcroundh",    "a_jcroundh");
    ExpectAnim("stk_jcknee",      "a_jcknee");
    ExpectAnim("stk_jcelbow",     "a_jcelbow");
    ExpectAnim("stk_jcshadow",    "a_jcshadow");

    /* ---- The abbreviated limb ---- */
    ExpectAnim("stk_jcflipk", "a_jcflipkick");
    ExpectAnim("stk_jcflipp", "a_jcflippunch");

    /* ---- The dropped sequence number: four zaps, one animation ---- */
    ExpectAnim("stk_jczap1", "a_jczap");
    ExpectAnim("stk_jczap4", "a_jczap");

    /* ---- The trailing-letter case that is NOT a limb: sweepk / sweep.
       "sweepk" normalises to "sweepkick", which no longer equals "sweep", so
       this lands on the prefix rule rather than the exact one. ---- */
    ExpectAnim("stk_jcsweepk", "a_jcsweep");

    /* ---- The character prefix is not always there ---- */
    ExpectAnim("stk_jcshoruken", "a_shoruken");

    /* ---- Refusals. Nothing in the anitab is jc_split, and guessing at
       a_splits (a different move, Cage's leg splits) would silently arm the
       wrong box. ---- */
    ExpectAnim("stk_jc_split", NULL);

    /* A move with no animation at all must not fall back to something that
       merely starts the same way. */
    {
        std::vector<std::string> two = { "a_lkhikick", "a_lkhikick2" };
        CHECK(strike_bind::MatchAnimForStrike("stk_lkhikick", two) == 0);
        /* Two candidates that normalise identically are a tie, not a pick. */
        std::vector<std::string> tie = { "a_lkflipkick", "a_lk_flip_kick" };
        CHECK(strike_bind::MatchAnimForStrike("stk_lkflipk", tie) == -1);
    }

    /* ---- The relation runs both ways ---- */
    {
        std::vector<std::string> strikes = {
            "stk_jchikick", "stk_jcflipk", "stk_jczap1", "stk_jc_split"
        };
        CHECK(strike_bind::MatchStrikeForAnim("a_jchikick", strikes) == 0);
        CHECK(strike_bind::MatchStrikeForAnim("a_jcflipkick", strikes) == 1);
        CHECK(strike_bind::MatchStrikeForAnim("a_jcstance", strikes) == -1);
    }

    /* ---- Stem / normalise ---- */
    CHECK(strike_bind::Stem("stk_jchikick") == "jchikick");
    CHECK(strike_bind::Stem("a_jchikick") == "jchikick");
    CHECK(strike_bind::Stem("JCHIKICK") == "jchikick");
    CHECK(strike_bind::NormalizeStem("jc_split") == "jcsplit");
    CHECK(strike_bind::NormalizeStem("jczap1") == "jczap");
    /* Normalising must not invent a limb: "jczap" ends in p and is not a
       punch, which is why the limb rule lives in Variants instead. */
    CHECK(strike_bind::NormalizeStem("jczap") == "jczap");
    CHECK(strike_bind::NormalizeStem("jcflipk") == "jcflipk");

    {
        std::vector<std::string> v = strike_bind::Variants("jcflipk");
        bool has_long = false, has_short = false;
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i] == "jcflipkick") has_long = true;
            if (v[i] == "jcflip") has_short = true;
        }
        CHECK(has_long);   /* flipk  -> flipkick */
        CHECK(has_short);  /* sweepk -> sweep    */
        /* The character code is optional in one direction only. */
        std::vector<std::string> s = strike_bind::Variants("jcshoruken");
        bool has_bare = false;
        for (size_t i = 0; i < s.size(); i++)
            if (s[i] == "shoruken") has_bare = true;
        CHECK(has_bare);
    }

    /* ---- Sidecar round-trip ---- */
    {
        const char *text =
            "# hand notes at the top\n"
            "\n"
            "a_jc_split = stk_jc_split\n"
            "a_jcjumpuppunch=stk_jcjumpupp\n"
            "   \n"
            "; semicolon comments too\n"
            "malformed line with no equals\n"
            " = stk_orphan\n"
            "a_jc_split = stk_jc_split_v2\n";   /* last writer wins */

        std::vector<strike_bind::Binding> b = strike_bind::ParseSidecar(text);
        CHECK(b.size() == 2);
        bool found_split = false, found_jump = false;
        for (size_t i = 0; i < b.size(); i++) {
            if (b[i].anim == "a_jc_split") {
                found_split = true;
                CHECK(b[i].strike == "stk_jc_split_v2");
            }
            if (b[i].anim == "a_jcjumpuppunch") {
                found_jump = true;
                CHECK(b[i].strike == "stk_jcjumpupp");
            }
        }
        CHECK(found_split);
        CHECK(found_jump);

        std::vector<std::string> comments = strike_bind::SidecarComments(text);
        CHECK(comments.size() == 2);

        std::string out = strike_bind::WriteSidecar(b, comments);
        std::vector<strike_bind::Binding> again = strike_bind::ParseSidecar(out);
        CHECK(again.size() == 2);
        /* Sorted, so the file does not churn between saves. */
        CHECK(again[0].anim == "a_jc_split");
        CHECK(again[1].anim == "a_jcjumpuppunch");
        /* The hand notes survived. */
        CHECK(strike_bind::SidecarComments(out).size() == 2);
    }

    /* ---- Sidecar path ---- */
    CHECK(strike_bind::SidecarPathFor("C:\\mk2\\src\\MKSTK.ASM") ==
          "C:\\mk2\\src\\MKSTK.imgtool");
    CHECK(strike_bind::SidecarPathFor("/mk2/src/MKSTK.ASM") ==
          "/mk2/src/MKSTK.imgtool");
    CHECK(strike_bind::SidecarPathFor("MKSTK") == "MKSTK.imgtool");
    CHECK(strike_bind::SidecarPathFor("").empty());

    if (g_fails == 0) std::printf("strike_bind_test: all checks passed\n");
    else std::fprintf(stderr, "strike_bind_test: %d failure(s)\n", g_fails);
    return g_fails ? 1 : 0;
}
