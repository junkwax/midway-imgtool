/*************************************************************
 * platform/reaction_class.cpp
 * See reaction_class.h for what this is sorting and why.
 *************************************************************/
#include "reaction_class.h"

#include <cctype>
#include <cstring>

namespace {

std::string Lower(const char *s)
{
    std::string out;
    if (!s) return out;
    while (*s) { out += (char)tolower((unsigned char)*s); s++; }
    /* Trim: anitab comments arrive with the tabs and trailing spaces the
       original author left in ("a_banged\t; 3a - \t"). */
    size_t b = out.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = out.find_last_not_of(" \t\r\n");
    return out.substr(b, e - b + 1);
}

bool Has(const std::string &hay, const char *needle)
{
    return hay.find(needle) != std::string::npos;
}

/* A rule is a substring plus the family it implies. Checked in table order, so
   longer and more specific spellings must come first — "sweepfall" has to beat
   "fall", and the attacker veto table has to run before any of these. */
struct Rule {
    const char  *needle;
    ReactionKind kind;
};

/* "label 'kdown'" — the phrasing the UI shows when a rule fires. */
std::string Because(const char *where, const char *needle)
{
    return std::string(where) + " '" + needle + "'";
}

/* Attacker-side spellings that would otherwise trip a victim rule. These name
   the fighter DOING the thing, so they are not reactions no matter what else
   matches. Kept as its own pass rather than as ordering luck inside the victim
   table: the pairs are near-homographs and a future edit to the victim rules
   should not be able to silently flip one of these. */
const char *kAttackerLabels[] = {
    "back_breaker",   /* breaking someone's back  (vs a_back_broke) */
    "arm_rip",        /* ripping the other dude's arms (vs a_torso_ripped) */
    "jcrip",          /* Cage's torso rip (vs a_torso_ripped) */
    "eat_head",       /* Reptile's fatality (vs a_head) */
    "sz_head_",       /* Sub-Zero holding a torn-off head — his prop, his hand */
    "jc_headhole",    /* Cage's fatality (vs a_headhole_fall) */
    "sashred",        /* Baraka shredding (vs a_shredded) */
    "st_drain",       /* Shang draining (vs a_drained) */
    "st_decap",       /* Shang's decap windup/laugh (vs a_decapfall) */
    "death_",         /* death_blow/kiss/zap/bolt/shock/spear — all attacker */
    "sc_swipe",       /* Scorpion's death-blow swipe (vs a_swipe_torso) */
    "sastab", "fn2_stab",
    "scorpion_skull",
    "fatham",         /* Kintaro's hammer-smash fatality body */
    "sk_slam",        /* Kintaro's body-slam STRIKE (vs a_*bodyslam, the victim) */
    /* Kintaro's taunt pair. a_sk_talkdown is the only label in the whole
       character corpus where "kdown" is a coincidence rather than a knockdown,
       and it ends in the stem, so no anchoring rule separates it — it has to be
       named. Its partner a_sk_talkup is listed for symmetry. */
    "sk_talkdown", "sk_talkup",
};

const char *kAttackerComments[] = {
    "death blow",
    "ripping of other dude",
    "breaking someones",
    "breaking someone's",
};

/* Victim-side label spellings. */
const Rule kLabelRules[] = {
    /* Death: fatality victim bodies and the pieces that come off them. Each
       spelling is the past-tense/result form so it cannot collide with the
       attacker move of the same name. */
    { "decapfall",     ReactionKind::Death },
    { "headhole_fall", ReactionKind::Death },
    { "torso_ripped",  ReactionKind::Death },
    { "swipe_torso",   ReactionKind::Death },
    { "back_broke",    ReactionKind::Death },
    { "impaled",       ReactionKind::Death },
    { "drained",       ReactionKind::Death },
    { "shredded",      ReactionKind::Death },
    { "sz_spine",      ReactionKind::Death },  /* the spine pulled out of you */

    /* Getup before Knockdown: "sweepup" and "sweepfall" share a stem, and
       a getup is the recovery, not the fall. */
    { "sweepup",       ReactionKind::Getup },
    { "getup",         ReactionKind::Getup },

    { "sweepfall",     ReactionKind::Knockdown },
    { "kdown",         ReactionKind::Knockdown },
    { "swept",         ReactionKind::Knockdown },
    { "bodyslam",      ReactionKind::Knockdown },

    { "stumble",       ReactionKind::Stagger },
    { "stunned",       ReactionKind::Stagger },
    { "nutcrunched",   ReactionKind::Stagger },
    { "bike_kicked",   ReactionKind::Stagger },
    { "noogied",       ReactionKind::Stagger },
    { "banged",        ReactionKind::Stagger },  /* not "bang": a_ring_bang is a prop */

    { "duckhit",       ReactionKind::Hit },
    { "hithi",         ReactionKind::Hit },
    { "hitlo",         ReactionKind::Hit },
};

/* Comment-side rules, consulted only when the label said nothing. The comments
   are prose, so these are phrases rather than stems. */
const Rule kCommentRules[] = {
    { "decapitated",        ReactionKind::Death },
    { "fatality fall",      ReactionKind::Death },
    { "torso ripped",       ReactionKind::Death },
    { "swiped me",          ReactionKind::Death },
    { "back getting broken", ReactionKind::Death },
    { "impaled",            ReactionKind::Death },
    { "drained of my soul", ReactionKind::Death },

    { "getup",              ReactionKind::Getup },

    { "knocked down",       ReactionKind::Knockdown },
    { "sweep fall",         ReactionKind::Knockdown },
    { "body slam",          ReactionKind::Knockdown },
    { "swept",              ReactionKind::Knockdown },

    { "stumble back",       ReactionKind::Stagger },
    { "stunned",            ReactionKind::Stagger },
    { "ouch",               ReactionKind::Stagger },
    { "bicycle kicked",     ReactionKind::Stagger },
    /* "noogy by", not "noogy": a_noogy is Jax GIVING one ("noogy !!"), while
       a_sk_noogied is Kintaro taking it ("noogy by jax"). The label rule
       already catches the victim, so this only has to not claim the giver. */
    { "noogy by",           ReactionKind::Stagger },
    { "slow proj",          ReactionKind::Stagger },

    { "hit high",           ReactionKind::Hit },
    { "hit low",            ReactionKind::Hit },
    { "hit while ducking",  ReactionKind::Hit },
    { "getting hit",        ReactionKind::Hit },
};

} /* namespace */

const char *ReactionKindName(ReactionKind kind)
{
    switch (kind) {
    case ReactionKind::Hit:       return "Hit";
    case ReactionKind::Stagger:   return "Stagger";
    case ReactionKind::Knockdown: return "Knocked down";
    case ReactionKind::FlippedBy: return "Flipped by";
    case ReactionKind::Getup:     return "Getup";
    case ReactionKind::Death:     return "Death / fatality victim";
    case ReactionKind::NotReaction:
    default:                      return "Not a reaction";
    }
}

const char *ReactionKindHelp(ReactionKind kind)
{
    switch (kind) {
    case ReactionKind::Hit:
        return "Took a blow and recovered: hit high, hit low, hit while ducking.";
    case ReactionKind::Stagger:
        return "Staggered but still standing: stumble back, stunned, ouch, "
               "bicycle kicked.";
    case ReactionKind::Knockdown:
        return "Put on the floor: knocked down, sweep fall, body slam.";
    case ReactionKind::FlippedBy:
        return "Thrown, with one variant per fighter doing the throwing.";
    case ReactionKind::Getup:
        return "Standing back up after a knockdown or a sweep.";
    case ReactionKind::Death:
        return "Fatality victim bodies and the pieces torn off them.";
    case ReactionKind::NotReaction:
    default:
        return "Something this fighter does on its own: stance, movement, "
               "attacks, projectiles, and the attacking half of a fatality.";
    }
}

const char *ReactionFighterName(const char *code)
{
    if (!code) return NULL;
    std::string c = Lower(code);
    if (c == "jc") return "Johnny Cage";
    if (c == "lk") return "Liu Kang";
    if (c == "hh") return "Kung Lao";
    if (c == "jx") return "Jax";
    if (c == "rd") return "Raiden";
    if (c == "sa") return "Baraka";
    if (c == "st") return "Shang Tsung";
    if (c == "nj") return "Ninja";
    if (c == "fn") return "Female Ninja";
    if (c == "sz") return "Sub-Zero";
    if (c == "sc") return "Scorpion";
    if (c == "rp") return "Reptile";
    if (c == "sk") return "Kintaro";
    if (c == "kn") return "Shao Kahn";
    if (c == "goro") return "Goro";
    return NULL;
}

ReactionInfo ReactionClassify(const char *label, const char *comment)
{
    ReactionInfo info;
    std::string lab = Lower(label);
    std::string com = Lower(comment);
    if (lab.empty()) {
        info.rule = "no label";
        return info;
    }

    /* Pass 1: attacker veto. Runs first so no victim rule can claim the
       attacking half of a fatality pair. */
    for (const char *a : kAttackerLabels) {
        if (Has(lab, a)) {
            info.kind = ReactionKind::NotReaction;
            info.rule = "attacker half of a fatality pair";
            return info;
        }
    }
    for (const char *a : kAttackerComments) {
        if (Has(com, a)) {
            info.kind = ReactionKind::NotReaction;
            info.rule = "comment describes the attacker";
            return info;
        }
    }

    /* Pass 2: throws. a_<me>_fb_<thrower> is one entry per fighter who can
       throw you, so the thrower code is worth pulling out — the list is
       otherwise nine near-identical rows. */
    size_t fb = lab.find("_fb_");
    if (fb != std::string::npos) {
        info.kind = ReactionKind::FlippedBy;
        info.rule = "label '_fb_' (thrown)";
        info.thrower = lab.substr(fb + 4);
        return info;
    }
    if (Has(lab, "fb_goro")) {
        info.kind = ReactionKind::FlippedBy;
        info.rule = "label 'fb_goro' (thrown)";
        info.thrower = "goro";
        return info;
    }
    if (Has(com, "flipped by")) {
        info.kind = ReactionKind::FlippedBy;
        info.rule = "comment 'flipped by'";
        return info;
    }

    /* Pass 3: victim spellings in the label. */
    for (const Rule &r : kLabelRules) {
        if (Has(lab, r.needle)) {
            info.kind = r.kind;
            info.rule = Because("label", r.needle);
            return info;
        }
    }
    /* "hit" on its own is only safe anchored to the end of the label
       (a_skhit = "getting hit"); loose, it would swallow a_hhhipunch-shaped
       names the moment a character code ends in h-i-t. */
    if (lab.size() >= 3 && lab.compare(lab.size() - 3, 3, "hit") == 0) {
        info.kind = ReactionKind::Hit;
        info.rule = "label ends in 'hit'";
        return info;
    }

    /* Pass 4: the comment, for entries whose label says nothing useful. */
    for (const Rule &r : kCommentRules) {
        if (Has(com, r.needle)) {
            info.kind = r.kind;
            info.rule = Because("comment", r.needle);
            return info;
        }
    }

    info.rule = "no reaction spelling in label or comment";
    return info;
}
