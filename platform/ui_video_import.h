/*************************************************************
 * platform/ui_video_import.h
 * "Import Video" dialog: the frame grabber in front of the digitize wizard.
 *
 * Scrub the footage, mark the frames that make up a move (a range at every
 * Nth frame, or picked one at a time), crop to the actor, and click the floor
 * point. The grabbed frames go straight into "Import Digitized Frame(s)",
 * which keys, segments, fits the shared palette, and builds the sprites.
 *************************************************************/
#ifndef UI_VIDEO_IMPORT_H
#define UI_VIDEO_IMPORT_H

#include <string>

/* The ';'-separated extension list the file browser shows for video. */
extern const char *const kVideoImportExtensions;

/* True for an extension (no dot, any case) the importer accepts. */
bool VideoImportIsVideoExt(const std::string &ext);

/* Probe `path` and open the dialog, or toast why it can't. */
void OpenVideoImportDialog(const std::string &path);

/* Draw it. Safe to call every frame; does nothing while hidden. */
void DrawVideoImportDialog(void);

#endif /* UI_VIDEO_IMPORT_H */
