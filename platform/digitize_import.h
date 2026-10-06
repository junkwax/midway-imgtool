/*************************************************************
 * platform/digitize_import.h
 * "Import Digitized Frame(s)" dialog: turn video-extracted PNGs into an
 * IMG + shared palette using digitize_matte / material_mask / palette_ramp /
 * ramp_remap, the way Midway built a character from keyed video footage.
 *************************************************************/
#ifndef UI_DIGITIZE_IMPORT_H
#define UI_DIGITIZE_IMPORT_H

#include <string>
#include <vector>

/* Open the wizard with an already-picked batch of frame paths. Frame 0
   becomes the reference frame materials are seeded on by hand; every other
   frame is classified against those materials automatically once ramps are
   fit (see material_mask.h for why that, not a spatial copy, is how a later
   frame gets its mask). */
void OpenDigitizeImportDialog(const std::vector<std::string> &paths);

/* One frame already in memory, e.g. grabbed from a video. */
struct DigitizeSourceFrame {
    std::string name;                 /* sprite name, at most 15 characters */
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;  /* w*h*4, straight alpha */
};

struct DigitizeHandoff {
    std::vector<DigitizeSourceFrame> frames;
    std::string palette_name;         /* "" = first frame's name + "P" */
    /* The floor point every frame is anchored on, in source pixels; -1 means
       bottom center. With a locked-off camera this is one spot on the stage,
       so anchoring every frame on it registers the whole animation. */
    int anchor_x = -1, anchor_y = -1;
};

/* Same wizard, fed frames that are already decoded. */
void OpenDigitizeImportDialogFrames(DigitizeHandoff &&handoff);

/* Draw it. Safe to call every frame; does nothing while hidden. */
void DrawDigitizeImportDialog(void);

#endif /* UI_DIGITIZE_IMPORT_H */
