/*************************************************************
 * platform/img_io.cpp
 * File I/O: IMG load/save, TGA/LBM/PNG import/export.
 * Extracted from imgui_overlay.cpp.
 *************************************************************/
#include "img_io.h"
#include "load2_verify.h"
#include "compat.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <vector>
#include <algorithm>
#include <string>
#include <regex>
#include <stdarg.h>

/* From imgui_overlay.cpp */
extern void undo_push(void);

/* Shared texture cache index (invalidated on I/O that changes pixels) */
int  g_img_tex_idx = -2;

/* Status message for restore operation */
char  g_restore_msg[128] = {0};
float g_restore_msg_timer = 0.0f;

/* Verbose logging toggle */
bool  g_verbose = false;
std::vector<std::string> g_log_lines;

void verbose_log(const char *fmt, ...)
{
    if (!g_verbose) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    g_log_lines.push_back(buf);
    if (g_log_lines.size() > 2000) {
        g_log_lines.erase(g_log_lines.begin(), g_log_lines.begin() + 1000);
    }

#ifdef _WIN32
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
#else
    fprintf(stderr, "%s\n", buf);
#endif
}

/* TGA structures shared by SaveTga/LoadTga/BuildTgaFromMarked */
#pragma pack(push, 1)
struct TGA_HEADER {
    uint8_t  id_len;
    uint8_t  cm_type;
    uint8_t  i_type;
    uint16_t cm_first;
    uint16_t cm_length;
    uint8_t  cm_size;
    uint16_t x_origin;
    uint16_t y_origin;
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;
    uint8_t  desc;
};
struct AFACE {
    uint32_t CTRL;
    uint32_t PAL;
    uint32_t O1;
    uint32_t O2;
    uint32_t O3;
    uint32_t O4;
    uint16_t AYX;
    uint16_t BYX;
    uint16_t CYX;
    uint16_t DYX;
    uint32_t LINE;
};
#pragma pack(pop)

/* ---- ImgLoad: port of img_load (IMG file reader) ----
 *
 * On-disk layout (per doc/it/itimg.asm img_load):
 *   [0 .. 28)               LIB_HDR
 *   [28 .. hdr.oset)        Pixel data + palette data, packed in arbitrary order.
 *                           Reachable only via IMAGE.oset / PALETTE.oset fields.
 *   [hdr.oset ..)           IMAGE_disk records (imgcnt of them)
 *   [.. ..)                 PALETTE_disk records (palcnt - NUMDEFPAL of them)
 *   [.. ..)                 SEQSCR/ENTRY blob (seqcnt sequences + scrcnt scripts)
 *   [.. EOF)                PTTBL records (40 bytes each, indexed by IMAGE.pttblnum)
 */
static void build_full_path(char *dst, int dstsz)
{
    size_t plen = strlen(fpath_s);
    if (plen > 0 && fpath_s[plen - 1] != '\\' && fpath_s[plen - 1] != '/')
        _snprintf(dst, dstsz, "%s\\%s", fpath_s, fname_s);
    else
        _snprintf(dst, dstsz, "%s%s", fpath_s, fname_s);
}

void LoadImgFile(void)
{
    char full[MAX_PATH];
    build_full_path(full, sizeof(full));
    verbose_log("LoadImgFile: %s", full);
    FILE *f = fopen(full, "rb");
    if (!f) { verbose_log("  -> fopen failed"); return; }

    LIB_HDR hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return; }

    if (hdr.temp != 0xABCD || hdr.version < 0x500) { fclose(f); return; }
    fileversion = hdr.version;

    if (hdr.imgcnt == 0 || hdr.imgcnt > 2000) { fclose(f); return; }

    verbose_log("  version=0x%04X  imgcnt=%u  palcnt=%u  seqcnt=%d  scrcnt=%d",
        hdr.version, hdr.imgcnt, hdr.palcnt,
        (signed short)hdr.seqcnt, (signed short)hdr.scrcnt);

    /* Drop any prior seq/scr blob — we rebuild it from this file's contents. */
    if (scrseqmem_p) { free(scrseqmem_p); scrseqmem_p = NULL; }
    scrseqbytes = 0;

    /* Capture LIB_HDR fields that have to round-trip verbatim. The original
     * DOS imgtool clobbered these on save (bufscr=-1, spare*=0), but real
     * game-asset files have meaningful values that LOAD2 consumes. */
    memcpy(file_bufscr, hdr.bufscr, 4);
    file_spare1 = hdr.spare1;
    file_spare2 = hdr.spare2;
    file_spare3 = hdr.spare3;

    ilpalloaded = -1;
    damcnt = 0;

    /* Compute offset to the seq/scr region: it lives just past the
     * IMAGE_disk and PALETTE_disk record arrays. */
    unsigned int seqscr_oset = (unsigned int)hdr.oset
                             + (unsigned int)hdr.imgcnt * sizeof(IMAGE_disk);
    if (hdr.palcnt > NUMDEFPAL)
        seqscr_oset += (unsigned int)(hdr.palcnt - NUMDEFPAL) * sizeof(PALETTE_disk);

    /* ---- Walk SEQSCR + ENTRY blob to compute its size, then slurp it.
     *
     * On disk (v0x634+, "far ptr" — empirically verified against CAGE1.IMG):
     *   SEQSCR record = 98 bytes:
     *      [+0..16) name_s  (zero-padded ASCII)
     *      [+16..18) flags  (0x40 = SEQFLG bit; sequences have it set, scripts don't)
     *      [+18..20) num    (count of ENTRY records that follow this SEQSCR)
     *      [+20..84) entry_t  (16 dwords — placeholder pointers used at runtime)
     *      [+84..86) startx
     *      [+86..88) starty
     *      [+88..94) dam[6]
     *      [+94..96) spare1
     *      [+96..98) spare2
     *   ENTRY record = 18 bytes (not 16 as wmpstruc.inc's struct field math suggests —
     *   the on-disk record carries an extra 2-byte field).
     *
     * For v < 0x634 ("near ptr" model), the original asm subtracts 2*16+8=40 bytes
     * from SEQSCR size and 2 bytes from ENTRY size: SEQSCR=58, ENTRY=16.
     */
    long seqscr_blob_start = (long)seqscr_oset;
    {
        int seqcnt_local = (signed short)hdr.seqcnt;
        int scrcnt_local = (signed short)hdr.scrcnt;
        int is_far     = (hdr.version >= 0x634);
        int seqscr_sz  = is_far ? 98 : 58;
        int entry_sz   = is_far ? 18 : 16;
        unsigned int total_bytes = 0;
        long pos = seqscr_blob_start;

        for (int s = 0; s < seqcnt_local + scrcnt_local; s++) {
            unsigned short num = 0;
            fseek(f, pos + 18, SEEK_SET);  /* SEQSCR.num at +18 */
            if (fread(&num, 1, 2, f) != 2) break;
            unsigned int rec_bytes = (unsigned int)seqscr_sz + (unsigned int)num * (unsigned int)entry_sz;
            total_bytes += rec_bytes;
            pos += (long)rec_bytes;
        }

        scrseqbytes = total_bytes;
        if (scrseqbytes > 0) {
            scrseqmem_p = malloc(scrseqbytes);
            if (scrseqmem_p) {
                fseek(f, seqscr_blob_start, SEEK_SET);
                if (fread(scrseqmem_p, 1, scrseqbytes, f) != scrseqbytes) {
                    free(scrseqmem_p); scrseqmem_p = NULL; scrseqbytes = 0;
                }
            } else {
                scrseqbytes = 0;
            }
        }
    }
    /* Point tables follow the seq/scr blob. */
    long ptoset = seqscr_blob_start + (long)scrseqbytes;

    seqcnt = (unsigned int)(signed short)hdr.seqcnt;
    scrcnt = (unsigned int)(signed short)hdr.scrcnt;

    int pal_base = (int)palcnt;
    unsigned int img_oset = hdr.oset;

    for (int i = 0; i < hdr.imgcnt; i++) {
        fseek(f, (long)img_oset, SEEK_SET);
        img_oset += sizeof(IMAGE_disk);

        IMAGE_disk idisk;
        if (fread(&idisk, 1, sizeof(idisk), f) != sizeof(idisk)) break;

        IMG *img = AllocImg();
        if (!img) break;

        if (hdr.version < 0x634) {
            unsigned short tmp = idisk.aniz2;
            idisk.aniz2 = idisk.frm;
            idisk.frm   = tmp;
            idisk.opals = (unsigned short)-1;
        }

        img->flags  = idisk.flags;
        img->anix   = idisk.anix;
        img->aniy   = idisk.aniy;
        img->w      = (idisk.w < 3) ? 3 : idisk.w;
        img->h      = idisk.h;
        img->palnum = (unsigned short)((int)idisk.palnum - NUMDEFPAL + pal_base);
        img->anix2  = idisk.anix2;
        img->aniy2  = idisk.aniy2;
        img->aniz2  = idisk.aniz2;
        img->opals  = idisk.opals;
        img->pttbl_p = NULL;

        img->file_oset     = idisk.oset;
        img->file_data     = idisk.data;
        img->file_lib      = idisk.lib;
        img->file_frm      = idisk.frm;
        img->file_pttblnum = idisk.pttblnum;
        memcpy(img->file_name_raw, idisk.n_s, 16);

        strncpy(img->n_s, idisk.n_s, 15);
        img->n_s[15] = '\0';

        strncpy(img->src_filename, fname_s, 15);
        img->src_filename[15] = '\0';

        unsigned int stride = ((unsigned int)img->w + 3) & ~3;
        unsigned int pix_sz = stride * img->h;
        img->data_p = PoolAlloc(pix_sz);
        if (!img->data_p) break;

        fseek(f, (long)idisk.oset, SEEK_SET);

        if (img->flags & 0x0080) { // CMP
            int lm_mult = 1 << ((img->flags >> 8) & 3);
            int tm_mult = 1 << ((img->flags >> 10) & 3);
            unsigned char *dst = (unsigned char *)img->data_p;
            for (int y = 0; y < img->h; y++) {
                int comp_byte = fgetc(f);
                if (comp_byte == EOF) break;

                int leading = (comp_byte & 0x0F) * lm_mult;
                int trailing = ((comp_byte >> 4) & 0x0F) * tm_mult;
                int visible = img->w - leading - trailing;
                if (visible < 0) visible = 0;

                memset(dst, 0, leading);
                if (visible > 0) fread(dst + leading, 1, visible, f);
                memset(dst + leading + visible, 0, trailing);
                for (int x = img->w; x < (int)stride; x++) dst[x] = 0;

                dst += stride;
            }
        } else {
            fread(img->data_p, 1, pix_sz, f);
        }

        if (hdr.version >= 0x60A && (signed short)idisk.pttblnum >= 0) {
            fseek(f, (long)(ptoset + (unsigned int)(signed short)idisk.pttblnum * 40), SEEK_SET);
            img->pttbl_p = PoolAlloc(40);
            if (img->pttbl_p) fread(img->pttbl_p, 1, 40, f);
        }

        /* Snapshot the just-loaded pixel data as the baseline. The diff-mode
         * bulk-restore uses this to propagate ONLY the user's edits to a
         * master sprite into its child pieces, leaving any pixel that wasn't
         * touched alone (preserving hand-tuned per-piece details). */
        img->baseline_p = PoolAlloc(pix_sz);
        if (img->baseline_p && img->data_p)
            memcpy(img->baseline_p, img->data_p, pix_sz);
    }

    unsigned int pal_foffset = (unsigned int)hdr.oset + (unsigned int)hdr.imgcnt * sizeof(IMAGE_disk);
    int num_pals = (int)hdr.palcnt - NUMDEFPAL;
    if (num_pals < 0) num_pals = 0;

    for (int i = 0; i < num_pals; i++) {
        fseek(f, (long)pal_foffset, SEEK_SET);
        pal_foffset += sizeof(PALETTE_disk);

        PALETTE_disk pdisk;
        if (fread(&pdisk, 1, sizeof(pdisk), f) != sizeof(pdisk)) break;

        PAL *pal = AllocPal();
        if (!pal) break;

        pal->flags   = pdisk.flags;
        pal->bitspix = pdisk.bitspix;
        pal->numc    = pdisk.numc;
        pal->pad     = 0;

        strncpy(pal->n_s, pdisk.n_s, 9);
        pal->n_s[9] = '\0';

        memcpy(pal->file_name_raw, pdisk.n_s, 10);
        pal->file_data  = pdisk.data;
        pal->file_lib   = pdisk.lib;
        pal->file_colind = pdisk.colind;
        pal->file_cmap  = pdisk.cmap;
        pal->file_spare = pdisk.spare;

        unsigned int col_sz = (unsigned int)pal->numc * 2;
        pal->data_p = PoolAlloc(col_sz);
        if (!pal->data_p) break;

        fseek(f, (long)pdisk.oset, SEEK_SET);
        fread(pal->data_p, 1, col_sz, f);
    }

    fclose(f);

    if (imgcnt > 0) ilselected = 0;
}

/* ---- SaveImgFile: port of img_save (IMG file writer) ----
 *
 * On-disk layout produced (matches doc/it/itimg.asm img_save exactly):
 *   1. Reserve LIB_HDR placeholder at [0..28).
 *   2. Write all palette pixel data — each PAL.DATA, no padding. Stash the
 *      file offset at write time into PAL.temp (used in step 6).
 *   3. Write all image pixel data — each IMG.DATA. Stash file offset into
 *      IMG.temp.
 *   4. Snap LIB_HDR.OSET = current file pointer (= start of records).
 *   5. Write all IMAGE_disk records (using IMG.temp for OSET fields).
 *   6. Write all PALETTE_disk records (using PAL.temp for OSET fields).
 *   7. Write the SEQSCR+ENTRY blob verbatim (scrseqmem_p, scrseqbytes).
 *   8. Write all per-image PTTBL records (40 bytes each) for any image
 *      that has one.
 *   9. Rewind, rewrite the finalized LIB_HDR.
 */
/* PPP> setting from MK2MIL.LOD; other LODs differ. 6 covers MK2's
 * fighter sprites (≤64 colors per palette). Verifier compares
 * palette numc against (1<<ppp); set to 0 to disable the check. */
int g_load2_ppp = 6;
bool g_load2_limit_scales_to_3 = false;

void SaveImgFile(void)
{
    /* Pre-save advisory: pop a toast if edits will misalign SAGs
     * after LOAD2 processes the saved IMG. Save proceeds either way. */
    VerifyLoad2BeforeSave(g_load2_ppp, g_load2_limit_scales_to_3);

    char full[MAX_PATH];
    build_full_path(full, sizeof(full));
    FILE *f = fopen(full, "wb");
    if (!f) return;

    int num_imgs = (int)imgcnt;
    int num_pals = (int)palcnt;

    /* ---- 1. Header placeholder ---- */
    LIB_HDR hdr = {};
    hdr.imgcnt  = (unsigned short)num_imgs;
    hdr.palcnt  = (unsigned short)(num_pals + NUMDEFPAL);
    hdr.version = (fileversion != 0) ? (unsigned short)fileversion : 0x0634;
    hdr.temp    = 0xABCD;
    hdr.oset    = 0;  /* finalized in step 4 */
    hdr.seqcnt  = (unsigned short)seqcnt;
    hdr.scrcnt  = (unsigned short)scrcnt;
    hdr.damcnt  = 0;  /* original asm always zeros this on save */
    /* Preserve bufscr + spare1/2/3 from load (real game files use them;
     * LOAD2 consumes bufscr to compute IRW layout). Default is all 0xFF /
     * zero (matches a freshly-created file). */
    memcpy(hdr.bufscr, file_bufscr, 4);
    hdr.spare1 = file_spare1;
    hdr.spare2 = file_spare2;
    hdr.spare3 = file_spare3;
    fwrite(&hdr, 1, sizeof(hdr), f);

    /* ---- 2. Palette pixel data ---- */
    PAL *pal = (PAL *)pal_p;
    for (int i = 0; i < num_pals && pal; i++, pal = (PAL *)pal->nxt_p) {
        unsigned int data_oset = (unsigned int)ftell(f);
        pal->temp = (void *)(uintptr_t)data_oset;
        unsigned int sz = (unsigned int)pal->numc * 2;
        if (pal->data_p) {
            fwrite(pal->data_p, 1, sz, f);
        } else if (sz > 0) {
            unsigned char *z = (unsigned char *)calloc(1, sz);
            if (z) { fwrite(z, 1, sz, f); free(z); }
        }
    }

    /* ---- 3. Image pixel data ---- */
    IMG *img = (IMG *)img_p;
    for (int i = 0; i < num_imgs && img; i++, img = (IMG *)img->nxt_p) {
        unsigned int data_oset = (unsigned int)ftell(f);
        img->temp = (void *)(uintptr_t)data_oset;

        unsigned int stride = ((unsigned int)img->w + 3) & ~3;
        unsigned int sz = stride * img->h;

        if (img->flags & 0x0080) {
            /* CMP mode — leading/trailing zero RLE per row. */
            int lm_mult = 1 << ((img->flags >> 8) & 3);
            int tm_mult = 1 << ((img->flags >> 10) & 3);
            unsigned char *src = (unsigned char *)img->data_p;
            bool free_z = false;
            if (!src) { src = (unsigned char *)calloc(1, sz); free_z = true; }
            if (src) {
                for (int y = 0; y < img->h; y++) {
                    unsigned char *row = src + y * stride;
                    int leading = 0;
                    while (leading < img->w && row[leading] == 0) leading++;
                    int trailing = 0;
                    if (leading < img->w) {
                        while (trailing < img->w && row[img->w - 1 - trailing] == 0) trailing++;
                    }
                    int l_enc = leading / lm_mult;
                    int t_enc = trailing / tm_mult;
                    if (l_enc > 15) l_enc = 15;
                    if (t_enc > 15) t_enc = 15;
                    int actual_leading  = l_enc * lm_mult;
                    int actual_trailing = t_enc * tm_mult;
                    int visible = img->w - actual_leading - actual_trailing;
                    if (visible < 0) visible = 0;

                    unsigned char comp_byte = (unsigned char)((t_enc << 4) | (l_enc & 0x0F));
                    fwrite(&comp_byte, 1, 1, f);
                    if (visible > 0)
                        fwrite(row + actual_leading, 1, visible, f);
                }
                if (free_z) free(src);
            }
        } else {
            if (img->data_p) {
                fwrite(img->data_p, 1, sz, f);
            } else if (sz > 0) {
                unsigned char *z = (unsigned char *)calloc(1, sz);
                if (z) { fwrite(z, 1, sz, f); free(z); }
            }
        }
    }

    /* ---- 4. Snap OSET ---- */
    hdr.oset = (unsigned int)ftell(f);

    /* ---- 5. IMAGE_disk records ---- */
    int pt_index = 0;
    img = (IMG *)img_p;
    for (int i = 0; i < num_imgs && img; i++, img = (IMG *)img->nxt_p) {
        IMAGE_disk idisk = {};

        /* Name: write the verbatim 16-byte buffer from load. If the user
         * renamed the sprite, we splice the new name into the head and keep
         * the original tail-bytes after the first null. If this is a brand
         * new sprite (no file_name_raw populated — it's all zeros), zero
         * pad like a fresh save. */
        bool fresh_name = true;
        for (int k = 0; k < 16; k++) if (img->file_name_raw[k]) { fresh_name = false; break; }
        if (fresh_name) {
            strncpy(idisk.n_s, img->n_s, 15);
            idisk.n_s[15] = '\0';
        } else {
            /* Copy original tail; overlay new name up to first null */
            memcpy(idisk.n_s, img->file_name_raw, 16);
            size_t name_len = strnlen(img->n_s, 15);
            memcpy(idisk.n_s, img->n_s, name_len);
            /* Place a null at name_len if it isn't already there. The bytes
             * after name_len stay as the original tail (which itself contains
             * a null somewhere — that's how the original tool wrote them). */
            idisk.n_s[name_len] = '\0';
        }

        idisk.flags    = img->flags;
        idisk.anix     = img->anix;
        idisk.aniy     = img->aniy;
        idisk.w        = img->w;
        idisk.h        = img->h;
        idisk.palnum   = (unsigned short)((int)img->palnum + NUMDEFPAL);
        idisk.oset     = (unsigned int)(uintptr_t)img->temp;
        idisk.anix2    = img->anix2;
        idisk.aniy2    = img->aniy2;
        idisk.aniz2    = img->aniz2;
        idisk.lib      = img->file_lib;
        idisk.data     = img->file_data;  /* preserve from load */
        idisk.frm      = img->file_frm;  /* preserve from load (real files
                                            often have 0xFFFF; original DOS
                                            imgtool clobbered to 0) */
        idisk.opals    = img->opals;
        idisk.pttblnum = img->pttbl_p
            ? (img->file_oset ? img->file_pttblnum : (unsigned short)(pt_index++))
            : (unsigned short)0xFFFF;
        fwrite(&idisk, 1, sizeof(idisk), f);
    }

    /* ---- 6. PALETTE_disk records ---- */
    pal = (PAL *)pal_p;
    for (int i = 0; i < num_pals && pal; i++, pal = (PAL *)pal->nxt_p) {
        PALETTE_disk pdisk = {};

        bool fresh_name = true;
        for (int k = 0; k < 10; k++) if (pal->file_name_raw[k]) { fresh_name = false; break; }
        if (fresh_name) {
            strncpy(pdisk.n_s, pal->n_s, 9);
            pdisk.n_s[9] = '\0';
        } else {
            memcpy(pdisk.n_s, pal->file_name_raw, 10);
            size_t name_len = strnlen(pal->n_s, 9);
            memcpy(pdisk.n_s, pal->n_s, name_len);
            pdisk.n_s[name_len] = '\0';
        }

        pdisk.flags   = pal->flags;
        pdisk.bitspix = pal->bitspix;
        pdisk.numc    = pal->numc;
        pdisk.oset    = (unsigned int)(uintptr_t)pal->temp;
        pdisk.data    = pal->file_data;
        pdisk.lib     = pal->file_lib;
        pdisk.colind  = pal->file_colind;
        pdisk.cmap    = pal->file_cmap;
        pdisk.spare   = pal->file_spare;
        fwrite(&pdisk, 1, sizeof(pdisk), f);
    }

    /* ---- 7. SEQSCR+ENTRY blob ---- */
    if (scrseqmem_p && scrseqbytes > 0) {
        fwrite(scrseqmem_p, 1, scrseqbytes, f);
    }

    /* ---- 8. Per-image PTTBL records (40 bytes each) ---- */
    img = (IMG *)img_p;
    for (int i = 0; i < num_imgs && img; i++, img = (IMG *)img->nxt_p) {
        if (img->pttbl_p) {
            fwrite(img->pttbl_p, 1, 40, f);
        }
    }

    /* ---- 9. Rewrite finalized LIB_HDR ---- */
    rewind(f);
    fwrite(&hdr, 1, sizeof(hdr), f);

    fclose(f);
    fileversion = hdr.version;
}

/* ---- Restore Marked Images from Source ---- */
int RestoreMarkedFromSource(void)
{
    IMG *src = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!src || !src->data_p || src->w == 0 || src->h == 0) return 0;

    bool any_target = false;
    for (IMG *t = (IMG *)img_p; t; t = (IMG *)t->nxt_p) {
        if ((t->flags & 1) && t != src) { any_target = true; break; }
    }
    if (!any_target) return 0;

    undo_push();

    const unsigned char *src_pix = (const unsigned char *)src->data_p;
    int src_stride = (src->w + 3) & ~3;
    int total_written = 0;

    for (IMG *t = (IMG *)img_p; t; t = (IMG *)t->nxt_p) {
        if (!(t->flags & 1) || t == src || !t->data_p || t->w == 0 || t->h == 0) continue;
        if (t->palnum != src->palnum) continue;

        unsigned char *dst_pix = (unsigned char *)t->data_p;
        int dst_stride = (t->w + 3) & ~3;

        int dx = (int)(short)src->anix - (int)(short)t->anix;
        int dy = (int)(short)src->aniy - (int)(short)t->aniy;

        for (int y = 0; y < t->h; y++) {
            int sy = y + dy;
            if (sy < 0 || sy >= src->h) continue;
            for (int x = 0; x < t->w; x++) {
                int sx = x + dx;
                if (sx < 0 || sx >= src->w) continue;
                unsigned char dst_p = dst_pix[y * dst_stride + x];
                if (dst_p != 0) continue;
                unsigned char src_p = src_pix[sy * src_stride + sx];
                if (src_p == 0) continue;
                dst_pix[y * dst_stride + x] = src_p;
                total_written++;
            }
        }
    }

    if (total_written > 0) {
        g_img_tex_idx = -2;
        int idx = 0;
        for (IMG *t2 = (IMG *)img_p; t2; t2 = (IMG *)t2->nxt_p, idx++) {
            if ((t2->flags & 1) && t2 != src) { ilselected = idx; break; }
        }
    }
    return total_written;
}

/* Force-restore: unconditionally copies all source pixels into every
   marked image.  No transparency or existing-pixel checks — this
   overwrites every pixel.  Useful for rebuilding splits (1A/1B/2A...)
   from a full unchopped source when the strips have drifted. */
int RestoreMarkedFromSourceForce(void)
{
    IMG *src = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!src || !src->data_p || src->w == 0 || src->h == 0) return 0;

    bool any_target = false;
    for (IMG *t = (IMG *)img_p; t; t = (IMG *)t->nxt_p) {
        if ((t->flags & 1) && t != src) { any_target = true; break; }
    }
    if (!any_target) return 0;

    undo_push();

    const unsigned char *src_pix = (const unsigned char *)src->data_p;
    int src_stride = (src->w + 3) & ~3;
    int total_written = 0;
    int imgs_processed = 0;

    for (IMG *t = (IMG *)img_p; t; t = (IMG *)t->nxt_p) {
        if (!(t->flags & 1) || t == src || !t->data_p || t->w == 0 || t->h == 0) continue;
        imgs_processed++;

        unsigned char *dst_pix = (unsigned char *)t->data_p;
        int dst_stride = (t->w + 3) & ~3;

        /* Clear the entire strip first, then repaint from source.
           This guarantees old art is gone and only source pixels remain. */
        memset(dst_pix, 0, (size_t)dst_stride * t->h);

        int dx = (int)(short)src->anix - (int)(short)t->anix;
        int dy = (int)(short)src->aniy - (int)(short)t->aniy;

        for (int y = 0; y < t->h; y++) {
            int sy = y + dy;
            if (sy < 0 || sy >= src->h) continue;
            for (int x = 0; x < t->w; x++) {
                int sx = x + dx;
                if (sx < 0 || sx >= src->w) continue;
                dst_pix[y * dst_stride + x] = src_pix[sy * src_stride + sx];
                total_written++;
            }
        }
    }

    if (total_written > 0) {
        g_img_tex_idx = -2;
        /* Jump to first marked image so the user sees the result */
        int idx = 0;
        for (IMG *t2 = (IMG *)img_p; t2; t2 = (IMG *)t2->nxt_p, idx++) {
            if ((t2->flags & 1) && t2 != src) { ilselected = idx; break; }
        }
    }
    return total_written;
}

/* Cheap rect-clip: fills covered_pixels / total_pixels per match.
 * Mirrors the dx/dy shift used by Pairs and Diff exactly so the
 * preview number matches what Start Restore will actually copy. */
void ComputeBulkRestoreCoverage(std::vector<BulkRestoreMatch>& matches)
{
    for (auto& m : matches) {
        m.covered_pixels = 0;
        m.total_pixels   = 0;
        IMG *child  = m.child;
        IMG *parent = m.parent;
        if (!child) continue;
        m.total_pixels = (int)child->w * (int)child->h;
        if (!parent || parent == child) continue;
        if (child->palnum != parent->palnum) continue;
        if (child->w == 0 || child->h == 0)  continue;
        if (parent->w == 0 || parent->h == 0) continue;

        int dx = (int)(short)parent->anix - (int)(short)child->anix;
        int dy = (int)(short)parent->aniy - (int)(short)child->aniy;

        /* Child rect [0..w) × [0..h) shifted by (dx,dy) into parent space.
         * Intersect with parent's [0..pw) × [0..ph). Width/height of the
         * intersection (in child coords) gives covered_pixels. */
        int x0 = dx > 0 ? 0 : -dx;
        int y0 = dy > 0 ? 0 : -dy;
        int x1 = (parent->w - dx < child->w) ? parent->w - dx : child->w;
        int y1 = (parent->h - dy < child->h) ? parent->h - dy : child->h;
        if (x1 <= x0 || y1 <= y0) continue;
        m.covered_pixels = (x1 - x0) * (y1 - y0);
    }
}

/* Bulk restore given matches.
   Overwrites child pixels with parent pixels (respecting anipoints). */
int ExecuteBulkRestorePairs(const std::vector<BulkRestoreMatch>& matches)
{
    if (!img_p || matches.empty()) return 0;

    undo_push();

    int total_written = 0;
    int imgs_processed = 0;

    for (const auto& match : matches) {
        if (!match.selected) continue;
        
        IMG *child = match.child;
        IMG *parent = match.parent;

        if (!child->data_p || child->w == 0 || child->h == 0) continue;
        if (!parent || !parent->data_p || parent->w == 0 || parent->h == 0 || parent == child) continue;

        /* Skip cross-palette pairs — pixel indices aren't comparable across
         * palettes, so a byte-for-byte copy would produce garbled colors.
         * (The single-frame RestoreMarkedFromSource has the same guard.) */
        if (child->palnum != parent->palnum) continue;

        imgs_processed++;
        unsigned char *dst_pix = (unsigned char *)child->data_p;
        int dst_stride = (child->w + 3) & ~3;

        const unsigned char *src_pix = (const unsigned char *)parent->data_p;
        int src_stride = (parent->w + 3) & ~3;

        memset(dst_pix, 0, (size_t)dst_stride * child->h);

        int dx = (int)(short)parent->anix - (int)(short)child->anix;
        int dy = (int)(short)parent->aniy - (int)(short)child->aniy;

        for (int y = 0; y < child->h; y++) {
            int sy = y + dy;
            if (sy < 0 || sy >= parent->h) continue;
            for (int x = 0; x < child->w; x++) {
                int sx = x + dx;
                if (sx < 0 || sx >= parent->w) continue;
                dst_pix[y * dst_stride + x] = src_pix[sy * src_stride + sx];
                total_written++;
            }
        }
    }

    if (total_written > 0) {
        g_img_tex_idx = -2;
    }
    return imgs_processed;
}

/* Diff-mode bulk restore.
 *
 * Propagates ONLY the user's edits to the parent (master) sprite into each
 * child piece, leaving every untouched pixel alone. Preserves hand-tuned
 * per-piece details that the plain Replace mode would clobber.
 *
 * Algorithm: for each child pixel (x, y), look up the corresponding parent
 * pixel at (x + dx, y + dy). Compare against parent->baseline_p (snapshot
 * taken at file load). If parent[sx,sy] != baseline[sx,sy], the user has
 * edited that pixel — write it into child[x,y]. Otherwise leave child alone.
 *
 * This is the right tool when adding a small detail to a master sprite
 * (e.g. a logo on pants) and you want it to appear on the A/B/C piece
 * sprites that the game actually renders, without disturbing edge cleanup
 * or other per-piece adjustments.
 */
int ExecuteBulkRestoreDiff(const std::vector<BulkRestoreMatch>& matches)
{
    if (!img_p || matches.empty()) return 0;

    undo_push();

    int imgs_processed = 0;
    int total_written  = 0;

    for (const auto& match : matches) {
        if (!match.selected) continue;

        IMG *child  = match.child;
        IMG *parent = match.parent;

        if (!child->data_p || child->w == 0 || child->h == 0) continue;
        if (!parent || !parent->data_p || parent->w == 0 || parent->h == 0 || parent == child) continue;

        /* Without a baseline we can't tell what the user edited, so skip
         * (this can happen for sprites added in this session that never had
         * a snapshot taken at file load). */
        if (!parent->baseline_p) continue;

        if (child->palnum != parent->palnum) continue;

        unsigned char       *dst_pix    = (unsigned char *)child->data_p;
        const unsigned char *src_pix    = (const unsigned char *)parent->data_p;
        const unsigned char *baseline   = (const unsigned char *)parent->baseline_p;
        int dst_stride = (child->w  + 3) & ~3;
        int src_stride = (parent->w + 3) & ~3;

        int dx = (int)(short)parent->anix - (int)(short)child->anix;
        int dy = (int)(short)parent->aniy - (int)(short)child->aniy;

        int written_this = 0;
        for (int y = 0; y < child->h; y++) {
            int sy = y + dy;
            if (sy < 0 || sy >= parent->h) continue;
            for (int x = 0; x < child->w; x++) {
                int sx = x + dx;
                if (sx < 0 || sx >= parent->w) continue;
                int p_idx = sy * src_stride + sx;
                if (src_pix[p_idx] != baseline[p_idx]) {
                    dst_pix[y * dst_stride + x] = src_pix[p_idx];
                    written_this++;
                }
            }
        }
        if (written_this > 0) {
            total_written += written_this;
        }
        imgs_processed++;
    }

    if (total_written > 0) {
        g_img_tex_idx = -2;
    }
    return imgs_processed;
}

/* Reconstruct-mode bulk restore.
 *
 * Treats the parent as ground truth and pulls each child toward it.
 * For every child pixel within the anipoint-shifted parent rect,
 * if the child diverges from the parent, overwrite the child with
 * the parent's value. Out-of-overlap child pixels are untouched.
 *
 * Use case: shipping art has censored/redacted regions in child
 * pieces (e.g. logos blacked out for licensing) but the master
 * sprite retains the original. This pulls the original detail
 * back into the children using the same chop geometry as the
 * original WIMP toolchain.
 *
 * Differences from the other modes:
 *  - Pairs:  zeros the whole child first, then copies the parent
 *            rect → destroys per-piece detail outside the parent
 *            rect, and overwrites EVERY pixel inside it.
 *  - Diff:   copies parent pixels where parent CURRENT differs from
 *            parent BASELINE → only propagates session-edits.
 *  - Reconstruct: copies parent pixels where CHILD differs from
 *            parent → propagates the original parent content into
 *            wherever the child has been blacked out / drifted.
 */
int ExecuteBulkRestoreReconstruct(const std::vector<BulkRestoreMatch>& matches)
{
    if (!img_p || matches.empty()) return 0;

    undo_push();

    int imgs_processed = 0;
    int total_written  = 0;

    for (const auto& match : matches) {
        if (!match.selected) continue;

        IMG *child  = match.child;
        IMG *parent = match.parent;

        if (!child->data_p  || child->w  == 0 || child->h  == 0) continue;
        if (!parent || !parent->data_p || parent->w == 0 || parent->h == 0
            || parent == child) continue;

        if (child->palnum != parent->palnum) continue;

        unsigned char       *dst_pix    = (unsigned char *)child->data_p;
        const unsigned char *src_pix    = (const unsigned char *)parent->data_p;
        int dst_stride = (child->w  + 3) & ~3;
        int src_stride = (parent->w + 3) & ~3;

        int dx = (int)(short)parent->anix - (int)(short)child->anix;
        int dy = (int)(short)parent->aniy - (int)(short)child->aniy;

        int written_this = 0;
        for (int y = 0; y < child->h; y++) {
            int sy = y + dy;
            if (sy < 0 || sy >= parent->h) continue;
            for (int x = 0; x < child->w; x++) {
                int sx = x + dx;
                if (sx < 0 || sx >= parent->w) continue;
                int s_idx = sy * src_stride + sx;
                int d_idx = y  * dst_stride + x;
                /* Restore only where:
                 *   - parent has actual content (non-zero palette index), AND
                 *   - child also has content at this position (non-zero).
                 *
                 * Both halves matter:
                 *   - Skip when parent is 0: prevents transparent-parent
                 *     pixels from blanking out per-piece child detail.
                 *   - Skip when child is 0: preserves the child's
                 *     original silhouette. The shipping art was cropped
                 *     inward in some children (master sprite is the
                 *     uncropped pants; children have less extent at the
                 *     edges). Filling parent content into the child's
                 *     transparent regions would extend the silhouette,
                 *     which changes leading/trailing-zero counts and
                 *     breaks SAG alignment under ZON+PPP packing.
                 *
                 * Net behavior: re-paint censored INTERIOR regions of the
                 * child (where Midway replaced logo pixels with a flat
                 * color), but never grow the silhouette. */
                unsigned char s = src_pix[s_idx];
                unsigned char d = dst_pix[d_idx];
                if (s != 0 && d != 0 && d != s) {
                    dst_pix[d_idx] = s;
                    written_this++;
                }
            }
        }
        if (written_this > 0) total_written += written_this;
        imgs_processed++;
    }

    if (total_written > 0) {
        g_img_tex_idx = -2;
    }
    return imgs_processed;
}

/* ---- Write ANILST (Export Marked Images to Assembly) ---- */
void WriteAnilstFromMarked(const char* filepath)
{
    FILE* f = fopen(filepath, "w");
    if (!f) return;

    fprintf(f, "\t.asg\t1,N\n");

    IMG* p = (IMG*)img_p;
    int aninum = 0;
    while (p) {
        if (p->flags & 1) {
            fprintf(f, "\t.word\tN,%d\t;%.15s\n", aninum, p->n_s);
            aninum++;
        }
        p = (IMG*)p->nxt_p;
    }
    fclose(f);
}

/* ---- Write TBL (Export Marked Images to MK2 format Assembly Table) ----
 *
 * Format produced (MK2 / non-MK3 branch), matching e.g. data/MKMK3.TBL:
 *   <whole file is wrapped in .DATA ... .TEXT>
 *   LABEL:
 *       .word  W, H, ANIX, ANIY                   ; (mk3_format adds anix2/y2/z2)
 *       .long  <ROM_BIT_ADDRESS>H
 *       .word  <FLAGS>H
 *       .long  <PALETTE_LABEL>                    ; ONLY emitted when palette
 *                                                 ; differs from previous entry
 *
 * Caveat: <ROM_BIT_ADDRESS> is the bit-address of the sprite's pixel data
 * inside the IROM blob, which is determined by LOAD2 when it builds the IRW
 * — NOT by the .IMG file alone. The value below is base_address + (file_oset
 * in the .IMG file)*8 as a placeholder; if you're producing a TBL meant to be
 * linked against a real IRW, run LOAD2 and use its emitted .TBL instead.
 */
void WriteTblFromMarked(const char* filepath, unsigned int base_address, bool mk3_format, bool include_pal, bool pad_4bit, bool align_16bit, bool dual_bank, int bank)
{
    FILE* f = fopen(filepath, "w");
    if (!f) return;

    fprintf(f, "\t.DATA\n");

    IMG* p = (IMG*)img_p;
    int prev_palnum = -1;
    unsigned int current_bit_address = base_address;

    while (p) {
        if (p->flags & 1) {
            fprintf(f, "%s:\n", p->n_s);
            if (mk3_format) {
                fprintf(f, "\t.word   %d,%d,%d,%d,%d,%d,%d\n",
                        p->w, p->h,
                        (int)(short)p->anix, (int)(short)p->aniy,
                        (int)(short)p->anix2, (int)(short)p->aniy2,
                        (int)(short)p->aniz2);
            } else {
                fprintf(f, "\t.word   %d,%d,%d,%d\n",
                        p->w, p->h,
                        (int)(short)p->anix, (int)(short)p->aniy);
            }

            if (align_16bit) {
                current_bit_address = (current_bit_address + 15) & ~15;
            } else if (pad_4bit) {
                current_bit_address = (current_bit_address + 3) & ~3;
            }

            unsigned int sag = current_bit_address;
            if (dual_bank) {
                sag += (bank) ? 0x2000000 : (unsigned int)-0x2000000;
            }

            fprintf(f, "\t.long   0%XH\n", sag);
            fprintf(f, "\t.word   0%04XH\n", p->flags);
            if (include_pal && (int)p->palnum != prev_palnum) {
                PAL* pal = get_pal(p->palnum);
                fprintf(f, "\t.long   %s\n", (pal && pal->n_s[0]) ? pal->n_s : "0");
                prev_palnum = (int)p->palnum;
            }

            // Accumulate the bit size of this image for the next marked image.
            // Using 8bpp uncompressed size as a baseline.
            current_bit_address += (p->w * p->h * 8);
        }
        p = (IMG*)p->nxt_p;
    }
    fprintf(f, "\t.TEXT\n");
    fclose(f);
}

/* ---- IRW structs ---- */
#pragma pack(push, 1)
struct IRW_HEADER {
    char     version[32];
    uint32_t ver_cksum;
    uint32_t magic_num;
    uint32_t spare1;
    uint32_t spare2;
};

struct IRW_RECORD {
    uint32_t start_addr;
    uint32_t byte_count;
    uint32_t checksum;
    uint16_t interleave;
    uint16_t skipbytes;
    int16_t  bank;
    int16_t  spare1;
    uint32_t spare2;
};
#pragma pack(pop)

#define IRW_MAGIC   0x64
#define IRW_VERSION "IMAGETOOL IRW 1.0"

static unsigned int irw_cksum_str(const char *s)
{
    unsigned int sum = 0;
    while (*s) sum += (unsigned char)*s++;
    return sum;
}

static void irw_flush_word(FILE *f, unsigned int *dataword, int *bits_filled,
                           uint32_t *byte_count, uint32_t *checksum)
{
    if (*bits_filled > 0) {
        uint16_t word = (uint16_t)(*dataword & 0xFFFF);
        fwrite(&word, 1, 2, f);
        *byte_count += 2;
        *checksum += (uint32_t)word;
        *dataword = 0;
        *bits_filled = 0;
    }
}

static void irw_write_bits(FILE *f, unsigned int data, int nbits,
                           unsigned int *dataword, int *bits_filled,
                           uint32_t *byte_count, uint32_t *checksum)
{
    *dataword |= (data & ((1u << nbits) - 1)) << *bits_filled;
    *bits_filled += nbits;
    while (*bits_filled >= 16) {
        uint16_t word = (uint16_t)(*dataword & 0xFFFF);
        fwrite(&word, 1, 2, f);
        *byte_count += 2;
        *checksum += (uint32_t)word;
        *dataword >>= 16;
        *bits_filled -= 16;
    }
}

void WriteIrwFromMarked(const char *filepath, unsigned int base_address,
                        int bpp, bool align_16bit)
{
    FILE *f = fopen(filepath, "wb");
    if (!f) return;

    /* Gather marked images */
    std::vector<IMG*> marked;
    IMG *p = (IMG*)img_p;
    while (p) {
        if ((p->flags & 1) && p->w > 0 && p->h > 0 && p->data_p)
            marked.push_back(p);
        p = (IMG*)p->nxt_p;
    }
    if (marked.empty()) { fclose(f); return; }

    /* Write header placeholder */
    IRW_HEADER hdr = {};
    strncpy(hdr.version, IRW_VERSION, sizeof(hdr.version) - 1);
    hdr.magic_num = IRW_MAGIC;
    hdr.ver_cksum = irw_cksum_str(hdr.version);
    fwrite(&hdr, sizeof(hdr), 1, f);

    unsigned int addr = base_address;
    unsigned int dataword = 0;
    int bits_filled = 0;

    for (size_t i = 0; i < marked.size(); i++) {
        IMG *img = marked[i];

        int current_bpp = bpp;
        if (current_bpp == 0) {
            /* Auto (Image Data) */
            unsigned char max_val = 0;
            unsigned char *ip = (unsigned char *)img->data_p;
            int total_pixels = img->w * img->h; /* We just scan the rect, padding zeros don't matter as they are 0 */
            unsigned short stride = (img->w + 3) & ~3;
            for (int y = 0; y < img->h; y++) {
                for (int x = 0; x < img->w; x++) {
                    if (ip[y * stride + x] > max_val) max_val = ip[y * stride + x];
                }
            }
            if (max_val <= 1) current_bpp = 1;
            else if (max_val < 4) current_bpp = 2;
            else if (max_val < 8) current_bpp = 3;
            else if (max_val < 16) current_bpp = 4;
            else if (max_val < 32) current_bpp = 5;
            else if (max_val < 64) current_bpp = 6;
            else if (max_val < 128) current_bpp = 7;
            else current_bpp = 8;
        } else if (current_bpp == -1) {
            /* Auto (Palette Size) */
            PAL *pal = get_pal(img->palnum);
            int cols = pal ? pal->numc : 256;
            if (cols <= 2) current_bpp = 1;
            else if (cols <= 4) current_bpp = 2;
            else if (cols <= 8) current_bpp = 3;
            else if (cols <= 16) current_bpp = 4;
            else if (cols <= 32) current_bpp = 5;
            else if (cols <= 64) current_bpp = 6;
            else if (cols <= 128) current_bpp = 7;
            else current_bpp = 8;
        } else {
            if (current_bpp < 1) current_bpp = 1;
            if (current_bpp > 8) current_bpp = 8;
        }

        /* Ensure we start on a 16-bit word boundary */
        irw_flush_word(f, &dataword, &bits_filled, NULL, NULL);

        /* Align address to 16-bit */
        if (align_16bit && (addr & 0xF)) {
            /* already handled by flush above, but also round address */
            addr = (addr + 15) & ~15u;
        }

        /* Write record placeholder (we'll patch byte_count & checksum after) */
        long rec_pos = ftell(f);
        IRW_RECORD rec = {};
        rec.start_addr = addr;
        rec.byte_count = 0;
        rec.checksum = 0;
        rec.interleave = 4;
        rec.skipbytes = 0;
        rec.bank = 0;
        fwrite(&rec, sizeof(rec), 1, f);

        /* Write pixel data packed at bpp */
        uint32_t byte_count = 0;
        uint32_t checksum = 0;
        unsigned int dw = 0;
        int bf = 0;
        unsigned short stride = (img->w + 3) & ~3;
        unsigned char *src = (unsigned char *)img->data_p;

        for (int y = 0; y < (int)img->h; y++) {
            for (int x = 0; x < (int)img->w; x++) {
                irw_write_bits(f, src[y * stride + x], bpp,
                               &dw, &bf, &byte_count, &checksum);
            }
        }
        irw_flush_word(f, &dw, &bf, &byte_count, &checksum);

        /* Compute bit size for address advance */
        unsigned int bit_size = (unsigned int)img->w * (unsigned int)img->h * (unsigned int)bpp;
        addr += bit_size;

        /* Pad to 16-bit boundary */
        if (align_16bit && (bit_size & 0xF)) {
            unsigned int pad_bits = 16 - (bit_size & 0xF);
            addr += pad_bits;
        }

        /* Patch record */
        long end_pos = ftell(f);
        fseek(f, rec_pos, SEEK_SET);
        rec.byte_count = byte_count;
        rec.checksum   = checksum;
        fwrite(&rec, sizeof(rec), 1, f);
        fseek(f, end_pos, SEEK_SET);
    }

    fclose(f);
}

/* ---- Build TGA (Export Marked Images) ---- */
void BuildTgaFromMarked(const char* filepath)
{
    std::vector<IMG*> marked_imgs;
    IMG* p = (IMG*)img_p;
    int pal_num = -1;
    while (p) {
        if ((p->flags & 1) && p->w <= 256 && p->h > 0 && p->data_p) {
            marked_imgs.push_back(p);
            if (pal_num == -1) pal_num = p->palnum;
        }
        p = (IMG*)p->nxt_p;
    }
    if (marked_imgs.empty()) return;

    std::sort(marked_imgs.begin(), marked_imgs.end(), [](IMG* a, IMG* b) { return a->h > b->h; });

    const int MAX_LINES = 6000;
    std::vector<int> free_width(MAX_LINES, 256);
    std::vector<uint8_t> pixels(MAX_LINES * 256, 0);
    struct PackedImg { IMG* img; int x, y; };
    std::vector<PackedImg> packed;
    int max_y = 0;

    for (IMG* img : marked_imgs) {
        int w = img->w, h = img->h, best_y = -1, best_free_w = 0;
        for (int y = 0; y <= MAX_LINES - h; y++) {
            int min_free = 256;
            for (int dy = 0; dy < h; dy++) if (free_width[y + dy] < min_free) min_free = free_width[y + dy];
            if (min_free >= w) {
                bool ok = true;
                for (int dy = 0; dy < h; dy++) {
                    if (min_free < 254 && free_width[y + dy] >= min_free + 10) { ok = false; break; }
                }
                if (ok) { best_y = y; best_free_w = min_free; break; }
            }
        }
        if (best_y != -1) {
            int x = 256 - best_free_w;
            for (int dy = 0; dy < h; dy++) free_width[best_y + dy] -= w;
            packed.push_back({img, x, best_y});
            if (best_y + h > max_y) max_y = best_y + h;

            int stride = (w + 3) & ~3;
            unsigned char* src = (unsigned char*)img->data_p;
            for (int py = 0; py < h; py++) memcpy(&pixels[(best_y + py) * 256 + x], src + py * stride, w);
        }
    }
    if (max_y == 0) return;
    PAL* pal = get_pal(pal_num);
    if (!pal) return;

    FILE* f = fopen(filepath, "wb");
    if (f) {
        TGA_HEADER tga = {0};
        tga.cm_type = 1; tga.i_type = 1; tga.cm_length = pal->numc;
        tga.cm_size = 15; tga.width = 256; tga.height = max_y; tga.bpp = 8;
        fwrite(&tga, 1, sizeof(tga), f);
        fwrite(pal->data_p, 2, pal->numc, f);
        for (int y = max_y - 1; y >= 0; y--) fwrite(&pixels[y * 256], 1, 256, f);
        fclose(f);
    }

    std::string anf_path = filepath;
    size_t dot = anf_path.find_last_of('.');
    if (dot != std::string::npos) anf_path = anf_path.substr(0, dot);
    anf_path += ".ANF";
    f = fopen(anf_path.c_str(), "wb");
    if (f) {
        fwrite("ANF ", 1, 4, f);
            uint32_t fcnt = (uint32_t)packed.size();
        fwrite(&fcnt, 1, 4, f);
        for (auto& pk : packed) {
            uint32_t zero = 0; fwrite(&zero, 1, 4, f);
            AFACE face = {0};
            face.CTRL = (pk.img->aniy << 16) | pk.img->anix;
            face.O2 = 1 * 3; face.O3 = 2 * 3; face.O4 = 3 * 3; face.LINE = pk.y;
            face.AYX = (uint16_t)(-pk.x);
            face.BYX = (uint16_t)(uint8_t)(pk.img->w - 1 - pk.x);
            face.CYX = (uint16_t)(((pk.img->h - 1) << 8) | (uint8_t)(pk.img->w - 1 - pk.x));
            face.DYX = (uint16_t)(((pk.img->h - 1) << 8) | (uint8_t)(-pk.x));
            fwrite(&face, 1, sizeof(face), f);
        }
        fclose(f);
    }
}

/* ---- Save TGA ---- */
void SaveTga(const char *filepath)
{
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;

    PAL *pal = get_pal(0);
    if (!pal || !pal->data_p) pal = get_pal(img->palnum);
    if (!pal || !pal->data_p) return;

    int w = img->w, h = img->h;
    unsigned short stride = (unsigned short)((w + 3) & ~3);
    unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4);
    if (!rgba) return;
    const unsigned char *pal_data = (const unsigned char *)pal->data_p;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char ci = ((const unsigned char *)img->data_p)[y * stride + x];
            int off = (y * w + x) * 4;
            if (ci == 0) { rgba[off+0]=0; rgba[off+1]=0; rgba[off+2]=0; rgba[off+3]=0; }
            else {
                unsigned short pw = (unsigned short)(pal_data[ci*2] | (pal_data[ci*2+1] << 8));
                rgba[off+0] = (unsigned char)(((pw >> 10) & 0x1F) << 3);
                rgba[off+1] = (unsigned char)(((pw >>  5) & 0x1F) << 3);
                rgba[off+2] = (unsigned char)(( pw        & 0x1F) << 3);
                rgba[off+3] = 255;
            }
        }
    }
    stbi_write_tga(filepath, w, h, 4, rgba);
    free(rgba);
}

/* ---- Save LBM ---- */
void SaveLbm(const char *filepath)
{
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;

    PAL *pal = get_pal(img->palnum);
    if (!pal || !pal->data_p) return;

    FILE *f = fopen(filepath, "wb");
    if (!f) return;

    auto wbe32 = [&](unsigned int v) {
        unsigned char b[4] = { (unsigned char)(v>>24), (unsigned char)(v>>16),
                               (unsigned char)(v>>8),  (unsigned char)v };
        fwrite(b, 1, 4, f);
    };
    auto wbe16 = [&](unsigned short v) {
        unsigned char b[2] = { (unsigned char)(v>>8), (unsigned char)v };
        fwrite(b, 1, 2, f);
    };

    unsigned short row_bytes = (unsigned short)((img->w + 1) & ~1);
    unsigned int body_len = (unsigned int)row_bytes * img->h;

    fwrite("FORM", 1, 4, f);
    wbe32(0);
    fwrite("PBM ", 1, 4, f);

    fwrite("BMHD", 1, 4, f);
    wbe32(20);
    wbe16(img->w); wbe16(img->h);
    wbe16(0); wbe16(0);
    fputc(8, f); fputc(0, f); fputc(0, f); fputc(0, f);
    wbe16(0);
    fputc(5, f); fputc(6, f);
    wbe16(img->w); wbe16(img->h);

    fwrite("CMAP", 1, 4, f);
    wbe32(256 * 3);
    const unsigned char *pal_data = (const unsigned char *)pal->data_p;
    for (int i = 0; i < 256; i++) {
        if (i < pal->numc) {
            unsigned short w = (unsigned short)(pal_data[i*2] | (pal_data[i*2+1] << 8));
            fputc((w >> 7) & 0xF8, f);
            fputc((w >> 2) & 0xF8, f);
            fputc((w << 3) & 0xF8, f);
        } else {
            fputc(0, f); fputc(0, f); fputc(0, f);
        }
    }

    fwrite("BODY", 1, 4, f);
    wbe32(body_len);
    const unsigned char *src = (const unsigned char *)img->data_p;
    unsigned short stride = (img->w + 3) & ~3;
    for (int y = 0; y < img->h; y++) {
        fwrite(src, 1, row_bytes, f);
        src += stride;
    }

    long end_pos = ftell(f);
    fseek(f, 4, SEEK_SET);
    wbe32((unsigned int)(end_pos - 8));
    fclose(f);
}

/* ---- Load TGA ---- */
void LoadTga(const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    IMG *img = NULL;
    unsigned short stride = 0;
    unsigned int   pix_sz = 0;
    unsigned short num_colors = 0;
    unsigned char *pal_buf = NULL;
    PAL *pal = NULL;
    unsigned char *dst = NULL;

    TGA_HEADER hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) goto err;

    if (hdr.id_len > 0) fseek(f, hdr.id_len, SEEK_CUR);

    if (hdr.i_type != 1 || hdr.cm_type != 1 || hdr.bpp != 8) goto err;
    if (hdr.cm_size != 15 && hdr.cm_size != 16 && hdr.cm_size != 24) goto err;

    img = (IMG *)AllocImg();
    if (!img) goto err;

    img->w = hdr.width;
    img->h = hdr.height;
    if (img->w == 0 || img->h == 0) goto err;

    stride = (img->w + 3) & ~3;
    pix_sz = (unsigned int)stride * img->h;
    img->data_p = (unsigned char *)PoolAlloc(pix_sz);
    if (!img->data_p) goto err;

    img->palnum = (unsigned short)palcnt;
    img->flags  = 0;
    img->anix   = 0; img->aniy  = 0;
    img->anix2  = 0; img->aniy2 = 0; img->aniz2 = 0;
    img->pttbl_p = NULL;
    img->opals  = (unsigned short)-1;

    {
        std::string name = fnametmp_s;
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        strncpy(img->n_s, name.c_str(), 15);
        img->n_s[15] = '\0';
    }

    num_colors = hdr.cm_length;
    if (num_colors == 0) num_colors = 256;

    pal_buf = (unsigned char *)PoolAlloc((unsigned int)num_colors * 2);
    if (!pal_buf) goto err;

    for (int i = 0; i < num_colors; i++) {
        int r, g, b;
        if (hdr.cm_size == 24) {
            unsigned char rgb[3];
            if (fread(rgb, 1, 3, f) != 3) { free(pal_buf); goto err; }
            b = rgb[0]; g = rgb[1]; r = rgb[2];
        } else {
            unsigned char w2[2];
            if (fread(w2, 1, 2, f) != 2) { free(pal_buf); goto err; }
            unsigned short w = (unsigned short)(w2[0] | (w2[1] << 8));
            if (hdr.cm_size == 15) {
                r = (w >> 7) & 0xF8;
                g = (w >> 2) & 0xF8;
                b = (w << 3) & 0xF8;
            } else {
                r = (w >> 8) & 0xF8;
                g = (w >> 3) & 0xF8;
                b = (w << 3) & 0xF8;
            }
        }
        unsigned short r5 = (unsigned short)(r >> 3);
        unsigned short g5 = (unsigned short)(g >> 3);
        unsigned short b5 = (unsigned short)(b >> 3);
        unsigned short p15 = (unsigned short)((r5 << 10) | (g5 << 5) | b5);
        pal_buf[i * 2]     = (unsigned char)(p15 & 0xFF);
        pal_buf[i * 2 + 1] = (unsigned char)(p15 >> 8);
    }

    pal = (PAL *)AllocPal();
    if (!pal) { free(pal_buf); goto err; }

    pal->flags   = 0;
    pal->bitspix = 8;
    pal->numc    = num_colors;
    pal->data_p  = pal_buf;
    pal->pad     = 0;
    {
        std::string name = fnametmp_s;
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        name += "P";
        strncpy(pal->n_s, name.c_str(), 9);
        pal->n_s[9] = '\0';
    }

    dst = (unsigned char *)img->data_p;
    if (hdr.desc & 0x20) {
        for (int y = 0; y < img->h; y++) {
            if (fread(dst, 1, img->w, f) != (size_t)img->w) goto err;
            dst += stride;
        }
    } else {
        dst += (unsigned int)stride * (img->h - 1);
        for (int y = 0; y < img->h; y++) {
            if (fread(dst, 1, img->w, f) != (size_t)img->w) goto err;
            dst -= stride;
        }
    }

    fclose(f);

    if (imgcnt > 0) ilselected = (int)imgcnt - 1;
    return;

err:
    if (f) fclose(f);
}

/* ---- Load LBM ---- */
void LoadLbm(const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    auto rbe32 = [&](unsigned int *out) -> bool {
        unsigned char b[4];
        if (fread(b, 1, 4, f) != 4) return false;
        *out = ((unsigned int)b[0]<<24)|((unsigned int)b[1]<<16)|((unsigned int)b[2]<<8)|(unsigned int)b[3];
        return true;
    };
    auto rbe16 = [&](unsigned short *out) -> bool {
        unsigned char b[2];
        if (fread(b, 1, 2, f) != 2) return false;
        *out = (unsigned short)((b[0]<<8)|b[1]);
        return true;
    };
    auto try_close = [&] { if (f) { fclose(f); f = NULL; } };

    enum { TAG_FORM=0x4D524F46, TAG_ILBM=0x4D424C49, TAG_PBM =0x204D4250,
           TAG_BMHD=0x44484D42, TAG_CMAP=0x50414D43, TAG_BODY=0x59444F42,
           TAG_ANIM=0x4D494E41 };

    unsigned int tag, form_len;
    if (!rbe32(&tag) || tag != TAG_FORM) { try_close(); return; }
    if (!rbe32(&form_len)) { try_close(); return; }
    if (!rbe32(&tag) || (tag != TAG_PBM && tag != TAG_ILBM && tag != TAG_ANIM)) { try_close(); return; }

    int have_bmhd = 0;
    unsigned short bm_w = 0, bm_h = 0;
    unsigned char  bm_comp = 0;
    PAL *loaded_pal = NULL;
    IMG *loaded_img = NULL;

    for (;;) {
        long pos = ftell(f);
        if (pos & 1) fseek(f, 1, SEEK_CUR);

        unsigned int chunk_tag = 0, chunk_len = 0;
        if (fread(&chunk_tag, 1, 4, f) != 4) break;
        if (!rbe32(&chunk_len)) break;

        if (chunk_tag == TAG_FORM) { fseek(f, 4, SEEK_CUR); continue; }

        if (chunk_tag == TAG_BMHD) {
            unsigned short w, h, xo, yo, tcol, pagew, pageh;
            unsigned char  nplanes, masking, comp, pad1, xasp, yasp;
            if (!rbe16(&w)||!rbe16(&h)||!rbe16(&xo)||!rbe16(&yo)) { try_close(); return; }
            nplanes=(unsigned char)fgetc(f); masking=(unsigned char)fgetc(f);
            comp   =(unsigned char)fgetc(f); pad1   =(unsigned char)fgetc(f);
            if (!rbe16(&tcol)) { try_close(); return; }
            xasp=(unsigned char)fgetc(f); yasp=(unsigned char)fgetc(f);
            if (!rbe16(&pagew)||!rbe16(&pageh)) { try_close(); return; }
            bm_w = w; bm_h = h; bm_comp = comp;
            have_bmhd |= 1;
            continue;
        }

        if (chunk_tag == TAG_CMAP) {
            unsigned int num_colors = chunk_len / 3;
            if (num_colors > 256) { try_close(); return; }

            loaded_pal = (PAL *)AllocPal();
            if (!loaded_pal) { try_close(); return; }
            loaded_pal->flags = 0; loaded_pal->bitspix = 8;
            loaded_pal->numc = (unsigned short)num_colors; loaded_pal->pad = 0;

            unsigned char *pal_buf = (unsigned char *)PoolAlloc(num_colors * 2);
            if (!pal_buf) { try_close(); return; }
            loaded_pal->data_p = pal_buf;

            for (unsigned int i = 0; i < num_colors; i++) {
                int r = fgetc(f), g = fgetc(f), b = fgetc(f);
                if (r<0||g<0||b<0) { try_close(); return; }
                unsigned short r5=(unsigned short)(r>>3), g5=(unsigned short)(g>>3), b5=(unsigned short)(b>>3);
                unsigned short w15 = (unsigned short)((r5<<10)|(g5<<5)|b5);
                pal_buf[i*2]=(unsigned char)(w15&0xFF); pal_buf[i*2+1]=(unsigned char)(w15>>8);
            }
            { std::string n=fnametmp_s; size_t d=n.find_last_of('.'); if(d!=std::string::npos)n=n.substr(0,d); n+="P";
              strncpy(loaded_pal->n_s,n.c_str(),9); loaded_pal->n_s[9]='\0'; }
            have_bmhd |= 2;
            continue;
        }

        if (chunk_tag == TAG_BODY) {
            if (have_bmhd != 3) { try_close(); return; }

            loaded_img = (IMG *)AllocImg();
            if (!loaded_img) { try_close(); return; }
            loaded_img->w=bm_w; loaded_img->h=bm_h;
            if (!bm_w||!bm_h) { try_close(); return; }

            unsigned short stride=(bm_w+3)&~3;
            loaded_img->data_p=PoolAlloc((unsigned)stride*bm_h);
            if (!loaded_img->data_p) { try_close(); return; }

            loaded_img->palnum=(unsigned short)(palcnt-1); loaded_img->flags=0;
            loaded_img->anix=0; loaded_img->aniy=0; loaded_img->anix2=0; loaded_img->aniy2=0; loaded_img->aniz2=0;
            loaded_img->pttbl_p=NULL; loaded_img->opals=(unsigned short)-1;
            { std::string n=fnametmp_s; size_t d=n.find_last_of('.'); if(d!=std::string::npos)n=n.substr(0,d);
              strncpy(loaded_img->n_s,n.c_str(),15); loaded_img->n_s[15]='\0'; }

            unsigned short even_w=(unsigned short)((bm_w+1)&~1);
            if (bm_comp != 0) {
                unsigned char *dst=(unsigned char*)loaded_img->data_p;
                for (int y=0; y<bm_h; y++) {
                    int remaining=even_w;
                    while (remaining>0) {
                        int b1=fgetc(f); if(b1<0){try_close();return;}
                        signed char sc=(signed char)(unsigned char)b1;
                        if (sc>=0) {
                            int n=sc+1; if(fread(dst,1,(size_t)n,f)!=(size_t)n){try_close();return;}
                            dst+=n; remaining-=n;
                        } else {
                            int rb=fgetc(f); if(rb<0){try_close();return;}
                            int n=(-sc)+1; memset(dst,rb,(size_t)n); dst+=n; remaining-=n;
                        }
                    }
                    for (int x=bm_w; x<stride; x++) dst[x]=0;
                    dst+=stride;
                }
            } else {
                unsigned char *dst=(unsigned char*)loaded_img->data_p;
                for (int y=0; y<bm_h; y++) {
                    if (fread(dst,1,even_w,f)!=even_w) { try_close(); return; }
                    for (int x=bm_w; x<stride; x++) dst[x]=0;
                    dst+=stride;
                }
            }
            break;
        }

        fseek(f, (long)chunk_len, SEEK_CUR);
    }

    try_close();
    if (loaded_img && imgcnt>0) ilselected=(int)imgcnt-1;
    verbose_log("  -> loaded, total images=%u palettes=%u", imgcnt, palcnt);
}

/* ---- PNG Import ---- */

void ImportPng(const char *path)
{
    verbose_log("ImportPng: %s", path);
    int w, h, channels;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);
    if (!data || w == 0 || h == 0) return;

    struct ColorCount { unsigned int rgb; int count; unsigned char idx; };
    ColorCount hist[4096] = {};
    int hist_count = 0;

    for (int i = 0; i < w * h; i++) {
        unsigned char r = data[i * 4], g = data[i * 4 + 1], b = data[i * 4 + 2], a = data[i * 4 + 3];
        if (a < 128) continue;
        unsigned int rgb = ((unsigned int)(r & 0xF8) << 7) | ((unsigned int)(g & 0xF8) << 2) | ((unsigned int)(b >> 3));
        int found = -1;
        for (int j = 0; j < hist_count; j++) { if (hist[j].rgb == rgb) { found = j; break; } }
        if (found >= 0) hist[found].count++;
        else if (hist_count < 4096) { hist[hist_count].rgb = rgb; hist[hist_count].count = 1; hist_count++; }
    }
    for (int i = 0; i < hist_count - 1; i++)
        for (int j = i + 1; j < hist_count; j++)
            if (hist[j].count > hist[i].count) { ColorCount t = hist[i]; hist[i] = hist[j]; hist[j] = t; }

    int pal_colors = hist_count < 255 ? hist_count : 255;
    PAL *pal = AllocPal();
    if (!pal) { stbi_image_free(data); return; }
    pal->flags = 0; pal->bitspix = 8; pal->numc = (unsigned short)(pal_colors + 1);
    pal->data_p = PoolAlloc((unsigned int)(pal_colors + 1) * 2);
    if (!pal->data_p) { stbi_image_free(data); return; }
    ((unsigned char *)pal->data_p)[0] = 0; ((unsigned char *)pal->data_p)[1] = 0;
    for (int i = 0; i < pal_colors; i++) {
        unsigned int rgb = hist[i].rgb;
        unsigned short w15 = (unsigned short)((((rgb >> 7) & 0x1F) << 10) | (((rgb >> 2) & 0x1F) << 5) | (rgb & 0x1F));
        ((unsigned char *)pal->data_p)[(i + 1) * 2] = (unsigned char)(w15 & 0xFF);
        ((unsigned char *)pal->data_p)[(i + 1) * 2 + 1] = (unsigned char)(w15 >> 8);
        hist[i].idx = (unsigned char)(i + 1);
    }

    IMG *img = AllocImg();
    if (!img) { stbi_image_free(data); return; }
    img->w = (unsigned short)w; img->h = (unsigned short)h;
    img->palnum = (unsigned short)(palcnt - 1); img->flags = 0;
    img->anix = 0; img->aniy = 0; img->anix2 = 0; img->aniy2 = 0; img->aniz2 = 0;
    img->pttbl_p = NULL; img->opals = (unsigned short)-1;
    unsigned short stride = (unsigned short)((w + 3) & ~3);
    img->data_p = PoolAlloc((unsigned int)stride * h);
    if (!img->data_p) { stbi_image_free(data); return; }
    memset(img->data_p, 0, (unsigned int)stride * h);

    const char *name = strrchr(path, '/'); if (!name) name = strrchr(path, '\\'); if (!name) name = path; else name++;
    strncpy(img->n_s, name, 15); img->n_s[15] = '\0';
    char *dot = strrchr(img->n_s, '.'); if (dot) *dot = '\0';
    strncpy(pal->n_s, img->n_s, 8); pal->n_s[8] = 'P'; pal->n_s[9] = '\0';

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char *p = data + (y * w + x) * 4;
            if (p[3] < 128) continue;
            unsigned int rgb = ((unsigned int)(p[0] & 0xF8) << 7) | ((unsigned int)(p[1] & 0xF8) << 2) | ((unsigned int)(p[2] >> 3));
            for (int j = 0; j < pal_colors; j++) {
                if (hist[j].rgb == rgb) { ((unsigned char *)img->data_p)[y * stride + x] = hist[j].idx; break; }
            }
        }
    }
    stbi_image_free(data);
    if (imgcnt > 0) ilselected = (int)imgcnt - 1;
    g_img_tex_idx = -2;
    verbose_log("  -> %dx%d px, %d palette colors", w, h, pal_colors);
}

void ImportPngMatch(const char *path)
{
    verbose_log("ImportPngMatch: %s", path);
    IMG *active_img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!active_img) { verbose_log("  -> no active image to copy palette from"); return; }
    PAL *pal = get_pal(active_img->palnum);
    if (!pal || !pal->data_p) return;

    int w, h, channels;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);
    if (!data || w == 0 || h == 0) return;

    /* Build a fast lookup cache for palette matching */
    const unsigned char *pal_data = (const unsigned char *)pal->data_p;
    struct PalColor { int r, g, b; };
    PalColor pcolors[256] = {};
    for (int i = 1; i < pal->numc && i < 256; i++) {
        unsigned short pw = (unsigned short)(pal_data[i*2] | (pal_data[i*2+1] << 8));
        pcolors[i].r = ((pw >> 10) & 0x1F) << 3;
        pcolors[i].g = ((pw >> 5) & 0x1F) << 3;
        pcolors[i].b = (pw & 0x1F) << 3;
    }

    IMG *img = AllocImg();
    if (!img) { stbi_image_free(data); return; }
    img->w = (unsigned short)w; img->h = (unsigned short)h;
    img->palnum = active_img->palnum; img->flags = 0;
    img->anix = 0; img->aniy = 0; img->anix2 = 0; img->aniy2 = 0; img->aniz2 = 0;
    img->pttbl_p = NULL; img->opals = (unsigned short)-1;
    unsigned short stride = (unsigned short)((w + 3) & ~3);
    img->data_p = PoolAlloc((unsigned int)stride * h);
    if (!img->data_p) { stbi_image_free(data); return; }
    memset(img->data_p, 0, (unsigned int)stride * h);

    const char *name = strrchr(path, '/'); if (!name) name = strrchr(path, '\\'); if (!name) name = path; else name++;
    strncpy(img->n_s, name, 15); img->n_s[15] = '\0';
    char *dot = strrchr(img->n_s, '.'); if (dot) *dot = '\0';

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char *p = data + (y * w + x) * 4;
            if (p[3] < 128) continue;
            int r = p[0], g = p[1], b = p[2];
            int best_idx = 1;
            int best_dist = 999999999;
            for (int j = 1; j < pal->numc && j < 256; j++) {
                int dr = r - pcolors[j].r;
                int dg = g - pcolors[j].g;
                int db = b - pcolors[j].b;
                int dist = dr*dr + dg*dg + db*db;
                if (dist < best_dist) {
                    best_dist = dist;
                    best_idx = j;
                }
            }
            ((unsigned char *)img->data_p)[y * stride + x] = (unsigned char)best_idx;
        }
    }
    stbi_image_free(data);
    if (imgcnt > 0) ilselected = (int)imgcnt - 1;
    g_img_tex_idx = -2;
    verbose_log("  -> %dx%d px, matched to palette %u", w, h, pal->numc);
}

/* ---- PNG Export ---- */

void ExportPng(const char *path)
{
    verbose_log("ExportPng: %s", path);
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;
    PAL *pal = get_pal(0);
    if (!pal || !pal->data_p) pal = get_pal(img->palnum);
    if (!pal || !pal->data_p) return;
    int w = img->w, h = img->h;
    unsigned short stride = (unsigned short)((w + 3) & ~3);
    unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4);
    if (!rgba) return;
    const unsigned char *pal_data = (const unsigned char *)pal->data_p;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char ci = ((const unsigned char *)img->data_p)[y * stride + x];
            int off = (y * w + x) * 4;
            if (ci == 0) { rgba[off+0]=0; rgba[off+1]=0; rgba[off+2]=0; rgba[off+3]=0; }
            else {
                unsigned short pw = (unsigned short)(pal_data[ci*2] | (pal_data[ci*2+1] << 8));
                rgba[off+0] = (unsigned char)(((pw >> 10) & 0x1F) << 3);
                rgba[off+1] = (unsigned char)(((pw >>  5) & 0x1F) << 3);
                rgba[off+2] = (unsigned char)(( pw        & 0x1F) << 3);
                rgba[off+3] = 255;
            }
        }
    }
    stbi_write_png(path, w, h, 4, rgba, w * 4);
    free(rgba);
}
