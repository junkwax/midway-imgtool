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

/* Draw it. Safe to call every frame; does nothing while hidden. */
void DrawDigitizeImportDialog(void);

#endif /* UI_DIGITIZE_IMPORT_H */
