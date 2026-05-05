/*************************************************************
 * platform/load2_verify.h
 *
 * LOAD2 packing verifier. Detects edits that would break SAG
 * alignment when LOAD2 processes the saved IMG, by comparing
 * each image against its baseline_p snapshot.
 *
 * Three failure modes detected (each shifts destbits for the
 * affected image and misaligns every following image's SAG):
 *   1. w/h drift           (any change → uncompressed-size shift)
 *   2. palette numc > N    (PPP fallback → silent bpp jump)
 *   3. zero-shape drift    (per-row leading/trailing zero counts
 *                           change → zcom_analysis emits different
 *                           destbits)
 *
 * See doc/load2/load2.c:2299-2547 and doc/load2/zcom.c for the
 * exact rules being mirrored.
 *************************************************************/
#ifndef LOAD2_VERIFY_H
#define LOAD2_VERIFY_H

#include <vector>
#include <string>

enum class L2Severity { OK, Warn, Break };

struct L2Issue {
    int          img_idx;
    std::string  img_name;
    L2Severity   sev;
    std::string  message;   /* short, single-line */
};

struct L2Report {
    std::vector<L2Issue> issues;
    int  imgs_checked     = 0;
    int  imgs_no_baseline = 0;
    int  break_count      = 0;
    int  warn_count       = 0;
};

/* Run the full check. ppp = bits-per-pixel from LOD's PPP> directive
 * (6 for MK2MIL.LOD). Pass 0 to use auto (palette's bitspix).
 * limit_scales_to_3 enforces the /3 limit (no eighth scales). */
L2Report VerifyLoad2Packing(int ppp, bool limit_scales_to_3 = false);

/* Pre-save hook. Runs the check and, if any breaking issues are
 * found, populates g_restore_msg with a summary. Save proceeds
 * either way — this is advisory. Returns the report so callers
 * can display details. */
L2Report VerifyLoad2BeforeSave(int ppp, bool limit_scales_to_3 = false);

#endif /* LOAD2_VERIFY_H */
