/*************************************************************
 * platform/palette_effects.cpp
 * See palette_effects.h.
 *************************************************************/
#include "palette_effects.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

const CharacterEffectKind kCharacterEffects[CHARACTER_EFFECT_COUNT] = {
    { "FREEZE",   "FREZ",  "Freeze" },
    { "FIVEFRZ",  "NUFRZ", "Five-color freeze" },
    { "ELEC",     "ELPUP", "Electric" },
    { "BGRDEL_P", "ELRED", "Red electric" },
    { "REDELI",   "REDEL", "Red electric (light)" },
    { "PURPELI",  "PURPL", "Purple electric" },
};

static void word_rgb8(unsigned short w, int *r, int *g, int *b)
{
    /* Same expansion as the Indexed Color Gradient's luma pass. */
    *r = ((w >> 10) & 31) * 255 / 31;
    *g = ((w >> 5) & 31) * 255 / 31;
    *b = (w & 31) * 255 / 31;
}

int FitPaletteToGradient(const unsigned short *src, int n, const unsigned char *mask,
                         const float *stops_rgb, int nstops, unsigned short *out)
{
    if (!src || !out || !stops_rgb || n < 1 || n > 256 || nstops < 2) return -1;
    unsigned short base[256];
    memcpy(base, src, (size_t)n * sizeof(unsigned short));
    memcpy(out, base, (size_t)n * sizeof(unsigned short));

    int luma[256] = {};
    int min_luma = 0x7fffffff, max_luma = -1, fitted = 0;
    for (int i = 1; i < n; i++) {
        if (mask && !mask[i]) continue;
        int r, g, b;
        word_rgb8(base[i], &r, &g, &b);
        luma[i] = 30 * r + 59 * g + 11 * b;
        min_luma = std::min(min_luma, luma[i]);
        max_luma = std::max(max_luma, luma[i]);
        fitted++;
    }
    if (!fitted) return 0;

    int changed = 0;
    for (int i = 1; i < n; i++) {
        if (mask && !mask[i]) continue;
        float t = max_luma > min_luma
                ? (float)(luma[i] - min_luma) / (float)(max_luma - min_luma)
                : 0.5f;
        float scaled = t * (float)(nstops - 1);
        int seg = (int)floorf(scaled);
        if (seg >= nstops - 1) seg = nstops - 2;
        float f = scaled - (float)seg;
        int rgb[3];
        for (int c = 0; c < 3; c++) {
            float a = stops_rgb[seg * 3 + c], z = stops_rgb[(seg + 1) * 3 + c];
            rgb[c] = (int)lroundf(255.0f * (a + (z - a) * f));
            rgb[c] = std::max(0, std::min(255, rgb[c]));
        }
        unsigned short word = (unsigned short)((base[i] & 0x8000) |
                                               ((rgb[0] >> 3) << 10) |
                                               ((rgb[1] >> 3) << 5) |
                                                (rgb[2] >> 3));
        if (word != base[i]) changed++;
        out[i] = word;
    }
    return changed;
}

void FillBloodRamp(unsigned short *words, int first, int count)
{
    if (!words || count <= 0) return;
    int den = std::max(1, count - 1);
    for (int n = 0; n < count; n++) {
        int r = 4 + (27 * n + den / 2) / den;
        int g = (8 * n * n + den * den / 2) / (den * den);
        int b = (5 * n * n + den * den / 2) / (den * den);
        words[first + n] = (unsigned short)((r << 10) | (g << 5) | b);
    }
}

void SampleGradientWords(const float *stops_rgb, int nstops, int count, unsigned short *out)
{
    if (!stops_rgb || nstops < 2 || !out || count <= 0) return;
    for (int i = 0; i < count; i++) {
        float t = count > 1 ? (float)i / (float)(count - 1) : 0.0f;
        float scaled = t * (float)(nstops - 1);
        int seg = (int)floorf(scaled);
        if (seg >= nstops - 1) seg = nstops - 2;
        float f = scaled - (float)seg;
        int rgb[3];
        for (int c = 0; c < 3; c++) {
            float a = stops_rgb[seg * 3 + c], z = stops_rgb[(seg + 1) * 3 + c];
            rgb[c] = std::max(0, std::min(255, (int)lroundf(255.0f * (a + (z - a) * f))));
        }
        out[i] = (unsigned short)(((rgb[0] >> 3) << 10) | ((rgb[1] >> 3) << 5) | (rgb[2] >> 3));
    }
}

/* Squared RGB555 distance between two slots, summed over every costume. */
static long long slot_distance(const unsigned short *const *costumes, int count, int a, int b)
{
    long long d = 0;
    for (int k = 0; k < count; k++) {
        unsigned short x = costumes[k][a], y = costumes[k][b];
        for (int shift = 0; shift <= 10; shift += 5) {
            int e = ((x >> shift) & 31) - ((y >> shift) & 31);
            d += e * e;
        }
    }
    return d;
}

bool PlanBloodCondense(const unsigned short *const *costumes, int count, int n,
                       const unsigned long long *usage, int blood,
                       BloodCondensePlan &out)
{
    out = BloodCondensePlan();
    if (!costumes || count < 1 || n < 3 || n > 256 || blood < 1 || blood > n - 2) return false;
    for (int k = 0; k < count; k++) if (!costumes[k]) return false;
    out.numc = n;
    out.blood = blood;
    const int room = n - blood - 1;

    bool any_drawn = false;
    if (usage) for (int i = 1; i < n; i++) any_drawn |= usage[i] > 0;
    const bool weighted = usage && any_drawn;

    struct Cluster { int rep; unsigned long long weight; std::vector<int> members; };
    std::vector<Cluster> clusters;
    for (int i = 1; i < n; i++) {
        if (weighted && !usage[i]) { out.dropped++; continue; }
        clusters.push_back({ i, weighted ? usage[i] : 1ull, { i } });
    }
    const int drawn = (int)clusters.size();

    /* Merge the cheapest pair until the costume fits: distance weighted by
       the smaller side's share of pixels (Ward's criterion), so a slot
       covering a whole torso is not traded away for a few stray pixels. */
    while ((int)clusters.size() > room) {
        int best_a = 0, best_b = 1;
        double best = -1.0;
        for (int a = 0; a < (int)clusters.size(); a++)
            for (int b = a + 1; b < (int)clusters.size(); b++) {
                double wa = (double)clusters[a].weight, wb = (double)clusters[b].weight;
                double cost = wa * wb / (wa + wb) *
                              (double)slot_distance(costumes, count, clusters[a].rep, clusters[b].rep);
                if (best < 0 || cost < best) { best = cost; best_a = a; best_b = b; }
            }
        Cluster &a = clusters[best_a];
        Cluster &b = clusters[best_b];
        if (b.weight > a.weight || (b.weight == a.weight && b.rep < a.rep)) a.rep = b.rep;
        a.weight += b.weight;
        a.members.insert(a.members.end(), b.members.begin(), b.members.end());
        clusters.erase(clusters.begin() + best_b);
    }
    out.merged = drawn - (int)clusters.size();

    /* Keep the original slot order, so ramps stay ramps. */
    for (Cluster &c : clusters) std::sort(c.members.begin(), c.members.end());
    std::sort(clusters.begin(), clusters.end(),
              [](const Cluster &x, const Cluster &y) { return x.members[0] < y.members[0]; });

    bool mapped[256] = {};
    mapped[0] = true;
    for (int j = 0; j < (int)clusters.size(); j++) {
        const Cluster &c = clusters[j];
        out.reps.push_back(c.rep);
        for (int m : c.members) {
            out.map[m] = (unsigned char)(j + 1);
            mapped[m] = true;
            if (m != c.rep && weighted && slot_distance(costumes, count, m, c.rep))
                out.changed_pixels += usage[m];
        }
    }
    /* Undrawn slots still get somewhere sensible to go. */
    for (int i = 1; i < n; i++) {
        if (mapped[i]) continue;
        long long best = -1;
        for (int j = 0; j < (int)out.reps.size(); j++) {
            long long d = slot_distance(costumes, count, i, out.reps[j]);
            if (best < 0 || d < best) { best = d; out.map[i] = (unsigned char)(j + 1); }
        }
    }
    return true;
}

void BuildBloodPalette(const BloodCondensePlan &plan, const unsigned short *costume,
                       const unsigned short *blood_words, unsigned short *out)
{
    if (!costume || !blood_words || !out || plan.numc < 1) return;
    for (int i = 0; i < plan.numc; i++) out[i] = 0;
    out[0] = costume[0];
    for (int j = 0; j < (int)plan.reps.size(); j++) out[j + 1] = costume[plan.reps[j]];
    for (int i = 0; i < plan.blood; i++) out[plan.numc - plan.blood + i] = blood_words[i];
}

std::string CharacterPrefixOf(const char *pal_name)
{
    if (!pal_name || !pal_name[0] || !pal_name[1]) return std::string();
    std::string p(pal_name, 2);
    for (char &c : p) c = (char)toupper((unsigned char)c);
    return p;
}

std::string EffectPaletteName(const std::string &prefix, const char *suffix)
{
    return prefix + (suffix ? suffix : "") + "_P";
}

std::string BloodPaletteName(const std::string &prefix, const char *source)
{
    std::string digits;
    if (source) {
        /* The last digit run before any "_P": JCALT12_P -> "12". */
        int len = (int)strnlen(source, 16);
        int end = len;
        while (end > 0 && !isdigit((unsigned char)source[end - 1])) end--;
        int start = end;
        while (start > 0 && isdigit((unsigned char)source[start - 1])) start--;
        /* Digits inside the prefix ("KT1...") are not a costume number. */
        if (start >= (int)prefix.size()) digits.assign(source + start, (size_t)(end - start));
    }
    return prefix + "BLUD" + digits + "_P";
}

static bool ieq_prefix(const char *s, const std::string &p)
{
    for (size_t i = 0; i < p.size(); i++)
        if (!s[i] || toupper((unsigned char)s[i]) != toupper((unsigned char)p[i])) return false;
    return true;
}

bool IsGeneratedPaletteName(const char *name, const std::string &prefix)
{
    if (!name || !ieq_prefix(name, prefix)) return false;
    std::string rest;
    for (const char *s = name + prefix.size(); *s; s++) rest += (char)toupper((unsigned char)*s);
    for (const CharacterEffectKind &e : kCharacterEffects)
        if (rest == std::string(e.suffix) + "_P") return true;
    return rest.compare(0, 4, "BLUD") == 0 || rest.compare(0, 3, "BLD") == 0;
}
