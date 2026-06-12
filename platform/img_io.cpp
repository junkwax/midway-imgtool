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
#include <utility>
#include <climits>
#include <cmath>
#include <cctype>
#include <string>
#include <regex>
#include <stdarg.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

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
 *   [hdr.oset ..)           IMAGE_disk records (g_doc->imgcnt of them)
 *   [.. ..)                 PALETTE_disk records (g_doc->palcnt - NUMDEFPAL of them)
 *   [.. ..)                 SEQSCR/ENTRY blob (g_doc->seqcnt sequences + g_doc->scrcnt scripts)
 *   [.. EOF)                PTTBL records (40 bytes each, indexed by IMAGE.pttblnum)
 */
static void build_full_path(char *dst, int dstsz)
{
    size_t plen = strlen(g_doc->fpath_s);
    if (plen > 0 && g_doc->fpath_s[plen - 1] != '\\' && g_doc->fpath_s[plen - 1] != '/')
        _snprintf(dst, dstsz, "%s\\%s", g_doc->fpath_s, g_doc->fname_s);
    else
        _snprintf(dst, dstsz, "%s%s", g_doc->fpath_s, g_doc->fname_s);
}

void LoadImgFile(void)
{
    char full[sizeof(g_doc->fpath_s) + sizeof(g_doc->fname_s) + 2];
    build_full_path(full, sizeof(full));
    verbose_log("LoadImgFile: %s", full);
    FILE *f = fopen(full, "rb");
    if (!f) { verbose_log("  -> fopen failed"); return; }

    LIB_HDR hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return; }

    if (hdr.temp != 0xABCD || hdr.version < 0x500) { fclose(f); return; }
    g_doc->fileversion = hdr.version;

    if (hdr.imgcnt == 0 || hdr.imgcnt > 2000) { fclose(f); return; }

    verbose_log("  version=0x%04X  g_doc->imgcnt=%u  g_doc->palcnt=%u  g_doc->seqcnt=%d  g_doc->scrcnt=%d",
        hdr.version, hdr.imgcnt, hdr.palcnt,
        (signed short)hdr.seqcnt, (signed short)hdr.scrcnt);

    /* Drop any prior seq/scr blob — we rebuild it from this file's contents. */
    if (g_doc->scrseqmem_p) { free(g_doc->scrseqmem_p); g_doc->scrseqmem_p = NULL; }
    g_doc->scrseqbytes = 0;

    /* Capture LIB_HDR fields that have to round-trip verbatim. The original
     * DOS imgtool clobbered these on save (bufscr=-1, spare*=0), but real
     * game-asset files have meaningful values that LOAD2 consumes. */
    memcpy(g_doc->file_bufscr, hdr.bufscr, 4);
    g_doc->file_spare1 = hdr.spare1;
    g_doc->file_spare2 = hdr.spare2;
    g_doc->file_spare3 = hdr.spare3;

    g_doc->ilpalloaded = -1;
    g_doc->damcnt = 0;

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

        g_doc->scrseqbytes = total_bytes;
        if (g_doc->scrseqbytes > 0) {
            g_doc->scrseqmem_p = malloc(g_doc->scrseqbytes);
            if (g_doc->scrseqmem_p) {
                fseek(f, seqscr_blob_start, SEEK_SET);
                if (fread(g_doc->scrseqmem_p, 1, g_doc->scrseqbytes, f) != g_doc->scrseqbytes) {
                    free(g_doc->scrseqmem_p); g_doc->scrseqmem_p = NULL; g_doc->scrseqbytes = 0;
                }
            } else {
                g_doc->scrseqbytes = 0;
            }
        }
    }
    /* Point tables follow the seq/scr blob. */
    long ptoset = seqscr_blob_start + (long)g_doc->scrseqbytes;

    g_doc->seqcnt = (unsigned int)(signed short)hdr.seqcnt;
    g_doc->scrcnt = (unsigned int)(signed short)hdr.scrcnt;

    int pal_base = (int)g_doc->palcnt;
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

        strncpy(img->src_filename, g_doc->fname_s, 15);
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
        if (img->baseline_p && img->data_p) {
            memcpy(img->baseline_p, img->data_p, pix_sz);
            img->baseline_w = img->w;
            img->baseline_h = img->h;
        }
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

    if (g_doc->imgcnt > 0) g_doc->ilselected = 0;
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
 *   7. Write the SEQSCR+ENTRY blob verbatim (g_doc->scrseqmem_p, g_doc->scrseqbytes).
 *   8. Write all per-image PTTBL records (40 bytes each) for any image
 *      that has one.
 *   9. Rewind, rewrite the finalized LIB_HDR.
 */
/* PPP> setting from MK2MIL.LOD; other LODs differ. 6 covers MK2's
 * fighter sprites (≤64 colors per palette). Verifier compares
 * palette numc against (1<<ppp); set to 0 to disable the check. */
int g_load2_ppp = 6;
bool g_load2_limit_scales_to_3 = false;

/* ---- Per-sprite overlay layer save hooks ----
   Flatten every visible layer onto its image just for the file write, then
   restore the pre-flatten pixels so the layers stay editable in the session.
   The SpriteLayer struct + composite_layer_onto live in img_format.h, shared
   with the editor. */
namespace {
struct LayerSaveStash { IMG *img; unsigned char *pixels; size_t bytes; };
std::vector<LayerSaveStash> g_layer_save_stash;
}

void FlattenLayersForSave(void)
{
    g_layer_save_stash.clear();
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        SpriteLayer *L = img_layer(img);
        if (!L || !L->visible || !img->data_p || img->w == 0 || img->h == 0) continue;
        int stride = (img->w + 3) & ~3;
        size_t bytes = (size_t)stride * img->h;
        unsigned char *saved = (unsigned char *)malloc(bytes);
        if (!saved) continue;
        memcpy(saved, img->data_p, bytes);
        g_layer_save_stash.push_back({img, saved, bytes});
        composite_layer_onto(L, (unsigned char *)img->data_p, img->w, img->h, stride);
    }
}

void RestoreLayersAfterSave(void)
{
    for (auto &s : g_layer_save_stash) {
        if (s.img && s.img->data_p && s.pixels)
            memcpy(s.img->data_p, s.pixels, s.bytes);
        free(s.pixels);
    }
    g_layer_save_stash.clear();
    g_img_tex_idx = -2;
}

/* Lightweight probe: read just an IMG's frame names from its header records,
   without loading pixel data. Used by the ASM viewer to auto-pick the matching
   IMG for a character ASM. */
void ProbeImgFrameNames(const char *path, std::vector<std::string> &out)
{
    out.clear();
    if (!path || !path[0]) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    LIB_HDR hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return; }
    if (hdr.temp != 0xABCD || hdr.imgcnt == 0 || hdr.imgcnt > 4000) { fclose(f); return; }
    for (int i = 0; i < (int)hdr.imgcnt; i++) {
        long pos = (long)hdr.oset + (long)i * (long)sizeof(IMAGE_disk);
        if (fseek(f, pos, SEEK_SET) != 0) break;
        IMAGE_disk idisk;
        if (fread(&idisk, 1, sizeof(idisk), f) != sizeof(idisk)) break;
        char name[17]; memcpy(name, idisk.n_s, 16); name[16] = '\0';
        out.push_back(name);
    }
    fclose(f);
}

void SaveImgFile(void)
{
    /* Pre-save advisory: pop a toast if edits will misalign SAGs
     * after LOAD2 processes the saved IMG. Save proceeds either way. */
    VerifyLoad2BeforeSave(g_load2_ppp, g_load2_limit_scales_to_3);

    char full[sizeof(g_doc->fpath_s) + sizeof(g_doc->fname_s) + 2];
    build_full_path(full, sizeof(full));
    FILE *f = fopen(full, "wb");
    if (!f) return;

    /* Bake any non-destructive overlay layers into the image pixels for the
       write; restored at the end so the layers remain editable in-session. */
    FlattenLayersForSave();

    int num_imgs = (int)g_doc->imgcnt;
    int num_pals = (int)g_doc->palcnt;

    /* ---- 1. Header placeholder ---- */
    LIB_HDR hdr = {};
    hdr.imgcnt  = (unsigned short)num_imgs;
    hdr.palcnt  = (unsigned short)(num_pals + NUMDEFPAL);
    hdr.version = (g_doc->fileversion != 0) ? (unsigned short)g_doc->fileversion : 0x0634;
    hdr.temp    = 0xABCD;
    hdr.oset    = 0;  /* finalized in step 4 */
    hdr.seqcnt  = (unsigned short)g_doc->seqcnt;
    hdr.scrcnt  = (unsigned short)g_doc->scrcnt;
    hdr.damcnt  = 0;  /* original asm always zeros this on save */
    /* Preserve bufscr + spare1/2/3 from load (real game files use them;
     * LOAD2 consumes bufscr to compute IRW layout). Default is all 0xFF /
     * zero (matches a freshly-created file). */
    memcpy(hdr.bufscr, g_doc->file_bufscr, 4);
    hdr.spare1 = g_doc->file_spare1;
    hdr.spare2 = g_doc->file_spare2;
    hdr.spare3 = g_doc->file_spare3;
    fwrite(&hdr, 1, sizeof(hdr), f);

    /* ---- 2. Palette pixel data ---- */
    PAL *pal = (PAL *)g_doc->pal_p;
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
    IMG *img = (IMG *)g_doc->img_p;
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
    img = (IMG *)g_doc->img_p;
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
    pal = (PAL *)g_doc->pal_p;
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
    if (g_doc->scrseqmem_p && g_doc->scrseqbytes > 0) {
        fwrite(g_doc->scrseqmem_p, 1, g_doc->scrseqbytes, f);
    }

    /* ---- 8. Per-image PTTBL records (40 bytes each) ---- */
    img = (IMG *)g_doc->img_p;
    for (int i = 0; i < num_imgs && img; i++, img = (IMG *)img->nxt_p) {
        if (img->pttbl_p) {
            fwrite(img->pttbl_p, 1, 40, f);
        }
    }

    /* ---- 9. Rewrite finalized LIB_HDR ---- */
    rewind(f);
    fwrite(&hdr, 1, sizeof(hdr), f);

    fclose(f);
    g_doc->fileversion = hdr.version;

    /* Restore base pixels — the overlay layers live on past the save. */
    RestoreLayersAfterSave();
}

/* ---- Restore Marked Images from Source ---- */
int RestoreMarkedFromSource(void)
{
    IMG *src = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!src || !src->data_p || src->w == 0 || src->h == 0) return 0;

    bool any_target = false;
    for (IMG *t = (IMG *)g_doc->img_p; t; t = (IMG *)t->nxt_p) {
        if ((t->flags & 1) && t != src) { any_target = true; break; }
    }
    if (!any_target) return 0;

    undo_push();

    const unsigned char *src_pix = (const unsigned char *)src->data_p;
    int src_stride = (src->w + 3) & ~3;
    int total_written = 0;

    for (IMG *t = (IMG *)g_doc->img_p; t; t = (IMG *)t->nxt_p) {
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
        for (IMG *t2 = (IMG *)g_doc->img_p; t2; t2 = (IMG *)t2->nxt_p, idx++) {
            if ((t2->flags & 1) && t2 != src) { g_doc->ilselected = idx; break; }
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
    IMG *src = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!src || !src->data_p || src->w == 0 || src->h == 0) return 0;

    bool any_target = false;
    for (IMG *t = (IMG *)g_doc->img_p; t; t = (IMG *)t->nxt_p) {
        if ((t->flags & 1) && t != src) { any_target = true; break; }
    }
    if (!any_target) return 0;

    undo_push();

    const unsigned char *src_pix = (const unsigned char *)src->data_p;
    int src_stride = (src->w + 3) & ~3;
    int total_written = 0;
    int imgs_processed = 0;

    for (IMG *t = (IMG *)g_doc->img_p; t; t = (IMG *)t->nxt_p) {
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
        for (IMG *t2 = (IMG *)g_doc->img_p; t2; t2 = (IMG *)t2->nxt_p, idx++) {
            if ((t2->flags & 1) && t2 != src) { g_doc->ilselected = idx; break; }
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
    if (!g_doc->img_p || matches.empty()) return 0;

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
    if (!g_doc->img_p || matches.empty()) return 0;

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
    if (!g_doc->img_p || matches.empty()) return 0;

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
                 * child (where the publisher replaced logo pixels with a flat
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

struct SourceSpriteColor {
    unsigned char idx;
    unsigned char r, g, b;
};

static int match_all_sprites_to_source_colors_impl(int source_idx,
                                                   int *pixels_changed_out,
                                                   bool apply)
{
    if (pixels_changed_out) *pixels_changed_out = 0;
    IMG *source = get_img(source_idx);
    if (!source || !source->data_p || source->w == 0 || source->h == 0)
        return 0;
    PAL *source_pal = get_pal(source->palnum);
    if (!source_pal || !source_pal->data_p || source_pal->numc <= 1)
        return 0;

    bool used[256] = {};
    const unsigned char *src_pix = (const unsigned char *)source->data_p;
    int src_stride = (source->w + 3) & ~3;
    int source_color_limit = source_pal->numc < 256 ? source_pal->numc : 256;
    for (int y = 0; y < source->h; y++) {
        for (int x = 0; x < source->w; x++) {
            unsigned char ci = src_pix[y * src_stride + x];
            if (ci != 0 && ci < source_color_limit)
                used[ci] = true;
        }
    }

    std::vector<SourceSpriteColor> source_colors;
    source_colors.reserve(256);
    const unsigned char *src_pal_data = (const unsigned char *)source_pal->data_p;
    for (int i = 1; i < source_color_limit; i++) {
        if (!used[i]) continue;
        SourceSpriteColor c;
        c.idx = (unsigned char)i;
        pal_word_to_rgb8(src_pal_data + i * 2, &c.r, &c.g, &c.b);
        source_colors.push_back(c);
    }
    if (source_colors.empty())
        return 0;

    int changed_images = 0;
    int pixels_changed = 0;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (idx == source_idx) continue;
        if (!img->data_p || img->w == 0 || img->h == 0) continue;
        PAL *target_pal = get_pal(img->palnum);
        if (!target_pal || !target_pal->data_p || target_pal->numc <= 1) continue;

        unsigned char remap[256] = {};
        bool remap_valid[256] = {};
        const unsigned char *target_pal_data = (const unsigned char *)target_pal->data_p;
        int target_color_limit = target_pal->numc < 256 ? target_pal->numc : 256;
        for (int ci = 1; ci < target_color_limit; ci++) {
            unsigned char r, g, b;
            pal_word_to_rgb8(target_pal_data + ci * 2, &r, &g, &b);

            int best_dist = INT_MAX;
            unsigned char best_idx = source_colors[0].idx;
            for (const SourceSpriteColor &src : source_colors) {
                int dr = (int)r - (int)src.r;
                int dg = (int)g - (int)src.g;
                int db = (int)b - (int)src.b;
                int dist = dr * dr + dg * dg + db * db;
                if (dist < best_dist) {
                    best_dist = dist;
                    best_idx = src.idx;
                }
            }
            remap[ci] = best_idx;
            remap_valid[ci] = true;
        }

        unsigned char *dst_pix = (unsigned char *)img->data_p;
        int stride = (img->w + 3) & ~3;
        int writes_this = 0;
        for (int y = 0; y < img->h; y++) {
            for (int x = 0; x < img->w; x++) {
                unsigned char old_idx = dst_pix[y * stride + x];
                if (old_idx == 0 || !remap_valid[old_idx]) continue;
                unsigned char new_idx = remap[old_idx];
                if (dst_pix[y * stride + x] != new_idx) {
                    if (apply)
                        dst_pix[y * stride + x] = new_idx;
                    writes_this++;
                }
            }
        }

        bool changed_this = writes_this > 0;
        if (img->palnum != source->palnum) {
            if (apply)
                img->palnum = source->palnum;
            changed_this = true;
        }
        if (writes_this > 0)
            pixels_changed += writes_this;
        if (changed_this)
            changed_images++;
    }

    if (apply && changed_images > 0) {
        g_doc->plselected = source->palnum;
        g_img_tex_idx = -2;
    }
    if (pixels_changed_out) *pixels_changed_out = pixels_changed;
    return changed_images;
}

int PreviewMatchAllSpritesToSourceColors(int source_idx, int *pixels_changed_out)
{
    return match_all_sprites_to_source_colors_impl(source_idx, pixels_changed_out, false);
}

int MatchAllSpritesToSourceColors(int source_idx, int *pixels_changed_out)
{
    return match_all_sprites_to_source_colors_impl(source_idx, pixels_changed_out, true);
}

/* ---- Auto-Sprite Chopper ---- */
static bool img_secondary_anipoint_in_use(const IMG *img)
{
    if (!img) return false;
    if ((short)img->anix2 < 0 || (short)img->aniy2 < 0) return false;
    return (short)img->aniz2 != -1;
}

static void img_clear_secondary_anipoint(IMG *img)
{
    if (!img) return;
    img->anix2 = (unsigned short)-1;
    img->aniy2 = (unsigned short)-1;
    img->aniz2 = (unsigned short)-1;
}

static size_t img_name_len15(const char *s)
{
    size_t n = 0;
    while (n < 15 && s && s[n] != '\0') n++;
    return n;
}

static std::string shorten_parent_name_for_suffix(const char *name,
                                                  size_t suffix_len)
{
    if (suffix_len >= 15) suffix_len = 14;
    std::string base(name ? name : "", img_name_len15(name));
    if (base.empty()) base = "SPRITE";

    while (base.size() + suffix_len > 15 && !base.empty()) {
        size_t digit_start = base.size();
        while (digit_start > 0 &&
               base[digit_start - 1] >= '0' &&
               base[digit_start - 1] <= '9') {
            digit_start--;
        }

        size_t remove_pos = std::string::npos;
        if (digit_start < base.size() && digit_start > 0) {
            size_t before_digits = digit_start - 1;
            if ((base[before_digits] >= 'A' && base[before_digits] <= 'Z') ||
                (base[before_digits] >= 'a' && base[before_digits] <= 'z')) {
                remove_pos = before_digits;
            }
        }

        if (remove_pos == std::string::npos)
            remove_pos = base.size() - 1;
        base.erase(remove_pos, 1);
    }

    if (base.empty()) base = "SPRITE";
    if (base.size() > 15 - suffix_len)
        base.resize(15 - suffix_len);
    return base;
}

int ChopMarkedImages(int grid_w, int grid_h, bool trim)
{
    if (grid_w <= 0 || grid_h <= 0) return 0;
    int count = 0;

    std::vector<IMG*> targets;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        if (p->flags & 1) targets.push_back(p);
    }

    if (targets.empty() && g_doc->ilselected >= 0) {
        IMG *selected = get_img(g_doc->ilselected);
        if (selected) targets.push_back(selected);
    }
    if (targets.empty()) return 0;

    undo_push();

    for (IMG *master : targets) {
        if (!master->data_p || master->w == 0 || master->h == 0) continue;

        std::string child_base = shorten_parent_name_for_suffix(master->n_s, 1);
        if (child_base != std::string(master->n_s, img_name_len15(master->n_s))) {
            strncpy(master->n_s, child_base.c_str(), 15);
            master->n_s[15] = '\0';
        }

        int rows = (master->h + grid_h - 1) / grid_h;
        int cols = (master->w + grid_w - 1) / grid_w;

        int src_stride = (master->w + 3) & ~3;
        unsigned char *src_pix = (unsigned char *)master->data_p;
        int piece_no = 0;

        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int cell_x = c * grid_w;
                int cell_y = r * grid_h;
                int cell_w = grid_w;
                int cell_h = grid_h;
                if (cell_x + cell_w > master->w) cell_w = master->w - cell_x;
                if (cell_y + cell_h > master->h) cell_h = master->h - cell_y;

                int min_x = cell_w, max_x = -1;
                int min_y = cell_h, max_y = -1;

                for (int y = 0; y < cell_h; y++) {
                    for (int x = 0; x < cell_w; x++) {
                        if (src_pix[(cell_y + y) * src_stride + (cell_x + x)] != 0) {
                            if (x < min_x) min_x = x;
                            if (x > max_x) max_x = x;
                            if (y < min_y) min_y = y;
                            if (y > max_y) max_y = y;
                        }
                    }
                }

                if (max_x < min_x) continue; /* Empty */

                if (!trim) {
                    min_x = 0; min_y = 0;
                    max_x = cell_w - 1; max_y = cell_h - 1;
                }

                int new_w = max_x - min_x + 1;
                int new_h = max_y - min_y + 1;

                IMG *new_img = AllocImg();
                if (!new_img) continue;

                new_img->w = new_w;
                new_img->h = new_h;
                int new_stride = (new_w + 3) & ~3;
                new_img->data_p = (unsigned char *)PoolAlloc((size_t)new_stride * new_h);
                if (!new_img->data_p) continue;
                memset(new_img->data_p, 0, (size_t)new_stride * new_h);

                unsigned char *dst_pix = (unsigned char *)new_img->data_p;
                for (int y = 0; y < new_h; y++) {
                    for (int x = 0; x < new_w; x++) {
                        dst_pix[y * new_stride + x] = src_pix[(cell_y + min_y + y) * src_stride + (cell_x + min_x + x)];
                    }
                }

                new_img->anix = master->anix - (short)(cell_x + min_x);
                new_img->aniy = master->aniy - (short)(cell_y + min_y);
                new_img->palnum = master->palnum;
                new_img->flags = 0; /* Unmarked */
                new_img->opals = master->opals;

                /* Shipping Midway sprite pieces are commonly named BASE1A,
                   BASE1B, BASE1C rather than carrying hierarchy metadata in
                   the IMG itself. Use direct A..Z suffixes for generated
                   pieces and fall back to _NN only for unusually large chops. */
                char suffix[8];
                if (piece_no < 26) {
                    snprintf(suffix, sizeof(suffix), "%c", 'A' + piece_no);
                } else {
                    snprintf(suffix, sizeof(suffix), "_%02d", piece_no + 1);
                }
                piece_no++;
                size_t suf_len = strlen(suffix);
                std::string base_name = child_base;
                if (base_name.length() + suf_len > 15)
                    base_name = shorten_parent_name_for_suffix(child_base.c_str(), suf_len);
                base_name += suffix;
                strncpy(new_img->n_s, base_name.c_str(), 15);
                new_img->n_s[15] = '\0';
                
                /* src_filename is [16]; copy at most 15 chars and NUL-terminate.
                   Previously this used 63 (a leftover constant from an older
                   field layout) which clobbered ~48 bytes past the buffer. */
                strncpy(new_img->src_filename, master->src_filename,
                        sizeof(new_img->src_filename) - 1);
                new_img->src_filename[sizeof(new_img->src_filename) - 1] = '\0';

                count++;
            }
        }
        master->flags &= ~1; /* Unmark master so chopped pieces are easier to handle */
    }

    if (count > 0) {
        g_img_tex_idx = -2;
    }
    return count;
}

/* ---- Defringe Edges (one-shot) ----
 * Performs a single-pass 8-neighborhood average on every opaque pixel that
 * sits next to a transparent one. Run repeatedly (or with radius > 1) to
 * eat further into the blue spill. */
int DefringeMarkedImages(int radius)
{
    if (radius < 1) radius = 1;
    if (radius > 4) radius = 4;
    int total_edited = 0;

    std::vector<IMG*> targets;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        if (p->flags & 1) targets.push_back(p);
    }
    if (targets.empty()) return 0;
    undo_push();

    for (IMG *img : targets) {
        if (!img->data_p || img->w == 0 || img->h == 0) continue;
        int w = img->w, h = img->h;
        int stride = (w + 3) & ~3;

        for (int pass = 0; pass < radius; pass++) {
            unsigned char *pix = (unsigned char *)img->data_p;
            std::vector<std::pair<int,unsigned char>> writes;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    unsigned char self = pix[y * stride + x];
                    if (self == 0) continue;
                    bool touches_zero = false;
                    int sum = 0, n = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (!dx && !dy) continue;
                            int nx = x + dx, ny = y + dy;
                            if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                            unsigned char nv = pix[ny * stride + nx];
                            if (nv == 0) touches_zero = true;
                            else { sum += nv; n++; }
                        }
                    }
                    if (touches_zero && n > 0) {
                        unsigned char avg = (unsigned char)(sum / n);
                        if (avg != self) {
                            writes.push_back({y * stride + x, avg});
                        }
                    }
                }
            }
            for (auto &w_ : writes) pix[w_.first] = w_.second;
            total_edited += (int)writes.size();
        }
    }
    if (total_edited > 0) g_img_tex_idx = -2;
    return total_edited;
}

static int CropOneImageToContent(IMG *img, bool apply)
{
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return 0;

    int w = img->w, h = img->h;
    int stride = (w + 3) & ~3;
    unsigned char *src = (unsigned char *)img->data_p;

    int min_x = w, max_x = -1, min_y = h, max_y = -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (src[y * stride + x] != 0) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }
    if (max_x < 0) return 0; /* fully transparent — leave alone */
    if (min_x == 0 && min_y == 0 && max_x == w - 1 && max_y == h - 1) return 0;

    int new_w = max_x - min_x + 1;
    int new_h = max_y - min_y + 1;
    if (!apply) return 1;

    int new_stride = (new_w + 3) & ~3;
    unsigned char *dst = (unsigned char *)PoolAlloc((size_t)new_stride * new_h);
    if (!dst) return 0;

    memset(dst, 0, (size_t)new_stride * new_h);
    for (int y = 0; y < new_h; y++) {
        memcpy(dst + y * new_stride,
               src + (y + min_y) * stride + min_x,
               new_w);
    }

    free(img->data_p);
    img->data_p = dst;
    img->w = (unsigned short)new_w;
    img->h = (unsigned short)new_h;
    img->anix = (unsigned short)((short)img->anix - (short)min_x);
    img->aniy = (unsigned short)((short)img->aniy - (short)min_y);
    if (img_secondary_anipoint_in_use(img)) {
        img->anix2 = (unsigned short)((short)img->anix2 - (short)min_x);
        img->aniy2 = (unsigned short)((short)img->aniy2 - (short)min_y);
    }
    return 1;
}

/* ---- Crop to Content (single-image smart trim) ---- */
int CropSelectedImageToContent(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img) return 0;

    if (!CropOneImageToContent(img, false)) return 0;

    /* Undo is taken by the caller (doc_undo_push) — crop changes w/h + data_p,
       which the metadata-only undo_push cannot restore. */
    int count = CropOneImageToContent(img, true);
    if (count > 0) g_img_tex_idx = -2;
    return count;
}

int CropMarkedImagesToContent(void)
{
    int count = 0;
    std::vector<IMG*> targets;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        if (p->flags & 1) targets.push_back(p);
    }
    if (targets.empty()) return 0;

    bool any_crop = false;
    for (IMG *img : targets) {
        if (CropOneImageToContent(img, false)) { any_crop = true; break; }
    }
    if (!any_crop) return 0;

    /* Undo is taken by the caller (doc_undo_push). */
    for (IMG *img : targets) {
        count += CropOneImageToContent(img, true);
    }
    if (count > 0) g_img_tex_idx = -2;
    return count;
}

/* ---- Align Anipoints across marked frames ----
 * The reference image's anipoint is treated as the canonical anchor; for
 * every other marked image, both the anix and aniy are set to match the
 * reference. (Anipoints in IMG are local to each frame, so visually this
 * shifts where the engine pegs each frame relative to the world anchor —
 * the user's job afterward is to confirm the anipoint sits on the same
 * body part across frames.) */
int AlignAnipointsToMarked(int reference_idx)
{
    if (reference_idx < 0 || (unsigned int)reference_idx >= g_doc->imgcnt) return 0;
    IMG *ref = NULL;
    {
        IMG *p = (IMG *)g_doc->img_p;
        for (int i = 0; p && i < reference_idx; i++) p = (IMG *)p->nxt_p;
        ref = p;
    }
    if (!ref) return 0;

    undo_push();
    int count = 0;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        if (!(p->flags & 1)) continue;
        if (p == ref) continue;
        p->anix = ref->anix;
        p->aniy = ref->aniy;
        count++;
    }
    return count;
}

static unsigned short mirror_anipoint_x(unsigned short x, unsigned short w)
{
    return (unsigned short)(short)((int)w - (int)(short)x);
}

/* ---- Mirror marked anipoints for reverse-facing sprites ----
 * This commits the same X-anchor transform used by World View's view-only
 * mirror: if a normal sprite anchors at X, the reverse-facing/mirrored
 * version anchors at width - X. Secondary X is mirrored only when the
 * secondary point appears to be in use, matching the local extra-data
 * convention used by crop/paste. */
int MirrorMarkedAnipointsToReverse(void)
{
    if (!g_doc || !g_doc->img_p) return 0;

    bool any_change = false;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        if (!(p->flags & 1)) continue;

        unsigned short mx1 = mirror_anipoint_x(p->anix, p->w);
        bool has_second = img_secondary_anipoint_in_use(p);
        unsigned short mx2 = has_second ? mirror_anipoint_x(p->anix2, p->w) : p->anix2;
        if (p->anix != mx1 || p->anix2 != mx2) {
            any_change = true;
            break;
        }
    }
    if (!any_change) return 0;

    undo_push();
    int count = 0;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        if (!(p->flags & 1)) continue;

        unsigned short mx1 = mirror_anipoint_x(p->anix, p->w);
        bool has_second = img_secondary_anipoint_in_use(p);
        unsigned short mx2 = has_second ? mirror_anipoint_x(p->anix2, p->w) : p->anix2;
        if (p->anix == mx1 && p->anix2 == mx2) continue;

        p->anix = mx1;
        p->anix2 = mx2;
        count++;
    }
    return count;
}

/* ---- Write ANILST (Export Marked Images to Assembly) ---- */
void WriteAnilstFromMarked(const char* filepath)
{
    FILE* f = fopen(filepath, "w");
    if (!f) return;

    fprintf(f, "\t.asg\t1,N\n");

    IMG* p = (IMG*)g_doc->img_p;
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

    IMG* p = (IMG*)g_doc->img_p;
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
    IMG *p = (IMG*)g_doc->img_p;
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
    IMG* p = (IMG*)g_doc->img_p;
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
            /* Every spanned row's free space must measure from the glyph's
             * right edge (x + w). Subtracting w instead left rows that had
             * more free space than the binding row claiming pixels under the
             * glyph as free, so later glyphs were blitted over earlier ones
             * (see MK2FONT1.TGA corruption). */
            for (int dy = 0; dy < h; dy++) free_width[best_y + dy] = best_free_w - w;
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
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
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
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
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

    img->palnum = (unsigned short)g_doc->palcnt;
    img->flags  = 0;
    img->anix   = 0; img->aniy  = 0;
    img_clear_secondary_anipoint(img);
    img->pttbl_p = NULL;
    img->opals  = (unsigned short)-1;

    {
        std::string name = g_doc->fnametmp_s;
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
        std::string name = g_doc->fnametmp_s;
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

    if (g_doc->imgcnt > 0) g_doc->ilselected = (int)g_doc->imgcnt - 1;
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
            { std::string n=g_doc->fnametmp_s; size_t d=n.find_last_of('.'); if(d!=std::string::npos)n=n.substr(0,d); n+="P";
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

            loaded_img->palnum=(unsigned short)(g_doc->palcnt-1); loaded_img->flags=0;
            loaded_img->anix=0; loaded_img->aniy=0; img_clear_secondary_anipoint(loaded_img);
            loaded_img->pttbl_p=NULL; loaded_img->opals=(unsigned short)-1;
            { std::string n=g_doc->fnametmp_s; size_t d=n.find_last_of('.'); if(d!=std::string::npos)n=n.substr(0,d);
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
    if (loaded_img && g_doc->imgcnt>0) g_doc->ilselected=(int)g_doc->imgcnt-1;
    verbose_log("  -> loaded, total images=%u palettes=%u", g_doc->imgcnt, g_doc->palcnt);
}

/* ---- PNG Import ---- */

/* Import a PNG into a new image + new palette.

   The arcade IMG format stores 1 byte per pixel into a palette of up to 256
   entries (index 0 reserved for transparent), with each palette entry being a
   15-bit packed RGB word. So the source PNG's full-color pixels must be
   quantized down to <=255 representative colors, and *every* PNG pixel must
   end up mapped to one of them.

   The previous implementation here was broken in two ways:
     - It capped the unique-color histogram at 4096 entries, silently dropping
       any beyond-budget colors (which then mis-mapped to index 0 at draw time).
     - It selected the palette by raw frequency (top-N most common 15-bit
       colors), and mapped pixels by exact 15-bit equality only — so any pixel
       whose color wasn't in the top 255 fell through the match loop and was
       written as index 0 (transparent).

   This implementation uses median-cut quantization in 15-bit RGB space to
   pick a representative 255-color palette weighted by pixel count, then maps
   every opaque pixel to the nearest palette entry by Euclidean RGB distance.
   No dithering — flat sprite art is the dominant use case and dithering
   produces speckle that looks wrong against arcade backgrounds. */

namespace {

struct ColorBucket {
    /* Compact list of (15-bit rgb, pixel-count) pairs that make up this bucket. */
    std::vector<std::pair<unsigned short, int>> entries;
    int    pixel_count = 0;       /* sum of entries[i].second */
    int    r_lo = 31, r_hi = 0;
    int    g_lo = 31, g_hi = 0;
    int    b_lo = 31, b_hi = 0;
};

static inline void unpack15(unsigned short c, int &r5, int &g5, int &b5)
{
    r5 = (c >> 10) & 0x1F;
    g5 = (c >>  5) & 0x1F;
    b5 =  c        & 0x1F;
}

static void recompute_bucket_bounds(ColorBucket &b)
{
    b.r_lo = b.g_lo = b.b_lo = 31;
    b.r_hi = b.g_hi = b.b_hi = 0;
    b.pixel_count = 0;
    for (auto &e : b.entries) {
        int r, g, bl;
        unpack15(e.first, r, g, bl);
        if (r < b.r_lo) b.r_lo = r; if (r > b.r_hi) b.r_hi = r;
        if (g < b.g_lo) b.g_lo = g; if (g > b.g_hi) b.g_hi = g;
        if (bl < b.b_lo) b.b_lo = bl; if (bl > b.b_hi) b.b_hi = bl;
        b.pixel_count += e.second;
    }
}

struct PalRGB5 { unsigned char r, g, b; };

static void import_base_name(const char *path, char *out, size_t outsz)
{
    if (!out || outsz == 0) return;
    const char *name = path ? path : "";
    const char *slash = strrchr(name, '/');
    const char *back  = strrchr(name, '\\');
    if (slash && back) name = (slash > back) ? slash + 1 : back + 1;
    else if (slash)    name = slash + 1;
    else if (back)     name = back + 1;

    size_t n = 0;
    while (name[n] && name[n] != '.' && n < outsz - 1) {
        out[n] = name[n];
        n++;
    }
    if (n == 0) {
        strncpy(out, "IMPORT", outsz - 1);
        out[outsz - 1] = '\0';
    } else {
        out[n] = '\0';
    }
}

static void make_frame_name(const char *base, int frame_idx, int frame_count, char out[16])
{
    if (frame_count <= 1) {
        strncpy(out, base && base[0] ? base : "GIF", 15);
        out[15] = '\0';
        return;
    }

    char suffix[8];
    int digits = (frame_count >= 1000) ? 4 : 3;
    snprintf(suffix, sizeof(suffix), "%0*d", digits, frame_idx + 1);
    size_t suffix_len = strlen(suffix);
    int base_len = 15 - (int)suffix_len;
    if (base_len < 1) base_len = 1;
    snprintf(out, 16, "%.*s%s", base_len, base && base[0] ? base : "GIF", suffix);
}

static int build_quantized_palette_from_rgba(const unsigned char *rgba, int w, int h,
                                             int frame_count, PalRGB5 pcolors[256],
                                             int *unique_colors_out)
{
    if (unique_colors_out) *unique_colors_out = 0;
    if (!rgba || w <= 0 || h <= 0 || frame_count <= 0) return 0;

    static int hist[32768];
    memset(hist, 0, sizeof(hist));

    const size_t pixels_per_frame = (size_t)w * (size_t)h;
    for (int f = 0; f < frame_count; f++) {
        const unsigned char *frame = rgba + (size_t)f * pixels_per_frame * 4;
        for (size_t i = 0; i < pixels_per_frame; i++) {
            const unsigned char *p = frame + i * 4;
            if (p[3] < 128) continue;
            int r5 = p[0] >> 3;
            int g5 = p[1] >> 3;
            int b5 = p[2] >> 3;
            unsigned short c = (unsigned short)((r5 << 10) | (g5 << 5) | b5);
            hist[c]++;
        }
    }

    std::vector<ColorBucket> buckets;
    buckets.reserve(256);
    buckets.emplace_back();
    int unique_colors = 0;
    {
        ColorBucket &b0 = buckets.back();
        for (int c = 0; c < 32768; c++) {
            if (hist[c] > 0) {
                b0.entries.push_back({ (unsigned short)c, hist[c] });
                unique_colors++;
            }
        }
        if (unique_colors == 0) {
            if (unique_colors_out) *unique_colors_out = 0;
            return 0;
        }
        recompute_bucket_bounds(b0);
    }

    const int MAX_PAL_COLORS = 255;
    while ((int)buckets.size() < MAX_PAL_COLORS) {
        int best = -1;
        int best_range = 0;
        int best_axis  = 0;
        for (int i = 0; i < (int)buckets.size(); i++) {
            ColorBucket &b = buckets[i];
            if (b.entries.size() < 2) continue;
            int rr = b.r_hi - b.r_lo;
            int gr = b.g_hi - b.g_lo;
            int br = b.b_hi - b.b_lo;
            int axis = 0, range = rr;
            if (gr > range) { range = gr; axis = 1; }
            if (br > range) { range = br; axis = 2; }
            if (range > best_range) {
                best_range = range;
                best       = i;
                best_axis  = axis;
            }
        }
        if (best < 0) break;

        ColorBucket &src = buckets[best];
        std::sort(src.entries.begin(), src.entries.end(),
                  [best_axis](const std::pair<unsigned short, int> &a,
                              const std::pair<unsigned short, int> &b) {
                      int ar, ag, ab; unpack15(a.first, ar, ag, ab);
                      int br, bg, bb; unpack15(b.first, br, bg, bb);
                      int av = (best_axis == 0) ? ar : (best_axis == 1) ? ag : ab;
                      int bv = (best_axis == 0) ? br : (best_axis == 1) ? bg : bb;
                      return av < bv;
                  });

        int half = src.pixel_count / 2;
        int acc  = 0;
        size_t split = 0;
        for (; split < src.entries.size() - 1; split++) {
            acc += src.entries[split].second;
            if (acc >= half) { split++; break; }
        }
        if (split == 0) split = 1;
        if (split >= src.entries.size()) split = src.entries.size() - 1;

        ColorBucket right;
        right.entries.assign(src.entries.begin() + split, src.entries.end());
        src.entries.erase(src.entries.begin() + split, src.entries.end());
        recompute_bucket_bounds(src);
        recompute_bucket_bounds(right);
        buckets.push_back(std::move(right));
    }

    int pal_colors = (int)buckets.size();
    memset(pcolors, 0, sizeof(PalRGB5) * 256);
    for (int i = 0; i < pal_colors; i++) {
        long long rs = 0, gs = 0, bs = 0, ws = 0;
        for (auto &e : buckets[i].entries) {
            int r, g, b; unpack15(e.first, r, g, b);
            rs += (long long)r * e.second;
            gs += (long long)g * e.second;
            bs += (long long)b * e.second;
            ws += e.second;
        }
        if (ws == 0) ws = 1;
        pcolors[i + 1].r = (unsigned char)((rs + ws / 2) / ws);
        pcolors[i + 1].g = (unsigned char)((gs + ws / 2) / ws);
        pcolors[i + 1].b = (unsigned char)((bs + ws / 2) / ws);
    }

    if (unique_colors_out) *unique_colors_out = unique_colors;
    return pal_colors;
}

static int import_rgba_frames_as_images(const char *path, const unsigned char *rgba,
                                        int w, int h, int frame_count,
                                        int *palette_colors_out,
                                        int *unique_colors_out)
{
    if (palette_colors_out) *palette_colors_out = 0;
    if (unique_colors_out) *unique_colors_out = 0;
    if (!rgba || w <= 0 || h <= 0 || frame_count <= 0) return 0;

    PalRGB5 pcolors[256];
    int unique_colors = 0;
    int pal_colors = build_quantized_palette_from_rgba(rgba, w, h, frame_count,
                                                       pcolors, &unique_colors);

    PAL *pal = AllocPal();
    if (!pal) return 0;
    pal->flags   = 0;
    pal->bitspix = 8;
    pal->numc    = (unsigned short)(pal_colors + 1);
    pal->data_p  = PoolAlloc((size_t)(pal_colors + 1) * 2);
    if (!pal->data_p) return 0;

    char base[32];
    import_base_name(path, base, sizeof(base));
    snprintf(pal->n_s, sizeof(pal->n_s), "%.8sP", base);

    unsigned char *pal_bytes = (unsigned char *)pal->data_p;
    pal_bytes[0] = 0; pal_bytes[1] = 0;
    for (int i = 1; i <= pal_colors; i++) {
        unsigned short w15 = (unsigned short)(((pcolors[i].r & 0x1F) << 10) |
                                              ((pcolors[i].g & 0x1F) <<  5) |
                                              ( pcolors[i].b & 0x1F));
        pal_bytes[i * 2]     = (unsigned char)(w15 & 0xFF);
        pal_bytes[i * 2 + 1] = (unsigned char)(w15 >> 8);
    }

    unsigned short pal_idx = (unsigned short)(g_doc->palcnt - 1);
    unsigned short stride = (unsigned short)((w + 3) & ~3);
    const size_t pixels_per_frame = (size_t)w * (size_t)h;

    static unsigned char color_to_idx[32768];
    static bool          color_resolved[32768];
    memset(color_resolved, 0, sizeof(color_resolved));

    int imported = 0;
    for (int f = 0; f < frame_count; f++) {
        IMG *img = AllocImg();
        if (!img) break;
        img->w = (unsigned short)w; img->h = (unsigned short)h;
        img->palnum = pal_idx; img->flags = 0;
        img->anix = 0; img->aniy = 0; img_clear_secondary_anipoint(img);
        img->pttbl_p = NULL; img->opals = (unsigned short)-1;
        img->data_p = PoolAlloc((size_t)stride * h);
        if (!img->data_p) break;
        memset(img->data_p, 0, (size_t)stride * h);

        make_frame_name(base, f, frame_count, img->n_s);

        const unsigned char *frame = rgba + (size_t)f * pixels_per_frame * 4;
        unsigned char *out = (unsigned char *)img->data_p;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const unsigned char *p = frame + ((size_t)y * w + x) * 4;
                if (p[3] < 128 || pal_colors == 0) continue;
                int r5 = p[0] >> 3, g5 = p[1] >> 3, b5 = p[2] >> 3;
                unsigned short c = (unsigned short)((r5 << 10) | (g5 << 5) | b5);
                unsigned char idx;
                if (color_resolved[c]) {
                    idx = color_to_idx[c];
                } else {
                    int best_idx = 1, best_dist = INT_MAX;
                    for (int j = 1; j <= pal_colors; j++) {
                        int dr = r5 - pcolors[j].r;
                        int dg = g5 - pcolors[j].g;
                        int db = b5 - pcolors[j].b;
                        int dist = dr*dr + dg*dg + db*db;
                        if (dist < best_dist) { best_dist = dist; best_idx = j; }
                    }
                    idx = (unsigned char)best_idx;
                    color_to_idx[c] = idx;
                    color_resolved[c] = true;
                }
                out[y * stride + x] = idx;
            }
        }
        imported++;
    }

    g_doc->plselected = (int)pal_idx;
    if (g_doc->imgcnt > 0) g_doc->ilselected = (int)g_doc->imgcnt - 1;
    g_img_tex_idx = -2;

    if (palette_colors_out) *palette_colors_out = pal_colors;
    if (unique_colors_out) *unique_colors_out = unique_colors;
    return imported;
}

struct SheetFrameCandidate {
    int x0, y0, x1, y1; /* inclusive */
    int pixels;
    int islands;
    int row;
    int frame;
};

struct SheetIslandCandidate {
    int x0, y0, x1, y1; /* inclusive */
    int pixels;
    int colorful_pixels;
    int dark_gray_pixels;
    int assigned;
};

struct SheetDetectTrace {
    int raw_islands;
    int line_rows;
    int line_cols;
    std::vector<unsigned char> mask;
    std::vector<SheetIslandCandidate> islands;
};

struct SheetPalColor {
    int r, g, b;
};

struct SheetPaletteMatch {
    unsigned short palnum;
    int count;
    SheetPalColor colors[256];
    unsigned char color_to_idx[32768];
    bool color_resolved[32768];
};

static int sheet_clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int sheet_box_gap(int ax0, int ay0, int ax1, int ay1,
                         int bx0, int by0, int bx1, int by1)
{
    int dx = 0;
    if (ax1 < bx0) dx = bx0 - ax1 - 1;
    else if (bx1 < ax0) dx = ax0 - bx1 - 1;

    int dy = 0;
    if (ay1 < by0) dy = by0 - ay1 - 1;
    else if (by1 < ay0) dy = ay0 - by1 - 1;

    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    return dx > dy ? dx : dy;
}

static void sheet_expand_frame(SheetFrameCandidate *frame,
                               const SheetIslandCandidate &island)
{
    if (!frame) return;
    if (island.x0 < frame->x0) frame->x0 = island.x0;
    if (island.y0 < frame->y0) frame->y0 = island.y0;
    if (island.x1 > frame->x1) frame->x1 = island.x1;
    if (island.y1 > frame->y1) frame->y1 = island.y1;
    frame->pixels += island.pixels;
    frame->islands++;
}

static bool sheet_is_skinny_box(int w, int h)
{
    return (w > h * 8 && h < 32) || (h > w * 8 && w < 32);
}

static bool sheet_is_core_island(const SheetIslandCandidate &island, int min_pixels)
{
    int bw = island.x1 - island.x0 + 1;
    int bh = island.y1 - island.y0 + 1;
    if (island.pixels < min_pixels) return false;
    if (bw < 10 || bh < 14) return false;
    if (sheet_is_skinny_box(bw, bh)) return false;

    /* Plain text labels are mostly dark grayscale. A sprite core should have
       at least a little color, or enough area that it is unlikely to be a
       sheet label. This keeps row labels from turning into imported frames. */
    int colorful_min = island.pixels / 60;
    if (colorful_min < 6) colorful_min = 6;
    if (island.colorful_pixels < colorful_min &&
        island.dark_gray_pixels > (island.pixels * 3) / 4 &&
        island.pixels < min_pixels * 6)
        return false;
    return true;
}

static bool sheet_can_attach_island(const SheetIslandCandidate &island, int gap,
                                    int tight_gap)
{
    if (gap <= tight_gap) return true;
    if (island.colorful_pixels > 0) return true;

    /* Dark grayscale fragments at a loose distance are usually label glyphs,
       not separated sprite parts. Light low-saturation fragments are allowed:
       they often represent hands, hair highlights, or shoes. */
    if (island.dark_gray_pixels > (island.pixels * 3) / 4)
        return false;
    return true;
}

static bool sheet_base_bg_pixel(const unsigned char *p, int threshold)
{
    if (!p || p[3] < 128) return true;
    return p[0] >= threshold && p[1] >= threshold && p[2] >= threshold;
}

static bool sheet_detection_bg_pixel(const unsigned char *p, int threshold)
{
    if (sheet_base_bg_pixel(p, threshold)) return true;
    int mn = (int)p[0];
    if ((int)p[1] < mn) mn = (int)p[1];
    if ((int)p[2] < mn) mn = (int)p[2];
    int mx = (int)p[0];
    if ((int)p[1] > mx) mx = (int)p[1];
    if ((int)p[2] > mx) mx = (int)p[2];
    return mn >= 220 && (mx - mn) <= 10; /* light gray grid paper */
}

static bool sheet_separator_rule_pixel(const unsigned char *p, int threshold)
{
    if (!p || p[3] < 128) return false;
    int mn = (int)p[0];
    if ((int)p[1] < mn) mn = (int)p[1];
    if ((int)p[2] < mn) mn = (int)p[2];
    int mx = (int)p[0];
    if ((int)p[1] > mx) mx = (int)p[1];
    if ((int)p[2] > mx) mx = (int)p[2];
    if (mx - mn > 18) return false;
    if (mx >= threshold) return false; /* ordinary white page background */
    return mn >= 120;                  /* gray grid/page rules */
}

static bool sheet_flood_bg_pixel(const unsigned char *p, int threshold)
{
    if (sheet_base_bg_pixel(p, threshold)) return true;
    int mn = (int)p[0];
    if ((int)p[1] < mn) mn = (int)p[1];
    if ((int)p[2] < mn) mn = (int)p[2];
    int mx = (int)p[0];
    if ((int)p[1] > mx) mx = (int)p[1];
    if ((int)p[2] > mx) mx = (int)p[2];
    return mn >= 205 && (mx - mn) <= 12; /* connected grid/page gray */
}

struct SheetBgSamples {
    unsigned char rgba[4][4];
    int count;
};

static void sheet_add_bg_sample(SheetBgSamples *samples, const unsigned char *p)
{
    if (!samples || !p || samples->count >= 4) return;
    memcpy(samples->rgba[samples->count], p, 4);
    samples->count++;
}

static void sheet_collect_corner_samples(const unsigned char *rgba, int sheet_w,
                                         int x0, int y0, int w, int h,
                                         SheetBgSamples *samples)
{
    if (!samples) return;
    samples->count = 0;
    sheet_add_bg_sample(samples, rgba + ((size_t)y0 * sheet_w + x0) * 4);
    if (w > 1)
        sheet_add_bg_sample(samples, rgba + ((size_t)y0 * sheet_w + (x0 + w - 1)) * 4);
    if (h > 1)
        sheet_add_bg_sample(samples, rgba + ((size_t)(y0 + h - 1) * sheet_w + x0) * 4);
    if (w > 1 && h > 1)
        sheet_add_bg_sample(samples, rgba + ((size_t)(y0 + h - 1) * sheet_w + (x0 + w - 1)) * 4);
}

static bool sheet_close_to_bg_sample(const unsigned char *p,
                                     const SheetBgSamples *samples)
{
    if (!p || p[3] < 128 || !samples) return false;
    for (int i = 0; i < samples->count; i++) {
        const unsigned char *s = samples->rgba[i];
        int dr = abs((int)p[0] - (int)s[0]);
        int dg = abs((int)p[1] - (int)s[1]);
        int db = abs((int)p[2] - (int)s[2]);
        if (dr <= 32 && dg <= 32 && db <= 32)
            return true;
    }
    return false;
}

static bool sheet_sample_bg_pixel(const unsigned char *p, int threshold,
                                  const SheetBgSamples *samples)
{
    return sheet_flood_bg_pixel(p, threshold) ||
           sheet_close_to_bg_sample(p, samples);
}

static void build_sheet_edge_bg_mask(const unsigned char *rgba, int w, int h,
                                     int threshold,
                                     std::vector<unsigned char> &edge_bg)
{
    edge_bg.assign((size_t)w * h, 0);
    if (!rgba || w <= 0 || h <= 0) return;

    SheetBgSamples samples;
    sheet_collect_corner_samples(rgba, w, 0, 0, w, h, &samples);

    std::vector<int> queue;
    queue.reserve((size_t)(w + h) * 2);
    auto try_seed = [&](int x, int y) {
        size_t idx = (size_t)y * w + x;
        if (edge_bg[idx]) return;
        const unsigned char *p = rgba + idx * 4;
        if (!sheet_sample_bg_pixel(p, threshold, &samples)) return;
        edge_bg[idx] = 1;
        queue.push_back((int)idx);
    };

    for (int x = 0; x < w; x++) {
        try_seed(x, 0);
        try_seed(x, h - 1);
    }
    for (int y = 1; y < h - 1; y++) {
        try_seed(0, y);
        try_seed(w - 1, y);
    }

    const int dx[4] = {-1, 1, 0, 0};
    const int dy[4] = {0, 0, -1, 1};
    for (int qpos = 0; qpos < (int)queue.size(); qpos++) {
        int idx = queue[(size_t)qpos];
        int x = idx % w;
        int y = idx / w;
        for (int n = 0; n < 4; n++) {
            int nx = x + dx[n], ny = y + dy[n];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
            size_t ni = (size_t)ny * w + nx;
            if (edge_bg[ni]) continue;
            const unsigned char *p = rgba + ni * 4;
            if (!sheet_sample_bg_pixel(p, threshold, &samples)) continue;
            edge_bg[ni] = 1;
            queue.push_back((int)ni);
        }
    }
}

static void sheet_sanitize_prefix(const char *src, char *dst, size_t dstsz)
{
    if (!dst || dstsz == 0) return;
    size_t n = 0;
    if (src) {
        for (size_t i = 0; src[i] && n < dstsz - 1; i++) {
            unsigned char c = (unsigned char)src[i];
            if (isalnum(c))
                dst[n++] = (char)toupper(c);
        }
    }
    if (n == 0) {
        const char *fallback = "FRAME";
        while (*fallback && n < dstsz - 1) dst[n++] = *fallback++;
    }
    dst[n] = '\0';
}

static void sheet_row_code(int row, char *dst, size_t dstsz)
{
    if (!dst || dstsz == 0) return;
    if (row < 0) row = 0;
    char tmp[8];
    int n = 0;
    do {
        tmp[n++] = (char)('A' + (row % 26));
        row = row / 26 - 1;
    } while (row >= 0 && n < (int)sizeof(tmp));
    size_t out = 0;
    while (n > 0 && out < dstsz - 1)
        dst[out++] = tmp[--n];
    dst[out] = '\0';
}

static void make_sheet_frame_name(const char *prefix, int row, int frame, char out[16])
{
    char clean[12];
    char rowbuf[8];
    char suffix[12];
    sheet_sanitize_prefix(prefix, clean, sizeof(clean));
    sheet_row_code(row, rowbuf, sizeof(rowbuf));
    snprintf(suffix, sizeof(suffix), "%s%d", rowbuf, frame + 1);
    size_t suffix_len = strlen(suffix);
    int prefix_len = 15 - (int)suffix_len;
    if (prefix_len < 1) prefix_len = 1;
    snprintf(out, 16, "%.*s%s", prefix_len, clean, suffix);
}

static bool init_sheet_palette_match(SheetPaletteMatch *ctx, unsigned short palnum)
{
    if (!ctx) return false;
    memset(ctx, 0, sizeof(*ctx));
    PAL *pal = get_pal(palnum);
    if (!pal || !pal->data_p || pal->numc <= 1) return false;
    ctx->palnum = palnum;
    ctx->count = pal->numc < 256 ? pal->numc : 256;
    const unsigned char *pal_data = (const unsigned char *)pal->data_p;
    for (int i = 1; i < ctx->count; i++) {
        unsigned char r, g, b;
        pal_word_to_rgb8(pal_data + i * 2, &r, &g, &b);
        ctx->colors[i].r = r;
        ctx->colors[i].g = g;
        ctx->colors[i].b = b;
    }
    return true;
}

static unsigned char sheet_match_palette_index(SheetPaletteMatch *ctx,
                                               const unsigned char *p)
{
    if (!ctx || !p || ctx->count <= 1) return 0;
    unsigned short c = (unsigned short)(((p[0] >> 3) << 10) |
                                        ((p[1] >> 3) <<  5) |
                                         (p[2] >> 3));
    if (ctx->color_resolved[c])
        return ctx->color_to_idx[c];

    int best_idx = 1;
    int best_dist = INT_MAX;
    for (int i = 1; i < ctx->count; i++) {
        int dr = (int)p[0] - ctx->colors[i].r;
        int dg = (int)p[1] - ctx->colors[i].g;
        int db = (int)p[2] - ctx->colors[i].b;
        int dist = dr * dr + dg * dg + db * db;
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }
    ctx->color_to_idx[c] = (unsigned char)best_idx;
    ctx->color_resolved[c] = true;
    return (unsigned char)best_idx;
}

static void detect_sheet_candidates(const unsigned char *rgba, int w, int h,
                                    const SpriteSheetImportOptions *opts,
                                    std::vector<SheetFrameCandidate> &frames,
                                    SheetDetectTrace *trace = NULL)
{
    frames.clear();
    if (trace) {
        trace->raw_islands = 0;
        trace->line_rows = 0;
        trace->line_cols = 0;
        trace->mask.clear();
        trace->islands.clear();
    }
    if (!rgba || w <= 0 || h <= 0) return;

    int threshold = sheet_clamp_int(opts ? opts->background_threshold : 245, 180, 255);
    int min_pixels = opts ? opts->min_pixels : 160;
    if (min_pixels < 1) min_pixels = 1;

    const size_t total = (size_t)w * (size_t)h;
    std::vector<unsigned char> edge_bg;
    build_sheet_edge_bg_mask(rgba, w, h, threshold, edge_bg);

    std::vector<unsigned char> mask(total, 0);
    for (size_t i = 0; i < total; i++) {
        const unsigned char *p = rgba + i * 4;
        mask[i] = (edge_bg[i] || sheet_detection_bg_pixel(p, threshold)) ? 0 : 1;
    }

    /* Grid sheets often carry one-pixel separator rules. Removing rows/cols
       that are mostly foreground prevents the grid itself becoming one giant
       connected component while leaving sprite silhouettes intact. */
    std::vector<unsigned char> line_rows((size_t)h, 0), line_cols((size_t)w, 0);
    int line_row_count = 0;
    int line_col_count = 0;
    for (int y = 0; y < h; y++) {
        int count = 0;
        int sep_count = 0;
        for (int x = 0; x < w; x++) {
            count += mask[(size_t)y * w + x] ? 1 : 0;
            const unsigned char *p = rgba + ((size_t)y * w + x) * 4;
            sep_count += sheet_separator_rule_pixel(p, threshold) ? 1 : 0;
        }
        if (count > (w * 19) / 20 || sep_count > (w * 9) / 20) {
            line_rows[(size_t)y] = 1;
            line_row_count++;
            if (trace) trace->line_rows++;
        }
    }
    for (int x = 0; x < w; x++) {
        int count = 0;
        int sep_count = 0;
        for (int y = 0; y < h; y++) {
            count += mask[(size_t)y * w + x] ? 1 : 0;
            const unsigned char *p = rgba + ((size_t)y * w + x) * 4;
            sep_count += sheet_separator_rule_pixel(p, threshold) ? 1 : 0;
        }
        if (count > (h * 19) / 20 || sep_count > (h * 9) / 20) {
            line_cols[(size_t)x] = 1;
            line_col_count++;
            if (trace) trace->line_cols++;
        }
    }
    for (int y = 0; y < h; y++) {
        if (!line_rows[(size_t)y]) continue;
        for (int x = 0; x < w; x++) mask[(size_t)y * w + x] = 0;
    }
    for (int x = 0; x < w; x++) {
        if (!line_cols[(size_t)x]) continue;
        for (int y = 0; y < h; y++) mask[(size_t)y * w + x] = 0;
    }
    if (trace) trace->mask = mask;

    std::vector<unsigned char> seen(total, 0);
    std::vector<int> queue;
    queue.reserve(4096);
    const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    const int dy[8] = {-1,-1,-1,  0, 0,  1, 1, 1};
    int island_min = min_pixels / 20;
    if (island_min < 6) island_min = 6;
    if (island_min > 64) island_min = 64;

    std::vector<SheetIslandCandidate> islands;

    for (int sy = 0; sy < h; sy++) {
        for (int sx = 0; sx < w; sx++) {
            size_t start = (size_t)sy * w + sx;
            if (!mask[start] || seen[start]) continue;

            queue.clear();
            queue.push_back((int)start);
            seen[start] = 1;
            int qpos = 0;
            int x0 = sx, y0 = sy, x1 = sx, y1 = sy, pixels = 0;
            int colorful_pixels = 0;
            int dark_gray_pixels = 0;
            while (qpos < (int)queue.size()) {
                int idx = queue[(size_t)qpos++];
                int x = idx % w;
                int y = idx / w;
                pixels++;
                if (x < x0) x0 = x; if (x > x1) x1 = x;
                if (y < y0) y0 = y; if (y > y1) y1 = y;

                const unsigned char *p = rgba + (size_t)idx * 4;
                int mn = (int)p[0];
                if ((int)p[1] < mn) mn = (int)p[1];
                if ((int)p[2] < mn) mn = (int)p[2];
                int mx = (int)p[0];
                if ((int)p[1] > mx) mx = (int)p[1];
                if ((int)p[2] > mx) mx = (int)p[2];
                if (mx - mn >= 18 && mx >= 56) colorful_pixels++;
                if (mx <= 96 && mx - mn <= 18) dark_gray_pixels++;

                for (int n = 0; n < 8; n++) {
                    int nx = x + dx[n], ny = y + dy[n];
                    if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                    size_t ni = (size_t)ny * w + nx;
                    if (mask[ni] && !seen[ni]) {
                        seen[ni] = 1;
                        queue.push_back((int)ni);
                    }
                }
            }

            int bw = x1 - x0 + 1;
            int bh = y1 - y0 + 1;
            if (pixels < island_min) continue;
            if (bw < 2 || bh < 2) continue;

            islands.push_back({x0, y0, x1, y1, pixels,
                               colorful_pixels, dark_gray_pixels, -1});
        }
    }

    if (trace) {
        trace->raw_islands = (int)islands.size();
        trace->islands = islands;
    }
    if (islands.empty()) return;

    auto separated_by_lines = [&](int ax0, int ay0, int ax1, int ay1,
                                  int bx0, int by0, int bx1, int by1) -> bool {
        if (ax1 < bx0) {
            for (int x = ax1 + 1; x < bx0; x++) {
                if (x >= 0 && x < w && line_cols[(size_t)x]) return true;
            }
        } else if (bx1 < ax0) {
            for (int x = bx1 + 1; x < ax0; x++) {
                if (x >= 0 && x < w && line_cols[(size_t)x]) return true;
            }
        }
        if (ay1 < by0) {
            for (int y = ay1 + 1; y < by0; y++) {
                if (y >= 0 && y < h && line_rows[(size_t)y]) return true;
            }
        } else if (by1 < ay0) {
            for (int y = by1 + 1; y < ay0; y++) {
                if (y >= 0 && y < h && line_rows[(size_t)y]) return true;
            }
        }
        return false;
    };
    bool grid_guided = line_row_count >= 4 && line_col_count >= 4;

    for (int i = 0; i < (int)islands.size(); i++) {
        SheetIslandCandidate &island = islands[(size_t)i];
        if (!sheet_is_core_island(island, min_pixels)) continue;
        island.assigned = (int)frames.size();
        frames.push_back({island.x0, island.y0, island.x1, island.y1,
                          island.pixels, 1, 0, 0});
    }

    if (frames.empty()) {
        /* Fallback for tiny sheets where every sprite is below the requested
           core threshold: group islands by proximity, then let the final
           filters decide what is large enough to import. */
        int fallback_gap = sheet_clamp_int((w < h ? w : h) / 80, 4, 12);
        for (int i = 0; i < (int)islands.size(); i++) {
            SheetIslandCandidate &island = islands[(size_t)i];
            if (island.assigned >= 0) continue;
            int best = -1;
            int best_gap = INT_MAX;
            for (int f = 0; f < (int)frames.size(); f++) {
                const SheetFrameCandidate &frame = frames[(size_t)f];
                int gap = sheet_box_gap(island.x0, island.y0, island.x1, island.y1,
                                        frame.x0, frame.y0, frame.x1, frame.y1);
                if (gap <= fallback_gap && gap < best_gap) {
                    if (separated_by_lines(island.x0, island.y0, island.x1, island.y1,
                                           frame.x0, frame.y0, frame.x1, frame.y1))
                        continue;
                    best = f;
                    best_gap = gap;
                }
            }
            if (best >= 0) {
                island.assigned = best;
                sheet_expand_frame(&frames[(size_t)best], island);
            } else {
                island.assigned = (int)frames.size();
                frames.push_back({island.x0, island.y0, island.x1, island.y1,
                                  island.pixels, 1, 0, 0});
            }
        }
    } else {
        if (grid_guided) {
            int core_merge_gap = sheet_clamp_int((w < h ? w : h) / 16, 36, 84);
            bool merged_core = true;
            while (merged_core) {
                merged_core = false;
                for (int a = 0; !merged_core && a < (int)frames.size(); a++) {
                    for (int b = a + 1; b < (int)frames.size(); b++) {
                        SheetFrameCandidate &fa = frames[(size_t)a];
                        SheetFrameCandidate &fb = frames[(size_t)b];
                        int gap = sheet_box_gap(fa.x0, fa.y0, fa.x1, fa.y1,
                                                fb.x0, fb.y0, fb.x1, fb.y1);
                        if (gap > core_merge_gap) continue;
                        if (separated_by_lines(fa.x0, fa.y0, fa.x1, fa.y1,
                                               fb.x0, fb.y0, fb.x1, fb.y1))
                            continue;

                        if (fb.x0 < fa.x0) fa.x0 = fb.x0;
                        if (fb.y0 < fa.y0) fa.y0 = fb.y0;
                        if (fb.x1 > fa.x1) fa.x1 = fb.x1;
                        if (fb.y1 > fa.y1) fa.y1 = fb.y1;
                        fa.pixels += fb.pixels;
                        fa.islands += fb.islands;
                        frames.erase(frames.begin() + b);
                        for (SheetIslandCandidate &island : islands) {
                            if (island.assigned == b) island.assigned = a;
                            else if (island.assigned > b) island.assigned--;
                        }
                        merged_core = true;
                        break;
                    }
                }
            }
        }

        int span = w < h ? w : h;
        int attach_gap = grid_guided
            ? sheet_clamp_int(span / 18, 28, 72)
            : sheet_clamp_int(span / 36, 14, 34);
        int tight_gap = grid_guided
            ? attach_gap
            : sheet_clamp_int(attach_gap / 3, 4, 10);
        bool changed = true;
        for (int pass = 0; changed && pass < 4; pass++) {
            changed = false;
            for (int i = 0; i < (int)islands.size(); i++) {
                SheetIslandCandidate &island = islands[(size_t)i];
                if (island.assigned >= 0) continue;
                int best = -1;
                int best_gap = INT_MAX;
                for (int f = 0; f < (int)frames.size(); f++) {
                    const SheetFrameCandidate &frame = frames[(size_t)f];
                    int gap = sheet_box_gap(island.x0, island.y0, island.x1, island.y1,
                                            frame.x0, frame.y0, frame.x1, frame.y1);
                    if (gap > attach_gap || gap >= best_gap) continue;
                    if (separated_by_lines(island.x0, island.y0, island.x1, island.y1,
                                           frame.x0, frame.y0, frame.x1, frame.y1))
                        continue;
                    if (!sheet_can_attach_island(island, gap, tight_gap)) continue;
                    best = f;
                    best_gap = gap;
                }
                if (best >= 0) {
                    island.assigned = best;
                    sheet_expand_frame(&frames[(size_t)best], island);
                    changed = true;
                }
            }
        }
    }

    std::vector<SheetFrameCandidate> filtered;
    filtered.reserve(frames.size());
    for (const SheetFrameCandidate &frame : frames) {
        int bw = frame.x1 - frame.x0 + 1;
        int bh = frame.y1 - frame.y0 + 1;
        if (frame.pixels < min_pixels) continue;
        if (bw < 10 || bh < 14) continue;
        if (sheet_is_skinny_box(bw, bh)) continue;
        filtered.push_back(frame);
    }
    frames.swap(filtered);
    if (frames.empty()) return;

    std::sort(frames.begin(), frames.end(),
              [](const SheetFrameCandidate &a, const SheetFrameCandidate &b) {
                  int acy = (a.y0 + a.y1) / 2;
                  int bcy = (b.y0 + b.y1) / 2;
                  if (acy != bcy) return acy < bcy;
                  return a.x0 < b.x0;
              });

    struct SheetRow {
        int center_sum;
        int height_sum;
        int count;
        std::vector<int> indices;
    };
    std::vector<SheetRow> rows;
    for (int i = 0; i < (int)frames.size(); i++) {
        int cy = (frames[i].y0 + frames[i].y1) / 2;
        int fh = frames[i].y1 - frames[i].y0 + 1;
        int best = -1;
        int best_dist = INT_MAX;
        for (int r = 0; r < (int)rows.size(); r++) {
            int row_center = rows[r].center_sum / rows[r].count;
            int row_height = rows[r].height_sum / rows[r].count;
            int max_h = fh > row_height ? fh : row_height;
            int tol = max_h * 3 / 5;
            if (tol < 16) tol = 16;
            int dist = abs(cy - row_center);
            if (dist <= tol && dist < best_dist) {
                best = r;
                best_dist = dist;
            }
        }
        if (best < 0) {
            rows.push_back({cy, fh, 1, std::vector<int>()});
            best = (int)rows.size() - 1;
        } else {
            rows[best].center_sum += cy;
            rows[best].height_sum += fh;
            rows[best].count++;
        }
        rows[best].indices.push_back(i);
    }

    std::sort(rows.begin(), rows.end(),
              [](const SheetRow &a, const SheetRow &b) {
                  return (a.center_sum / a.count) < (b.center_sum / b.count);
              });

    std::vector<SheetFrameCandidate> ordered;
    ordered.reserve(frames.size());
    for (int r = 0; r < (int)rows.size(); r++) {
        std::vector<int> &idxs = rows[r].indices;
        std::sort(idxs.begin(), idxs.end(), [&frames](int a, int b) {
            if (frames[a].x0 != frames[b].x0) return frames[a].x0 < frames[b].x0;
            return frames[a].y0 < frames[b].y0;
        });
        for (int f = 0; f < (int)idxs.size(); f++) {
            SheetFrameCandidate c = frames[idxs[(size_t)f]];
            c.row = r;
            c.frame = f;
            ordered.push_back(c);
        }
    }
    frames.swap(ordered);
    if (trace) trace->islands = islands;
}

static bool build_sheet_bg_mask(const unsigned char *rgba, int sheet_w, int sheet_h,
                                int x0, int y0, int w, int h, int threshold,
                                std::vector<unsigned char> &bg)
{
    bg.assign((size_t)w * h, 0);
    if (!rgba || w <= 0 || h <= 0) return false;

    SheetBgSamples samples;
    sheet_collect_corner_samples(rgba, sheet_w, x0, y0, w, h, &samples);

    std::vector<int> queue;
    queue.reserve((size_t)(w + h) * 2);
    auto try_seed = [&](int lx, int ly) {
        size_t bi = (size_t)ly * w + lx;
        if (bg[bi]) return;
        const unsigned char *p = rgba + ((size_t)(y0 + ly) * sheet_w + (x0 + lx)) * 4;
        if (!sheet_sample_bg_pixel(p, threshold, &samples)) return;
        bg[bi] = 1;
        queue.push_back((int)bi);
    };

    for (int x = 0; x < w; x++) {
        try_seed(x, 0);
        try_seed(x, h - 1);
    }
    for (int y = 1; y < h - 1; y++) {
        try_seed(0, y);
        try_seed(w - 1, y);
    }

    const int dx[4] = {-1, 1, 0, 0};
    const int dy[4] = {0, 0, -1, 1};
    for (int qpos = 0; qpos < (int)queue.size(); qpos++) {
        int idx = queue[(size_t)qpos];
        int lx = idx % w;
        int ly = idx / w;
        for (int n = 0; n < 4; n++) {
            int nx = lx + dx[n], ny = ly + dy[n];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
            size_t ni = (size_t)ny * w + nx;
            if (bg[ni]) continue;
            const unsigned char *p = rgba + ((size_t)(y0 + ny) * sheet_w + (x0 + nx)) * 4;
            if (!sheet_sample_bg_pixel(p, threshold, &samples)) continue;
            bg[ni] = 1;
            queue.push_back((int)ni);
        }
    }
    return true;
}

static bool import_sheet_candidate(const unsigned char *rgba, int sheet_w, int sheet_h,
                                   const SheetFrameCandidate &candidate,
                                   SheetPaletteMatch *palctx,
                                   const SpriteSheetImportOptions *opts)
{
    if (!rgba || !palctx) return false;
    int threshold = sheet_clamp_int(opts ? opts->background_threshold : 245, 180, 255);
    int pad = opts ? opts->padding : 2;
    if (pad < 0) pad = 0;

    int x0 = sheet_clamp_int(candidate.x0 - pad, 0, sheet_w - 1);
    int y0 = sheet_clamp_int(candidate.y0 - pad, 0, sheet_h - 1);
    int x1 = sheet_clamp_int(candidate.x1 + pad, 0, sheet_w - 1);
    int y1 = sheet_clamp_int(candidate.y1 + pad, 0, sheet_h - 1);
    int cw = x1 - x0 + 1;
    int ch = y1 - y0 + 1;

    std::vector<unsigned char> bg;
    if (!build_sheet_bg_mask(rgba, sheet_w, sheet_h, x0, y0, cw, ch, threshold, bg))
        return false;

    int tx0 = cw, ty0 = ch, tx1 = -1, ty1 = -1;
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            const unsigned char *p = rgba + ((size_t)(y0 + y) * sheet_w + (x0 + x)) * 4;
            if (p[3] < 128 || bg[(size_t)y * cw + x]) continue;
            if (x < tx0) tx0 = x; if (x > tx1) tx1 = x;
            if (y < ty0) ty0 = y; if (y > ty1) ty1 = y;
        }
    }
    if (tx1 < tx0 || ty1 < ty0) return false;

    if (!(opts && opts->crop)) {
        tx0 = 0; ty0 = 0; tx1 = cw - 1; ty1 = ch - 1;
    }

    int out_w = tx1 - tx0 + 1;
    int out_h = ty1 - ty0 + 1;
    if (out_w <= 0 || out_h <= 0 || out_w > 65535 || out_h > 65535) return false;

    IMG *img = AllocImg();
    if (!img) return false;
    img->w = (unsigned short)out_w;
    img->h = (unsigned short)out_h;
    img->palnum = palctx->palnum;
    img->flags = 0;
    img->anix = 0;
    img->aniy = 0;
    img_clear_secondary_anipoint(img);
    img->pttbl_p = NULL;
    img->opals = (unsigned short)-1;
    make_sheet_frame_name(opts ? opts->name_prefix : "FRAME",
                          candidate.row, candidate.frame, img->n_s);

    unsigned short stride = (unsigned short)((out_w + 3) & ~3);
    img->data_p = PoolAlloc((size_t)stride * out_h);
    if (!img->data_p) return false;
    memset(img->data_p, 0, (size_t)stride * out_h);

    unsigned char *dst = (unsigned char *)img->data_p;
    for (int y = 0; y < out_h; y++) {
        for (int x = 0; x < out_w; x++) {
            int lx = tx0 + x;
            int ly = ty0 + y;
            const unsigned char *p = rgba + ((size_t)(y0 + ly) * sheet_w + (x0 + lx)) * 4;
            if (p[3] < 128 || bg[(size_t)ly * cw + lx]) continue;
            dst[(size_t)y * stride + x] = sheet_match_palette_index(palctx, p);
        }
    }
    return true;
}

static void fill_sheet_debug_report(SpriteSheetDebugReport *report, int w, int h,
                                    const SheetDetectTrace &trace,
                                    const std::vector<SheetFrameCandidate> &frames)
{
    if (!report) return;
    report->sheet_w = w;
    report->sheet_h = h;
    report->raw_islands = trace.raw_islands;
    report->accepted_frames = (int)frames.size();
    report->line_rows = trace.line_rows;
    report->line_cols = trace.line_cols;
    report->frames.clear();
    report->frames.reserve(frames.size());
    for (const SheetFrameCandidate &f : frames) {
        report->frames.push_back({f.x0, f.y0, f.x1, f.y1,
                                  f.pixels, f.islands, f.row, f.frame});
    }
}

static bool ensure_sheet_debug_dir(const char *path)
{
    if (!path || !*path) return false;
#ifdef _WIN32
    if (_mkdir(path) == 0) return true;
#else
    if (mkdir(path, 0755) == 0) return true;
#endif
    struct stat st;
    if (stat(path, &st) != 0) return false;
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

static std::string sheet_join_path(const char *dir, const char *name)
{
    std::string out = dir && *dir ? std::string(dir) : std::string(".");
    if (!out.empty()) {
        char last = out[out.size() - 1];
        if (last != '\\' && last != '/') {
#ifdef _WIN32
            out += "\\";
#else
            out += "/";
#endif
        }
    }
    out += name ? name : "";
    return out;
}

static void sheet_blend_pixel(std::vector<unsigned char> &rgba, int w, int h,
                              int x, int y,
                              unsigned char r, unsigned char g,
                              unsigned char b, unsigned char a)
{
    if (x < 0 || x >= w || y < 0 || y >= h || a == 0) return;
    unsigned char *p = rgba.data() + ((size_t)y * w + x) * 4;
    int ia = 255 - a;
    p[0] = (unsigned char)((r * a + p[0] * ia + 127) / 255);
    p[1] = (unsigned char)((g * a + p[1] * ia + 127) / 255);
    p[2] = (unsigned char)((b * a + p[2] * ia + 127) / 255);
    p[3] = 255;
}

static void sheet_draw_rect(std::vector<unsigned char> &rgba, int w, int h,
                            int x0, int y0, int x1, int y1,
                            unsigned char r, unsigned char g, unsigned char b)
{
    for (int t = 0; t < 2; t++) {
        for (int x = x0; x <= x1; x++) {
            sheet_blend_pixel(rgba, w, h, x, y0 + t, r, g, b, 255);
            sheet_blend_pixel(rgba, w, h, x, y1 - t, r, g, b, 255);
        }
        for (int y = y0; y <= y1; y++) {
            sheet_blend_pixel(rgba, w, h, x0 + t, y, r, g, b, 255);
            sheet_blend_pixel(rgba, w, h, x1 - t, y, r, g, b, 255);
        }
    }
}

static void sheet_write_mask_png(const char *out_dir, int w, int h,
                                 const std::vector<unsigned char> &mask)
{
    if ((int)mask.size() != w * h) return;
    std::vector<unsigned char> rgba((size_t)w * h * 4, 0);
    for (int i = 0; i < w * h; i++) {
        unsigned char v = mask[(size_t)i] ? 255 : 0;
        rgba[(size_t)i * 4 + 0] = v;
        rgba[(size_t)i * 4 + 1] = v;
        rgba[(size_t)i * 4 + 2] = v;
        rgba[(size_t)i * 4 + 3] = 255;
    }
    std::string path = sheet_join_path(out_dir, "mask.png");
    stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4);
}

static void sheet_write_overlay_png(const char *out_dir,
                                    const unsigned char *src, int w, int h,
                                    const std::vector<SheetFrameCandidate> &frames)
{
    if (!src || w <= 0 || h <= 0) return;
    std::vector<unsigned char> rgba(src, src + (size_t)w * h * 4);
    static const unsigned char colors[][3] = {
        {255,  64,  64}, { 64, 220, 255}, {255, 210,  64}, {120, 255, 120},
        {220, 120, 255}, {255, 150,  64}, { 64, 128, 255}, {255,  80, 180}
    };
    for (int i = 0; i < (int)frames.size(); i++) {
        const SheetFrameCandidate &f = frames[(size_t)i];
        const unsigned char *c = colors[i % (int)(sizeof(colors) / sizeof(colors[0]))];
        sheet_draw_rect(rgba, w, h, f.x0, f.y0, f.x1, f.y1, c[0], c[1], c[2]);
    }
    std::string path = sheet_join_path(out_dir, "overlay.png");
    stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4);
}

static bool build_sheet_candidate_rgba(const unsigned char *rgba,
                                       int sheet_w, int sheet_h,
                                       const SheetFrameCandidate &candidate,
                                       const SpriteSheetImportOptions *opts,
                                       std::vector<unsigned char> &out,
                                       int *out_w, int *out_h)
{
    if (!rgba || !out_w || !out_h) return false;
    int threshold = sheet_clamp_int(opts ? opts->background_threshold : 245, 180, 255);
    int pad = opts ? opts->padding : 2;
    if (pad < 0) pad = 0;

    int x0 = sheet_clamp_int(candidate.x0 - pad, 0, sheet_w - 1);
    int y0 = sheet_clamp_int(candidate.y0 - pad, 0, sheet_h - 1);
    int x1 = sheet_clamp_int(candidate.x1 + pad, 0, sheet_w - 1);
    int y1 = sheet_clamp_int(candidate.y1 + pad, 0, sheet_h - 1);
    int cw = x1 - x0 + 1;
    int ch = y1 - y0 + 1;

    std::vector<unsigned char> bg;
    if (!build_sheet_bg_mask(rgba, sheet_w, sheet_h, x0, y0, cw, ch, threshold, bg))
        return false;

    int tx0 = cw, ty0 = ch, tx1 = -1, ty1 = -1;
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            const unsigned char *p = rgba + ((size_t)(y0 + y) * sheet_w + (x0 + x)) * 4;
            if (p[3] < 128 || bg[(size_t)y * cw + x]) continue;
            if (x < tx0) tx0 = x; if (x > tx1) tx1 = x;
            if (y < ty0) ty0 = y; if (y > ty1) ty1 = y;
        }
    }
    if (tx1 < tx0 || ty1 < ty0) return false;
    if (!(opts && opts->crop)) {
        tx0 = 0; ty0 = 0; tx1 = cw - 1; ty1 = ch - 1;
    }

    *out_w = tx1 - tx0 + 1;
    *out_h = ty1 - ty0 + 1;
    out.assign((size_t)(*out_w) * (*out_h) * 4, 0);
    for (int y = 0; y < *out_h; y++) {
        for (int x = 0; x < *out_w; x++) {
            int lx = tx0 + x;
            int ly = ty0 + y;
            const unsigned char *p = rgba + ((size_t)(y0 + ly) * sheet_w + (x0 + lx)) * 4;
            unsigned char *d = out.data() + ((size_t)y * (*out_w) + x) * 4;
            if (p[3] < 128 || bg[(size_t)ly * cw + lx]) {
                d[0] = d[1] = d[2] = d[3] = 0;
            } else {
                d[0] = p[0];
                d[1] = p[1];
                d[2] = p[2];
                d[3] = 255;
            }
        }
    }
    return true;
}

static void sheet_write_candidate_pngs(const char *out_dir,
                                       const unsigned char *rgba,
                                       int sheet_w, int sheet_h,
                                       const std::vector<SheetFrameCandidate> &frames,
                                       const SpriteSheetImportOptions *opts)
{
    for (int i = 0; i < (int)frames.size(); i++) {
        std::vector<unsigned char> crop;
        int cw = 0, ch = 0;
        if (!build_sheet_candidate_rgba(rgba, sheet_w, sheet_h,
                                        frames[(size_t)i], opts,
                                        crop, &cw, &ch))
            continue;
        char name[64];
        char frame_name[16];
        make_sheet_frame_name(opts ? opts->name_prefix : "FRAME",
                              frames[(size_t)i].row, frames[(size_t)i].frame,
                              frame_name);
        snprintf(name, sizeof(name), "%03d_%s.png", i + 1, frame_name);
        std::string path = sheet_join_path(out_dir, name);
        stbi_write_png(path.c_str(), cw, ch, 4, crop.data(), cw * 4);
    }
}

static void sheet_write_report_txt(const char *out_dir, const char *src_path,
                                   int w, int h,
                                   const SheetDetectTrace &trace,
                                   const std::vector<SheetFrameCandidate> &frames)
{
    std::string path = sheet_join_path(out_dir, "report.txt");
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "Sprite Sheet Debug Report\n");
    fprintf(f, "Source: %s\n", src_path ? src_path : "(null)");
    fprintf(f, "Size: %dx%d\n", w, h);
    fprintf(f, "Raw islands: %d\n", trace.raw_islands);
    fprintf(f, "Line rows removed: %d\n", trace.line_rows);
    fprintf(f, "Line columns removed: %d\n", trace.line_cols);
    fprintf(f, "Accepted frames: %d\n\n", (int)frames.size());
    for (int i = 0; i < (int)frames.size(); i++) {
        const SheetFrameCandidate &c = frames[(size_t)i];
        fprintf(f, "%03d row=%d frame=%d box=%d,%d..%d,%d size=%dx%d pixels=%d islands=%d\n",
                i + 1, c.row, c.frame, c.x0, c.y0, c.x1, c.y1,
                c.x1 - c.x0 + 1, c.y1 - c.y0 + 1, c.pixels, c.islands);
    }
    fclose(f);
}

static int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static int blend_channel(int s, int d, int mode)
{
    switch (mode) {
        case GifBlend_Darken:      return s < d ? s : d;
        case GifBlend_Multiply:    return (s * d + 127) / 255;
        case GifBlend_ColorBurn:   return s == 0 ? 0 : clamp255(255 - ((255 - d) * 255 + s / 2) / s);
        case GifBlend_LinearBurn:  return clamp255(s + d - 255);
        case GifBlend_Lighten:     return s > d ? s : d;
        case GifBlend_Screen:      return 255 - ((255 - s) * (255 - d) + 127) / 255;
        case GifBlend_ColorDodge:  return s == 255 ? 255 : clamp255((d * 255 + (255 - s) / 2) / (255 - s));
        case GifBlend_Overlay:
            return d < 128 ? (2 * s * d + 127) / 255
                           : 255 - (2 * (255 - s) * (255 - d) + 127) / 255;
        case GifBlend_SoftLight:
        {
            double sf = s / 255.0;
            double df = d / 255.0;
            double out = (sf <= 0.5)
                ? df - (1.0 - 2.0 * sf) * df * (1.0 - df)
                : df + (2.0 * sf - 1.0) * (std::sqrt(df) - df);
            return clamp255((int)(out * 255.0 + 0.5));
        }
        case GifBlend_HardLight:
            return s < 128 ? (2 * s * d + 127) / 255
                           : 255 - (2 * (255 - s) * (255 - d) + 127) / 255;
        case GifBlend_Difference:  return abs(d - s);
        case GifBlend_Exclusion:   return clamp255(d + s - (2 * d * s + 127) / 255);
        case GifBlend_Normal:
        case GifBlend_Dissolve:
        default:                   return s;
    }
}

static unsigned int dissolve_hash(int x, int y, int frame)
{
    unsigned int v = (unsigned int)(x * 73856093u) ^ (unsigned int)(y * 19349663u) ^
                     (unsigned int)(frame * 83492791u);
    v ^= v >> 13;
    v *= 1274126177u;
    v ^= v >> 16;
    return v & 255u;
}

static void blend_rgba_pixel(unsigned char *dst, const unsigned char *src,
                             int x, int y, int frame, int mode, int opacity_percent)
{
    int sa_i = (src[3] * opacity_percent + 50) / 100;
    if (mode == GifBlend_Dissolve) {
        if (sa_i >= 255) sa_i = 255;
        else if ((int)dissolve_hash(x, y, frame) >= sa_i) sa_i = 0;
        else sa_i = 255;
    }
    if (sa_i <= 0) return;

    int da_i = dst[3];
    int out_a = sa_i + (da_i * (255 - sa_i) + 127) / 255;
    if (out_a <= 0) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
        return;
    }

    int br = blend_channel(src[0], dst[0], mode);
    int bg = blend_channel(src[1], dst[1], mode);
    int bb = blend_channel(src[2], dst[2], mode);
    int keep = (da_i * (255 - sa_i) + 127) / 255;

    dst[0] = (unsigned char)clamp255((br * sa_i + dst[0] * keep + out_a / 2) / out_a);
    dst[1] = (unsigned char)clamp255((bg * sa_i + dst[1] * keep + out_a / 2) / out_a);
    dst[2] = (unsigned char)clamp255((bb * sa_i + dst[2] * keep + out_a / 2) / out_a);
    dst[3] = (unsigned char)clamp255(out_a);
}

} /* namespace */

void ImportPng(const char *path)
{
    verbose_log("ImportPng: %s", path);
    int w, h, channels;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);
    if (!data || w == 0 || h == 0) return;

    /* Step 1: histogram all opaque pixels by their 15-bit color.
       2^15 = 32768 possible colors total, so a flat lookup table is fine. */
    static int hist[32768];
    memset(hist, 0, sizeof(hist));
    int opaque_pixels = 0;
    for (int i = 0; i < w * h; i++) {
        unsigned char a = data[i * 4 + 3];
        if (a < 128) continue;
        int r5 = data[i * 4 + 0] >> 3;
        int g5 = data[i * 4 + 1] >> 3;
        int b5 = data[i * 4 + 2] >> 3;
        unsigned short c = (unsigned short)((r5 << 10) | (g5 << 5) | b5);
        hist[c]++;
        opaque_pixels++;
    }

    /* Step 2: seed median-cut with one bucket containing every unique color. */
    std::vector<ColorBucket> buckets;
    buckets.reserve(256);
    buckets.emplace_back();
    {
        ColorBucket &b0 = buckets.back();
        for (int c = 0; c < 32768; c++) {
            if (hist[c] > 0) b0.entries.push_back({ (unsigned short)c, hist[c] });
        }
        recompute_bucket_bounds(b0);
    }

    const int MAX_PAL_COLORS = 255; /* +1 for transparent at index 0 */

    /* Step 3: repeatedly split the bucket with the widest channel range
       (weighted by that range) until we have MAX_PAL_COLORS buckets or no
       further splits are possible. */
    while ((int)buckets.size() < MAX_PAL_COLORS) {
        int best = -1;
        int best_range = 0;
        int best_axis  = 0;   /* 0=r, 1=g, 2=b */
        for (int i = 0; i < (int)buckets.size(); i++) {
            ColorBucket &b = buckets[i];
            if (b.entries.size() < 2) continue;
            int rr = b.r_hi - b.r_lo;
            int gr = b.g_hi - b.g_lo;
            int br = b.b_hi - b.b_lo;
            int axis = 0, range = rr;
            if (gr > range) { range = gr; axis = 1; }
            if (br > range) { range = br; axis = 2; }
            if (range > best_range) {
                best_range = range;
                best       = i;
                best_axis  = axis;
            }
        }
        if (best < 0) break; /* every remaining bucket is a single color */

        ColorBucket &src = buckets[best];
        std::sort(src.entries.begin(), src.entries.end(),
                  [best_axis](const std::pair<unsigned short, int> &a,
                              const std::pair<unsigned short, int> &b) {
                      int ar, ag, ab; unpack15(a.first, ar, ag, ab);
                      int br, bg, bb; unpack15(b.first, br, bg, bb);
                      int av = (best_axis == 0) ? ar : (best_axis == 1) ? ag : ab;
                      int bv = (best_axis == 0) ? br : (best_axis == 1) ? bg : bb;
                      return av < bv;
                  });

        /* Split at the pixel-weighted median, so both halves hold roughly
           half the pixel count rather than half the unique colors. */
        int half = src.pixel_count / 2;
        int acc  = 0;
        size_t split = 0;
        for (; split < src.entries.size() - 1; split++) {
            acc += src.entries[split].second;
            if (acc >= half) { split++; break; }
        }
        if (split == 0) split = 1;
        if (split >= src.entries.size()) split = src.entries.size() - 1;

        ColorBucket right;
        right.entries.assign(src.entries.begin() + split, src.entries.end());
        src.entries.erase(src.entries.begin() + split, src.entries.end());
        recompute_bucket_bounds(src);
        recompute_bucket_bounds(right);
        buckets.push_back(std::move(right));
    }

    /* Step 4: compute representative color per bucket = pixel-weighted average
       of its constituent colors, rounded to the nearest 5-bit value. */
    int pal_colors = (int)buckets.size();
    struct PalRGB5 { unsigned char r, g, b; };
    PalRGB5 pcolors[256] = {};
    for (int i = 0; i < pal_colors; i++) {
        long long rs = 0, gs = 0, bs = 0, ws = 0;
        for (auto &e : buckets[i].entries) {
            int r, g, b; unpack15(e.first, r, g, b);
            rs += (long long)r * e.second;
            gs += (long long)g * e.second;
            bs += (long long)b * e.second;
            ws += e.second;
        }
        if (ws == 0) ws = 1;
        pcolors[i + 1].r = (unsigned char)((rs + ws / 2) / ws);
        pcolors[i + 1].g = (unsigned char)((gs + ws / 2) / ws);
        pcolors[i + 1].b = (unsigned char)((bs + ws / 2) / ws);
    }

    /* Step 5: allocate the new palette (index 0 = transparent black). */
    PAL *pal = AllocPal();
    if (!pal) { stbi_image_free(data); return; }
    pal->flags   = 0;
    pal->bitspix = 8;
    pal->numc    = (unsigned short)(pal_colors + 1);
    pal->data_p  = PoolAlloc((unsigned int)(pal_colors + 1) * 2);
    if (!pal->data_p) { stbi_image_free(data); return; }
    unsigned char *pal_bytes = (unsigned char *)pal->data_p;
    pal_bytes[0] = 0; pal_bytes[1] = 0;
    for (int i = 1; i <= pal_colors; i++) {
        unsigned short w15 = (unsigned short)(((pcolors[i].r & 0x1F) << 10) |
                                              ((pcolors[i].g & 0x1F) <<  5) |
                                              ( pcolors[i].b & 0x1F));
        pal_bytes[i * 2]     = (unsigned char)(w15 & 0xFF);
        pal_bytes[i * 2 + 1] = (unsigned char)(w15 >> 8);
    }

    /* Step 6: allocate the new image. */
    IMG *img = AllocImg();
    if (!img) { stbi_image_free(data); return; }
    img->w = (unsigned short)w; img->h = (unsigned short)h;
    img->palnum = (unsigned short)(g_doc->palcnt - 1); img->flags = 0;
    img->anix = 0; img->aniy = 0; img_clear_secondary_anipoint(img);
    img->pttbl_p = NULL; img->opals = (unsigned short)-1;
    unsigned short stride = (unsigned short)((w + 3) & ~3);
    img->data_p = PoolAlloc((size_t)stride * h);
    if (!img->data_p) { stbi_image_free(data); return; }
    memset(img->data_p, 0, (size_t)stride * h);

    const char *name = strrchr(path, '/'); if (!name) name = strrchr(path, '\\'); if (!name) name = path; else name++;
    strncpy(img->n_s, name, 15); img->n_s[15] = '\0';
    char *dot = strrchr(img->n_s, '.'); if (dot) *dot = '\0';
    strncpy(pal->n_s, img->n_s, 8); pal->n_s[8] = 'P'; pal->n_s[9] = '\0';

    /* Step 7: map every opaque pixel to the nearest palette entry.
       To avoid the O(w*h*pal) hot loop, cache the lookup per unique 15-bit
       color so each color is only matched against the palette once. */
    static unsigned char color_to_idx[32768];
    static bool          color_resolved[32768];
    memset(color_resolved, 0, sizeof(color_resolved));

    unsigned char *out = (unsigned char *)img->data_p;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char *p = data + (y * w + x) * 4;
            if (p[3] < 128) continue;
            int r5 = p[0] >> 3, g5 = p[1] >> 3, b5 = p[2] >> 3;
            unsigned short c = (unsigned short)((r5 << 10) | (g5 << 5) | b5);
            unsigned char idx;
            if (color_resolved[c]) {
                idx = color_to_idx[c];
            } else {
                int best_idx = 1, best_dist = INT_MAX;
                for (int j = 1; j <= pal_colors; j++) {
                    int dr = r5 - pcolors[j].r;
                    int dg = g5 - pcolors[j].g;
                    int db = b5 - pcolors[j].b;
                    int dist = dr*dr + dg*dg + db*db;
                    if (dist < best_dist) { best_dist = dist; best_idx = j; }
                }
                idx = (unsigned char)best_idx;
                color_to_idx[c] = idx;
                color_resolved[c] = true;
            }
            out[y * stride + x] = idx;
        }
    }
    stbi_image_free(data);
    if (g_doc->imgcnt > 0) g_doc->ilselected = (int)g_doc->imgcnt - 1;
    g_img_tex_idx = -2;
    verbose_log("  -> %dx%d px, %d palette colors from %d unique source colors",
                w, h, pal_colors, (int)buckets.size());
}

void ImportPngMatch(const char *path)
{
    verbose_log("ImportPngMatch: %s", path);
    IMG *active_img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
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
    img->anix = 0; img->aniy = 0; img_clear_secondary_anipoint(img);
    img->pttbl_p = NULL; img->opals = (unsigned short)-1;
    unsigned short stride = (unsigned short)((w + 3) & ~3);
    img->data_p = PoolAlloc((size_t)stride * h);
    if (!img->data_p) { stbi_image_free(data); return; }
    memset(img->data_p, 0, (size_t)stride * h);

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
    if (g_doc->imgcnt > 0) g_doc->ilselected = (int)g_doc->imgcnt - 1;
    g_img_tex_idx = -2;
    verbose_log("  -> %dx%d px, matched to palette %u", w, h, pal->numc);
}

int ImportSpriteSheetMatch(const char *path, const SpriteSheetImportOptions *options)
{
    verbose_log("ImportSpriteSheetMatch: %s", path ? path : "(null)");

    SpriteSheetImportOptions opt = {
        SpriteSheetDetect_Auto,
        245,
        160,
        2,
        true,
        "FRAME"
    };
    if (options) {
        opt = *options;
        opt.name_prefix[sizeof(opt.name_prefix) - 1] = '\0';
    }
    opt.background_threshold = sheet_clamp_int(opt.background_threshold, 180, 255);
    if (opt.min_pixels < 1) opt.min_pixels = 1;
    if (opt.padding < 0) opt.padding = 0;
    if (opt.detect_mode != SpriteSheetDetect_Auto &&
        opt.detect_mode != SpriteSheetDetect_Islands)
        opt.detect_mode = SpriteSheetDetect_Auto;

    IMG *active_img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    unsigned short palnum = 0xFFFF;
    if (active_img && get_pal(active_img->palnum))
        palnum = active_img->palnum;
    else if (g_doc->plselected >= 0 && get_pal(g_doc->plselected))
        palnum = (unsigned short)g_doc->plselected;

    SheetPaletteMatch palctx;
    if (palnum == 0xFFFF || !init_sheet_palette_match(&palctx, palnum)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Sprite sheet import needs an active palette.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    int w = 0, h = 0, channels = 0;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);
    if (!data || w <= 0 || h <= 0) {
        if (data) stbi_image_free(data);
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Sprite sheet import failed: could not read image.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    std::vector<SheetFrameCandidate> frames;
    detect_sheet_candidates(data, w, h, &opt, frames);
    if (frames.empty()) {
        stbi_image_free(data);
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No sprites detected. Try lowering Min Pixels or Background.");
        g_restore_msg_timer = 5.0f;
        verbose_log("  -> no sprite candidates detected (%dx%d)", w, h);
        return 0;
    }

    int imported = 0;
    for (const SheetFrameCandidate &frame : frames) {
        if (import_sheet_candidate(data, w, h, frame, &palctx, &opt))
            imported++;
    }
    stbi_image_free(data);

    if (imported > 0) {
        g_doc->plselected = (int)palctx.palnum;
        if (g_doc->imgcnt > 0) g_doc->ilselected = (int)g_doc->imgcnt - 1;
        g_img_tex_idx = -2;
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Imported %d/%d sprite sheet frame(s) to palette %u.",
                 imported, (int)frames.size(), palctx.palnum);
        verbose_log("  -> %dx%d px, %d/%d frames matched to palette %u",
                    w, h, imported, (int)frames.size(), palctx.palnum);
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Sprite sheet import failed: no frames imported.");
    }
    g_restore_msg_timer = 4.0f;
    return imported;
}

int AnalyzeSpriteSheet(const char *path, const SpriteSheetImportOptions *options,
                       SpriteSheetDebugReport *report)
{
    if (report) {
        report->sheet_w = report->sheet_h = 0;
        report->raw_islands = 0;
        report->accepted_frames = 0;
        report->line_rows = report->line_cols = 0;
        report->frames.clear();
    }

    SpriteSheetImportOptions opt = {
        SpriteSheetDetect_Auto,
        245,
        160,
        2,
        true,
        "FRAME"
    };
    if (options) {
        opt = *options;
        opt.name_prefix[sizeof(opt.name_prefix) - 1] = '\0';
    }
    opt.background_threshold = sheet_clamp_int(opt.background_threshold, 180, 255);
    if (opt.min_pixels < 1) opt.min_pixels = 1;
    if (opt.padding < 0) opt.padding = 0;

    int w = 0, h = 0, channels = 0;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);
    if (!data || w <= 0 || h <= 0) {
        if (data) stbi_image_free(data);
        return 0;
    }

    SheetDetectTrace trace;
    std::vector<SheetFrameCandidate> frames;
    detect_sheet_candidates(data, w, h, &opt, frames, &trace);
    fill_sheet_debug_report(report, w, h, trace, frames);
    stbi_image_free(data);
    return (int)frames.size();
}

int DebugSpriteSheetImport(const char *path, const char *output_dir,
                           const SpriteSheetImportOptions *options,
                           SpriteSheetDebugReport *report)
{
    if (report) {
        report->sheet_w = report->sheet_h = 0;
        report->raw_islands = 0;
        report->accepted_frames = 0;
        report->line_rows = report->line_cols = 0;
        report->frames.clear();
    }
    if (!ensure_sheet_debug_dir(output_dir)) return 0;

    SpriteSheetImportOptions opt = {
        SpriteSheetDetect_Auto,
        245,
        160,
        2,
        true,
        "FRAME"
    };
    if (options) {
        opt = *options;
        opt.name_prefix[sizeof(opt.name_prefix) - 1] = '\0';
    }
    opt.background_threshold = sheet_clamp_int(opt.background_threshold, 180, 255);
    if (opt.min_pixels < 1) opt.min_pixels = 1;
    if (opt.padding < 0) opt.padding = 0;

    int w = 0, h = 0, channels = 0;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);
    if (!data || w <= 0 || h <= 0) {
        if (data) stbi_image_free(data);
        return 0;
    }

    SheetDetectTrace trace;
    std::vector<SheetFrameCandidate> frames;
    detect_sheet_candidates(data, w, h, &opt, frames, &trace);
    fill_sheet_debug_report(report, w, h, trace, frames);

    sheet_write_mask_png(output_dir, w, h, trace.mask);
    sheet_write_overlay_png(output_dir, data, w, h, frames);
    sheet_write_candidate_pngs(output_dir, data, w, h, frames, &opt);
    sheet_write_report_txt(output_dir, path, w, h, trace, frames);

    stbi_image_free(data);
    return (int)frames.size();
}

/* ---- GIF Import ---- */

const char *GifBlendModeName(int mode)
{
    static const char *names[] = {
        "Normal",
        "Dissolve",
        "Darken",
        "Multiply",
        "Color Burn",
        "Linear Burn",
        "Lighten",
        "Screen",
        "Color Dodge",
        "Overlay",
        "Soft Light",
        "Hard Light",
        "Difference",
        "Exclusion",
    };
    if (mode < 0 || mode >= (int)(sizeof(names) / sizeof(names[0]))) return names[0];
    return names[mode];
}

static bool read_entire_file(const char *path, std::vector<unsigned char> &bytes)
{
    bytes.clear();
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz <= 0 || sz > INT_MAX) { fclose(f); return false; }
    rewind(f);
    bytes.resize((size_t)sz);
    bool ok = fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
    if (!ok) bytes.clear();
    return ok;
}

void ImportGif(const char *path, int blend_mode, int opacity_percent, bool import_all_frames)
{
    verbose_log("ImportGif: %s", path);
    if (blend_mode < 0 || blend_mode >= GifBlend_Count) blend_mode = GifBlend_Normal;
    if (opacity_percent < 0) opacity_percent = 0;
    if (opacity_percent > 100) opacity_percent = 100;

    std::vector<unsigned char> bytes;
    if (!read_entire_file(path, bytes)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "GIF import failed: could not read file.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    int *delays = NULL;
    int w = 0, h = 0, frames = 0, comp = 0;
    unsigned char *gif = stbi_load_gif_from_memory(bytes.data(), (int)bytes.size(),
                                                   &delays, &w, &h, &frames, &comp, 4);
    if (!gif || w <= 0 || h <= 0 || frames <= 0) {
        if (gif) stbi_image_free(gif);
        if (delays) stbi_image_free(delays);
        snprintf(g_restore_msg, sizeof(g_restore_msg), "GIF import failed: unsupported or corrupt GIF.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    int import_count = import_all_frames ? frames : 1;
    const size_t pixels_per_frame = (size_t)w * (size_t)h;
    std::vector<unsigned char> frames_rgba;
    frames_rgba.resize((size_t)import_count * pixels_per_frame * 4);

    bool preserve_gif_frames = (blend_mode == GifBlend_Normal && opacity_percent >= 100);
    if (preserve_gif_frames) {
        memcpy(frames_rgba.data(), gif, frames_rgba.size());
    } else {
        std::vector<unsigned char> canvas(pixels_per_frame * 4, 0);
        for (int f = 0; f < import_count; f++) {
            const unsigned char *src_frame = gif + (size_t)f * pixels_per_frame * 4;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    unsigned char *dst = canvas.data() + ((size_t)y * w + x) * 4;
                    const unsigned char *src = src_frame + ((size_t)y * w + x) * 4;
                    blend_rgba_pixel(dst, src, x, y, f, blend_mode, opacity_percent);
                }
            }
            memcpy(frames_rgba.data() + (size_t)f * pixels_per_frame * 4,
                   canvas.data(), pixels_per_frame * 4);
        }
    }

    int pal_colors = 0, unique_colors = 0;
    int imported = import_rgba_frames_as_images(path, frames_rgba.data(), w, h,
                                                import_count, &pal_colors, &unique_colors);

    stbi_image_free(gif);
    if (delays) stbi_image_free(delays);

    if (imported > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Imported %d GIF frame(s), %d palette color(s).", imported, pal_colors + 1);
        verbose_log("  -> %dx%d px, %d/%d frame(s), %d colors from %d unique source colors, blend=%s opacity=%d%%",
                    w, h, imported, frames, pal_colors + 1, unique_colors,
                    GifBlendModeName(blend_mode), opacity_percent);
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "GIF import failed: no frames imported.");
    }
    g_restore_msg_timer = 4.0f;
}

/* ---- PNG Export ---- */

void ExportPng(const char *path)
{
    verbose_log("ExportPng: %s", path);
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
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

/* ---- Palette Export ---- */

void ExportPalette(const char *path, bool adobe_act)
{
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No palette selected.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Palette export failed.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    int n = pal->numc;
    if (n < 0) n = 0;
    if (n > 256) n = 256;
    const unsigned char *src = (const unsigned char *)pal->data_p;

    if (adobe_act) {
        for (int i = 0; i < 256; i++) {
            unsigned char rgb[3] = {0, 0, 0};
            if (i < n) pal_word_to_rgb8(src + i * 2, &rgb[0], &rgb[1], &rgb[2]);
            fwrite(rgb, 1, 3, f);
        }
    } else {
        fwrite(src, 2, (size_t)n, f);
    }
    fclose(f);

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Exported palette %s (%d color%s).", pal->n_s, n, n == 1 ? "" : "s");
    g_restore_msg_timer = 4.0f;
    verbose_log("ExportPalette: %s (%s, %d colors)", path,
                adobe_act ? "ACT" : "raw PAL", n);
}

/* ---- Palette Import ---- */

void ImportPalette(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Failed to open palette file.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Palette file is empty.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    /* Extract filename for palette name */
    const char *base = strrchr(path, '/');
    const char *base_win = strrchr(path, '\\');
    if (base_win && (!base || base_win > base)) base = base_win;
    base = base ? base + 1 : path;

    char pal_name[32] = {0};
    strncpy(pal_name, base, sizeof(pal_name) - 1);
    char *dot = strrchr(pal_name, '.');
    if (dot) *dot = '\0';

    bool is_act = (size == 768 || size == 772);
    /* Also check extension if size is ambiguous, though ACT is very strictly sized. */
    const char *ext = strrchr(path, '.');
    if (ext && (_stricmp(ext, ".act") == 0)) {
        is_act = true;
    } else if (ext && (_stricmp(ext, ".pal") == 0)) {
        is_act = false;
    }

    int n_colors = is_act ? 256 : (int)(size / 2);
    if (n_colors > 256) n_colors = 256;

    PAL *new_pal = AllocPal();
    if (!new_pal) {
        fclose(f);
        return;
    }
    strncpy(new_pal->n_s, pal_name, 11);
    new_pal->numc = n_colors;
    new_pal->data_p = calloc(256, 2); /* Always allocate max */

    if (is_act) {
        unsigned char rgb[768];
        size_t read_bytes = fread(rgb, 1, 768, f);
        int read_colors = (int)read_bytes / 3;
        if (read_colors < n_colors) n_colors = read_colors;
        new_pal->numc = n_colors;

        unsigned char *dst = (unsigned char *)new_pal->data_p;
        for (int i = 0; i < n_colors; i++) {
            rgb8_to_pal_word(rgb[i*3], rgb[i*3+1], rgb[i*3+2], dst + i * 2);
        }
    } else {
        fread(new_pal->data_p, 2, n_colors, f);
    }
    fclose(f);

    g_doc->plselected = (int)g_doc->palcnt - 1; /* Select the new palette */

    snprintf(g_restore_msg, sizeof(g_restore_msg), "Imported palette %s (%d colors).", pal_name, n_colors);
    g_restore_msg_timer = 4.0f;
    verbose_log("ImportPalette: %s (%s, %d colors)", path, is_act ? "ACT" : "raw PAL", n_colors);
}
