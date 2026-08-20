/*************************************************************
 * test/reaction_class_test.cpp
 *
 * Coverage for platform/reaction_class.cpp. Every label and comment used here
 * is copied verbatim out of the MK2 character sources (MKJC/MKLK/MKHH/MKSK/...
 * anitabs), because the whole module is a bet on that hand-written 1993
 * vocabulary. The interesting cases are the attacker/victim near-homographs
 * that sit next to each other in the file.
 *************************************************************/
#include "reaction_class.h"

#include <cstdio>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

/* Assert a label/comment pair lands in `want`, and print what it actually
   decided (plus why) when it does not. */
static void ExpectKind(const char *label, const char *comment, ReactionKind want)
{
    ReactionInfo got = ReactionClassify(label, comment);
    if (got.kind != want) {
        std::fprintf(stderr, "FAIL %s: %s (%s) -> %s [%s], wanted %s\n",
                     __FILE__, label, comment ? comment : "",
                     ReactionKindName(got.kind), got.rule.c_str(),
                     ReactionKindName(want));
        g_fails++;
    }
}

int main(void)
{
    /* ---- Hit: the three ordinary blows every fighter has ---- */
    ExpectKind("a_jchithi", "hit high", ReactionKind::Hit);
    ExpectKind("a_lkhitlo", "hit low", ReactionKind::Hit);
    ExpectKind("a_stduckhit", "hit while ducking", ReactionKind::Hit);
    /* Kintaro's abbreviated table: the label is just "hit", the comment
       carries the meaning. */
    ExpectKind("a_skhit", "c = getting hit", ReactionKind::Hit);

    /* Attacks whose names brush up against the hit stems must stay out. */
    ExpectKind("a_hhhipunch", "hi punch", ReactionKind::NotReaction);
    ExpectKind("a_hhhikick", "hi kick", ReactionKind::NotReaction);
    ExpectKind("a_jchikick", "hi kick", ReactionKind::NotReaction);
    ExpectKind("a_hhbackfist", "backfist", ReactionKind::NotReaction);

    /* ---- Stagger ---- */
    ExpectKind("a_jcstumble", "stumble back", ReactionKind::Stagger);
    ExpectKind("a_sk_stumble", "10 = stumble", ReactionKind::Stagger);
    ExpectKind("a_rdstunned", "stunned", ReactionKind::Stagger);
    ExpectKind("a_nutcrunched", "ouch", ReactionKind::Stagger);
    ExpectKind("a_bike_kicked", "bicycle kicked", ReactionKind::Stagger);
    ExpectKind("a_sk_noogied", "13 = noogy by jax", ReactionKind::Stagger);
    ExpectKind("a_banged", "banged by slow proj", ReactionKind::Stagger);
    /* "banged" not "bang": a_ring_bang is Reptile's prop, not a reaction. */
    ExpectKind("a_ring_bang", "reptile ring BANG!", ReactionKind::NotReaction);
    /* a_noogy is Jax GIVING the noogy; a_sk_noogied is Kintaro taking it. */
    ExpectKind("a_noogy", "noogy !!", ReactionKind::NotReaction);

    /* ---- Knockdown ---- */
    ExpectKind("a_jckdown", "knocked down", ReactionKind::Knockdown);
    ExpectKind("a_jcsweepfall", "sweep fall", ReactionKind::Knockdown);
    ExpectKind("a_sk_swept", "7 = swept", ReactionKind::Knockdown);
    ExpectKind("a_jcbodyslam", "body slam", ReactionKind::Knockdown);
    /* Kintaro's a_sk_slam is the strike that CAUSES a body slam. */
    ExpectKind("a_sk_slam", "1 = body slam strike", ReactionKind::NotReaction);
    /* a_sk_talkdown is a taunt that happens to end in the knockdown stem. */
    ExpectKind("a_sk_talkdown", "19 =", ReactionKind::NotReaction);
    ExpectKind("a_sk_talkup", "18 =", ReactionKind::NotReaction);
    /* The sweep itself is an attack; only the fall and the getup are not. */
    ExpectKind("a_jcsweep", "sweep kick", ReactionKind::NotReaction);

    /* ---- Getup beats Knockdown on the shared "sweep" stem ---- */
    ExpectKind("a_jcgetup", "normal getup", ReactionKind::Getup);
    ExpectKind("a_jcsweepup", "getup from sweep kick", ReactionKind::Getup);
    ExpectKind("a_skgetup", "b = getup", ReactionKind::Getup);

    /* ---- FlippedBy, with the thrower pulled out of the label ---- */
    {
        ReactionInfo r = ReactionClassify("a_jc_fb_lk", "cage flipped by liu kang");
        CHECK(r.kind == ReactionKind::FlippedBy);
        CHECK(r.thrower == "lk");
        const char *who = ReactionFighterName(r.thrower.c_str());
        CHECK(who != NULL && std::string(who) == "Liu Kang");
    }
    {
        ReactionInfo r = ReactionClassify("a_fb_goro", "flipped by goro's evil twin");
        CHECK(r.kind == ReactionKind::FlippedBy);
        CHECK(r.thrower == "goro");
    }
    /* Uppercase labels classify the same as lowercase ones. */
    {
        ReactionInfo r = ReactionClassify("A_HH_FB_ST", "hathead flipped by shang tsung");
        CHECK(r.kind == ReactionKind::FlippedBy);
        CHECK(r.thrower == "st");
    }
    /* Only FlippedBy fills in a thrower. */
    CHECK(ReactionClassify("a_jchithi", "hit high").thrower.empty());

    /* ---- Death: fatality victim bodies ---- */
    ExpectKind("a_decapfall", "decapitated dude fall", ReactionKind::Death);
    ExpectKind("a_torso_ripped", "torso ripped", ReactionKind::Death);
    ExpectKind("a_impaled", "impaled", ReactionKind::Death);
    ExpectKind("a_drained", "drained of my soul by shang tsung", ReactionKind::Death);
    ExpectKind("a_shredded", "", ReactionKind::Death);
    ExpectKind("a_back_broke", "back getting broken", ReactionKind::Death);
    ExpectKind("a_headhole_fall", "headhole fatality fall (Cage)", ReactionKind::Death);
    ExpectKind("a_swipe_torso", "scorpion swiped me torso", ReactionKind::Death);
    ExpectKind("a_sz_spine", "head-rip prop: the spine", ReactionKind::Death);
    /* a_head has no reaction stem in the label; the comment carries it. */
    ExpectKind("a_head", "decapitated head", ReactionKind::Death);

    /* ---- The attacker halves of those same fatalities ---- */
    ExpectKind("a_back_breaker", "breaking someones back", ReactionKind::NotReaction);
    ExpectKind("a_arm_rip", "ripping of other dude's arms", ReactionKind::NotReaction);
    ExpectKind("a_jcrip", "torso rip", ReactionKind::NotReaction);
    ExpectKind("a_jc_headhole", "headhole fatality", ReactionKind::NotReaction);
    ExpectKind("a_sashred", "shredder !", ReactionKind::NotReaction);
    ExpectKind("a_st_drain", "c", ReactionKind::NotReaction);
    ExpectKind("a_eat_head", "reptile fatality", ReactionKind::NotReaction);
    ExpectKind("a_sz_head_jc", "Johnny Cage", ReactionKind::NotReaction);
    ExpectKind("a_death_zap1", "lift fatality body", ReactionKind::NotReaction);
    ExpectKind("a_death_kiss1", "death kiss", ReactionKind::NotReaction);
    ExpectKind("a_sc_swipe", "death blow swipe", ReactionKind::NotReaction);
    ExpectKind("a_sastab", "stab death blow", ReactionKind::NotReaction);
    ExpectKind("a_clap", "clap death blow", ReactionKind::NotReaction);
    ExpectKind("a_sk_fatham", "d = hammer smash fatality body", ReactionKind::NotReaction);
    ExpectKind("a_st_decap_laugh", "now Old Shang decap laugh", ReactionKind::NotReaction);
    ExpectKind("a_scorpion_skull", "scorpion skull head", ReactionKind::NotReaction);

    /* ---- Ordinary non-reactions ---- */
    ExpectKind("a_jcstance", "stance", ReactionKind::NotReaction);
    ExpectKind("a_jcwalkf", "walk forward", ReactionKind::NotReaction);
    ExpectKind("a_jcuppercut", "uppercut", ReactionKind::NotReaction);
    ExpectKind("a_jcblockhi", "standing block", ReactionKind::NotReaction);
    ExpectKind("a_jcvictory", "victory", ReactionKind::NotReaction);
    ExpectKind("a_jczap", "cage throwing fireball", ReactionKind::NotReaction);
    ExpectKind("a_st_2_lk", "shang tsung morph into liu kang", ReactionKind::NotReaction);

    /* ---- Degenerate input ---- */
    {
        ReactionInfo r = ReactionClassify(NULL, NULL);
        CHECK(r.kind == ReactionKind::NotReaction);
        CHECK(r.rule == "no label");
    }
    ExpectKind("", "hit high", ReactionKind::NotReaction);  /* label is required */
    /* A comment alone is enough when the label is opaque. */
    ExpectKind("a_xyzzy", "knocked down", ReactionKind::Knockdown);

    /* ---- Every kind names itself and explains itself ---- */
    for (int i = 0; i < kReactionKindCount; i++) {
        ReactionKind k = (ReactionKind)i;
        CHECK(ReactionKindName(k) != NULL && ReactionKindName(k)[0] != '\0');
        CHECK(ReactionKindHelp(k) != NULL && ReactionKindHelp(k)[0] != '\0');
    }
    CHECK(ReactionFighterName("zz") == NULL);
    CHECK(ReactionFighterName(NULL) == NULL);

    /* ---- Whatever fires, it says why ---- */
    CHECK(!ReactionClassify("a_jckdown", "knocked down").rule.empty());
    CHECK(!ReactionClassify("a_jcstance", "stance").rule.empty());

    if (g_fails == 0) {
        std::printf("PASS: reaction classification separates victim from attacker\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d reaction check(s)\n", g_fails);
    return 1;
}
