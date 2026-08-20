/*************************************************************
 * platform/reaction_class.h
 * Sort a character ASM's animations into "things that happen TO this fighter".
 *
 * A per-character MK*.ASM anitab mixes everything the fighter can do into one
 * flat directory: stance, walk, punches, projectiles, and — scattered among
 * them — the reactions. A reaction is the animation the fighter plays because
 * the OTHER fighter did something: hit high, knocked down, stumble back,
 * flipped by liu kang, torso ripped. Authoring an attack means staring at the
 * opponent's reaction next to it, so the reactions have to be findable as a
 * group rather than hunted for by eye through eighty entries.
 *
 * Attacker and victim halves of the same move are named almost identically and
 * sit next to each other in the file, so most of the work here is telling them
 * apart: a_back_breaker is the fighter breaking someone's back, a_back_broke is
 * the fighter's back being broken; a_arm_rip rips the other dude's arms,
 * a_torso_ripped is the torso that got ripped; a_st_drain drains a soul,
 * a_drained is the soul being drained. Only the second of each pair is a
 * reaction. Bare stem matching cannot see that difference, which is why the
 * rules below key on the victim-side spelling ("ripped", not "rip") and why an
 * explicit attacker-side veto runs before anything else.
 *
 * Every classification reports the rule that produced it. The vocabulary is
 * hand-written 1993 assembly comments, not a schema, so a caller showing these
 * groups should show the reason too and let a human overrule it -- the point is
 * to sort a long list into a useful order, not to be an authority on what MK2
 * considers a reaction.
 *
 * Pure string logic: no IMG, no ImGui, no globals.
 *************************************************************/
#ifndef REACTION_CLASS_H
#define REACTION_CLASS_H

#include <string>

/* Reaction families, ordered the way a fight reads: get hit, get staggered,
   get knocked down, get thrown, get up, get killed. kNotReaction covers
   everything the fighter does of its own accord, including the attacking half
   of a fatality. */
enum class ReactionKind {
    NotReaction = 0,
    Hit,        /* hit high / hit low / hit while ducking */
    Stagger,    /* stumble back, stunned, ouch, bicycle kicked, noogied */
    Knockdown,  /* knocked down, sweep fall, swept, body slam */
    FlippedBy,  /* a_*_fb_* — thrown, with the thrower baked into the label */
    Getup,      /* normal getup, getup from a sweep */
    Death,      /* fatality victim bodies and the props torn off them */
};

/* How many kinds there are, for callers that group by kind. */
constexpr int kReactionKindCount = 7;

struct ReactionInfo {
    ReactionKind kind = ReactionKind::NotReaction;
    /* Why this landed where it did, phrased for a tooltip: "label 'kdown'",
       "comment 'hit high'", "attacker half of a fatality pair". Owned, because
       the caller keeps these rows around long after the classify call. */
    std::string  rule;
    /* FlippedBy only: the two-letter thrower code from the label
       (a_jc_fb_lk -> "lk"). Empty for every other kind. */
    std::string  thrower;
};

/* Classify one anitab entry. `label` is the ASM symbol (a_jchithi); `comment`
   is the trailing directory comment ("hit high"), which may be empty. Case and
   surrounding whitespace do not matter. */
ReactionInfo ReactionClassify(const char *label, const char *comment);

/* Display name for a kind ("Hit", "Knocked down", ...). */
const char *ReactionKindName(ReactionKind kind);

/* One-line description of what the family means, for tooltips. */
const char *ReactionKindHelp(ReactionKind kind);

/* Full fighter name for a two-letter MK2 character code ("lk" -> "Liu Kang"),
   or NULL when the code is not one of the known fighters. Used to spell out
   ReactionInfo::thrower. */
const char *ReactionFighterName(const char *code);

#endif /* REACTION_CLASS_H */
