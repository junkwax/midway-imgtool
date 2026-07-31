/*************************************************************
 * platform/body_split.h
 * Head / arms / torso / legs segmentation of a humanoid sprite.
 *
 * Pure analysis over a raw indexed-pixel buffer (index 0 = transparent): no
 * g_doc, ImGui, or undo coupling. The heuristic proposes five rects from the
 * sprite's silhouette; the UI shows them as editable boxes and only then cuts
 * them into child IMGs. Non-humanoid art will produce a poor guess by design —
 * the split dialog exists so the guess can be corrected before it commits.
 *************************************************************/
#pragma once
#include <stddef.h>

enum BodyPartKind {
    BodyPart_Head = 0,
    BodyPart_ArmL,      /* limb column left of the torso core */
    BodyPart_Torso,
    BodyPart_ArmR,      /* limb column right of the torso core */
    BodyPart_Legs,
    BodyPart_Count
};

struct BodyPartRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool enabled = false;
};

struct BodySplitPlan {
    BodyPartRect parts[BodyPart_Count];
    /* Opaque bounding box the plan was derived from. */
    int content_x = 0, content_y = 0, content_w = 0, content_h = 0;
    /* Horizontal cuts, in image coordinates. */
    int neck_y = 0;     /* first row of the torso band */
    int waist_y = 0;    /* first row of the legs band */
    /* True when no convincing neck/waist narrowing was found and the bands
       fell back to fixed proportions of the content height. */
    bool used_proportional_fallback = false;
    bool valid = false;
};

/* Display name ("Head", "Arm L", ...) and the IMG name suffix ("HD", "AL",
   ...) for a part. Both return "" for an out-of-range part. */
const char *BodyPartName(int part);
const char *BodyPartSuffix(int part);

/* Propose a five-part split of the sprite. `pixels` is row-major indexed data
   with `stride` bytes per row; index 0 is transparent. Returns false (and
   leaves out->valid false) when the sprite is empty or too small to divide. */
bool BuildBodySplitPlan(const unsigned char *pixels, int w, int h, int stride,
                        BodySplitPlan *out);

/* Shrink every enabled rect to its own opaque content, disabling any part that
   turned out to hold no pixels. Returns the number of parts still enabled. */
int TrimBodySplitPlan(const unsigned char *pixels, int w, int h, int stride,
                      BodySplitPlan *plan);

/* Number of opaque pixels inside a rect, clipped to the image. */
int BodyRectOpaquePixels(const unsigned char *pixels, int w, int h, int stride,
                         const BodyPartRect &rect);

/* Build a child IMG name for a part: the parent name truncated as needed so
   "<parent>_<suffix>" fits in 15 characters plus a terminator. */
void BodyPartChildName(const char *parent_name, int part,
                       char *out, size_t out_sz);
