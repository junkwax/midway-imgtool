/*************************************************************
 * platform/stamp_erase.cpp
 * Content-driven stamp search and subtract. See stamp_erase.h.
 *************************************************************/
#include "stamp_erase.h"

#include <algorithm>
#include <vector>

/* How many stamp pixels vote on where the object is. Real compositing is
   brutal - in UMK3FIRE the flame recolors roughly three quarters of the
   skeleton, so any scheme that needs a specific pixel, or a specific run of
   pixels, to survive will miss it. Voting does not care which quarter
   survives, only that some of it does. */
static const int kAnchorCount = 96;

/* The stamp is divided into this many cells per axis and anchors are taken
   round-robin from them, so the voters cover the whole object rather than
   clustering in whichever highlight holds the rarest colors. A cluster all
   disappears under the same blob of flame. */
static const int kAnchorGrid = 8;

/* At most this many voters may share one palette index. Without the cap, a
   large flat region hands one color a bloc vote, and any placement that slides
   that region onto itself wins - which is how a search lands ten pixels off
   with total confidence. */
static const int kAnchorsPerIndex = 4;

/* Ceiling on votes cast. A stamp whose rarest color is still everywhere in the
   target would otherwise cast millions. */
static const int kMaxVotes = 120000;

/* Placements taken off the ballot and scored properly. Generous, because a
   vote is only a hint: the peak is regularly a pixel or two off the real
   placement, and the runners-up are where the true offset hides when a flat
   region skews the count. */
static const int kScoredPlacements = 192;

/* After scoring, the winner is walked to its local optimum: +/- this many
   pixels, repeatedly, until it stops improving. Cheap, and it turns a
   near-miss from the ballot into the exact placement. One pixel out erases
   nothing at all. */
static const int kRefineRadius = 3;
static const int kRefineRounds = 6;

/* Default cap on fully-scored placements, as a backstop for callers that pass
   nothing. */
static const int kDefaultMaxCandidates = 20000;

static inline unsigned char StampPixel(const StampBuf &b, int x, int y)
{
    return b.pixels[(size_t)y * (size_t)b.stride + (size_t)x];
}

static inline bool StampBufUsable(const StampBuf &b)
{
    return b.pixels != NULL && b.w > 0 && b.h > 0 && b.stride >= b.w;
}

float StampMatchScore(const StampMatch &m)
{
    if (!m.valid || m.total <= 0) return 0.0f;
    return (float)m.matched / (float)m.total;
}

int StampOpaqueCount(const StampBuf &stamp)
{
    if (!StampBufUsable(stamp)) return 0;
    int n = 0;
    for (int y = 0; y < stamp.h; y++)
        for (int x = 0; x < stamp.w; x++)
            if (StampPixel(stamp, x, y) != 0) n++;
    return n;
}

/* Core scorer. `total` is passed in because the callers already know it and it
   is the same for every placement. `give_up_below` lets a search abandon a
   placement as soon as it cannot beat the incumbent; pass 0 to score fully. */
static StampMatch ScoreInternal(const StampBuf &stamp, const StampBuf &target,
                                int dx, int dy, int total,
                                const std::vector<int> &opaque_at_or_below,
                                int give_up_below)
{
    StampMatch m;
    m.valid = true;
    m.dx = dx;
    m.dy = dy;
    m.total = total;

    for (int y = 0; y < stamp.h; y++) {
        /* Everything still unscored, including this row. Even if all of it
           matched we would fall short, so stop. */
        if (give_up_below > 0 &&
            m.matched + opaque_at_or_below[(size_t)y] < give_up_below) {
            m.matched = -1;   /* flagged as beaten; caller discards it */
            return m;
        }
        int ty = dy + y;
        if (ty < 0 || ty >= target.h) continue;
        for (int x = 0; x < stamp.w; x++) {
            unsigned char sv = StampPixel(stamp, x, y);
            if (sv == 0) continue;
            int tx = dx + x;
            if (tx < 0 || tx >= target.w) continue;
            m.covered++;
            if (StampPixel(target, tx, ty) == sv) m.matched++;
        }
    }
    return m;
}

/* Opaque pixels in rows y..h-1, indexed by y. Lets the scorer bail out of a
   hopeless placement without finishing it. */
static std::vector<int> BuildOpaqueSuffix(const StampBuf &stamp)
{
    std::vector<int> suffix((size_t)stamp.h + 1, 0);
    for (int y = stamp.h - 1; y >= 0; y--) {
        int row = 0;
        for (int x = 0; x < stamp.w; x++)
            if (StampPixel(stamp, x, y) != 0) row++;
        suffix[(size_t)y] = suffix[(size_t)y + 1] + row;
    }
    return suffix;
}

StampMatch StampScoreAt(const StampBuf &stamp, const StampBuf &target,
                        int dx, int dy)
{
    StampMatch m;
    if (!StampBufUsable(stamp) || !StampBufUsable(target)) return m;
    int total = StampOpaqueCount(stamp);
    if (total <= 0) return m;
    std::vector<int> suffix;   /* unused when not bailing out early */
    return ScoreInternal(stamp, target, dx, dy, total, suffix, 0);
}

/* Keep whichever placement is better: more matched pixels wins. Ties go to the
   incumbent, so a scan over equally good placements settles on the first one
   tried instead of drifting to the last. */
static void KeepBetter(StampMatch *best, const StampMatch &cand)
{
    if (cand.matched < 0) return;              /* abandoned early */
    if (!best->valid || cand.matched > best->matched) {
        int candidates = best->candidates;
        *best = cand;
        best->candidates = candidates;
    }
}

StampMatch StampSearchWindow(const StampBuf &stamp, const StampBuf &target,
                             int dx0, int dy0, int radius)
{
    StampMatch best;
    if (!StampBufUsable(stamp) || !StampBufUsable(target)) return best;
    int total = StampOpaqueCount(stamp);
    if (total <= 0) return best;

    int x_lo, x_hi, y_lo, y_hi;
    if (radius < 0) {
        x_lo = -(stamp.w - 1); x_hi = target.w - 1;
        y_lo = -(stamp.h - 1); y_hi = target.h - 1;
    } else {
        x_lo = dx0 - radius; x_hi = dx0 + radius;
        y_lo = dy0 - radius; y_hi = dy0 + radius;
    }

    std::vector<int> suffix = BuildOpaqueSuffix(stamp);
    for (int dy = y_lo; dy <= y_hi; dy++) {
        for (int dx = x_lo; dx <= x_hi; dx++) {
            StampMatch cand = ScoreInternal(stamp, target, dx, dy, total,
                                            suffix, best.matched);
            best.candidates++;
            KeepBetter(&best, cand);
            if (best.valid && best.matched == total) return best;
        }
    }
    return best;
}

/* One voting pixel: a stamp position and how many times its index occurs in
   the target. Rarity is both the cost of listening to it — a color the target
   uses four thousand times casts four thousand votes for one opinion — and the
   worth of its opinion, since a rare color landing in the right place is far
   less likely to be a coincidence. */
struct StampAnchor {
    int x, y;
    unsigned char index;
    int rarity;
};

static bool AnchorRarer(const StampAnchor &a, const StampAnchor &b)
{
    if (a.rarity != b.rarity) return a.rarity < b.rarity;
    if (a.y != b.y) return a.y < b.y;
    return a.x < b.x;
}

/* Choose the pixels that get to vote: rarest colors first, taken round-robin
   from a grid over the stamp so the voters cover the whole object, and never
   more than a few per color so no single flat region can outvote everything
   else. Anchors stop being accepted once their combined rarity would blow the
   vote budget, which is what keeps a flat-colored stamp from casting millions
   of votes for nothing. */
static void PickAnchors(const StampBuf &stamp, const int target_count[256],
                        std::vector<StampAnchor> *out)
{
    const int cells = kAnchorGrid * kAnchorGrid;
    std::vector< std::vector<StampAnchor> > buckets((size_t)cells);

    for (int y = 0; y < stamp.h; y++) {
        for (int x = 0; x < stamp.w; x++) {
            unsigned char v = StampPixel(stamp, x, y);
            if (v == 0) continue;
            int rarity = target_count[v];
            if (rarity == 0) continue;      /* target has no such pixel */
            StampAnchor a;
            a.x = x; a.y = y; a.index = v; a.rarity = rarity;
            int cx = x * kAnchorGrid / stamp.w;
            int cy = y * kAnchorGrid / stamp.h;
            if (cx >= kAnchorGrid) cx = kAnchorGrid - 1;
            if (cy >= kAnchorGrid) cy = kAnchorGrid - 1;
            buckets[(size_t)(cy * kAnchorGrid + cx)].push_back(a);
        }
    }
    for (size_t i = 0; i < buckets.size(); i++)
        std::sort(buckets[i].begin(), buckets[i].end(), AnchorRarer);

    int deepest = 0;
    for (size_t i = 0; i < buckets.size(); i++)
        if ((int)buckets[i].size() > deepest) deepest = (int)buckets[i].size();

    int per_index[256] = {0};
    long long budget = 0;
    for (int round = 0; round < deepest && (int)out->size() < kAnchorCount; round++) {
        for (size_t i = 0; i < buckets.size() && (int)out->size() < kAnchorCount; i++) {
            if ((int)buckets[i].size() <= round) continue;
            const StampAnchor &a = buckets[i][(size_t)round];
            if (per_index[a.index] >= kAnchorsPerIndex) continue;
            if (budget + a.rarity > kMaxVotes) {
                /* Always seat one voter, however expensive: a stamp made of a
                   single common color still deserves an answer. */
                if (!out->empty()) continue;
            }
            per_index[a.index]++;
            budget += a.rarity;
            out->push_back(a);
        }
    }
}

/* Offsets are packed into one integer so the ballot can be sorted and counted
   without a hash map. The bias covers any placement two sprite-sized buffers
   can imply. */
static const long long kOffsetBias = 1 << 16;
static const long long kOffsetSpan = 1 << 18;

struct StampVote {
    long long offset;
    int weight;
};

static bool VoteByOffset(const StampVote &a, const StampVote &b)
{
    return a.offset < b.offset;
}

/* Walk the placement to its local best. The ballot points at the right
   neighbourhood but rarely at the exact pixel, and one pixel out erases
   nothing at all. */
static void RefinePlacement(const StampBuf &stamp, const StampBuf &target,
                            int total, const std::vector<int> &suffix,
                            StampMatch *best)
{
    for (int round = 0; round < kRefineRounds; round++) {
        int base_x = best->dx, base_y = best->dy;
        bool improved = false;
        for (int dy = base_y - kRefineRadius; dy <= base_y + kRefineRadius; dy++) {
            for (int dx = base_x - kRefineRadius; dx <= base_x + kRefineRadius; dx++) {
                if (dx == base_x && dy == base_y) continue;
                StampMatch cand = ScoreInternal(stamp, target, dx, dy, total,
                                                suffix, best->matched);
                if (cand.matched <= best->matched) continue;
                int candidates = best->candidates + 1;
                *best = cand;
                best->candidates = candidates;
                improved = true;
            }
        }
        if (!improved) return;
    }
}

StampMatch StampSearchAuto(const StampBuf &stamp, const StampBuf &target,
                           int max_candidates)
{
    StampMatch best;
    if (!StampBufUsable(stamp) || !StampBufUsable(target)) return best;
    int total = StampOpaqueCount(stamp);
    if (total <= 0) return best;
    if (max_candidates <= 0) max_candidates = kDefaultMaxCandidates;

    int target_count[256] = {0};
    for (int y = 0; y < target.h; y++)
        for (int x = 0; x < target.w; x++)
            target_count[StampPixel(target, x, y)]++;

    std::vector<StampAnchor> anchors;
    PickAnchors(stamp, target_count, &anchors);
    if (anchors.empty())
        return best;    /* nothing the stamp is made of appears here at all */

    /* Every anchor votes for the placement each matching target pixel would
       imply. A pixel that survived the compositing votes for where the object
       really is; one that was painted over votes for noise, and noise does not
       agree with itself. Votes are weighted by how unusual the color is, so a
       coincidence in a common mid-tone cannot outweigh a rare highlight
       landing exactly where the object would put it. */
    bool wanted[256] = {false};
    for (size_t i = 0; i < anchors.size(); i++) wanted[anchors[i].index] = true;

    std::vector< std::vector<int> > positions((size_t)256);
    for (int ty = 0; ty < target.h; ty++) {
        for (int tx = 0; tx < target.w; tx++) {
            unsigned char v = StampPixel(target, tx, ty);
            if (!wanted[v]) continue;
            positions[(size_t)v].push_back(ty * target.w + tx);
        }
    }

    std::vector<StampVote> ballot;
    for (size_t i = 0; i < anchors.size(); i++) {
        const StampAnchor &a = anchors[i];
        int weight = 4096 / (a.rarity > 0 ? a.rarity : 1);
        if (weight < 1) weight = 1;
        const std::vector<int> &pos = positions[(size_t)a.index];
        for (size_t k = 0; k < pos.size(); k++) {
            int tx = pos[k] % target.w;
            int ty = pos[k] / target.w;
            StampVote v;
            v.offset = (long long)(ty - a.y + kOffsetBias) * kOffsetSpan +
                       (long long)(tx - a.x + kOffsetBias);
            v.weight = weight;
            ballot.push_back(v);
        }
        if ((int)ballot.size() >= kMaxVotes) break;
    }
    if (ballot.empty()) return best;

    std::sort(ballot.begin(), ballot.end(), VoteByOffset);

    /* Tally the runs of identical offsets, keeping the strongest handful. The
       shortlist is small, so insertion into a sorted array beats a heap. */
    struct Tally { long long offset; int weight; };
    std::vector<Tally> top;
    for (size_t i = 0; i < ballot.size(); ) {
        size_t j = i;
        int weight = 0;
        while (j < ballot.size() && ballot[j].offset == ballot[i].offset)
            weight += ballot[j++].weight;
        Tally t;
        t.offset = ballot[i].offset;
        t.weight = weight;
        if ((int)top.size() < kScoredPlacements || t.weight > top.back().weight) {
            size_t at = top.size();
            while (at > 0 && top[at - 1].weight < t.weight) at--;
            top.insert(top.begin() + (long)at, t);
            if ((int)top.size() > kScoredPlacements) top.pop_back();
        }
        i = j;
    }

    std::vector<int> suffix = BuildOpaqueSuffix(stamp);
    for (size_t i = 0; i < top.size(); i++) {
        int dx = (int)(top[i].offset % kOffsetSpan) - (int)kOffsetBias;
        int dy = (int)(top[i].offset / kOffsetSpan) - (int)kOffsetBias;
        StampMatch cand = ScoreInternal(stamp, target, dx, dy, total,
                                        suffix, best.matched);
        best.candidates++;
        KeepBetter(&best, cand);
        if (best.valid && best.matched == total) return best;
        if (best.candidates >= max_candidates) return best;
    }

    if (best.valid && best.matched < total)
        RefinePlacement(stamp, target, total, suffix, &best);
    return best;
}

int StampEraseAt(unsigned char *target, int target_w, int target_h,
                 int target_stride, const StampBuf &stamp, int dx, int dy)
{
    if (!target || target_w <= 0 || target_h <= 0 || target_stride < target_w)
        return 0;
    if (!StampBufUsable(stamp)) return 0;

    int cleared = 0;
    for (int y = 0; y < stamp.h; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= target_h) continue;
        unsigned char *trow = target + (size_t)ty * (size_t)target_stride;
        for (int x = 0; x < stamp.w; x++) {
            unsigned char sv = StampPixel(stamp, x, y);
            if (sv == 0) continue;
            int tx = dx + x;
            if (tx < 0 || tx >= target_w) continue;
            if (trow[tx] != sv) continue;
            trow[tx] = 0;
            cleared++;
        }
    }
    return cleared;
}
