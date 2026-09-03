/*************************************************************
 * platform/digitize_import.cpp
 * "Import Digitized Frame(s)" dialog.
 *
 * Glue over four pure modules, in pipeline order: digitize_matte.h keys and
 * decontaminates each frame against a shared backdrop color, material_mask.h
 * segments the REFERENCE frame (frame 0) into materials by hand and every
 * other frame automatically by remembered hue, palette_ramp.h fits one ramp
 * per material from frame 0's pixels, and ramp_remap.h assembles those ramps
 * into a palette and remaps every frame onto it. This file owns none of that
 * logic — it owns loading files, tracking per-frame state, drawing the
 * preview, and turning mouse clicks into calls into those modules.
 *************************************************************/
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include "digitize_import.h"
#include "ui_internal.h"
#include "digitize_matte.h"
#include "material_mask.h"
#include "palette_ramp.h"
#include "ramp_remap.h"
#include "img_format.h"
#include "img_io.h"
#include "palette_math.h"
#include "anipoint.h"
#include "ui_undo.h"
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>

/* ---- Session state --------------------------------------------------- */

enum DigitizeView { ViewSource = 0, ViewMatte, ViewMaterials, ViewResult };

struct DigitizeFrame {
    std::string path;
    std::string name;

    int src_w = 0, src_h = 0;
    std::vector<unsigned char> src_rgba;    /* w*h*4, straight alpha */

    int work_w = 0, work_h = 0;
    std::vector<unsigned char> work_rgb;    /* w*h*3, decontaminated + downscaled */
    std::vector<unsigned char> work_alpha;  /* w*h, binary 0/255 after MatteBinarize */
    std::vector<unsigned char> work_mask;   /* w*h, material ids */
    std::vector<unsigned char> indices;     /* w*h, final palette indices once ramps are fit */

    bool matte_done = false;
    MatteStats last_stats{};
};

struct DigitizeMaterial {
    unsigned char id = 0;
    unsigned char r = 128, g = 128, b = 128;   /* representative color */
    char name[24] = "";
    int ramp_count = 24;
    int fit_mode = 0;   /* 0 = auto, 1 = linear, 2 = lloyd */

    /* Match-Existing-Palette mode only: which existing palette index range
       this material is pinned to. Ignored in New-Palette mode. */
    int match_start = 0;
    int match_count = 0;
};

enum DigitizePaletteMode { PaletteMode_New = 0, PaletteMode_MatchExisting };

struct DigitizeSession {
    bool open = false;
    std::vector<DigitizeFrame> frames;
    int active_frame = 0;

    MatteParams key = MatteParamsDefault();
    float scale_pct = 100.0f;
    int alpha_threshold = 128;
    int shrink = 0;

    std::vector<DigitizeMaterial> materials;
    unsigned char next_material_id = 1;
    int active_material = -1;
    bool erase_mode = false;

    MaterialSeedParams seed = MaterialSeedParamsDefault();

    std::vector<MaterialRamp> fitted_ramps;
    AssembledPalette assembled{};

    DigitizeView view_mode = ViewSource;

    DigitizePaletteMode palette_mode = PaletteMode_New;
    int match_target_pal = -1;   /* index into g_doc's existing palettes */
};

static DigitizeSession g_session;

static const ImU32 kMaterialColors[8] = {
    IM_COL32(255,120,120,255), IM_COL32(120,190,255,255), IM_COL32(150,255,150,255),
    IM_COL32(255,210,110,255), IM_COL32(220,140,255,255), IM_COL32(120,230,220,255),
    IM_COL32(255,150,200,255), IM_COL32(200,200,120,255),
};

/* ---- Helpers ----------------------------------------------------------- */

static std::string BaseNameNoExt(const std::string &path)
{
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    if (base.size() > 15) base = base.substr(0, 15);
    return base.empty() ? std::string("DIGI") : base;
}

static void RecomputeMatteForFrame(int idx)
{
    if (idx < 0 || idx >= (int)g_session.frames.size()) return;
    DigitizeFrame &f = g_session.frames[idx];
    if (f.src_rgba.empty()) return;

    std::vector<unsigned char> full_rgb((size_t)f.src_w * f.src_h * 3);
    std::vector<unsigned char> full_alpha((size_t)f.src_w * f.src_h);
    MatteExtract(f.src_rgba.data(), f.src_w, f.src_h, g_session.key,
                full_rgb.data(), full_alpha.data(), &f.last_stats);

    int tw = (int)std::lround(f.src_w * (double)g_session.scale_pct / 100.0);
    int th = (int)std::lround(f.src_h * (double)g_session.scale_pct / 100.0);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    f.work_rgb.assign((size_t)tw * th * 3, 0);
    f.work_alpha.assign((size_t)tw * th, 0);
    MatteDownscale(full_rgb.data(), full_alpha.data(), f.src_w, f.src_h,
                   f.work_rgb.data(), f.work_alpha.data(), tw, th);

    std::vector<unsigned char> bin((size_t)tw * th);
    MatteBinarize(f.work_alpha.data(), tw, th, g_session.alpha_threshold,
                  g_session.shrink, bin.data());
    for (size_t i = 0; i < bin.size(); i++) f.work_alpha[i] = bin[i] ? 255 : 0;

    f.work_w = tw; f.work_h = th;
    f.work_mask.assign((size_t)tw * th, 0);
    f.indices.clear();
    f.matte_done = true;
}

/* Wipes everything downstream of the key/downscale settings: every frame's
   matte, frame 0's hand-seeded mask, and the fitted palette. A key change
   invalidates the colors segmentation was based on, so there is nothing
   honest to carry forward — the materials LIST (names, ids, slot counts)
   survives, so the user reseeds the same materials rather than rebuilding
   the list from scratch. */
static void InvalidateAllMatte(void)
{
    for (auto &f : g_session.frames) {
        f.matte_done = false;
        f.work_rgb.clear(); f.work_alpha.clear(); f.work_mask.clear();
        f.indices.clear();
    }
    g_session.assembled = AssembledPalette{};
    g_session.assembled.numc = 1;
    g_session.fitted_ramps.clear();
}

/* Frame 0's mask is hand-seeded (never touched here). Every other frame is
   reclassified fresh each call from the materials' current reference colors
   — see material_mask.h for why a spatial mask can't cross frames instead.
   Whichever mask a frame ends up with is then remapped through the current
   assembled palette, if one has been fit yet. */
static void ProcessFrame(int idx)
{
    if (idx < 0 || idx >= (int)g_session.frames.size()) return;
    DigitizeFrame &f = g_session.frames[idx];
    if (!f.matte_done) RecomputeMatteForFrame(idx);
    if (!f.matte_done) return;

    if (idx != 0 && !g_session.materials.empty()) {
        std::vector<MaterialRef> refs;
        refs.reserve(g_session.materials.size());
        for (auto &m : g_session.materials) refs.push_back({ m.id, m.r, m.g, m.b });
        f.work_mask.assign((size_t)f.work_w * f.work_h, 0);
        ClassifyMaterialsByHue(f.work_rgb.data(), f.work_alpha.data(), f.work_w, f.work_h,
                              refs.data(), (int)refs.size(), g_session.seed, f.work_mask.data());
    }

    if (g_session.assembled.span_count > 0) {
        f.indices.assign((size_t)f.work_w * f.work_h, 0);
        RemapFrameToRamps(f.work_rgb.data(), f.work_alpha.data(), f.work_mask.data(),
                          f.work_w, f.work_h, g_session.assembled, f.indices.data());
    } else {
        f.indices.clear();
    }
}

static void BuildHistogramForMaterial(const unsigned char *rgb, const unsigned char *mask,
                                      int w, int h, unsigned char id,
                                      std::vector<RampSample> &out)
{
    out.clear();
    static unsigned int hist[32768];
    std::memset(hist, 0, sizeof(hist));
    const size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) {
        if (mask[i] != id) continue;
        const int r5 = rgb[i*3+0] >> 3, g5 = rgb[i*3+1] >> 3, b5 = rgb[i*3+2] >> 3;
        hist[(r5 << 10) | (g5 << 5) | b5]++;
    }
    for (int c = 0; c < 32768; c++)
        if (hist[c]) out.push_back({ (unsigned short)c, hist[c] });
}

/* Fit one ramp per material from frame 0's segmented pixels, assemble the
   palette, and immediately reprocess every frame (including 0) so the whole
   batch's preview reflects the freshly fit palette right away. */
static void FitAllRamps(void)
{
    if (g_session.materials.empty() || g_session.frames.empty()) return;
    ProcessFrame(0);
    if (!g_session.frames[0].matte_done) return;

    DigitizeFrame &ref = g_session.frames[0];
    std::vector<MaterialRamp> ramps;
    ramps.reserve(g_session.materials.size());

    for (auto &m : g_session.materials) {
        std::vector<RampSample> hist;
        BuildHistogramForMaterial(ref.work_rgb.data(), ref.work_mask.data(),
                                  ref.work_w, ref.work_h, m.id, hist);

        MaterialRamp mr{};
        mr.material_id = m.id;
        mr.count = 0;

        if (!hist.empty()) {
            const int want = std::min(m.ramp_count, RAMP_REMAP_MAX_RAMP_COLORS);
            RampColor lin[RAMP_REMAP_MAX_RAMP_COLORS], llo[RAMP_REMAP_MAX_RAMP_COLORS];
            int nlin = 0, nllo = 0;
            if (m.fit_mode != 2)
                nlin = FitRampLinear(hist.data(), (int)hist.size(), want, 0.005, 0.995, lin);
            if (m.fit_mode != 1)
                nllo = FitRampLloyd(hist.data(), (int)hist.size(), want, 0.005, 0.995, 0, llo);

            const RampColor *chosen = lin;
            int nchosen = nlin;
            if (m.fit_mode == 2) {
                chosen = llo; nchosen = nllo;
            } else if (m.fit_mode == 0 && nllo > 0) {
                const double rl = nlin > 0
                    ? RampPopulationRMS(hist.data(), (int)hist.size(), lin, nlin) : 1e18;
                const double rL = RampPopulationRMS(hist.data(), (int)hist.size(), llo, nllo);
                if (rL <= rl) { chosen = llo; nchosen = nllo; }
            }
            mr.count = std::min(nchosen, RAMP_REMAP_MAX_RAMP_COLORS);
            for (int i = 0; i < mr.count; i++) mr.colors[i] = chosen[i];
        }
        ramps.push_back(mr);

        unsigned char r, g, b;
        if (MaterialMeanColor(ref.work_rgb.data(), ref.work_mask.data(),
                              ref.work_w, ref.work_h, m.id, &r, &g, &b)) {
            m.r = r; m.g = g; m.b = b;
        }
    }

    g_session.fitted_ramps = ramps;
    AssemblePalette(ramps.data(), (int)ramps.size(), &g_session.assembled);
    for (size_t i = 0; i < g_session.frames.size(); i++) ProcessFrame((int)i);
}

static std::vector<unsigned short> DecodePalWords(const PAL *pal)
{
    std::vector<unsigned short> words;
    if (!pal || !pal->data_p || pal->numc == 0) return words;
    words.resize(pal->numc);
    const unsigned char *pb = (const unsigned char *)pal->data_p;
    for (int i = 0; i < pal->numc; i++)
        words[i] = (unsigned short)(pb[i*2] | (pb[i*2+1] << 8));
    return words;
}

/* Replace the materials list with one entry per block DetectPaletteRampBlocks
   proposes in the chosen target palette — a starting point the user seeds
   pixels into and can still hand-adjust the start/count of before matching. */
static void DetectBlocksFromTarget(void)
{
    if (g_session.match_target_pal < 0) return;
    PAL *target = get_pal(g_session.match_target_pal);
    std::vector<unsigned short> words = DecodePalWords(target);
    if (words.empty()) return;

    PaletteRampBlock blocks[RAMP_REMAP_MAX_MATERIALS];
    int n = DetectPaletteRampBlocks(words.data(), (int)words.size(), g_session.seed,
                                    blocks, RAMP_REMAP_MAX_MATERIALS);

    g_session.materials.clear();
    if (g_session.frames[0].matte_done)
        std::fill(g_session.frames[0].work_mask.begin(), g_session.frames[0].work_mask.end(), 0);

    for (int i = 0; i < n; i++) {
        DigitizeMaterial m;
        m.id = g_session.next_material_id++;
        snprintf(m.name, sizeof(m.name), "Block %d", i + 1);
        m.match_start = blocks[i].start;
        m.match_count = blocks[i].count;
        m.ramp_count = blocks[i].count;
        const int mid_idx = blocks[i].start + blocks[i].count / 2;
        const int r5 = (words[mid_idx] >> 10) & 0x1F, g5 = (words[mid_idx] >> 5) & 0x1F, b5 = words[mid_idx] & 0x1F;
        m.r = (unsigned char)((r5 << 3) | (r5 >> 2));
        m.g = (unsigned char)((g5 << 3) | (g5 >> 2));
        m.b = (unsigned char)((b5 << 3) | (b5 >> 2));
        g_session.materials.push_back(m);
    }
    g_session.active_material = g_session.materials.empty() ? -1 : 0;
    g_session.assembled = AssembledPalette{};
    g_session.assembled.numc = 1;
}

/* Match-mode counterpart to FitAllRamps: instead of inventing new colors,
   pin each material to the existing palette range it was assigned and let
   RemapFrameToRamps pick nearest-luminance within that range, same as it
   would for a freshly fit one. */
static void MatchAllToExisting(void)
{
    if (g_session.materials.empty() || g_session.frames.empty()) return;
    if (g_session.match_target_pal < 0) return;
    PAL *target = get_pal(g_session.match_target_pal);
    std::vector<unsigned short> words = DecodePalWords(target);
    if (words.empty()) return;

    ProcessFrame(0);
    if (!g_session.frames[0].matte_done) return;

    std::vector<AssembledPalette::Span> spans;
    spans.reserve(g_session.materials.size());
    for (auto &m : g_session.materials)
        spans.push_back({ m.id, m.match_start, m.match_count });

    AssembleFromExistingPalette(words.data(), (int)words.size(),
                                spans.data(), (int)spans.size(), &g_session.assembled);

    DigitizeFrame &ref = g_session.frames[0];
    for (auto &m : g_session.materials) {
        unsigned char r, g, b;
        if (MaterialMeanColor(ref.work_rgb.data(), ref.work_mask.data(), ref.work_w, ref.work_h,
                              m.id, &r, &g, &b)) {
            m.r = r; m.g = g; m.b = b;
        }
    }
    for (size_t i = 0; i < g_session.frames.size(); i++) ProcessFrame((int)i);
}

static void CommitImport(void)
{
    if (g_session.assembled.span_count <= 0) return;
    if (g_session.palette_mode == PaletteMode_MatchExisting && g_session.match_target_pal < 0) return;
    if (!doc_undo_push()) return;

    unsigned short pal_idx;
    if (g_session.palette_mode == PaletteMode_MatchExisting) {
        /* No new PAL — the new frames point straight at the palette they
           were matched onto, so they sit right alongside its existing art. */
        pal_idx = (unsigned short)g_session.match_target_pal;
    } else {
        PAL *pal = AllocPal();
        if (!pal) return;
        pal->flags   = 0;
        pal->numc    = (unsigned short)g_session.assembled.numc;
        pal->bitspix = (unsigned char)PaletteBppForColorCount(g_session.assembled.numc);
        pal->data_p  = PoolAlloc((size_t)g_session.assembled.numc * 2);
        if (pal->data_p) {
            unsigned char *pb = (unsigned char *)pal->data_p;
            for (int i = 0; i < g_session.assembled.numc; i++) {
                pb[i*2+0] = (unsigned char)(g_session.assembled.words[i] & 0xFF);
                pb[i*2+1] = (unsigned char)(g_session.assembled.words[i] >> 8);
            }
        }
        std::string pname = (g_session.frames.empty() ? std::string("DIGI") : g_session.frames[0].name) + "P";
        snprintf(pal->n_s, sizeof(pal->n_s), "%.9s", pname.c_str());
        pal_idx = (unsigned short)(g_doc->palcnt - 1);
    }

    int created = 0, first_idx = -1;

    for (auto &f : g_session.frames) {
        if (f.indices.empty() || f.work_w <= 0 || f.work_h <= 0) continue;
        IMG *img = AllocImg();
        if (!img) continue;
        img->w = (unsigned short)f.work_w;
        img->h = (unsigned short)f.work_h;
        img->palnum = pal_idx;
        img->flags = 0;
        img->anix = 0; img->aniy = 0;
        clear_secondary_anipoint(img);
        img->pttbl_p = NULL;
        img->opals = (unsigned short)-1;

        const int stride = (f.work_w + 3) & ~3;
        img->data_p = PoolAlloc((size_t)stride * f.work_h);
        if (!img->data_p) continue;
        unsigned char *dst = (unsigned char *)img->data_p;
        std::memset(dst, 0, (size_t)stride * f.work_h);
        for (int y = 0; y < f.work_h; y++)
            std::memcpy(dst + (size_t)y * stride, f.indices.data() + (size_t)y * f.work_w, f.work_w);

        strncpy(img->n_s, f.name.c_str(), 15);
        img->n_s[15] = '\0';
        created++;
        if (first_idx < 0) first_idx = (int)g_doc->imgcnt - 1;
    }

    if (created > 0) {
        if (first_idx >= 0) g_doc->ilselected = first_idx;
        g_img_tex_idx = -2;
        g_zoom_reset = true;
        mark_dirty();
        if (g_session.palette_mode == PaletteMode_MatchExisting) {
            PAL *target = get_pal(g_session.match_target_pal);
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Imported %d digitized frame(s) matched onto palette %.9s.",
                     created, target ? target->n_s : "");
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Imported %d digitized frame(s) sharing a new %d-color palette.",
                     created, g_session.assembled.numc);
        }
        g_restore_msg_timer = 4.0f;
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Nothing to import — no frame had a mapped palette yet.");
        g_restore_msg_timer = 4.0f;
    }
    g_session.open = false;
}

void OpenDigitizeImportDialog(const std::vector<std::string> &paths)
{
    g_session = DigitizeSession();

    for (const auto &p : paths) {
        int w = 0, h = 0, ch = 0;
        unsigned char *data = stbi_load(p.c_str(), &w, &h, &ch, 4);
        if (!data) continue;
        DigitizeFrame f;
        f.path = p;
        f.name = BaseNameNoExt(p);
        f.src_w = w; f.src_h = h;
        f.src_rgba.assign(data, data + (size_t)w * h * 4);
        stbi_image_free(data);
        g_session.frames.push_back(std::move(f));
    }

    if (g_session.frames.empty()) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No frames could be loaded.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    ProcessFrame(0);
    g_session.open = true;
}

/* ---- Preview ------------------------------------------------------------ */

static std::vector<unsigned char> g_view_rgb, g_view_alpha;
static int g_view_w = 0, g_view_h = 0;

static void RebuildViewBuffers(void)
{
    if (g_session.frames.empty() ||
        g_session.active_frame < 0 || g_session.active_frame >= (int)g_session.frames.size()) {
        g_view_w = g_view_h = 0;
        return;
    }
    DigitizeFrame &f = g_session.frames[g_session.active_frame];

    if (g_session.view_mode == ViewSource) {
        g_view_w = f.src_w; g_view_h = f.src_h;
        const size_t n = (size_t)g_view_w * g_view_h;
        g_view_rgb.assign(n * 3, 0);
        g_view_alpha.assign(n, 255);
        for (size_t i = 0; i < n; i++) {
            g_view_rgb[i*3+0] = f.src_rgba[i*4+0];
            g_view_rgb[i*3+1] = f.src_rgba[i*4+1];
            g_view_rgb[i*3+2] = f.src_rgba[i*4+2];
        }
        return;
    }

    if (!f.matte_done) { g_view_w = g_view_h = 0; return; }
    g_view_w = f.work_w; g_view_h = f.work_h;
    const size_t n = (size_t)g_view_w * g_view_h;

    if (g_session.view_mode == ViewMatte) {
        g_view_rgb = f.work_rgb;
        g_view_alpha = f.work_alpha;
        return;
    }

    if (g_session.view_mode == ViewMaterials) {
        g_view_rgb.assign(n * 3, 0);
        g_view_alpha = f.work_alpha;
        for (size_t i = 0; i < n; i++) {
            unsigned char r = f.work_rgb[i*3+0], g = f.work_rgb[i*3+1], b = f.work_rgb[i*3+2];
            const unsigned char mid = f.work_mask[i];
            if (mid != 0) {
                int ci = -1;
                for (size_t k = 0; k < g_session.materials.size(); k++)
                    if (g_session.materials[k].id == mid) { ci = (int)k; break; }
                if (ci >= 0) {
                    ImVec4 wc = ImGui::ColorConvertU32ToFloat4(kMaterialColors[ci % 8]);
                    r = (unsigned char)(r * 0.4f + wc.x * 255.0f * 0.6f);
                    g = (unsigned char)(g * 0.4f + wc.y * 255.0f * 0.6f);
                    b = (unsigned char)(b * 0.4f + wc.z * 255.0f * 0.6f);
                }
            }
            g_view_rgb[i*3+0] = r; g_view_rgb[i*3+1] = g; g_view_rgb[i*3+2] = b;
        }
        return;
    }

    /* ViewResult */
    g_view_rgb.assign(n * 3, 0);
    g_view_alpha.assign(n, 0);
    for (size_t i = 0; i < n; i++) {
        const unsigned char idx = i < f.indices.size() ? f.indices[i] : 0;
        if (idx == 0) continue;
        const unsigned short w15 = g_session.assembled.words[idx];
        const int r5 = (w15 >> 10) & 0x1F, g5 = (w15 >> 5) & 0x1F, b5 = w15 & 0x1F;
        g_view_rgb[i*3+0] = (unsigned char)((r5 << 3) | (r5 >> 2));
        g_view_rgb[i*3+1] = (unsigned char)((g5 << 3) | (g5 >> 2));
        g_view_rgb[i*3+2] = (unsigned char)((b5 << 3) | (b5 >> 2));
        g_view_alpha[i] = 255;
    }
}

static SDL_Texture *g_preview_tex = nullptr;
static int g_preview_tex_w = 0, g_preview_tex_h = 0;

static SDL_Texture *BuildPreviewTexture(const unsigned char *rgb, const unsigned char *alpha, int w, int h)
{
    if (w <= 0 || h <= 0) return nullptr;
    if (!g_preview_tex || g_preview_tex_w != w || g_preview_tex_h != h) {
        if (g_preview_tex) SDL_DestroyTexture(g_preview_tex);
        g_preview_tex = SDL_CreateTexture(g_imgui_renderer, SDL_PIXELFORMAT_ARGB8888,
                                          SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureBlendMode(g_preview_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_preview_tex, SDL_ScaleModeNearest);
        g_preview_tex_w = w; g_preview_tex_h = h;
    }
    void *pixels; int pitch;
    if (SDL_LockTexture(g_preview_tex, NULL, &pixels, &pitch) != 0) return g_preview_tex;
    Uint32 *dst = (Uint32 *)pixels;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const size_t i = (size_t)y * w + x;
            const Uint32 a = alpha[i];
            dst[y * (pitch / 4) + x] =
                (a << 24) | ((Uint32)rgb[i*3+0] << 16) | ((Uint32)rgb[i*3+1] << 8) | rgb[i*3+2];
        }
    }
    SDL_UnlockTexture(g_preview_tex);
    return g_preview_tex;
}

static void DrawDigitizeCanvas(void)
{
    RebuildViewBuffers();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float pane_w = avail.x, pane_h = avail.y;
    if (pane_w < 64.0f) pane_w = 64.0f;
    if (pane_h < 64.0f) pane_h = 64.0f;

    if (g_view_w <= 0 || g_view_h <= 0) {
        ImGui::TextDisabled("Nothing to show yet — set the key color and apply.");
        return;
    }

    float scale = pane_w / (float)g_view_w;
    float fit_y = pane_h / (float)g_view_h;
    if (fit_y < scale) scale = fit_y;
    if (scale > 8.0f) scale = 8.0f;
    if (scale < 0.05f) scale = 0.05f;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 img_max(origin.x + g_view_w * scale, origin.y + g_view_h * scale);

    const float kCheck = 8.0f;
    dl->PushClipRect(origin, img_max, true);
    for (float y = origin.y; y < img_max.y; y += kCheck) {
        for (float x = origin.x; x < img_max.x; x += kCheck) {
            bool odd = (((int)((x - origin.x) / kCheck) + (int)((y - origin.y) / kCheck)) & 1) != 0;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + kCheck, y + kCheck),
                              odd ? IM_COL32(58, 58, 62, 255) : IM_COL32(42, 42, 46, 255));
        }
    }
    dl->PopClipRect();

    SDL_Texture *tex = BuildPreviewTexture(g_view_rgb.data(), g_view_alpha.data(), g_view_w, g_view_h);
    if (tex) dl->AddImage((ImTextureID)(intptr_t)tex, origin, img_max);
    dl->AddRect(origin, img_max, IM_COL32(90, 90, 96, 255));

    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##digitize_canvas", ImVec2(g_view_w * scale, g_view_h * scale),
                           ImGuiButtonFlags_MouseButtonLeft);
    bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetIO().MousePos;

    int px = -1, py = -1;
    if (hovered) {
        px = (int)((mouse.x - origin.x) / scale);
        py = (int)((mouse.y - origin.y) / scale);
        if (px < 0 || px >= g_view_w || py < 0 || py >= g_view_h) { px = -1; py = -1; }
    }

    if (px >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        DigitizeFrame &f = g_session.frames[g_session.active_frame];
        if (g_session.view_mode == ViewSource) {
            const size_t i = (size_t)py * g_view_w + px;
            g_session.key.key_r = f.src_rgba[i*4+0];
            g_session.key.key_g = f.src_rgba[i*4+1];
            g_session.key.key_b = f.src_rgba[i*4+2];
            InvalidateAllMatte();
            ProcessFrame(g_session.active_frame);
        } else if (g_session.view_mode == ViewMaterials && g_session.active_frame == 0 && f.matte_done) {
            if (g_session.erase_mode) {
                FloodFillMaterial(f.work_rgb.data(), f.work_alpha.data(), f.work_w, f.work_h,
                                  px, py, g_session.seed, 0, f.work_mask.data());
            } else if (g_session.active_material >= 0 &&
                      g_session.active_material < (int)g_session.materials.size()) {
                DigitizeMaterial &m = g_session.materials[g_session.active_material];
                FloodFillMaterial(f.work_rgb.data(), f.work_alpha.data(), f.work_w, f.work_h,
                                  px, py, g_session.seed, m.id, f.work_mask.data());
                unsigned char r, g, b;
                if (MaterialMeanColor(f.work_rgb.data(), f.work_mask.data(), f.work_w, f.work_h,
                                      m.id, &r, &g, &b)) {
                    m.r = r; m.g = g; m.b = b;
                }
            }
        }
    }

    if (hovered && px >= 0) ImGui::SetTooltip("%d, %d", px, py);
}

/* ---- Dialog -------------------------------------------------------------- */

void DrawDigitizeImportDialog(void)
{
    if (g_session.open) ImGui::OpenPopup("Import Digitized Frame(s)");
    ImGui::SetNextWindowSize(ImVec2(860, 600), ImGuiCond_Once);
    if (!ImGui::BeginPopupModal("Import Digitized Frame(s)", &g_session.open,
                                ImGuiWindowFlags_NoSavedSettings))
        return;

    if (g_session.frames.empty()) {
        ImGui::TextUnformatted("No frames loaded.");
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            g_session.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    DigitizeFrame &active = g_session.frames[g_session.active_frame];

    ImGui::BeginDisabled(g_session.active_frame == 0);
    if (ImGui::ArrowButton("##prev_frame", ImGuiDir_Left)) {
        g_session.active_frame--;
        ProcessFrame(g_session.active_frame);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(g_session.active_frame >= (int)g_session.frames.size() - 1);
    if (ImGui::ArrowButton("##next_frame", ImGuiDir_Right)) {
        g_session.active_frame++;
        ProcessFrame(g_session.active_frame);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("Frame %d/%d: %.15s (%dx%d)",
               g_session.active_frame + 1, (int)g_session.frames.size(),
               active.name.c_str(), active.src_w, active.src_h);
    if (g_session.active_frame == 0)
        ImGui::TextDisabled("Frame 1 is the reference frame — materials are seeded here by hand.");

    ImGui::Separator();

    const float kSidePane = 300.0f;
    ImGui::BeginChild("##digitize_preview",
                      ImVec2(-kSidePane, -ImGui::GetFrameHeightWithSpacing() * 1.6f), true);

    const char *view_names[] = { "Source", "Matte", "Materials", "Result" };
    int view_i = (int)g_session.view_mode;
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("##view_mode", &view_i, view_names, 4))
        g_session.view_mode = (DigitizeView)view_i;
    ImGui::SameLine();
    if (g_session.view_mode == ViewSource)
        ImGui::TextDisabled("Click the backdrop to set the key color.");
    else if (g_session.view_mode == ViewMaterials && g_session.active_frame == 0)
        ImGui::TextDisabled(g_session.erase_mode ? "Click to erase a material region."
                                                  : "Click to flood-fill the selected material.");

    DrawDigitizeCanvas();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##digitize_side",
                      ImVec2(kSidePane - 8.0f, -ImGui::GetFrameHeightWithSpacing() * 1.6f), true);

    bool key_changed = false;
    ImGui::TextUnformatted("Key");
    ImGui::ColorButton("##key_swatch",
                       ImVec4(g_session.key.key_r / 255.0f, g_session.key.key_g / 255.0f,
                              g_session.key.key_b / 255.0f, 1.0f),
                       0, ImVec2(24, 24));
    ImGui::SameLine();
    ImGui::TextDisabled("%d, %d, %d", g_session.key.key_r, g_session.key.key_g, g_session.key.key_b);

    ImGui::SetNextItemWidth(-1.0f);
    key_changed |= ImGui::SliderInt("##tolerance", &g_session.key.key_tolerance, 0, 64, "Tolerance %d");
    ImGui::SetNextItemWidth(-1.0f);
    key_changed |= ImGui::SliderInt("##softness", &g_session.key.key_softness, 0, 128, "Softness %d");
    key_changed |= ImGui::Checkbox("Despill", &g_session.key.despill);
    if (g_session.key.despill) {
        ImGui::SetNextItemWidth(-1.0f);
        key_changed |= ImGui::SliderInt("##despill", &g_session.key.despill_strength, 0, 100, "Strength %d");
    }

    if (active.matte_done) {
        ImGui::TextDisabled("keyed %d  partial %d  opaque %d",
                            active.last_stats.keyed, active.last_stats.partial, active.last_stats.opaque);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Downscale");
    ImGui::SetNextItemWidth(-1.0f);
    key_changed |= ImGui::SliderFloat("##scale", &g_session.scale_pct, 10.0f, 100.0f, "Scale %.0f%%");
    ImGui::SetNextItemWidth(-1.0f);
    key_changed |= ImGui::SliderInt("##athresh", &g_session.alpha_threshold, 0, 255, "Alpha cut %d");
    ImGui::SetNextItemWidth(-1.0f);
    key_changed |= ImGui::SliderInt("##shrink", &g_session.shrink, 0, 3, "Edge shrink %d");
    if (active.matte_done)
        ImGui::TextDisabled("-> %dx%d", active.work_w, active.work_h);

    if (key_changed) {
        InvalidateAllMatte();
        ProcessFrame(g_session.active_frame);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Target Palette");
    {
        const char *pal_modes[] = { "New Palette", "Match Existing" };
        int mode_i = (int)g_session.palette_mode;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##palmode", &mode_i, pal_modes, 2)) {
            g_session.palette_mode = (DigitizePaletteMode)mode_i;
            g_session.assembled = AssembledPalette{};
            g_session.assembled.numc = 1;
        }
    }
    if (g_session.palette_mode == PaletteMode_MatchExisting) {
        PAL *target = get_pal(g_session.match_target_pal);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##target_pal", target ? target->n_s : "(choose a palette)")) {
            for (int i = 0; i < (int)g_doc->palcnt; i++) {
                PAL *p = get_pal(i);
                if (!p) continue;
                bool sel = (i == g_session.match_target_pal);
                if (ImGui::Selectable(p->n_s, sel)) g_session.match_target_pal = i;
            }
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(g_session.match_target_pal < 0);
        if (ImGui::Button("Detect Blocks From Target", ImVec2(-1.0f, 0.0f)))
            DetectBlocksFromTarget();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Replaces the material list below with one entry per\n"
                              "hue-consistent run already in the target palette.\n"
                              "A proposal, not ground truth — adjust start/count by hand\n"
                              "if a block looks wrong, then seed each on Frame 1.");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Materials");

    int delete_idx = -1;
    for (int i = 0; i < (int)g_session.materials.size(); i++) {
        ImGui::PushID(i);
        DigitizeMaterial &m = g_session.materials[i];
        ImGui::ColorButton("##swatch", ImVec4(m.r / 255.0f, m.g / 255.0f, m.b / 255.0f, 1.0f),
                           0, ImVec2(16, 16));
        ImGui::SameLine();
        bool selected = (g_session.active_material == i);
        if (ImGui::Selectable(m.name[0] ? m.name : "(material)", selected, 0, ImVec2(90, 0))) {
            g_session.active_material = i;
            g_session.erase_mode = false;
            g_session.view_mode = ViewMaterials;
        }
        ImGui::SameLine();
        if (g_session.palette_mode == PaletteMode_MatchExisting) {
            ImGui::SetNextItemWidth(45.0f);
            ImGui::DragInt("##mstart", &m.match_start, 0.3f, 0, 255);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(45.0f);
            ImGui::DragInt("##mcount", &m.match_count, 0.3f, 1, 64);
        } else {
            ImGui::SetNextItemWidth(50.0f);
            ImGui::SliderInt("##slots", &m.ramp_count, 2, RAMP_REMAP_MAX_RAMP_COLORS);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) delete_idx = i;

        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputText("##name", m.name, sizeof(m.name));
        if (g_session.palette_mode == PaletteMode_New) {
            ImGui::SameLine();
            const char *modes[] = { "Auto", "Linear", "Lloyd" };
            ImGui::SetNextItemWidth(80.0f);
            ImGui::Combo("##fitmode", &m.fit_mode, modes, 3);
        }
        ImGui::PopID();
    }
    if (delete_idx >= 0) {
        unsigned char dead_id = g_session.materials[delete_idx].id;
        if (g_session.frames[0].matte_done) {
            for (auto &px : g_session.frames[0].work_mask)
                if (px == dead_id) px = 0;
        }
        g_session.materials.erase(g_session.materials.begin() + delete_idx);
        g_session.active_material = -1;
    }

    if (g_session.palette_mode == PaletteMode_New &&
        ImGui::Button("+ Add Material", ImVec2(-1.0f, 0.0f))) {
        DigitizeMaterial m;
        m.id = g_session.next_material_id++;
        snprintf(m.name, sizeof(m.name), "Material %d", (int)m.id);
        g_session.materials.push_back(m);
        g_session.active_material = (int)g_session.materials.size() - 1;
        g_session.erase_mode = false;
        g_session.view_mode = ViewMaterials;
        g_session.active_frame = 0;
    }
    ImGui::Checkbox("Erase tool", &g_session.erase_mode);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("While on, clicking the Materials view clears whichever\n"
                          "material currently owns the clicked region.");

    ImGui::Spacing();
    bool fit_disabled = g_session.materials.empty() || !g_session.frames[0].matte_done ||
        (g_session.palette_mode == PaletteMode_MatchExisting && g_session.match_target_pal < 0);
    ImGui::BeginDisabled(fit_disabled);
    const char *fit_label = g_session.palette_mode == PaletteMode_MatchExisting
                            ? "Match to Target" : "Fit Ramps";
    if (ImGui::Button(fit_label, ImVec2(-1.0f, 0.0f))) {
        if (g_session.palette_mode == PaletteMode_MatchExisting) MatchAllToExisting();
        else FitAllRamps();
        g_session.view_mode = ViewResult;
    }
    ImGui::EndDisabled();
    if (g_session.assembled.span_count > 0) {
        ImGui::TextDisabled("%d / 256 colors, %d material(s)",
                            g_session.assembled.numc - 1, g_session.assembled.span_count);
    } else {
        ImGui::TextDisabled("Not yet fit.");
    }

    ImGui::EndChild();

    ImGui::Separator();
    ImGui::BeginDisabled(g_session.assembled.span_count <= 0);
    if (ImGui::Button("Import All", ImVec2(120, 0))) {
        CommitImport();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_session.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d frame(s) loaded", (int)g_session.frames.size());

    ImGui::EndPopup();
}
