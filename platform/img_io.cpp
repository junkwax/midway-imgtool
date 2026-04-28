/*************************************************************
 * platform/img_io.cpp
 * File I/O: IMG load/save, TGA/LBM/PNG import/export.
 * Extracted from imgui_overlay.cpp.
 *************************************************************/
#include "img_io.h"
#include <vector>
#include <algorithm>
#include <string>
#include <regex>

/* From imgui_overlay.cpp */
extern void undo_push(void);

/* Shared texture cache index (invalidated on I/O that changes pixels) */
int  g_img_tex_idx = -2;

/* Status message for restore operation */
char  g_restore_msg[128] = {0};
float g_restore_msg_timer = 0.0f;

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

/* ---- ImgLoad: port of img_load (IMG file reader) ---- */
void LoadImgFile(void)
{
    FILE *f = fopen(fname_s, "rb");
    if (!f) return;

    LIB_HDR hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return; }

    if (hdr.temp != 0xABCD || hdr.version < 0x500) { fclose(f); return; }
    fileversion = hdr.version;

    if (hdr.imgcnt == 0 || hdr.imgcnt > 2000) { fclose(f); return; }

    ilpalloaded = -1;
    damcnt = 0;

    unsigned int pal_oset = (unsigned int)hdr.oset + (unsigned int)hdr.imgcnt * sizeof(IMAGE_disk);
    if (hdr.palcnt > NUMDEFPAL)
        pal_oset += (unsigned int)(hdr.palcnt - NUMDEFPAL) * sizeof(PALETTE_disk);

    {
        int seqcnt = (signed short)hdr.seqcnt;
        int scrcnt = (signed short)hdr.scrcnt;
        int is_far = (hdr.version >= 0x634);
        int entry_sz = is_far ? 16 : 14;
        int seqscr_sz = is_far ? 32 : 24;

        for (int s = 0; s < seqcnt; s++) {
            fseek(f, seqscr_sz - 2, SEEK_CUR);
            unsigned short num;
            fread(&num, 1, 2, f);
            fseek(f, (long)num * entry_sz, SEEK_CUR);
        }
        for (int s = 0; s < scrcnt; s++) {
            fseek(f, seqscr_sz - 2, SEEK_CUR);
            unsigned short num;
            fread(&num, 1, 2, f);
            fseek(f, (long)num * entry_sz, SEEK_CUR);
        }
    }

    long ptoset = ftell(f);

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
        img->file_lib      = idisk.lib;
        img->file_frm      = idisk.frm;
        img->file_pttblnum = idisk.pttblnum;

        strncpy(img->n_s, idisk.n_s, 15);
        img->n_s[15] = '\0';

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

        unsigned int col_sz = (unsigned int)pal->numc * 2;
        pal->data_p = PoolAlloc(col_sz);
        if (!pal->data_p) break;

        fseek(f, (long)pdisk.oset, SEEK_SET);
        fread(pal->data_p, 1, col_sz, f);
    }

    fclose(f);

    if (imgcnt > 0) ilselected = 0;
}

/* ---- SaveImgFile: port of img_save (IMG file writer) ---- */
void SaveImgFile(void)
{
    FILE *f = fopen(fname_s, "wb");
    if (!f) return;

    int num_imgs = (int)imgcnt;
    int num_pals = (int)palcnt;

    // 1. Count point tables
    int pt_count = 0;
    IMG *img = (IMG *)img_p;
    for (int i = 0; i < num_imgs && img; i++, img = (IMG *)img->nxt_p) {
        if (img->pttbl_p) pt_count++;
    }

    LIB_HDR hdr = {};
    hdr.imgcnt  = (unsigned short)num_imgs;
    hdr.palcnt  = (unsigned short)(num_pals + NUMDEFPAL);
    hdr.version = 0x0634;
    hdr.temp    = 0xABCD;
    hdr.oset    = (unsigned int)(sizeof(LIB_HDR) + pt_count * 40);
    hdr.seqcnt  = 0;
    hdr.scrcnt  = 0;
    hdr.damcnt  = 0;
    for (int i = 0; i < 4; i++) hdr.bufscr[i] = (unsigned char)-1;

    fwrite(&hdr, 1, sizeof(hdr), f);

    // 2. Write Point Tables
    img = (IMG *)img_p;
    for (int i = 0; i < num_imgs && img; i++, img = (IMG *)img->nxt_p) {
        if (img->pttbl_p) {
            fwrite(img->pttbl_p, 1, 40, f);
        }
    }

    // 3. Prepare positions
    long img_hdr_pos = (long)hdr.oset;
    long pal_hdr_pos = (long)(hdr.oset + num_imgs * sizeof(IMAGE_disk));
    long data_pos    = (long)(pal_hdr_pos + num_pals * sizeof(PALETTE_disk));

    // 4. Write IMAGE_disk array (with placeholders for oset)
    fseek(f, img_hdr_pos, SEEK_SET);
    int pt_index = 0;
    img = (IMG *)img_p;
    for (int i = 0; i < num_imgs && img; i++, img = (IMG *)img->nxt_p) {
        IMAGE_disk idisk = {};
        strncpy(idisk.n_s, img->n_s, 15); idisk.n_s[15] = '\0';
        idisk.flags  = img->flags;
        idisk.anix   = img->anix;
        idisk.aniy   = img->aniy;
        idisk.w      = img->w;
        idisk.h      = img->h;
        idisk.palnum = (unsigned short)((int)img->palnum + NUMDEFPAL);
        idisk.oset   = 0; // To be filled later
        idisk.anix2  = img->anix2;
        idisk.aniy2  = img->aniy2;
        idisk.aniz2  = img->aniz2;
        idisk.lib    = img->file_lib;
        idisk.frm    = img->file_frm;
        idisk.opals  = img->opals;
        idisk.pttblnum = img->pttbl_p ? (unsigned short)(pt_index++) : (unsigned short)0xFFFF;

        fwrite(&idisk, 1, sizeof(idisk), f);
    }

    // 5. Write PALETTE_disk array (with placeholders for oset)
    fseek(f, pal_hdr_pos, SEEK_SET);
    PAL *pal = (PAL *)pal_p;
    for (int i = 0; i < num_pals && pal; i++, pal = (PAL *)pal->nxt_p) {
        PALETTE_disk pdisk = {};
        strncpy(pdisk.n_s, pal->n_s, 9); pdisk.n_s[9] = '\0';
        pdisk.flags   = pal->flags;
        pdisk.bitspix = pal->bitspix;
        pdisk.numc    = pal->numc;
        pdisk.oset    = 0; // To be filled later

        fwrite(&pdisk, 1, sizeof(pdisk), f);
    }

    // 6. Write Image Data & Update IMAGE_disk.oset
    img = (IMG *)img_p;
    for (int i = 0; i < num_imgs && img; i++, img = (IMG *)img->nxt_p) {
        fseek(f, data_pos, SEEK_SET);
        unsigned int stride = ((unsigned int)img->w + 3) & ~3;
        unsigned int sz = stride * img->h;

        // Update oset
        unsigned int cur_oset = (unsigned int)data_pos;
        long save_pos = ftell(f);
        fseek(f, img_hdr_pos + i * sizeof(IMAGE_disk) + offsetof(IMAGE_disk, oset), SEEK_SET);
        fwrite(&cur_oset, 1, 4, f);
        fseek(f, save_pos, SEEK_SET);

        if (img->flags & 0x0080) { // CMP
            int lm_mult = 1 << ((img->flags >> 8) & 3);
            int tm_mult = 1 << ((img->flags >> 10) & 3);
            unsigned char* src = (unsigned char*)img->data_p;
            bool free_z = false;
            if (!src) {
                src = (unsigned char *)calloc(1, sz);
                free_z = true;
            }
            if (src) {
                for (int y = 0; y < img->h; y++) {
                    unsigned char* row = src + y * stride;
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

                    int actual_leading = l_enc * lm_mult;
                    int actual_trailing = t_enc * tm_mult;
                    int visible = img->w - actual_leading - actual_trailing;
                    if (visible < 0) visible = 0;

                    unsigned char comp_byte = (unsigned char)((t_enc << 4) | (l_enc & 0x0F));
                    fwrite(&comp_byte, 1, 1, f);
                    if (visible > 0)
                        fwrite(row + actual_leading, 1, visible, f);

                    data_pos += 1 + visible;
                }
                if (free_z) free(src);
            }
        } else {
            if (img->data_p)
                fwrite(img->data_p, 1, sz, f);
            else {
                unsigned char *z = (unsigned char *)calloc(1, sz);
                if (z) { fwrite(z, 1, sz, f); free(z); }
            }
            data_pos += sz;
        }
    }

    // 7. Write Palette Data & Update PALETTE_disk.oset
    pal = (PAL *)pal_p;
    for (int i = 0; i < num_pals && pal; i++, pal = (PAL *)pal->nxt_p) {
        fseek(f, data_pos, SEEK_SET);
        unsigned int sz = (unsigned int)pal->numc * 2;

        if (pal->data_p)
            fwrite(pal->data_p, 1, sz, f);
        else {
            unsigned char *z = (unsigned char *)calloc(1, sz);
            if (z) { fwrite(z, 1, sz, f); free(z); }
        }

        // Update oset
        unsigned int cur_oset = (unsigned int)data_pos;
        long save_pos = ftell(f);
        fseek(f, pal_hdr_pos + i * sizeof(PALETTE_disk) + offsetof(PALETTE_disk, oset), SEEK_SET);
        fwrite(&cur_oset, 1, 4, f);
        fseek(f, save_pos, SEEK_SET);

        data_pos += sz;
    }

    // 8. Update header
    hdr.palcnt = (unsigned short)(num_pals + NUMDEFPAL);
    hdr.imgcnt = (unsigned short)num_imgs;
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

/* Bulk restore using regex to map child names to parent names.
   Iterates all images, checks if name matches regex, uses capture group 1 as parent name.
   If parent exists, overwrites child pixels with parent pixels (respecting anipoints). */
int RestoreChildrenFromParentRegex(const char* regex_pattern)
{
    if (!img_p || !regex_pattern || !regex_pattern[0]) return 0;

    std::regex re;
    try {
        re = std::regex(regex_pattern);
    } catch (const std::regex_error&) {
        return -1; // Regex compilation failed
    }

    undo_push();

    int total_written = 0;
    int imgs_processed = 0;

    for (IMG *child = (IMG *)img_p; child; child = (IMG *)child->nxt_p) {
        if (!child->data_p || child->w == 0 || child->h == 0) continue;

        std::string name(child->n_s);
        std::smatch match;
        if (std::regex_match(name, match, re) && match.size() > 1) {
            std::string parent_name = match[1].str();

            IMG *parent = NULL;
            for (IMG *p = (IMG *)img_p; p; p = (IMG *)p->nxt_p) {
                if (parent_name == p->n_s) {
                    parent = p;
                    break;
                }
            }

            if (parent && parent->data_p && parent->w > 0 && parent->h > 0 && parent != child) {
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
        }
    }

    if (total_written > 0) {
        g_img_tex_idx = -2;
    }
    return imgs_processed; // Return number of child images restored instead of total pixels
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

/* ---- Write TBL (Export Marked Images to MK2 format Assembly Table) ---- */
void WriteTblFromMarked(const char* filepath, unsigned int base_address, bool mk3_format, bool include_pal)
{
    FILE* f = fopen(filepath, "w");
    if (!f) return;

    IMG* p = (IMG*)img_p;
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
            fprintf(f, "\t.long   0%XH\n", base_address + (p->file_oset * 8));
            fprintf(f, "\t.word   0%04XH\n", p->flags);
            if (include_pal) {
                PAL* pal = get_pal(p->palnum);
                fprintf(f, "\t.long   %s\n", (pal && pal->n_s[0]) ? pal->n_s : "0");
            }
        }
        p = (IMG*)p->nxt_p;
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
void SaveTga(void)
{
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;

    PAL *pal = get_pal(img->palnum);
    if (!pal || !pal->data_p) return;

    FILE *f = fopen(fnametmp_s, "wb");
    if (!f) return;

    TGA_HEADER hdr = {};
    hdr.cm_type   = 1;
    hdr.i_type    = 1;
    hdr.cm_length = pal->numc;
    hdr.cm_size   = 15;
    hdr.width     = img->w;
    hdr.height    = img->h;
    hdr.bpp       = 8;

    fwrite(&hdr, 1, sizeof(hdr), f);
    fwrite(pal->data_p, 2, pal->numc, f);

    unsigned short stride = (img->w + 3) & ~3;
    const unsigned char *src = (const unsigned char *)img->data_p
                                + (unsigned int)stride * (img->h - 1);
    for (int y = 0; y < img->h; y++) {
        fwrite(src, 1, img->w, f);
        src -= stride;
    }
    fclose(f);
}

/* ---- Save LBM ---- */
void SaveLbm(void)
{
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;

    PAL *pal = get_pal(img->palnum);
    if (!pal || !pal->data_p) return;

    FILE *f = fopen(fnametmp_s, "wb");
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
void LoadTga(void)
{
    FILE *f = fopen(fnametmp_s, "rb");
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
void LoadLbm(void)
{
    FILE *f = fopen(fnametmp_s, "rb");
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
}

/* ---- PNG Import ---- */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

void ImportPng(const char *path)
{
    int w, h, channels;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);
    if (!data || w == 0 || h == 0) return;

    struct ColorCount { unsigned int rgb; int count; unsigned char idx; };
    ColorCount hist[4096] = {};
    int hist_count = 0;

    for (int i = 0; i < w * h; i++) {
        unsigned char r = data[i * 4], g = data[i * 4 + 1], b = data[i * 4 + 2], a = data[i * 4 + 3];
        if (a < 128) continue;
        unsigned int rgb = ((unsigned int)(r & 0xF8) << 7) | ((unsigned int)(g & 0xFC) << 2) | ((unsigned int)(b >> 3));
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
            unsigned int rgb = ((unsigned int)(p[0] & 0xF8) << 7) | ((unsigned int)(p[1] & 0xFC) << 2) | ((unsigned int)(p[2] >> 3));
            for (int j = 0; j < pal_colors; j++) {
                if (hist[j].rgb == rgb) { ((unsigned char *)img->data_p)[y * stride + x] = hist[j].idx; break; }
            }
        }
    }
    stbi_image_free(data);
    if (imgcnt > 0) ilselected = (int)imgcnt - 1;
    g_img_tex_idx = -2;
}

/* ---- PNG Export ---- */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

void ExportPng(const char *path)
{
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;
    PAL *pal = get_pal(img->palnum);
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
