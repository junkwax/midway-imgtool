/*************************************************************
 * platform/strike_bind.cpp
 * See strike_bind.h.
 *************************************************************/
#include "strike_bind.h"

#include <algorithm>
#include <cctype>

namespace strike_bind {

static std::string Lower(const std::string &s)
{
    std::string out = s;
    for (size_t i = 0; i < out.size(); i++)
        out[i] = (char)tolower((unsigned char)out[i]);
    return out;
}

static std::string Trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

std::string Stem(const std::string &label)
{
    std::string s = Lower(Trim(label));
    if (s.compare(0, 4, "stk_") == 0) return s.substr(4);
    if (s.compare(0, 2, "a_") == 0)   return s.substr(2);
    return s;
}

std::string NormalizeStem(const std::string &stem)
{
    std::string s = Lower(stem);

    /* Underscores are decoration: jc_split and jcsplit are one move. */
    std::string flat;
    flat.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] != '_') flat += s[i];

    /* A trailing sequence number distinguishes variants of one move
       (stk_lkzap1..4 against a single a_lkzap), so it cannot take part in
       matching. */
    while (!flat.empty() && isdigit((unsigned char)flat[flat.size() - 1]))
        flat.erase(flat.size() - 1);

    return flat;
}

/* Variants carry a rank, because two of them can reach different animations
   from the same strike and one of those readings is better.

   Every character has this collision: stk_XXjumpupk truncates to a_XXjumpup
   (jump straight up) and expands to a_XXjumpupkick (the kick thrown from it),
   and both exist in the anitab. The strike name carries the limb letter, so
   the expansion is what it meant; ranked equally, the two tie and the match is
   refused for all six fighters at once. */
enum { kRankBase = 4, kRankExpand = 3, kRankShorten = 2 };

struct RankedVariant {
    std::string form;
    int rank;
};

static std::vector<RankedVariant> RankedVariants(const std::string &stem)
{
    std::vector<RankedVariant> out;
    const std::string base = NormalizeStem(stem);
    if (base.empty()) return out;
    out.push_back(RankedVariant{base, kRankBase});

    /* The limb, both ways round. Guarded on length so a two-letter character
       code plus a letter cannot become a move name of its own. */
    if (base.size() >= 5) {
        char last = base[base.size() - 1];
        std::string head = base.substr(0, base.size() - 1);
        if (last == 'k') {
            out.push_back(RankedVariant{base + "ick", kRankExpand});  /* flipk  -> flipkick */
            out.push_back(RankedVariant{head, kRankShorten});         /* sweepk -> sweep    */
        } else if (last == 'p') {
            out.push_back(RankedVariant{base + "unch", kRankExpand}); /* flipp   -> flippunch */
            out.push_back(RankedVariant{head, kRankShorten});         /* jumpupp -> jumpup    */
        }
    }

    /* The two-letter character code is not always carried: MKSTK writes
       stk_jcshoruken for MKJC.ASM's a_shoruken. */
    if (base.size() > 4)
        out.push_back(RankedVariant{base.substr(2), kRankShorten});

    return out;
}

std::vector<std::string> Variants(const std::string &stem)
{
    std::vector<RankedVariant> ranked = RankedVariants(stem);
    std::vector<std::string> out;
    out.reserve(ranked.size());
    for (size_t i = 0; i < ranked.size(); i++) out.push_back(ranked[i].form);
    return out;
}

/* Strength of the best variant rule connecting the two stems, 0 for none. */
static int VariantScore(const std::string &want_base,
                        const std::vector<RankedVariant> &want_vars,
                        const std::string &cand_base,
                        const std::vector<RankedVariant> &cand_vars)
{
    int score = 0;
    for (size_t i = 0; i < want_vars.size(); i++)
        if (want_vars[i].form == cand_base && want_vars[i].rank > score)
            score = want_vars[i].rank;
    for (size_t i = 0; i < cand_vars.size(); i++)
        if (cand_vars[i].form == want_base && cand_vars[i].rank > score)
            score = cand_vars[i].rank;
    return score;
}

/* Index of the single best match, or -1 when nothing matched or two
   candidates tied. Higher score = stronger rule. */
static int BestUnique(const std::vector<std::string> &candidates,
                      const std::string &want_stem)
{
    const std::string want_norm = NormalizeStem(want_stem);
    const std::vector<RankedVariant> want_vars = RankedVariants(want_stem);

    int best = -1;
    int best_score = 0;
    int best_count = 0;

    for (size_t i = 0; i < candidates.size(); i++) {
        const std::string cand_stem = Stem(candidates[i]);
        const std::string cand_norm = NormalizeStem(cand_stem);

        int score = 0;
        if (cand_stem == want_stem)      score = 6;
        else if (cand_norm == want_norm) score = 5;
        else score = VariantScore(want_norm, want_vars, cand_norm,
                                  RankedVariants(cand_stem));
        if (score == 0) continue;

        if (score > best_score) {
            best_score = score;
            best = (int)i;
            best_count = 1;
        } else if (score == best_score) {
            best_count++;
        }
    }

    /* A tie at the winning strength is not an answer. Attaching the wrong box
       to a move is worse than attaching none, because nothing downstream will
       question it. */
    if (best_count != 1) return -1;
    return best;
}

int MatchAnimForStrike(const std::string &strike_label,
                       const std::vector<std::string> &anim_labels)
{
    return BestUnique(anim_labels, Stem(strike_label));
}

int MatchStrikeForAnim(const std::string &anim_label,
                       const std::vector<std::string> &strike_labels)
{
    return BestUnique(strike_labels, Stem(anim_label));
}

/* ---- Sidecar ---------------------------------------------------------- */

static std::vector<std::string> SplitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

std::vector<Binding> ParseSidecar(const std::string &text)
{
    std::vector<Binding> out;
    std::vector<std::string> lines = SplitLines(text);
    for (size_t i = 0; i < lines.size(); i++) {
        std::string line = Trim(lines[i]);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        Binding b;
        b.anim = Trim(line.substr(0, eq));
        b.strike = Trim(line.substr(eq + 1));
        if (b.anim.empty() || b.strike.empty()) continue;
        /* Last writer wins, so a hand-edit appended at the bottom overrides
           whatever the tool wrote above it. */
        bool replaced = false;
        for (size_t j = 0; j < out.size(); j++) {
            if (Lower(out[j].anim) == Lower(b.anim)) {
                out[j].strike = b.strike;
                replaced = true;
                break;
            }
        }
        if (!replaced) out.push_back(b);
    }
    return out;
}

std::vector<std::string> SidecarComments(const std::string &text)
{
    std::vector<std::string> out;
    std::vector<std::string> lines = SplitLines(text);
    for (size_t i = 0; i < lines.size(); i++) {
        std::string line = Trim(lines[i]);
        if (line.empty()) continue;
        if (line[0] == '#' || line[0] == ';') out.push_back(lines[i]);
    }
    return out;
}

std::string WriteSidecar(const std::vector<Binding> &bindings,
                         const std::vector<std::string> &keep_comments)
{
    std::string out;
    if (keep_comments.empty()) {
        out += "# imgtool strike bindings for MKSTK.ASM\n";
        out += "# One per line: <animation label> = <strike label>.\n";
        out += "# Only the pairs imgtool cannot work out from the names; it\n";
        out += "# matches stk_jchikick to a_jchikick on its own.\n";
        out += "# Not assembled -- this file is imgtool's, beside the source.\n";
    } else {
        for (size_t i = 0; i < keep_comments.size(); i++) {
            out += keep_comments[i];
            out += "\n";
        }
    }
    out += "\n";

    std::vector<Binding> sorted = bindings;
    std::sort(sorted.begin(), sorted.end(),
              [](const Binding &a, const Binding &b) {
                  return Lower(a.anim) < Lower(b.anim);
              });
    for (size_t i = 0; i < sorted.size(); i++) {
        out += sorted[i].anim;
        out += " = ";
        out += sorted[i].strike;
        out += "\n";
    }
    return out;
}

std::string SidecarPathFor(const std::string &asm_path)
{
    if (asm_path.empty()) return std::string();
    size_t slash = asm_path.find_last_of("\\/");
    size_t dot = asm_path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return asm_path + ".imgtool";
    return asm_path.substr(0, dot) + ".imgtool";
}

} // namespace strike_bind
