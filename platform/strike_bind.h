/*************************************************************
 * platform/strike_bind.h
 * Which strike box belongs to which animation — the missing half of the
 * MK2 hitbox editor.
 *
 * MKSTK.ASM holds the collision boxes ("stk_jchikick": x, y, w, h, routine,
 * damage, score, sound) and a per-character table listing them in slot order.
 * Nothing in that file says which *sprites* a box is armed for: the move's
 * own code passes a slot number to get_char_stk at runtime, and the slot
 * ordering is not the anitab ordering. So a frame on the canvas has no
 * recorded connection to the box that will be tested against it.
 *
 * The connection is in the names. A strike is spelled after the animation it
 * belongs to — stk_jchikick / a_jchikick — and the character ASM imgtool
 * already parses resolves each animation to the sprites it draws. Chain those
 * and a frame reaches its box.
 *
 * The spelling drifts, though, and only in a handful of ways: an abbreviated
 * limb (stk_jcflipk / a_jcflipkick), a dropped or added sequence number
 * (stk_jczap1 / a_jczap), a missing underscore. Cage's 24 slots match MKJC.ASM
 * exactly 14 times; normalising those three habits recovers most of the rest.
 * What is left is genuinely unguessable (stk_jc_split) and belongs to a human,
 * which is what the sidecar is for.
 *
 * Pure string logic: no IMG, no ImGui, no globals, no file I/O beyond the
 * sidecar text the caller hands it.
 *************************************************************/
#ifndef STRIKE_BIND_H
#define STRIKE_BIND_H

#include <string>
#include <vector>

namespace strike_bind {

/* Drop a known prefix ("stk_", "a_") and lowercase. Returns the stem. */
std::string Stem(const std::string &label);

/* The stem with its decoration removed, for comparison only: lowercased,
   underscores dropped, trailing digits dropped (stk_lkzap1..4 are all
   a_lkzap). Never shown to the user — it is deliberately lossy. */
std::string NormalizeStem(const std::string &stem);

/* Every spelling of `stem` worth comparing against, the normalised form
   first. Internally these are ranked — expanding an abbreviated limb beats
   cutting one off, because stk_XXjumpupk can reach both a_XXjumpupkick and
   a_XXjumpup and only the first is what it meant. The limb is the reason this is a set rather than one more
   transformation: MKSTK abbreviates by appending the limb's initial to the
   animation name (a_jcsweep -> stk_jcsweepk) *and* by cutting the limb short
   (a_jcflipkick -> stk_jcflipk), so both directions have to be offered. Folded
   into the normal form instead, the rule would rewrite names that simply end
   in those letters — "jczap" would normalise to "jczapunch". */
std::vector<std::string> Variants(const std::string &stem);

/* Index into `anim_labels` of the animation belonging to `strike_label`, or
   -1. Tries exact stem equality first, then normalised equality, then a
   normalised prefix match, and refuses to answer when two candidates tie at
   the same strength — a wrong box silently attached to a move is worse than
   no box, and the caller can fall back to asking. */
int MatchAnimForStrike(const std::string &strike_label,
                       const std::vector<std::string> &anim_labels);

/* The same relation from the other side. */
int MatchStrikeForAnim(const std::string &anim_label,
                       const std::vector<std::string> &strike_labels);

/* ---- Sidecar ("MKSTK.imgtool", beside MKSTK.ASM) ----------------------
   One binding per line, `anim_label = strike_label`, for the pairs no rule
   can reach. Blank lines and `#` comments are kept on rewrite so a
   hand-annotated file survives a save from the tool. */
struct Binding {
    std::string anim;     /* a_jc_split */
    std::string strike;   /* stk_jc_split */
};

/* Parse sidecar text. Malformed lines are skipped rather than failing the
   whole file — a typo in one binding must not lose the others. */
std::vector<Binding> ParseSidecar(const std::string &text);

/* Render bindings back out, preserving `keep_comments` (the comment and blank
   lines of the file this replaces) at the top. Stable order: bindings are
   sorted by anim label so the file does not churn between saves. */
std::string WriteSidecar(const std::vector<Binding> &bindings,
                         const std::vector<std::string> &keep_comments);

/* Comment/blank lines of `text`, for handing back to WriteSidecar. */
std::vector<std::string> SidecarComments(const std::string &text);

/* The sidecar path for an MKSTK.ASM path: same directory and stem, extension
   replaced with ".imgtool". Empty when `asm_path` is empty. */
std::string SidecarPathFor(const std::string &asm_path);

} // namespace strike_bind

#endif /* STRIKE_BIND_H */
