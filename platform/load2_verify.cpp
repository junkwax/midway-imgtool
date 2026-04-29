/*************************************************************
 * platform/load2_verify.cpp
 *
 * Mirror of LOAD2's destbits-determining logic, run against
 * each image's baseline_p snapshot vs current data_p.
 *
 * Anything that changes destbits for image N shifts imgaddr
 * for image N+1..end, breaking SAGs for everything that follows.
 *************************************************************/
#include "load2_verify.h"
#include "img_format.h"
#include "img_io.h"

#include <cstdio>
#include <cstring>

/* Reproduce LOAD2's per-row zero analysis exactly enough to
 * catch drift. We don't replicate the lm/tm quantization (that
 * absorbs some small changes); we report any zero-count change.
 * Strictly stricter than LOAD2 → some warnings will be
 * "destbits-equivalent" false positives, but never misses a
 * real break. */
static void count_row_zeros(const unsigned char *row, int w,
                            int *out_lead, int *out_trail)
{
    int lead = 0, trail = 0;
    while (lead < w && row[lead] == 0) lead++;
    /* If the entire row is zero, lead==w and trail by convention 0
     * (zcom_analysis only counts trailing if a non-zero was seen). */
    if (lead < w) {
        int x = w - 1;
        while (x >= lead && row[x] == 0) { trail++; x--; }
    }
    *out_lead  = lead;
    *out_trail = trail;
}

static int sum_destbits_uncompressed(int w, int h, int bpp)
{
    return w * h * bpp;
}

/* Approximate LOAD2's zcom_analysis result without simulating
 * lm/tm selection. Returns total bits of (xsize - quantized_lead -
 * quantized_trail) * bpp + ysize*8, choosing the minimum across
 * factors {1,2,4,8} (matches the spirit). For drift detection
 * we just need: this value computed for baseline == this value
 * for current, since any zero-shape change moves it. */
static long sum_destbits_zcom(const unsigned char *pixels, int w, int h,
                              int stride, int bpp)
{
    /* Pick lm/tm that minimizes leftover zeros, like LOAD2.
     * Per-row leftover = lead - quant_lead*factor (with quant
     * capped at 15). We pick global lm minimizing sum of leftovers
     * across rows; same for tm. */
    long lead_left[4] = {0,0,0,0};
    long trail_left[4] = {0,0,0,0};
    /* We'll need per-row counts — store them on the stack with a
     * small dynamic alloc to keep this stateless. */
    int *zlc = (int *)calloc(h, sizeof(int));
    int *ztc = (int *)calloc(h, sizeof(int));
    if (!zlc || !ztc) { free(zlc); free(ztc); return -1; }

    for (int y = 0; y < h; y++) {
        count_row_zeros(pixels + y * stride, w, &zlc[y], &ztc[y]);
        for (int k = 0; k < 4; k++) {
            int factor0 = 1 << k;
            int q = zlc[y] / factor0;
            if (q > 15) q = 15;
            lead_left[k] += zlc[y] - q * factor0;

            q = ztc[y] / factor0;
            if (q > 15) q = 15;
            trail_left[k] += ztc[y] - q * factor0;
        }
    }
    int lm = 0, tm = 0;
    for (int k = 1; k < 4; k++) {
        if (lead_left[k]  < lead_left[lm])  lm = k;
        if (trail_left[k] < trail_left[tm]) tm = k;
    }
    int lm_mult = 1 << lm;
    int tm_mult = 1 << tm;

    long imgsize = 0;
    for (int y = 0; y < h; y++) {
        int qzl = zlc[y] / lm_mult; if (qzl > 15) qzl = 15;
        int qzt = ztc[y] / tm_mult; if (qzt > 15) qzt = 15;
        int x0 = qzl * lm_mult;
        int x1 = w - 1 - qzt * tm_mult;
        /* ZCOMPIXELS expansion path skipped — its only effect is
         * to grow x1-x0 to >=10 by un-quantizing zeros, which is
         * an absorbing tweak. For drift detection it's fine to
         * omit; if the original needed the path, the *delta* is
         * what we're looking at, and the edited copy will use it
         * the same way unless zero-shape changed (which is what
         * we're already flagging). */
        int visible = x1 - x0 + 1;
        if (visible < 0) visible = 0;
        imgsize += (long)visible * bpp;
    }
    imgsize += (long)h * 8;

    long uncompressed = (long)w * h * bpp;
    if (imgsize > uncompressed) imgsize = uncompressed; /* zcom.c:118-120 */

    free(zlc);
    free(ztc);
    return imgsize;
}

/* Find first row whose zero shape differs. Returns -1 if all rows
 * match. Also fills out_lead/trail deltas for the first mismatched
 * row so the report can show specifics. */
static int first_zero_drift_row(const unsigned char *base,
                                const unsigned char *cur,
                                int w, int h, int stride,
                                int *out_dl, int *out_dt)
{
    for (int y = 0; y < h; y++) {
        int bl, bt, cl, ct;
        count_row_zeros(base + y * stride, w, &bl, &bt);
        count_row_zeros(cur  + y * stride, w, &cl, &ct);
        if (bl != cl || bt != ct) {
            if (out_dl) *out_dl = cl - bl;
            if (out_dt) *out_dt = ct - bt;
            return y;
        }
    }
    return -1;
}

L2Report VerifyLoad2Packing(int ppp)
{
    L2Report r;

    /* Resolve effective bpp per image, mirroring load2.c:2299-2314.
     * If ppp==0, use the palette's own bitspix. If ppp set and the
     * palette has more colors than 1<<ppp, LOAD2 falls back to
     * palette bitspix — that's the silent jump we want to catch. */
    int img_idx = 0;
    for (IMG *img = (IMG *)img_p; img; img = (IMG *)img->nxt_p, img_idx++) {
        r.imgs_checked++;

        /* Geometry sanity: w<3 gets bumped to 3 by LOAD2; just
         * report if user set something unusual. */
        if (img->w < 3) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                "w=%u < MIN_IMG_WIDTH(3); LOAD2 will pad to 3 (xsize shift)",
                (unsigned)img->w);
            r.issues.push_back({img_idx, img->n_s, L2Severity::Warn, msg});
        }

        /* Palette ppp fallback check. */
        PAL *pal = get_pal((int)img->palnum);
        if (pal && ppp > 0) {
            unsigned int limit = 1u << ppp;
            if (pal->numc > limit) {
                char msg[200];
                snprintf(msg, sizeof(msg),
                    "palette '%s' has %u colors > %u (PPP=%d limit) — "
                    "LOAD2 will silent-fallback to %u bpp; SAGs after "
                    "this image will misalign",
                    pal->n_s, (unsigned)pal->numc, limit, ppp,
                    (unsigned)pal->bitspix);
                r.issues.push_back({img_idx, img->n_s, L2Severity::Break, msg});
            }
        }

        /* Baseline-vs-current shape check (only meaningful if we
         * have a baseline — fresh-loaded images do; new/duplicated
         * images don't). */
        if (!img->baseline_p) {
            r.imgs_no_baseline++;
            continue;
        }
        if (!img->data_p) continue;

        unsigned int stride = ((unsigned int)img->w + 3) & ~3u;
        const unsigned char *base = (const unsigned char *)img->baseline_p;
        const unsigned char *cur  = (const unsigned char *)img->data_p;

        /* Quick byte-equal check — most untouched images hit this
         * and skip the row scan entirely. */
        unsigned int pix_sz = stride * img->h;
        if (memcmp(base, cur, pix_sz) == 0) continue;

        /* Per-row zero-shape comparison. */
        int dl = 0, dt = 0;
        int row = first_zero_drift_row(base, cur, img->w, img->h,
                                       (int)stride, &dl, &dt);
        if (row >= 0) {
            char msg[220];
            snprintf(msg, sizeof(msg),
                "zero-shape drift at row %d (lead %+d, trail %+d) — "
                "destbits will shift; every SAG after this image "
                "misaligns",
                row, dl, dt);
            r.issues.push_back({img_idx, img->n_s, L2Severity::Break, msg});
        }
        /* If no row drift but pixels changed, that's a same-shape
         * recolor — safe under ZON+PPP. Do not warn. */
    }

    for (auto &i : r.issues) {
        if (i.sev == L2Severity::Break) r.break_count++;
        else if (i.sev == L2Severity::Warn) r.warn_count++;
    }
    return r;
}

L2Report VerifyLoad2BeforeSave(int ppp)
{
    L2Report r = VerifyLoad2Packing(ppp);
    if (r.break_count > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "LOAD2: %d breaking issue%s — Tools > Verify LOAD2 "
                 "Packing for details",
                 r.break_count, r.break_count == 1 ? "" : "s");
        g_restore_msg_timer = 8.0f;
    } else if (r.warn_count > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "LOAD2: %d warning%s (saved OK)",
                 r.warn_count, r.warn_count == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
    }
    return r;
}

/* Helper kept for completeness — not strictly used by the report
 * but useful for future "predict SAG offsets" features. */
extern "C" long load2_predict_destbits(const unsigned char *pixels,
                                       int w, int h, int stride,
                                       int bpp, int zcom_on)
{
    if (zcom_on) return sum_destbits_zcom(pixels, w, h, stride, bpp);
    return sum_destbits_uncompressed(w, h, bpp);
}
