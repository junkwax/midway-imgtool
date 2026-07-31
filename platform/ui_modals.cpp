#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <SDL.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cctype>
#include <cfloat>
#include <vector>
#include <algorithm>
#include <functional>
#include <regex>
#include <unordered_map>
#include <cmath>
#include "compat.h"
#ifdef _WIN32
#include <shlobj.h>
#include <commdlg.h>
#include <direct.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#else
#include <dirent.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#endif
#include "img_format.h"
#include "color_ops.h"
#include "image_ops.h"
#include "palette_math.h"
#include "ui_internal.h"
#include "ui_canvas.h"
#include "ui_timeline.h"
#include "ui_palette.h"
#include "ui_tools.h"
#include "world_render.h"
#include "anipoint.h"
#include "anipoint_edit.h"
#include "img_util.h"
#include "sprite_resize_ops.h"
#include "img_io.h"
#include "imgui_overlay.h"
#include "load2_verify.h"
#include "lod_parser.h"
#include "mk2_hitbox.h"
#include "mk2_fatality.h"
#include "ui_modals.h"

// Externs for globals accessed in this file
extern "C" {
extern char             exe_dir[];
extern struct SDL_Color g_palette[256];
}

// Make sure definitions moved are declared as non-static if accessed externally
std::vector<std::string> g_recent_files;

/* ---- ImGui Native File Dialog ---- */
bool g_show_file_dialog = false;
static FileDialogMode g_file_dialog_mode = FileDialogMode::OpenImg;
static char g_file_dialog_dir[1024] = "";
static char g_file_dialog_file[256] = "";
static std::vector<std::string> g_file_dialog_multi_files;
static std::string g_file_dialog_anchor_file;
static char g_lod_override_dir[1024] = "";
static std::string g_last_world_project_path;

static unsigned int g_tbl_base_address = 0x02000000;
static bool g_tbl_export_mk3_format = false;
static bool g_tbl_export_palette = false;
static bool g_tbl_export_pad_4bit = false;
static bool g_tbl_export_align_16bit = false;
static bool g_tbl_export_dual_bank = false;
static int  g_tbl_export_bank      = 0;
static int  g_irw_bpp             = 8;
static unsigned int g_irw_base_address = 0x02000000;
static bool g_irw_align_16bit     = true;
static int  g_gif_blend_mode      = GifBlend_Normal;
static int  g_gif_opacity_percent = 100;
static bool g_gif_import_all      = true;
static bool g_gif_trim_transparent = true;
static bool g_gif_export_loop = true;
static bool g_gif_export_pingpong = false;
static bool g_gif_export_align_anipoints = true;
static float g_gif_export_fps = 12.0f;
static int  g_sheet_bg_threshold  = 245;
static int  g_sheet_min_pixels    = 160;
static int  g_sheet_padding       = 2;
static bool g_sheet_crop          = true;
static char g_sheet_prefix[12]    = "FRAME";

static bool SaveWorldProjectFile(const char *path);
static bool LoadWorldProjectFile(const char *path);

static bool g_show_opacity_gradient = false;
static bool g_opacity_gradient_marked = false;
static int  g_opacity_gradient_direction = 2;
static int  g_opacity_gradient_start = 100;
static int  g_opacity_gradient_end = 0;
static bool g_opacity_gradient_content_bounds = true;
static bool g_opacity_gradient_trim = false;
static int  g_opacity_gradient_seed = 17;
static bool g_opacity_gradient_preview = true;

static bool g_show_inner_stroke = false;
static float g_inner_stroke_rgb[3] = { 0.52f, 0.25f, 0.84f };

static bool g_show_sprite_cleanup = false;
static int  g_sprite_cleanup_radius = 3;
static int  g_sprite_cleanup_similarity = 8;
static int  g_sprite_cleanup_min_similar = 1;
static int  g_sprite_cleanup_replacement_support = 4;
static int  g_sprite_cleanup_outlier = 10;
static bool g_sprite_cleanup_transparent = true;

struct SpriteCleanupPreview {
    SDL_Texture *current_tex;
    SDL_Texture *updated_tex;
    int current_w, current_h;
    int updated_w, updated_h;
    int image_idx;
    int changed_pixels;
    std::string key;
};
static SpriteCleanupPreview g_sprite_cleanup_preview = {NULL, NULL, 0, 0, 0, 0, -1, 0, ""};

static bool FileDialogSupportsMultiSelect(FileDialogMode mode)
{
    return mode == FileDialogMode::ImportPng ||
           mode == FileDialogMode::ImportPngMatch ||
           mode == FileDialogMode::ImportSpriteSheetMatch ||
           mode == FileDialogMode::ImportGif;
}

/* Group file-dialog modes into categories so each remembers its own last
   directory. Users tend to keep sprites, source PNGs, and TGA dumps in
   different folders — sharing one "last dir" was annoying for everyone. */
static const char *dialog_category_for_mode(FileDialogMode m)
{
    switch (m) {
        case FileDialogMode::OpenImg:
        case FileDialogMode::AppendImg:
        case FileDialogMode::SaveImg:
        case FileDialogMode::OpenLod:
        case FileDialogMode::WriteAniLst:
        case FileDialogMode::WriteTbl:
        case FileDialogMode::WriteIrw:        return "img";
        case FileDialogMode::ImportPng:
        case FileDialogMode::ImportPngMatch:
        case FileDialogMode::ImportSpriteSheetMatch:
        case FileDialogMode::ExportPng:
        case FileDialogMode::ExportWorldPng:
        case FileDialogMode::ExportWorldPngSeq: return "png";
        case FileDialogMode::ImportGif:
        case FileDialogMode::ExportGif:       return "gif";
        case FileDialogMode::ExportPalette:
        case FileDialogMode::ImportPalette:   return "palette";
        case FileDialogMode::LoadTga:
        case FileDialogMode::SaveTga:
        case FileDialogMode::ExportTga:       return "tga";
        case FileDialogMode::LoadLbm:
        case FileDialogMode::SaveLbm:
        case FileDialogMode::SaveMarkedLbm:   return "lbm";
        case FileDialogMode::LoadAsmAnim:
        case FileDialogMode::SaveAsmAnim:     return "asm";
        case FileDialogMode::LoadWorldProject:
        case FileDialogMode::SaveWorldProject:return "world";
    }
    return "img";
}

static const char *get_dialog_config_path(const char *category)
{
    /* Built lazily into a per-category static buffer so the returned pointer
       stays valid until the next call with a different category. */
    static char path[MAX_PATH] = "";
    static char last_cat[16]   = "";
    if (!category || !category[0]) category = "img";
    if (path[0] && strcmp(last_cat, category) == 0) return path;
    strncpy(last_cat, category, sizeof(last_cat) - 1);
    last_cat[sizeof(last_cat) - 1] = '\0';
#ifdef _WIN32
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        _snprintf(path, sizeof(path), "%s\\imgtool\\last_dir_%s.txt", appdata, category);
    } else {
        _snprintf(path, sizeof(path), "last_dir_%s.txt", category);
    }
#else
    _snprintf(path, sizeof(path), "%s/.imgtool_last_dir_%s",
        getenv("HOME") ? getenv("HOME") : ".", category);
#endif
    return path;
}

static void save_last_dir(const char *dir, FileDialogMode mode)
{
    if (!dir || !*dir) return;
#ifdef _WIN32
    char parent[MAX_PATH];
    _snprintf(parent, sizeof(parent), "%s\\imgtool",
        getenv("APPDATA") ? getenv("APPDATA") : ".");
    CreateDirectoryA(parent, NULL);
#endif
    FILE *f = fopen(get_dialog_config_path(dialog_category_for_mode(mode)), "w");
    if (f) { fprintf(f, "%s", dir); fclose(f); }
}

static void load_last_dir(char *dir, size_t dirsz, FileDialogMode mode)
{
    if (!dir || !dirsz) return;
    dir[0] = '\0';
    FILE *f = fopen(get_dialog_config_path(dialog_category_for_mode(mode)), "r");
    if (f) {
        if (fgets(dir, (int)dirsz, f)) {
            size_t len = strlen(dir);
            if (len > 0 && dir[len - 1] == '\n') dir[len - 1] = '\0';
        }
        fclose(f);
    }
}

/* Category-string overloads used by code paths that aren't routed through
   the FileDialogMode enum (e.g. the MK2 Browse button which calls the
   native Win32 picker directly). Reuses the same on-disk format and
   per-category file layout so users see a single coherent system. */
void save_last_dir_cat(const char *dir, const char *category)
{
    if (!dir || !*dir) return;
#ifdef _WIN32
    char parent[MAX_PATH];
    _snprintf(parent, sizeof(parent), "%s\\imgtool",
        getenv("APPDATA") ? getenv("APPDATA") : ".");
    CreateDirectoryA(parent, NULL);
#endif
    FILE *f = fopen(get_dialog_config_path(category), "w");
    if (f) { fprintf(f, "%s", dir); fclose(f); }
}

void load_last_dir_cat(char *dir, size_t dirsz, const char *category)
{
    if (!dir || !dirsz) return;
    dir[0] = '\0';
    FILE *f = fopen(get_dialog_config_path(category), "r");
    if (f) {
        if (fgets(dir, (int)dirsz, f)) {
            size_t len = strlen(dir);
            if (len > 0 && dir[len - 1] == '\n') dir[len - 1] = '\0';
        }
        fclose(f);
    }
}



static void FileDialogClearMultiSelection()
{
    g_file_dialog_multi_files.clear();
    g_file_dialog_anchor_file.clear();
}

static bool FileDialogHasMultiFile(const std::string &name)
{
    return std::find(g_file_dialog_multi_files.begin(),
                     g_file_dialog_multi_files.end(),
                     name) != g_file_dialog_multi_files.end();
}

static void FileDialogAddMultiFile(const std::string &name)
{
    if (!FileDialogHasMultiFile(name))
        g_file_dialog_multi_files.push_back(name);
}

static void FileDialogSetFocusedFile(const std::string &name)
{
    snprintf(g_file_dialog_file, sizeof(g_file_dialog_file), "%s", name.c_str());
}

static void FileDialogReplaceSelection(const std::string &name)
{
    g_file_dialog_multi_files.clear();
    FileDialogAddMultiFile(name);
    FileDialogSetFocusedFile(name);
    g_file_dialog_anchor_file = name;
}

static void FileDialogToggleSelection(const std::string &name)
{
    auto it = std::find(g_file_dialog_multi_files.begin(),
                        g_file_dialog_multi_files.end(),
                        name);
    if (it != g_file_dialog_multi_files.end())
        g_file_dialog_multi_files.erase(it);
    else
        g_file_dialog_multi_files.push_back(name);

    if (g_file_dialog_multi_files.empty())
        g_file_dialog_file[0] = '\0';
    else
        FileDialogSetFocusedFile(name);
    g_file_dialog_anchor_file = name;
}

static int FileDialogFindEntryIndex(const std::vector<FileEntry> &entries, const std::string &name)
{
    for (int i = 0; i < (int)entries.size(); i++) {
        if (!entries[i].is_dir && entries[i].name == name)
            return i;
    }
    return -1;
}

static void FileDialogSelectRange(const std::vector<FileEntry> &entries, int clicked_idx, bool append)
{
    if (clicked_idx < 0 || clicked_idx >= (int)entries.size() || entries[clicked_idx].is_dir)
        return;

    int anchor_idx = FileDialogFindEntryIndex(entries, g_file_dialog_anchor_file);
    if (anchor_idx < 0) {
        anchor_idx = clicked_idx;
        g_file_dialog_anchor_file = entries[clicked_idx].name;
    }

    if (!append)
        g_file_dialog_multi_files.clear();

    int lo = anchor_idx < clicked_idx ? anchor_idx : clicked_idx;
    int hi = anchor_idx > clicked_idx ? anchor_idx : clicked_idx;
    for (int i = lo; i <= hi; i++) {
        if (!entries[i].is_dir)
            FileDialogAddMultiFile(entries[i].name);
    }
    FileDialogSetFocusedFile(entries[clicked_idx].name);
}

static std::vector<std::string> FileDialogSelectedFiles()
{
    std::vector<std::string> files;
    if (FileDialogSupportsMultiSelect(g_file_dialog_mode) && !g_file_dialog_multi_files.empty())
        files = g_file_dialog_multi_files;
    else if (g_file_dialog_file[0])
        files.push_back(g_file_dialog_file);
    return files;
}

static void FileDialogSyncTypedFilename()
{
    if (!FileDialogSupportsMultiSelect(g_file_dialog_mode))
        return;

    FileDialogClearMultiSelection();
    if (g_file_dialog_file[0]) {
        g_file_dialog_multi_files.push_back(g_file_dialog_file);
        g_file_dialog_anchor_file = g_file_dialog_file;
    }
}

/* File-list sort key. Persists across dialog opens so the user keeps their
   preferred view. */
enum class FileSort { Name, Date, Size };
static FileSort g_file_sort     = FileSort::Name;
static bool     g_file_sort_desc = false; /* false = asc */

/* ---------------- Preview thumbnail for highlighted file ----------------
   Holds the most recent decode so we don't reparse every frame while the
   user hovers the same file. Limited to PNG / TGA today — IMG and LBM
   need their loaders refactored to not touch globals, which is a bigger
   change deferred to a later round. */
struct FilePreview {
    SDL_Texture *tex;
    int          w, h;
    std::string  path;     /* full path that produced the texture */
};
static FilePreview g_file_preview = {NULL, 0, 0, ""};
static FilePreview g_export_preview = {NULL, 0, 0, ""};

static void file_preview_clear(void)
{
    if (g_file_preview.tex) {
        SDL_DestroyTexture(g_file_preview.tex);
        g_file_preview.tex = NULL;
    }
    g_file_preview.w = g_file_preview.h = 0;
    g_file_preview.path.clear();
}

static void export_preview_clear(void)
{
    if (g_export_preview.tex) {
        SDL_DestroyTexture(g_export_preview.tex);
        g_export_preview.tex = NULL;
    }
    g_export_preview.w = g_export_preview.h = 0;
    g_export_preview.path.clear();
}

/* Build an SDL texture from a row-major RGBA buffer, scaled to fit inside
   max_side while preserving aspect ratio. Nearest-neighbor (matches the
   timeline-thumb path) so pixel art doesn't get blurred. */
static SDL_Texture *make_preview_texture(const unsigned char *rgba, int sw, int sh, int max_side)
{
    if (!rgba || sw <= 0 || sh <= 0 || !g_imgui_renderer) return NULL;
    int tw, th;
    if (sw >= sh) { tw = max_side; th = (int)((long long)max_side * sh / sw); if (th < 1) th = 1; }
    else          { th = max_side; tw = (int)((long long)max_side * sw / sh); if (tw < 1) tw = 1; }

    SDL_Texture *tex = SDL_CreateTexture(g_imgui_renderer, SDL_PIXELFORMAT_ABGR8888,
                                         SDL_TEXTUREACCESS_STREAMING, tw, th);
    if (!tex) return NULL;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
    void *pixels; int pitch;
    if (SDL_LockTexture(tex, NULL, &pixels, &pitch) != 0) { SDL_DestroyTexture(tex); return NULL; }
    Uint32 *dst = (Uint32 *)pixels;
    for (int y = 0; y < th; y++) {
        int sy = (int)((long long)y * sh / th);
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < tw; x++) {
            int sx_i = (int)((long long)x * sw / tw);
            if (sx_i >= sw) sx_i = sw - 1;
            const unsigned char *p = rgba + (sy * sw + sx_i) * 4;
            dst[y * (pitch / 4) + x] = ((Uint32)p[3] << 24) | ((Uint32)p[2] << 16) | ((Uint32)p[1] << 8) | (Uint32)p[0];
        }
    }
    SDL_UnlockTexture(tex);
    return tex;
}

static bool file_dialog_uses_image_export_preview(FileDialogMode mode)
{
    return mode == FileDialogMode::ExportPng ||
           mode == FileDialogMode::SaveTga ||
           mode == FileDialogMode::SaveLbm;
}

static void export_preview_refresh(void)
{
    IMG *img = (g_doc && g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) {
        export_preview_clear();
        return;
    }

    char key[160];
    snprintf(key, sizeof(key), "%p:%d:%p:%u:%u:%u:%u",
             (void *)g_doc, g_doc->ilselected, img->data_p,
             (unsigned)img->w, (unsigned)img->h, (unsigned)img->palnum,
             g_palette_sync_serial);
    if (g_export_preview.tex && g_export_preview.path == key) return;

    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    SDL_Texture *tex = NULL;
    if (BuildImageExportRgba(img, rgba, &w, &h))
        tex = make_preview_texture(rgba.data(), w, h, 192);

    export_preview_clear();
    if (tex) {
        int tw = 0, th = 0;
        SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
        g_export_preview.tex = tex;
        g_export_preview.w = tw;
        g_export_preview.h = th;
        g_export_preview.path = key;
    }
}

/* Decode a stb_image-supported file and build a preview texture. */
extern "C" unsigned char *stbi_load(const char *, int *, int *, int *, int);
extern "C" void stbi_image_free(void *);
static SDL_Texture *load_preview_png(const char *path, int max_side)
{
    int w, h, channels;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);
    if (!data) return NULL;
    SDL_Texture *tex = make_preview_texture(data, w, h, max_side);
    stbi_image_free(data);
    return tex;
}

/* Self-contained TGA preview. We don't call LoadTga because that one mutates
   global IMG / PAL lists. Supports the formats LoadTga supports (indexed
   8-bit, colormap 15/16/24-bit). */
#pragma pack(push, 1)
struct TgaPreviewHeader {
    unsigned char  id_len, cm_type, i_type;
    unsigned short cm_first, cm_length;
    unsigned char  cm_size;
    unsigned short x_origin, y_origin, width, height;
    unsigned char  bpp, descriptor;
};
#pragma pack(pop)
static SDL_Texture *load_preview_tga(const char *path, int max_side)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    TgaPreviewHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return NULL; }
    if (hdr.id_len > 0) fseek(f, hdr.id_len, SEEK_CUR);
    if (hdr.i_type != 1 || hdr.cm_type != 1 || hdr.bpp != 8 ||
        (hdr.cm_size != 15 && hdr.cm_size != 16 && hdr.cm_size != 24)) {
        fclose(f); return NULL;
    }
    int sw = hdr.width, sh = hdr.height;
    if (sw <= 0 || sh <= 0 || sw > 16384 || sh > 16384) { fclose(f); return NULL; }
    int ncolors = hdr.cm_length ? hdr.cm_length : 256;
    unsigned char palette[256][3] = {};
    for (int i = 0; i < ncolors && i < 256; i++) {
        if (hdr.cm_size == 24) {
            unsigned char rgb[3];
            if (fread(rgb, 1, 3, f) != 3) { fclose(f); return NULL; }
            palette[i][0] = rgb[2]; palette[i][1] = rgb[1]; palette[i][2] = rgb[0]; /* BGR -> RGB */
        } else {
            unsigned char w2[2];
            if (fread(w2, 1, 2, f) != 2) { fclose(f); return NULL; }
            unsigned short w15 = (unsigned short)(w2[0] | (w2[1] << 8));
            palette[i][0] = (unsigned char)(((w15 >> 10) & 0x1F) << 3);
            palette[i][1] = (unsigned char)(((w15 >>  5) & 0x1F) << 3);
            palette[i][2] = (unsigned char)(( w15        & 0x1F) << 3);
        }
    }
    /* TGA stores bottom-up by default; descriptor bit 5 set means top-down. */
    bool top_down = (hdr.descriptor & 0x20) != 0;
    std::vector<unsigned char> rgba((size_t)sw * sh * 4, 0);
    for (int y = 0; y < sh; y++) {
        int dy = top_down ? y : (sh - 1 - y);
        for (int x = 0; x < sw; x++) {
            int c = fgetc(f);
            if (c == EOF) { fclose(f); return NULL; }
            unsigned char ci = (unsigned char)c;
            unsigned char *p = &rgba[(dy * sw + x) * 4];
            if (ci == 0) { p[3] = 0; } /* index 0 = transparent */
            else { p[0] = palette[ci][0]; p[1] = palette[ci][1]; p[2] = palette[ci][2]; p[3] = 255; }
        }
    }
    fclose(f);
    return make_preview_texture(rgba.data(), sw, sh, max_side);
}

/* Refresh the preview for `path`, no-op if it's already cached. */
static void file_preview_refresh(const std::string &path)
{
    if (path.empty()) { file_preview_clear(); return; }
    if (path == g_file_preview.path && g_file_preview.tex) return; /* cached */

    /* Pick decoder by extension. */
    size_t dot = path.find_last_of('.');
    std::string ext;
    if (dot != std::string::npos) {
        ext = path.substr(dot + 1);
        for (auto &c : ext) c = (char)tolower((unsigned char)c);
    }
    SDL_Texture *tex = NULL;
    if (ext == "png")       tex = load_preview_png(path.c_str(), 192);
    else if (ext == "jpg" || ext == "jpeg") tex = load_preview_png(path.c_str(), 192);
    else if (ext == "gif")  tex = load_preview_png(path.c_str(), 192);
    else if (ext == "tga")  tex = load_preview_tga(path.c_str(), 192);
    /* IMG / LBM previews would require refactoring those loaders to write
       into a sandbox buffer rather than the global IMG / PAL lists. Saved
       for a follow-up. */

    file_preview_clear();
    if (tex) {
        int w, h;
        SDL_QueryTexture(tex, NULL, NULL, &w, &h);
        g_file_preview.tex  = tex;
        g_file_preview.w    = w;
        g_file_preview.h    = h;
        g_file_preview.path = path;
    }
}

void GetDirectoryFiles(const std::string& dir, std::vector<FileEntry>& entries, const char* ext_filter)
{
    entries.clear();
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    std::string search = dir;
    if (!search.empty() && search.back() != '\\' && search.back() != '/') search += "\\";
    if (ext_filter && ext_filter[0])
        search += std::string("*.") + ext_filter;
    else
        search += "*";
    HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0) continue;
            bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            long long sz = is_dir ? 0LL
                : ((long long)fd.nFileSizeHigh << 32) | (long long)fd.nFileSizeLow;
            /* FILETIME -> seconds since some epoch is fine for sort-only use. */
            long long mt = ((long long)fd.ftLastWriteTime.dwHighDateTime << 32) | (long long)fd.ftLastWriteTime.dwLowDateTime;
            entries.push_back({fd.cFileName, is_dir, sz, mt});
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    if (ext_filter && ext_filter[0]) {
        std::string dir_search = dir;
        if (!dir_search.empty() && dir_search.back() != '\\' && dir_search.back() != '/') dir_search += "\\";
        dir_search += "*";
        HANDLE hDir = FindFirstFileA(dir_search.c_str(), &fd);
        if (hDir != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") == 0) continue;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                bool dup = false;
                for (const auto& e : entries) { if (e.name == fd.cFileName) { dup = true; break; } }
                if (!dup) entries.push_back({fd.cFileName, true, 0LL, 0LL});
            } while (FindNextFileA(hDir, &fd));
            FindClose(hDir);
        }
    }
#else
    DIR* d = opendir(dir.empty() ? "." : dir.c_str());
    if (d) {
        struct dirent* dir_ent;
        while ((dir_ent = readdir(d)) != NULL) {
            if (strcmp(dir_ent->d_name, ".") == 0) continue;
            std::string full_path = dir;
            if (!full_path.empty() && full_path.back() != '/') full_path += "/";
            full_path += dir_ent->d_name;
            struct stat st = {};
            bool have_stat = (stat(full_path.c_str(), &st) == 0);
            bool is_dir = have_stat && S_ISDIR(st.st_mode);
            long long sz = is_dir ? 0LL : (have_stat ? (long long)st.st_size : 0LL);
            long long mt = have_stat ? (long long)st.st_mtime : 0LL;
            if (is_dir) {
                entries.push_back({dir_ent->d_name, true, 0LL, mt});
            } else if (ext_filter && ext_filter[0]) {
                const char* dot = strrchr(dir_ent->d_name, '.');
                if (dot && strcasecmp(dot + 1, ext_filter) == 0)
                    entries.push_back({dir_ent->d_name, false, sz, mt});
            } else {
                entries.push_back({dir_ent->d_name, false, sz, mt});
            }
        }
        closedir(d);
    }
#endif
}

std::string GetParentDirectory(const std::string& dir)
{
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
    {
        if (pos == 0) return dir.substr(0, 1);
#ifdef _WIN32
        if (pos == 2 && dir[1] == ':') return dir.substr(0, 3);
#endif
        return dir.substr(0, pos);
    }
    return dir;
}

std::string PathCombine(const std::string& dir, const std::string& file)
{
    if (dir.empty()) return file;
    char last = dir.back();
    if (last == '\\' || last == '/') return dir + file;
#ifdef _WIN32
    return dir + "\\" + file;
#else
    return dir + "/" + file;
#endif
}

/* ===== Recent files (most-recently-opened IMG files) =====
   Persisted as one absolute path per line in <exe_dir>/imgtool_recent.txt.
   Newest entry is at index 0. Capped at RECENT_MAX. */
extern "C" char exe_dir[];   /* defined in shim_file.c */
static const size_t RECENT_MAX = 8;


static std::string RecentFilesPath()
{
    std::string base = exe_dir[0] ? exe_dir : ".";
#ifdef _WIN32
    return base + "\\imgtool_recent.txt";
#else
    return base + "/imgtool_recent.txt";
#endif
}

void RecentLoad()
{
    g_recent_files.clear();
    FILE *f = fopen(RecentFilesPath().c_str(), "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f) && g_recent_files.size() < RECENT_MAX) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n) g_recent_files.push_back(line);
    }
    fclose(f);
}

void RecentSave()
{
    FILE *f = fopen(RecentFilesPath().c_str(), "w");
    if (!f) return;
    for (const std::string &p : g_recent_files) fprintf(f, "%s\n", p.c_str());
    fclose(f);
}

void RecentAdd(const std::string &full_path)
{
    auto it = std::find(g_recent_files.begin(), g_recent_files.end(), full_path);
    if (it != g_recent_files.end()) g_recent_files.erase(it);
    g_recent_files.insert(g_recent_files.begin(), full_path);
    if (g_recent_files.size() > RECENT_MAX) g_recent_files.resize(RECENT_MAX);
    RecentSave();
}

static std::string DocFullPath(const Document *doc)
{
    if (!doc || doc->fname_s[0] == '\0') return std::string();
    std::string dir(doc->fpath_s);
    std::string file(doc->fname_s);
    if (dir.empty()) return file;
    char last = dir[dir.size() - 1];
    if (last == '\\' || last == '/') return dir + file;
#ifdef _WIN32
    return dir + "\\" + file;
#else
    return dir + "/" + file;
#endif
}

static bool PathEqualsForPlatform(const std::string &a, const std::string &b)
{
#ifdef _WIN32
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char ca = (char)tolower((unsigned char)a[i]);
        char cb = (char)tolower((unsigned char)b[i]);
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca != cb) return false;
    }
    return true;
#else
    return a == b;
#endif
}

static int FindOpenDocumentByPath(const std::string &full_path)
{
    for (int i = 0; i < document_tab_count(); i++) {
        Document *doc = document_get(i);
        std::string doc_path = DocFullPath(doc);
        if (!doc_path.empty() && PathEqualsForPlatform(doc_path, full_path))
            return i;
    }
    return -1;
}

static bool DocumentCanReuseForOpen(Document *doc)
{
    return doc && !doc->dirty && doc->imgcnt == 0 && doc->palcnt == 0 &&
           doc->img2cnt == 0 && doc->fname_s[0] == '\0';
}

void ActivateDocumentTab(int idx)
{
    if (idx < 0 || idx >= document_tab_count()) return;
    if (idx != document_active_index()) {
        document_set_active(idx);
        /* A World View sequence deliberately spans open IMG documents.  Tab
           activation is then ordinary editing/navigation, not a new session;
           resetting the UI here used to discard its split/sequence staging. */
        if (!(g_world_state.enabled && g_world_marked_state.marked_play))
            ResetPerDocumentUiState(false);
    }
    g_doc_tab_select_request = idx;
    Mk2AutoSelectFromImg();
}

void PrepareDocumentForOpenedFile(void)
{
    if (!DocumentCanReuseForOpen(g_doc))
        document_new_tab();
    g_doc_tab_select_request = document_active_index();
    ResetPerDocumentUiState(false);
    ClearAll();
}

void SetActiveDocumentPath(const std::string &full_path)
{
    size_t sep = full_path.find_last_of("\\/");
    std::string dir  = (sep == std::string::npos) ? std::string(".") : full_path.substr(0, sep);
    std::string file = (sep == std::string::npos) ? full_path        : full_path.substr(sep + 1);

    size_t n_dir = dir.size();
    if (n_dir > sizeof(g_doc->fpath_s) - 1) n_dir = sizeof(g_doc->fpath_s) - 1;
    memset(g_doc->fpath_s, 0, sizeof(g_doc->fpath_s));
    memcpy(g_doc->fpath_s, dir.data(), n_dir);

    size_t n_file = file.size();
    if (n_file > 12) n_file = 12;
    memset(g_doc->fname_s, 0, 13);
    memset(g_doc->fnametmp_s, 0, 13);
    memcpy(g_doc->fname_s, file.data(), n_file);
    memcpy(g_doc->fnametmp_s, file.data(), n_file);
    for (size_t i = 0; i < n_file; i++) {
        g_doc->fname_s[i] = (char)toupper((unsigned char)g_doc->fname_s[i]);
        g_doc->fnametmp_s[i] = (char)toupper((unsigned char)g_doc->fnametmp_s[i]);
    }

    _chdir(dir.c_str());
}

/* Load an IMG by absolute path. Mirrors the Open-button branch of the file
   dialog, but now opens into its own document tab. A clean empty startup tab
   is reused; otherwise a new tab is created and activated. */
void OpenImgFile(const std::string &full_path)
{
    int existing = FindOpenDocumentByPath(full_path);
    if (existing >= 0) {
        ActivateDocumentTab(existing);
        RecentAdd(full_path);
        return;
    }

    PrepareDocumentForOpenedFile();
    SetActiveDocumentPath(full_path);
    LoadImgFile();
    g_dirty = false; /* fresh load = clean baseline */
    g_img_tex_idx = -2;
    RecentAdd(full_path);
    Mk2AutoSelectFromImg();
}

/* ===== Session restore (open IMG tabs from the last clean shutdown) =====
   Stored beside the MRU list so portable builds keep their state local to
   the executable folder. Only disk-backed IMG documents are persisted. */
static std::string SessionFilesPath()
{
    std::string base = exe_dir[0] ? exe_dir : ".";
#ifdef _WIN32
    return base + "\\imgtool_session.txt";
#else
    return base + "/imgtool_session.txt";
#endif
}

void SessionSave()
{
    FILE *f = fopen(SessionFilesPath().c_str(), "w");
    if (!f) return;

    std::vector<std::string> paths;
    int active_doc = document_active_index();
    int active_session_idx = -1;

    for (int i = 0; i < document_tab_count(); i++) {
        Document *doc = document_get(i);
        std::string path = DocFullPath(doc);
        if (!doc || path.empty() || doc->imgcnt == 0) continue;
        if (i == active_doc) active_session_idx = (int)paths.size();
        paths.push_back(path);
    }

    if (active_session_idx < 0 && !paths.empty()) active_session_idx = 0;
    fprintf(f, "active=%d\n", active_session_idx);
    for (const std::string &path : paths)
        fprintf(f, "%s\n", path.c_str());
    fclose(f);
}

static bool SessionLoad(std::vector<std::string> *paths, int *active_session_idx)
{
    if (paths) paths->clear();
    if (active_session_idx) *active_session_idx = 0;

    FILE *f = fopen(SessionFilesPath().c_str(), "r");
    if (!f) return false;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (!n) continue;

        if (strncmp(line, "active=", 7) == 0) {
            if (active_session_idx) *active_session_idx = (int)strtol(line + 7, NULL, 10);
            continue;
        }

        if (paths) paths->push_back(line);
    }

    fclose(f);
    return paths && !paths->empty();
}

static bool PathReadable(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

void SessionRestore()
{
    std::vector<std::string> paths;
    int active_session_idx = 0;
    if (!SessionLoad(&paths, &active_session_idx)) return;

    std::vector<std::string> recent_before = g_recent_files;
    std::vector<int> restored_tabs;
    for (const std::string &path : paths) {
        if (!PathReadable(path)) continue;

        OpenImgFile(path);
        int idx = FindOpenDocumentByPath(path);
        Document *doc = document_get(idx);
        if (!doc || doc->imgcnt == 0) continue;

        doc->dirty = false;
        restored_tabs.push_back(idx);
    }

    if (g_recent_files != recent_before) {
        g_recent_files = recent_before;
        RecentSave();
    }

    if (restored_tabs.empty()) return;
    if (active_session_idx < 0 || active_session_idx >= (int)restored_tabs.size())
        active_session_idx = 0;
    ActivateDocumentTab(restored_tabs[(size_t)active_session_idx]);
}

static const char* GetDialogExtension(FileDialogMode mode)
{
    switch (mode) {
        case FileDialogMode::OpenImg:
        case FileDialogMode::AppendImg:
        case FileDialogMode::SaveImg:   return "IMG";
        case FileDialogMode::OpenLod:   return "LOD";
        case FileDialogMode::LoadLbm:
        case FileDialogMode::SaveLbm:
        case FileDialogMode::SaveMarkedLbm: return "LBM";
        case FileDialogMode::LoadTga:
        case FileDialogMode::SaveTga:
        case FileDialogMode::ExportTga: return "TGA";
        case FileDialogMode::ImportPng:
        case FileDialogMode::ImportPngMatch:
        case FileDialogMode::ExportPng:
        case FileDialogMode::ExportWorldPng:
        case FileDialogMode::ExportWorldPngSeq: return "PNG";
        case FileDialogMode::ImportSpriteSheetMatch: return "";
        case FileDialogMode::ImportGif:
        case FileDialogMode::ExportGif: return "GIF";
        case FileDialogMode::ExportPalette: return g_palette_export_act ? "ACT" : "PAL";
        case FileDialogMode::ImportPalette: return "PAL";
        case FileDialogMode::WriteAniLst: return "ASM";
        case FileDialogMode::WriteTbl:  return "TBL";
        case FileDialogMode::WriteIrw:  return "IRW";
        case FileDialogMode::LoadAsmAnim:
        case FileDialogMode::SaveAsmAnim: return "ASM";
        case FileDialogMode::LoadWorldProject:
        case FileDialogMode::SaveWorldProject: return "WVP";
    }
    return "";
}

void OpenFileDialog(FileDialogMode mode) {
    /* For IMG-category modes, g_doc->fpath_s (set when an IMG is currently loaded)
       seeds the dialog so the user starts in the same dir as their open
       file. For other categories, g_doc->fpath_s is irrelevant — those have their
       own per-category remembered directory. Always re-load from disk on
       mode change so switching from Save IMG to Import PNG lands in the
       right folder. */
    const char *cat = dialog_category_for_mode(mode);
    bool is_img_cat = (strcmp(cat, "img") == 0);
    g_file_dialog_dir[0] = '\0';
    if (is_img_cat && g_doc->fpath_s[0] != '\0') {
        size_t n = 0;
        while (n < sizeof(g_doc->fpath_s) - 1 && g_doc->fpath_s[n] != '\0') n++;
        memcpy(g_file_dialog_dir, g_doc->fpath_s, n);
        g_file_dialog_dir[n] = '\0';
    } else {
        load_last_dir(g_file_dialog_dir, sizeof(g_file_dialog_dir), mode);
    }
    if (g_file_dialog_dir[0] == '\0') {
#ifdef _WIN32
        GetCurrentDirectoryA(sizeof(g_file_dialog_dir), g_file_dialog_dir);
#else
        if (getcwd(g_file_dialog_dir, sizeof(g_file_dialog_dir)) == NULL)
            g_file_dialog_dir[0] = '\0';
#endif
    }
    g_file_dialog_mode = mode;
    bool is_export = (mode == FileDialogMode::ExportTga || mode == FileDialogMode::SaveTga ||
                      mode == FileDialogMode::ExportPng || mode == FileDialogMode::ExportGif || mode == FileDialogMode::ExportPalette ||
                      mode == FileDialogMode::SaveLbm);
    if (mode == FileDialogMode::ExportPalette && g_doc->plselected >= 0) {
        PAL *pal = get_pal(g_doc->plselected);
        if (pal) {
            snprintf(g_file_dialog_file, sizeof(g_file_dialog_file), "%s.%s",
                     pal->n_s, GetDialogExtension(mode));
        }
    } else if (is_export && g_doc->ilselected >= 0) {
        IMG *img = get_img(g_doc->ilselected);
        if (img) {
            size_t n = 0;
            while (n < 16 && img->file_name_raw[n] != '\0') {
                g_file_dialog_file[n] = img->file_name_raw[n];
                n++;
            }
            g_file_dialog_file[n] = '\0';
            const char *ext = GetDialogExtension(mode);
            if (ext && ext[0] != '\0') {
                strcat(g_file_dialog_file, ".");
                strcat(g_file_dialog_file, ext);
            }
        }
    } else if (mode == FileDialogMode::ExportWorldPng ||
               mode == FileDialogMode::ExportWorldPngSeq) {
        /* The World View composite spans every marked tab, so the selected
           sprite's name would be misleading. Seed from the active document
           instead, which is at least the scene the user is looking at. */
        char stem[32] = "WORLD";
        if (g_doc->fname_s[0] != '\0') {
            size_t n = 0;
            while (n < 8 && g_doc->fname_s[n] != '\0' && g_doc->fname_s[n] != '.') {
                stem[n] = g_doc->fname_s[n];
                n++;
            }
            stem[n] = '\0';
            if (stem[0] == '\0') snprintf(stem, sizeof(stem), "WORLD");
        }
        snprintf(g_file_dialog_file, sizeof(g_file_dialog_file), "%s.PNG", stem);
    } else if (mode == FileDialogMode::SaveAsmAnim) {
        /* World View ASM is generated from the marked sprites across all open
           tabs, not from the current IMG document. Seeding g_doc->fname_s here
           produced a misleading default (the open IMG's name). Instead seed
           from the first generated animation label (e.g. a_imgtool_slot1_kick),
           falling back to a blank name if nothing has been generated yet. */
        g_file_dialog_file[0] = '\0';
        const std::string &asm_src = g_world_marked_state.generated_asm;
        for (size_t ls = 0; ls < asm_src.size(); ) {
            size_t le = asm_src.find('\n', ls);
            size_t line_end = (le == std::string::npos) ? asm_src.size() : le;
            size_t a = asm_src.find_first_not_of(" \t\r", ls);
            if (a != std::string::npos && a < line_end && asm_src[a] != ';') {
                size_t b = asm_src.find_first_of(" \t\r", a);
                if (b == std::string::npos || b > line_end) b = line_end;
                std::string label = asm_src.substr(a, b - a);
                snprintf(g_file_dialog_file, sizeof(g_file_dialog_file),
                         "%s.ASM", label.c_str());
                break;
            }
            if (le == std::string::npos) break;
            ls = le + 1;
        }
    } else if (mode == FileDialogMode::SaveWorldProject) {
        if (!g_last_world_project_path.empty()) {
            size_t sep = g_last_world_project_path.find_last_of("\\/");
            std::string dir = sep == std::string::npos ? std::string()
                                                       : g_last_world_project_path.substr(0, sep);
            std::string file = sep == std::string::npos ? g_last_world_project_path
                                                         : g_last_world_project_path.substr(sep + 1);
            snprintf(g_file_dialog_file, sizeof(g_file_dialog_file), "%s", file.c_str());
            if (!dir.empty())
                snprintf(g_file_dialog_dir, sizeof(g_file_dialog_dir), "%s", dir.c_str());
        } else {
            snprintf(g_file_dialog_file, sizeof(g_file_dialog_file), "world_view.WVP");
        }
    } else if (g_doc->fname_s[0] != '\0') {
        size_t n = 0;
        while (n < 12 && g_doc->fname_s[n] != '\0') n++;
        memcpy(g_file_dialog_file, g_doc->fname_s, n);
        g_file_dialog_file[n] = '\0';
    } else {
        g_file_dialog_file[0] = '\0';
    }
    FileDialogClearMultiSelection();
    file_preview_clear();
    export_preview_clear();
    if (FileDialogSupportsMultiSelect(mode)) {
        /* Import dialogs inherit their default name from the open IMG
           document, which is never one of the files they can import. Seeding
           the multi-selection with it meant a Ctrl-click batch carried a bogus
           first entry that silently failed to import. Only keep a seed that
           matches what this dialog actually lists. */
        const char *want_ext = GetDialogExtension(mode);
        bool seed_ok = false;
        if (g_file_dialog_file[0] && want_ext && want_ext[0]) {
            const char *dot = strrchr(g_file_dialog_file, '.');
            seed_ok = dot && _stricmp(dot + 1, want_ext) == 0;
        }
        if (!seed_ok) g_file_dialog_file[0] = '\0';
        if (g_file_dialog_file[0]) {
            g_file_dialog_multi_files.push_back(g_file_dialog_file);
            g_file_dialog_anchor_file = g_file_dialog_file;
        }
    }
    g_show_file_dialog = true;
}

/* Guarded entry points for "load a different file" operations. If there are
   unsaved changes, tabs let us avoid destructive replacement: Open creates a
   fresh document when needed, while Close/Quit still prompt per dirty tab. */
void RequestOpenDialog(void)
{
    OpenFileDialog(FileDialogMode::OpenImg);
}
void RequestOpenPath(const std::string &path)
{
    OpenImgFile(path);
}
void RequestOpenLodDialog(void)
{
    OpenFileDialog(FileDialogMode::OpenLod);
}

/* Drag-and-drop entry point. Extension dispatch:
     .img → RequestOpenPath (unsaved-changes guard + full reset)
     .tga → LoadTga import into the active document
     .lbm → LoadLbm import into the active document
     .png → ImportPng
     .gif → ImportGif
   Unknown extensions toast and return. */
extern "C" void imgui_overlay_open_path(const char *path)
{
    if (!path || !*path) return;
    std::string p = path;
    size_t dot = p.find_last_of('.');
    std::string ext;
    if (dot != std::string::npos) {
        ext = p.substr(dot + 1);
        for (char &c : ext) c = (char)tolower((unsigned char)c);
    }

    /* Import paths (TGA/LBM/PNG) need a document to import *into*. Drop on
       an empty workspace → bootstrap a fresh IMG, matching File → New. */
    auto ensure_new_doc_if_empty = []() {
        if (g_doc->imgcnt == 0) {
            ClearAll();
            g_doc->fileversion = 0x0634;
            g_doc->fname_s[0]  = 0;
            g_undo_count = 0;
            g_undo_idx   = 0;
        }
    };

    if (ext == "img") {
        RequestOpenPath(p);
    } else if (ext == "tga") {
        ensure_new_doc_if_empty();
        LoadTga(p.c_str());
        mark_dirty();
        g_img_tex_idx = -2;
    } else if (ext == "lbm") {
        ensure_new_doc_if_empty();
        LoadLbm(p.c_str());
        mark_dirty();
        g_img_tex_idx = -2;
    } else if (ext == "png") {
        ensure_new_doc_if_empty();
        /* Dropping PNGs used to always build a fresh palette per file, with no
           way to ask for a match. Honor the Import PNG dialog's setting here
           so both routes behave the same. */
        int match_pal = g_png_import_match_palette ? ResolveImportMatchPalette() : -1;
        if (match_pal >= 0) {
            PAL *target = get_pal(match_pal);
            if (ImportPngMatch(p.c_str(), match_pal)) {
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Imported dropped PNG matched to palette %.9s.",
                         target ? target->n_s : "");
                g_restore_msg_timer = 4.0f;
            }
        } else {
            ImportPng(p.c_str());
        }
        mark_dirty();
        g_img_tex_idx = -2;
    } else if (ext == "gif") {
        ensure_new_doc_if_empty();
        ImportGif(p.c_str(), g_gif_blend_mode, g_gif_opacity_percent, g_gif_import_all, g_gif_trim_transparent);
        mark_dirty();
        g_img_tex_idx = -2;
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Unsupported file type: .%s", ext.empty() ? "(none)" : ext.c_str());
        g_restore_msg_timer = 4.0f;
    }
}

void DrawFileDialog() {
    const char* title = "Open File";
    if (g_file_dialog_mode == FileDialogMode::SaveImg) title = "Save IMG File";
    else if (g_file_dialog_mode == FileDialogMode::ExportTga) title = "Export TGA";
    else if (g_file_dialog_mode == FileDialogMode::OpenImg) title = "Open IMG File";
    else if (g_file_dialog_mode == FileDialogMode::AppendImg) title = "Append IMG File";
    else if (g_file_dialog_mode == FileDialogMode::OpenLod) title = "Open LOD File";
    else if (g_file_dialog_mode == FileDialogMode::LoadLbm) title = "Load LBM File";
    else if (g_file_dialog_mode == FileDialogMode::SaveLbm) title = "Save LBM File";
    else if (g_file_dialog_mode == FileDialogMode::SaveMarkedLbm) title = "Save Marked LBM";
    else if (g_file_dialog_mode == FileDialogMode::LoadTga) title = "Load TGA File";
    else if (g_file_dialog_mode == FileDialogMode::SaveTga) title = "Save TGA File";
    else if (g_file_dialog_mode == FileDialogMode::ImportPng) title = "Import PNG File";
    else if (g_file_dialog_mode == FileDialogMode::ImportPngMatch) title = "Import PNG (Match Palette)";
    else if (g_file_dialog_mode == FileDialogMode::ImportSpriteSheetMatch) title = "Import Sprite Sheet (Match Palette)";
    else if (g_file_dialog_mode == FileDialogMode::ImportGif) title = "Import GIF File";
    else if (g_file_dialog_mode == FileDialogMode::ExportPng) title = "Export PNG File";
    else if (g_file_dialog_mode == FileDialogMode::ExportGif) title = "Export Animated GIF";
    else if (g_file_dialog_mode == FileDialogMode::ExportPalette) title = "Export Palette";
    else if (g_file_dialog_mode == FileDialogMode::ImportPalette) title = "Import Palette";
    else if (g_file_dialog_mode == FileDialogMode::WriteAniLst) title = "Write ANILST";
    else if (g_file_dialog_mode == FileDialogMode::WriteTbl) title = "Write TBL";
    else if (g_file_dialog_mode == FileDialogMode::WriteIrw) title = "Write IRW";
    else if (g_file_dialog_mode == FileDialogMode::LoadAsmAnim) title = "Load Character ASM";
    else if (g_file_dialog_mode == FileDialogMode::SaveAsmAnim) title = "Save World View ASM";
    else if (g_file_dialog_mode == FileDialogMode::LoadWorldProject) title = "Load World View Project";
    else if (g_file_dialog_mode == FileDialogMode::SaveWorldProject) title = "Save World View Project";
    else if (g_file_dialog_mode == FileDialogMode::ExportWorldPng) title = "Export World View PNG";
    else if (g_file_dialog_mode == FileDialogMode::ExportWorldPngSeq) title = "Export World View PNG Sequence";

    if (g_show_file_dialog) ImGui::OpenPopup(title);
    
    ImGui::SetNextWindowSize(ImVec2(800, 520), ImGuiCond_Once);
    if (ImGui::BeginPopupModal(title, &g_show_file_dialog, ImGuiWindowFlags_NoSavedSettings)) {
        
        if (ImGui::InputText("Directory", g_file_dialog_dir, sizeof(g_file_dialog_dir))) {
            g_file_dialog_file[0] = '\0';
            FileDialogClearMultiSelection();
            file_preview_clear();
        }

        /* Sort controls. Persistent across dialog opens. */
        ImGui::SameLine();
        const char *sort_labels[] = { "Name", "Date", "Size" };
        int sort_idx = (int)g_file_sort;
        ImGui::SetNextItemWidth(80);
        if (ImGui::Combo("##filesort", &sort_idx, sort_labels, 3))
            g_file_sort = (FileSort)sort_idx;
        ImGui::SameLine();
        if (ImGui::SmallButton(g_file_sort_desc ? "v" : "^")) g_file_sort_desc = !g_file_sort_desc;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle ascending / descending");

        ImGui::Separator();

        /* Two-pane layout: file list on the left, preview thumbnail on the
           right. Preview lives in the same vertical band as the list. */
        const float PREVIEW_PANE_W = 208.0f; /* 192 image + 16 padding */
        float content_h = -ImGui::GetFrameHeightWithSpacing() * 4;

        ImGui::BeginChild("##file_list", ImVec2(-PREVIEW_PANE_W, content_h), true);

        std::string current_dir = g_file_dialog_dir;
        std::string parent_dir  = GetParentDirectory(current_dir);

        if (current_dir != parent_dir) {
            if (ImGui::Selectable("[..] (Up one level)", false, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    snprintf(g_file_dialog_dir, sizeof(g_file_dialog_dir), "%s", parent_dir.c_str());
                    g_file_dialog_file[0] = '\0';
                    FileDialogClearMultiSelection();
                    file_preview_clear();
                }
            }
        }

        std::vector<FileEntry> entries;
        GetDirectoryFiles(current_dir, entries, GetDialogExtension(g_file_dialog_mode));

        std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
            /* Directories always come first regardless of sort key, so the
               user can drill down without scrolling past the file list. */
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            bool less;
            switch (g_file_sort) {
                case FileSort::Date: less = a.mtime < b.mtime; break;
                case FileSort::Size: less = a.size  < b.size;  break;
                case FileSort::Name:
                default:             less = a.name  < b.name;  break;
            }
            return g_file_sort_desc ? !less : less;
        });

        /* Double-click on a file = select + commit, matching native OS
           file dialogs. The actual commit re-uses the OK/Open/Save button
           handler below by OR-ing this flag with its click. */
        bool dbl_click_commit = false;
        const bool multi_select = FileDialogSupportsMultiSelect(g_file_dialog_mode);
        std::string hover_preview_path;
        for (int entry_idx = 0; entry_idx < (int)entries.size(); entry_idx++) {
            const auto& entry = entries[entry_idx];
            std::string label = (entry.is_dir ? "[Dir] " : "      ") + entry.name;
            bool selected = multi_select ? FileDialogHasMultiFile(entry.name)
                                         : (entry.name == g_file_dialog_file);
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (entry.is_dir) {
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        std::string new_dir = PathCombine(current_dir, entry.name);
                        snprintf(g_file_dialog_dir, sizeof(g_file_dialog_dir), "%s", new_dir.c_str());
                        g_file_dialog_file[0] = '\0';
                        FileDialogClearMultiSelection();
                        file_preview_clear();
                    }
                } else {
                    if (multi_select) {
                        const ImGuiIO &io = ImGui::GetIO();
                        if (io.KeyShift)
                            FileDialogSelectRange(entries, entry_idx, io.KeyCtrl);
                        else if (io.KeyCtrl)
                            FileDialogToggleSelection(entry.name);
                        else
                            FileDialogReplaceSelection(entry.name);
                    } else {
                        snprintf(g_file_dialog_file, sizeof(g_file_dialog_file), "%s", entry.name.c_str());
                    }
                    if (ImGui::IsMouseDoubleClicked(0)) dbl_click_commit = true;
                }
            }
            if (!entry.is_dir && ImGui::IsItemHovered())
                hover_preview_path = PathCombine(current_dir, entry.name);
        }
        ImGui::EndChild();

        /* Preview pane on the right. Decodes the currently-selected file
           lazily; cache is keyed by full path so cursoring up/down doesn't
           re-decode the same file. */
        ImGui::SameLine();
        ImGui::BeginChild("##file_preview", ImVec2(PREVIEW_PANE_W - 8, content_h), true);
        {
            if (file_dialog_uses_image_export_preview(g_file_dialog_mode)) {
                export_preview_refresh();
                if (g_export_preview.tex) {
                    ImGui::TextUnformatted("Export Preview");
                    ImGui::Image((ImTextureID)(intptr_t)g_export_preview.tex,
                                 ImVec2((float)g_export_preview.w, (float)g_export_preview.h));
                } else {
                    ImGui::TextDisabled("No sprite selected");
                }
            } else {
                std::string preview_path = hover_preview_path;
                if (preview_path.empty() && g_file_dialog_file[0])
                    preview_path = PathCombine(g_file_dialog_dir, g_file_dialog_file);
                if (!preview_path.empty()) {
                    file_preview_refresh(preview_path);
                    if (g_file_preview.tex) {
                        ImGui::TextUnformatted("Preview");
                        ImGui::Image((ImTextureID)(intptr_t)g_file_preview.tex,
                                     ImVec2((float)g_file_preview.w, (float)g_file_preview.h));
                    } else {
                        ImGui::TextDisabled("No preview\n(IMG / LBM previews\ncoming soon)");
                    }
                } else {
                    ImGui::TextDisabled("Select a file");
                }
            }
        }
        ImGui::EndChild();
        
        if (g_file_dialog_mode == FileDialogMode::WriteTbl) {
            ImGui::InputScalar("ROM Base Address (Hex)", ImGuiDataType_U32, &g_tbl_base_address, NULL, NULL, "%08X", ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::Checkbox("MK3 Format (7-value header)", &g_tbl_export_mk3_format);
            ImGui::SetItemTooltip("Includes the 3 extra animation points: ANIX2, ANIY2, and ANIZ2.");
            ImGui::Checkbox("Include Assigned Palette Name", &g_tbl_export_palette);
            ImGui::Checkbox("Pad to 4-bit boundary (/P)", &g_tbl_export_pad_4bit);
            ImGui::Checkbox("Align to 16-bit boundary (/L)", &g_tbl_export_align_16bit);
            ImGui::Checkbox("Dual-Banked Memory (/E)", &g_tbl_export_dual_bank);
            if (g_tbl_export_dual_bank) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("Bank (0/1)", &g_tbl_export_bank);
                if (g_tbl_export_bank < 0) g_tbl_export_bank = 0;
                if (g_tbl_export_bank > 1) g_tbl_export_bank = 1;
            }
        }
        if (g_file_dialog_mode == FileDialogMode::WriteIrw) {
            ImGui::InputScalar("ROM Base Address (Hex)", ImGuiDataType_U32, &g_irw_base_address, NULL, NULL, "%08X", ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::Text("Bits Per Pixel:");
            if (ImGui::RadioButton("Auto (Image Data)", g_irw_bpp == 0)) g_irw_bpp = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("Auto (Palette Size) /B", g_irw_bpp == -1)) g_irw_bpp = -1;
            ImGui::SameLine();
            int fixed_bpp = (g_irw_bpp > 0) ? g_irw_bpp : 8;
            if (ImGui::RadioButton("Fixed", g_irw_bpp > 0)) g_irw_bpp = fixed_bpp;
            if (g_irw_bpp > 0) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                if (ImGui::SliderInt("##bpp", &fixed_bpp, 1, 8)) g_irw_bpp = fixed_bpp;
            }
            ImGui::Checkbox("Align to 16-bit boundary (/L)", &g_irw_align_16bit);
        }

        if (g_file_dialog_mode == FileDialogMode::ImportGif) {
            if (ImGui::BeginCombo("Blend Mode", GifBlendModeName(g_gif_blend_mode))) {
                for (int i = 0; i < GifBlend_Count; i++) {
                    bool selected = (g_gif_blend_mode == i);
                    if (ImGui::Selectable(GifBlendModeName(i), selected)) g_gif_blend_mode = i;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SliderInt("Opacity", &g_gif_opacity_percent, 0, 100, "%d%%");
            ImGui::Checkbox("Import All Frames", &g_gif_import_all);
            ImGui::Checkbox("Trim Transparent Border", &g_gif_trim_transparent);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Crop away any border that's transparent across every imported frame.\nFrames stay aligned to each other.");
        }
        if (g_file_dialog_mode == FileDialogMode::ExportGif) {
            ImGui::SliderFloat("Frames Per Second", &g_gif_export_fps, 1.0f, 60.0f, "%.1f");
            ImGui::Checkbox("Loop Forever", &g_gif_export_loop);
            ImGui::Checkbox("Ping-Pong", &g_gif_export_pingpong);
            ImGui::Checkbox("Align Frames by Primary Anipoint", &g_gif_export_align_anipoints);
            ImGui::TextDisabled("Exports Animation Timeline order and Hold timing. Index #0 is transparent.");
        }

        if (g_file_dialog_mode == FileDialogMode::ImportPng) {
            ImGui::Checkbox("Match to Active Palette", &g_png_import_match_palette);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("On indexes every selected PNG into the palette already in use.\n"
                                  "Off builds a new palette per file.\n"
                                  "This setting also applies to PNGs dragged onto the window.");
            if (g_png_import_match_palette) {
                PAL *target = get_pal(ResolveImportMatchPalette());
                if (target) ImGui::TextDisabled("Matching into palette %.9s (%u colors).",
                                                target->n_s, target->numc);
                else        ImGui::TextDisabled("No palette available — files will import with a new palette.");
            }
        }

        if (g_file_dialog_mode == FileDialogMode::ImportPngMatch) {
            PAL *target = get_pal(ResolveImportMatchPalette());
            if (target) ImGui::TextDisabled("Matching into palette %.9s (%u colors). Every selected file uses this one.",
                                            target->n_s, target->numc);
            else        ImGui::TextDisabled("No palette to match against — select an image or palette first.");
        }

        if (g_file_dialog_mode == FileDialogMode::ExportWorldPng ||
            g_file_dialog_mode == FileDialogMode::ExportWorldPngSeq) {
            ImGui::Checkbox("Keep Lane Transparency", &g_world_png_lane_alpha);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Off composites every lane fully opaque, like the hardware would draw it.\n"
                                  "On keeps the faded lane colors the World View uses to keep\n"
                                  "overlapping rows readable while editing.");
            ImGui::Checkbox("Crop to Visible Content", &g_world_png_crop);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", g_file_dialog_mode == FileDialogMode::ExportWorldPngSeq
                    ? "Off writes the full world rect.\nOn crops to one shared rect covering every tick, so frames stay aligned."
                    : "Off writes the full world rect. On trims to the sprites actually drawn.");
            if (g_file_dialog_mode == FileDialogMode::ExportWorldPngSeq) {
                ImGui::TextDisabled("Writes <name>_0000.PNG onward, one file per tick (max %d).",
                                    (int)kWorldPngSequenceMaxFrames);
            } else {
                ImGui::TextDisabled("Composites every visible lane at tick %d. Index #0 stays transparent.",
                                    g_world_marked_state.frame);
            }
        }

        if (g_file_dialog_mode == FileDialogMode::ImportSpriteSheetMatch) {
            ImGui::InputText("Name Prefix", g_sheet_prefix, sizeof(g_sheet_prefix));
            ImGui::SliderInt("Background", &g_sheet_bg_threshold, 180, 255);
            ImGui::InputInt("Min Pixels", &g_sheet_min_pixels);
            if (g_sheet_min_pixels < 1) g_sheet_min_pixels = 1;
            ImGui::InputInt("Padding", &g_sheet_padding);
            if (g_sheet_padding < 0) g_sheet_padding = 0;
            if (g_sheet_padding > 64) g_sheet_padding = 64;
            ImGui::Checkbox("Crop to Sprite Bounds", &g_sheet_crop);
        }

        if (g_file_dialog_mode == FileDialogMode::ExportPalette) {
            ImGui::Checkbox("Adobe ACT (RGB)", &g_palette_export_act);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off writes raw Midway 15-bit palette words.");
        }

        if (g_file_dialog_mode == FileDialogMode::OpenLod) {
            ImGui::InputText("Force Override Directory (/O)", g_lod_override_dir, sizeof(g_lod_override_dir));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("If set, forces all IMGs to load from this directory, ignoring paths in the .lod file.");
        }

        if (ImGui::InputText("File Name", g_file_dialog_file, sizeof(g_file_dialog_file)))
            FileDialogSyncTypedFilename();
        ImGui::SameLine();
        
        const char* btn_text = (g_file_dialog_mode == FileDialogMode::ImportPng ||
                                g_file_dialog_mode == FileDialogMode::ImportPngMatch ||
                                g_file_dialog_mode == FileDialogMode::ImportSpriteSheetMatch ||
                                g_file_dialog_mode == FileDialogMode::ImportGif ||
                                g_file_dialog_mode == FileDialogMode::ExportPng) ? "OK" :
                               (g_file_dialog_mode == FileDialogMode::OpenImg ||
                                g_file_dialog_mode == FileDialogMode::AppendImg ||
                                g_file_dialog_mode == FileDialogMode::OpenLod ||
                                g_file_dialog_mode == FileDialogMode::LoadLbm ||
                                g_file_dialog_mode == FileDialogMode::LoadTga ||
                                g_file_dialog_mode == FileDialogMode::LoadWorldProject ||
                                g_file_dialog_mode == FileDialogMode::LoadAsmAnim ||
                                g_file_dialog_mode == FileDialogMode::ImportPalette) ? "Open" : "Save";
        if (ImGui::Button(btn_text, ImVec2(100, 0)) || dbl_click_commit) {
            std::vector<std::string> selected_files = FileDialogSelectedFiles();
            std::string full_path = PathCombine(g_file_dialog_dir, g_file_dialog_file);

            if (g_file_dialog_mode == FileDialogMode::ExportTga) {
                size_t dot = full_path.find_last_of('.');
                if (dot != std::string::npos) full_path = full_path.substr(0, dot);
                full_path += ".TGA";
                BuildTgaFromMarked(full_path.c_str());
            } else if (g_file_dialog_mode == FileDialogMode::ExportPng) {
                size_t dot = full_path.find_last_of('.');
                if (dot != std::string::npos) full_path = full_path.substr(0, dot);
                full_path += ".PNG";
                ExportPng(full_path.c_str());
            } else if (g_file_dialog_mode == FileDialogMode::ExportWorldPng) {
                size_t dot = full_path.find_last_of('.');
                if (dot != std::string::npos) full_path = full_path.substr(0, dot);
                full_path += ".PNG";
                ExportWorldViewPng(full_path.c_str(), g_world_png_crop,
                                   g_world_png_lane_alpha);
            } else if (g_file_dialog_mode == FileDialogMode::ExportWorldPngSeq) {
                int total_ticks = 0;
                ExportWorldViewPngSequence(full_path.c_str(), g_world_png_crop,
                                           g_world_png_lane_alpha, &total_ticks);
            } else if (g_file_dialog_mode == FileDialogMode::ExportGif) {
                size_t dot = full_path.find_last_of('.');
                if (dot != std::string::npos) full_path = full_path.substr(0, dot);
                full_path += ".GIF";
                ExportAnimatedGif(full_path.c_str(), g_timeline_frames, g_timeline_holds,
                                  g_gif_export_fps, g_gif_export_loop,
                                  g_gif_export_pingpong, g_gif_export_align_anipoints);
            } else if (g_file_dialog_mode == FileDialogMode::ExportPalette) {
                size_t dot = full_path.find_last_of('.');
                if (dot != std::string::npos) full_path = full_path.substr(0, dot);
                full_path += g_palette_export_act ? ".ACT" : ".PAL";
                ExportPalette(full_path.c_str(), g_palette_export_act);
            } else if (g_file_dialog_mode == FileDialogMode::ImportPalette) {
                doc_undo_push();
                ImportPalette(full_path.c_str());
                mark_dirty();
            } else if (g_file_dialog_mode == FileDialogMode::ImportPng) {
                /* Same palette pinning as the Match dialog when the option is
                   on; falls back to per-file palettes when nothing matchable
                   exists so the import still succeeds. */
                int match_pal = g_png_import_match_palette ? ResolveImportMatchPalette() : -1;
                PAL *match_pal_p = get_pal(match_pal);
                unsigned int before_count = g_doc->imgcnt;
                for (const std::string &file : selected_files) {
                    std::string path = PathCombine(g_file_dialog_dir, file);
                    if (match_pal >= 0) ImportPngMatch(path.c_str(), match_pal);
                    else                ImportPng(path.c_str());
                }
                unsigned int added = g_doc->imgcnt - before_count;
                if (selected_files.size() > 1 || match_pal >= 0) {
                    if (!added) {
                        snprintf(g_restore_msg, sizeof(g_restore_msg), "No PNG images imported.");
                    } else if (match_pal >= 0) {
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                                 "Imported %u image(s) matched to palette %.9s.",
                                 added, match_pal_p ? match_pal_p->n_s : "");
                    } else {
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                                 "Imported %u image(s) from %d PNG file(s).",
                                 added, (int)selected_files.size());
                    }
                    g_restore_msg_timer = 4.0f;
                }
                mark_dirty();
            } else if (g_file_dialog_mode == FileDialogMode::ImportPngMatch) {
                /* Pin the palette before the first file: importing selects the
                   image it just created, so re-reading the selection per file
                   would let the target drift mid-batch. */
                int match_pal = ResolveImportMatchPalette();
                PAL *match_pal_p = get_pal(match_pal);
                unsigned int before_count = g_doc->imgcnt;
                int failed = 0;
                for (const std::string &file : selected_files) {
                    std::string path = PathCombine(g_file_dialog_dir, file);
                    if (!ImportPngMatch(path.c_str(), match_pal)) failed++;
                }
                unsigned int added = g_doc->imgcnt - before_count;
                if (match_pal < 0) {
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "No palette to match against — load or select a palette first.");
                } else if (added == 0) {
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "No PNG images imported (%d file(s) failed to decode).", failed);
                } else {
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             failed ? "Imported %u image(s) matched to palette %.9s; %d file(s) failed."
                                    : "Imported %u image(s) matched to palette %.9s.",
                             added, match_pal_p ? match_pal_p->n_s : "", failed);
                }
                g_restore_msg_timer = 4.0f;
                mark_dirty();
            } else if (g_file_dialog_mode == FileDialogMode::ImportSpriteSheetMatch) {
                unsigned int before_count = g_doc->imgcnt;
                SpriteSheetImportOptions opts = {};
                opts.detect_mode = SpriteSheetDetect_Auto;
                opts.background_threshold = g_sheet_bg_threshold;
                opts.min_pixels = g_sheet_min_pixels;
                opts.padding = g_sheet_padding;
                opts.crop = g_sheet_crop;
                snprintf(opts.name_prefix, sizeof(opts.name_prefix), "%s", g_sheet_prefix);
                for (const std::string &file : selected_files) {
                    std::string path = PathCombine(g_file_dialog_dir, file);
                    ImportSpriteSheetMatch(path.c_str(), &opts);
                }
                unsigned int added = g_doc->imgcnt - before_count;
                if (selected_files.size() > 1) {
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             added ? "Imported %u frame(s) from %d sprite sheet(s)." : "No sprite sheet frames imported.",
                             added, (int)selected_files.size());
                    g_restore_msg_timer = 4.0f;
                }
                if (added > 0) mark_dirty();
            } else if (g_file_dialog_mode == FileDialogMode::ImportGif) {
                unsigned int before_count = g_doc->imgcnt;
                for (const std::string &file : selected_files) {
                    std::string path = PathCombine(g_file_dialog_dir, file);
                    ImportGif(path.c_str(), g_gif_blend_mode, g_gif_opacity_percent, g_gif_import_all, g_gif_trim_transparent);
                }
                if (selected_files.size() > 1) {
                    unsigned int added = g_doc->imgcnt - before_count;
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             added ? "Imported %u image(s) from %d GIF file(s)." : "No GIF images imported.",
                             added, (int)selected_files.size());
                    g_restore_msg_timer = 4.0f;
                }
                mark_dirty();
            } else if (g_file_dialog_mode == FileDialogMode::WriteAniLst) {
                size_t dot = full_path.find_last_of('.');
                if (dot == std::string::npos) full_path += ".ASM";
                WriteAnilstFromMarked(full_path.c_str());
            } else if (g_file_dialog_mode == FileDialogMode::SaveAsmAnim) {
                size_t dot = full_path.find_last_of('.');
                if (dot == std::string::npos) full_path += ".ASM";
                FILE *af = fopen(full_path.c_str(), "wb");
                if (af) {
                    fwrite(g_world_marked_state.generated_asm.data(), 1,
                           g_world_marked_state.generated_asm.size(), af);
                    fclose(af);
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Saved World View ASM (%d bytes).",
                             (int)g_world_marked_state.generated_asm.size());
                } else {
                    snprintf(g_restore_msg, sizeof(g_restore_msg), "Could not write ASM file.");
                }
                g_restore_msg_timer = 4.0f;
            } else if (g_file_dialog_mode == FileDialogMode::SaveWorldProject) {
                size_t dot = full_path.find_last_of('.');
                if (dot == std::string::npos) full_path += ".WVP";
                if (SaveWorldProjectFile(full_path.c_str()))
                    g_last_world_project_path = full_path;
            } else if (g_file_dialog_mode == FileDialogMode::LoadWorldProject) {
                LoadWorldProjectFile(full_path.c_str());
            } else if (g_file_dialog_mode == FileDialogMode::WriteTbl) {
                size_t dot = full_path.find_last_of('.');
                if (dot == std::string::npos) full_path += ".TBL";
                WriteTblFromMarked(full_path.c_str(), g_tbl_base_address, g_tbl_export_mk3_format, g_tbl_export_palette, g_tbl_export_pad_4bit, g_tbl_export_align_16bit, g_tbl_export_dual_bank, g_tbl_export_bank);
            } else if (g_file_dialog_mode == FileDialogMode::WriteIrw) {
                size_t dot = full_path.find_last_of('.');
                if (dot == std::string::npos) full_path += ".IRW";
                WriteIrwFromMarked(full_path.c_str(), g_irw_base_address, g_irw_bpp, g_irw_align_16bit);
            } else if (g_file_dialog_mode == FileDialogMode::SaveMarkedLbm) {
                _chdir(g_file_dialog_dir);
                IMG *p = (IMG *)g_doc->img_p;
                int original_selection = g_doc->ilselected;
                int i = 0;
                while (p) {
                    if ((p->flags & 1) && p->w > 0 && p->h > 0) {
                        g_doc->ilselected = i;
                        memset(g_doc->fnametmp_s, 0, 13);
                        snprintf(g_doc->fnametmp_s, 13, "%.8s.LBM", p->n_s);
                        SaveLbm(g_doc->fnametmp_s);
                    }
                    p = (IMG *)p->nxt_p;
                    i++;
                }
                g_doc->ilselected = original_selection;
            } else if (g_file_dialog_mode == FileDialogMode::OpenImg) {
                OpenImgFile(full_path);
                /* If this open was to locate an ASM viewer's IMG, re-resolve. */
                if (g_openimg_for_asm) {
                    g_openimg_for_asm = false;
                    if (!g_asm_anims.empty())
                        AsmAnimSelect(g_asm_anim_sel >= 0 ? g_asm_anim_sel : 0);
                }
                if (g_openimg_for_opp) {
                    g_openimg_for_opp = false;
                    g_asm_opp_doc = g_doc;
                    g_asm_opp_doc_idx = document_active_index();
                    if (g_asm_opp_sel >= 0 && g_asm_opp_sel < (int)g_asm_opp_anims.size())
                        AsmResolveAnimAgainstDoc(g_asm_opp_anims[g_asm_opp_sel], g_asm_opp_doc);
                }
            } else if (g_file_dialog_mode == FileDialogMode::LoadAsmAnim) {
                if (g_asm_dialog_opponent) {
                    LoadAsmOpponent(full_path.c_str());
                } else {
                    LoadAsmAnimations(full_path.c_str());
                }
                g_asm_dialog_opponent = false;
                g_show_asm_anim = true;
            } else if (g_file_dialog_mode == FileDialogMode::OpenLod) {
                LodManifest manifest = ParseLodFile(full_path.c_str(),
                    g_lod_override_dir[0] ? g_lod_override_dir : nullptr);
                verbose_log("OpenLod: %s -> %zu entries, PPP=%d", full_path.c_str(), manifest.entries.size(), manifest.ppp_value);
                if (manifest.parse_error) {
                    snprintf(g_restore_msg, sizeof(g_restore_msg), "LOD: %s", manifest.error_msg.c_str());
                    g_restore_msg_timer = 6.0f;
                } else {
                    if (manifest.has_ppp_value)
                        g_load2_ppp = manifest.ppp_value;

                    PrepareDocumentForOpenedFile();

                    std::string lod_dir(g_file_dialog_dir);

                    int loaded = 0;
                    for (size_t i = 0; i < manifest.entries.size(); i++) {
                        const std::string &rpath = manifest.entries[i].resolved_path;

                        size_t sep = rpath.find_last_of("\\/");
                        std::string dir, file;
                        if (sep != std::string::npos) {
                            dir  = rpath.substr(0, sep);
                            file = rpath.substr(sep + 1);
                        } else {
                            dir  = ".";
                            file = rpath;
                        }

                        size_t n_file = file.length();
                        if (n_file > 12) n_file = 12;

                        auto try_load = [&](const std::string &d) -> bool {
                            size_t nd = d.length();
                            if (nd > sizeof(g_doc->fpath_s) - 1) nd = sizeof(g_doc->fpath_s) - 1;
                            memset(g_doc->fpath_s, 0, sizeof(g_doc->fpath_s));
                            memcpy(g_doc->fpath_s, d.c_str(), nd);

                            memset(g_doc->fname_s, 0, 13);
                            memcpy(g_doc->fname_s, file.c_str(), n_file);
                            for (size_t j = 0; j < n_file; j++)
                                g_doc->fname_s[j] = (char)toupper((unsigned char)g_doc->fname_s[j]);

                            unsigned int prev = g_doc->imgcnt;
                            _chdir(d.c_str());
                            LoadImgFile();
                            return g_doc->imgcnt > prev;
                        };

                        if (try_load(dir)) {
                            loaded++;
                        } else if (lod_dir != dir && try_load(lod_dir)) {
                            loaded++;
                        } else {
                            const char *imgdir = getenv("IMGDIR");
                            if (imgdir && imgdir[0] && std::string(imgdir) != dir && std::string(imgdir) != lod_dir) {
                                if (try_load(imgdir)) loaded++;
                            }
                        }
                    }

                    g_doc->ilselected = g_doc->imgcnt > 0 ? 0 : -1;
                    g_dirty = false;
                    RecentAdd(full_path);

                    int total = (int)manifest.entries.size();
                    if (loaded == 0)
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                            "LOD: 0/%d IMG(s) loaded. Check IMGDIR or file paths.", total);
                    else if (loaded < total)
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                            "LOD: %d/%d IMG(s) loaded%s", loaded, total,
                            manifest.has_ppp_value ? " (PPP set)" : "");
                    else
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                            "Loaded %d IMG(s) from LOD%s", loaded,
                            manifest.has_ppp_value ? " (PPP set)" : "");
                    g_restore_msg_timer = 4.0f;
                }
            } else {
                size_t n_dir = strlen(g_file_dialog_dir);
                if (n_dir > sizeof(g_doc->fpath_s) - 1) n_dir = sizeof(g_doc->fpath_s) - 1;
                memset(g_doc->fpath_s, 0, sizeof(g_doc->fpath_s));
                memcpy(g_doc->fpath_s, g_file_dialog_dir, n_dir);
                
                size_t n_file = strlen(g_file_dialog_file);
                if (n_file > 12) n_file = 12;
                memset(g_doc->fname_s, 0, 13);
                memset(g_doc->fnametmp_s, 0, 13);
                memcpy(g_doc->fname_s, g_file_dialog_file, n_file);
                memcpy(g_doc->fnametmp_s, g_file_dialog_file, n_file);
                for (size_t i = 0; i < n_file; i++) {
                    g_doc->fname_s[i] = (char)toupper((unsigned char)g_doc->fname_s[i]);
                    g_doc->fnametmp_s[i] = (char)toupper((unsigned char)g_doc->fnametmp_s[i]);
                }
                _chdir(g_file_dialog_dir);
                
                if (g_file_dialog_mode == FileDialogMode::SaveImg) {
                    SaveImgFile();
                    g_dirty = false; /* Mark as saved in C++ state */
                    RecentAdd(full_path);
                } else if (g_file_dialog_mode == FileDialogMode::AppendImg) {
                    /* Append adds images on top of the current set — even if
                       the file was clean (matched disk) before, after Append
                       it no longer does, so mark dirty. */
                    LoadImgFile();
                    mark_dirty();
                    RecentAdd(full_path);
                } else if (g_file_dialog_mode == FileDialogMode::LoadLbm) {
                    LoadLbm(full_path.c_str());
                    mark_dirty();
                } else if (g_file_dialog_mode == FileDialogMode::LoadTga) {
                    LoadTga(full_path.c_str());
                    mark_dirty();
                } else if (g_file_dialog_mode == FileDialogMode::SaveLbm) {
                    SaveLbm(full_path.c_str());
                } else if (g_file_dialog_mode == FileDialogMode::SaveTga) {
                    SaveTga(full_path.c_str());
                }
            }
            g_img_tex_idx = -2; /* Force canvas texture refresh */
            save_last_dir(g_file_dialog_dir, g_file_dialog_mode);
            file_preview_clear();
            export_preview_clear();
            g_show_file_dialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            file_preview_clear();
            export_preview_clear();
            g_show_file_dialog = false;
            ImGui::CloseCurrentPopup();
        }
        if (FileDialogSupportsMultiSelect(g_file_dialog_mode) && g_file_dialog_multi_files.size() > 1)
            ImGui::TextDisabled("%d files selected", (int)g_file_dialog_multi_files.size());
        ImGui::EndPopup();
    }
}

/* Help modal */



/* ---- MK2 strike-table editor state (moved from overlay) ---- */

static std::string   g_mk2_status;
static bool          g_mk2_status_sticky = false;


static char          g_mk2_search[64] = "";

/* ---- MK2 fatality lab state (moved from overlay) ---- */
static char                 g_mk2_fatality_root[1024] = "..\\mk2-main";
static int                  g_mk2_fatality_command_idx = 0;
static int                  g_mk2_fatality_combo_idx = 0;
static int                  g_mk2_fatality_anim_idx = 0;
static int                  g_mk2_fatality_selected_line = 0;
static char                 g_mk2_fatality_filter[96] = "";
static char                 g_mk2_fatality_insert_anim[256] = "\t.long\t0";
static char                 g_mk2_fatality_insert_combo[256] = "\t.word\tsw_right";
static bool                 g_mk2_fatality_body_only = false;
static mk2fatal::AssetPlan  g_mk2_fatality_plan;
static std::string          g_mk2_fatality_stage_status;
static int                  g_mk2_fatality_fighter_idx = 0;
static int                  g_mk2_fatality_selected_fatality = 0;
static int                  g_mk2_fatality_attacker_anim_idx = 0;
static int                  g_mk2_fatality_victim_anim_idx = 0;
static float                g_mk2_fatality_preview_fps = 8.0f;

/* ---- ASM animation viewer state (moved from overlay) ---- */
static std::string  g_asm_anim_file;
static std::string  g_asm_opp_file;
static int          g_asm_anim_frame = 0;
static float        g_asm_anim_timer = 0.0f;
static SDL_Texture *g_asm_anim_tex = NULL;
static int          g_asm_anim_tex_w = 0, g_asm_anim_tex_h = 0;
static int          g_asm_anim_canvas_w = 0, g_asm_anim_canvas_h = 0;
static int          g_asm_anim_minx = 0, g_asm_anim_miny = 0;
static int          g_asm_anim_last_drawn = -1;
static bool         g_asm_anim_play = true;
static float        g_asm_anim_fps = 12.0f;

/* =========================================================
   Extracted dialogs and modals from imgui_overlay.cpp
   ========================================================= */
enum class RenameTarget { Image, Palette, MarkedImages };
static bool         g_show_rename = false;
static RenameTarget g_rename_target = RenameTarget::Palette;
static int          g_rename_idx = -1;
static char         g_rename_buf[20] = {0};
static bool         g_rename_tail_existing = false;
static int          g_rename_start_number = 1;

static int  g_new_blank_w = 32;
static int  g_new_blank_h = 32;

static char g_restore_regex_buf[256] = "^(.+)[A-Z]$";
static std::vector<BulkRestoreMatch> g_restore_matches;
static bool g_restore_regex_tested = false;
static bool g_restore_regex_error = false;
static int  g_restore_diff_mode = 1;

void OpenRenameImage(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img) return;
    g_rename_target = RenameTarget::Image;
    g_rename_idx = g_doc->ilselected;
    strncpy(g_rename_buf, img->n_s, 15);
    g_rename_buf[15] = '\0';
    g_show_rename = true;
}

void OpenRenamePalette(int idx)
{
    PAL *pal = get_pal(idx);
    if (!pal) return;
    g_rename_target = RenameTarget::Palette;
    g_rename_idx = idx;
    strncpy(g_rename_buf, pal->n_s, 9);
    g_rename_buf[9] = '\0';
    g_show_rename = true;
}

void OpenRenameMarkedImages(void)

{
    /* Find first marked image to seed the buffer with its name. */
    IMG *first = NULL;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        if (p->flags & 1) { first = p; break; }
    }
    if (!first) return;
    g_rename_target = RenameTarget::MarkedImages;
    g_rename_idx = -1;
    strncpy(g_rename_buf, first->n_s, 12);
    g_rename_buf[12] = '\0';
    g_rename_tail_existing = false;
    g_rename_start_number = 1;
    g_show_rename = true;
}

static void ApplyMarkedImageRename(const char *base)
{
    if (!base || (!*base && !g_rename_tail_existing)) return;
    doc_undo_push();
    bool prepend = (base[0] == '+') && !g_rename_tail_existing;
    const char *core = prepend ? base + 1 : base;
    int n = g_rename_start_number;
    if (n < 0) n = 0;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        if (!(p->flags & 1)) continue;
        char old[16];
        strncpy(old, p->n_s, 15); old[15] = '\0';
        if (g_rename_tail_existing) {
            snprintf(p->n_s, sizeof(p->n_s), "%s%s%d", old, core, n);
        } else if (prepend) {
            snprintf(p->n_s, sizeof(p->n_s), "%s%s", core, old);
        } else {
            snprintf(p->n_s, sizeof(p->n_s), "%s%d", core, n);
        }
        n++;
        p->n_s[15] = '\0';
    }
}

void DrawRenameDialog(void)
{
    const char *rename_title =
        g_rename_target == RenameTarget::Image          ? "Rename Image" :
        g_rename_target == RenameTarget::Palette        ? "Rename Palette" :
                                                          "Rename Marked Images";
    if (g_show_rename) ImGui::OpenPopup(rename_title);
    if (!ImGui::BeginPopupModal(rename_title, &g_show_rename, ImGuiWindowFlags_AlwaysAutoResize)) return;

    if (g_rename_target == RenameTarget::MarkedImages) {
        ImGui::TextWrapped("Base text for a numbered sequence. Tail mode keeps each current name and appends this text plus the number.");
    } else if (g_rename_target == RenameTarget::Image) {
        IMG *img = get_img(g_rename_idx);
        if (img) ImGui::Text("Rename: %s", img->n_s);
    } else {
        PAL *pal = get_pal(g_rename_idx);
        if (pal) ImGui::Text("Rename: %s", pal->n_s);
    }
    const int maxlen = g_rename_target == RenameTarget::Palette ? 10 : 16;
    ImGui::InputText("##rn", g_rename_buf,
                     (size_t)maxlen < sizeof(g_rename_buf) ? maxlen : sizeof(g_rename_buf));
    if (g_rename_target == RenameTarget::MarkedImages) {
        ImGui::Checkbox("Tail existing names", &g_rename_tail_existing);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputInt("Start", &g_rename_start_number, 1, 10)) {
            if (g_rename_start_number < 0) g_rename_start_number = 0;
        }
    }
    if (ImGui::Button("OK", ImVec2(100, 0))) {
        if (g_rename_target == RenameTarget::Image) {
            IMG *img = get_img(g_rename_idx);
            if (img) {
                doc_undo_push();   /* EditSnapshot doesn't store n_s */
                strncpy(img->n_s, g_rename_buf, 15);
                img->n_s[15] = '\0';
            }
        } else if (g_rename_target == RenameTarget::Palette) {
            PAL *pal = get_pal(g_rename_idx);
            if (pal) {
                doc_undo_push();
                strncpy(pal->n_s, g_rename_buf, 9);
                pal->n_s[9] = '\0';
            }
        } else {
            ApplyMarkedImageRename(g_rename_buf);
        }
        g_show_rename = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_rename = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawLoad2VerifyDialog(void)
{
    if (g_show_load2_verify) ImGui::OpenPopup("LOAD2 Packing Verify");
    if (!ImGui::BeginPopupModal("LOAD2 Packing Verify", &g_show_load2_verify,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Checked %d image%s against pristine baseline",
                g_load2_report.imgs_checked,
                g_load2_report.imgs_checked == 1 ? "" : "s");
    if (g_load2_report.imgs_no_baseline > 0) {
        ImGui::TextDisabled("(%d had no baseline — new/duplicated, "
                            "skipped shape check)",
                            g_load2_report.imgs_no_baseline);
    }
    ImGui::Separator();
    ImGui::Text("PPP: %d  (palette-colors limit = %d)",
                g_load2_ppp,
                g_load2_ppp > 0 ? (1 << g_load2_ppp) : 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("##ppp", &g_load2_ppp, 1, 0)) {
        if (g_load2_ppp < 0) g_load2_ppp = 0;
        if (g_load2_ppp > 8) g_load2_ppp = 8;
    }
    ImGui::SameLine();
    ImGui::Checkbox("/3 Limit (Max Scales)", &g_load2_limit_scales_to_3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Warns if any image uses an eighth scale (M_COPIES=3).");
    ImGui::SameLine();
    if (ImGui::Button("Re-check")) {
        g_load2_report = VerifyLoad2Packing(g_load2_ppp, g_load2_limit_scales_to_3);
        g_load2_selected_idx = -1;
    }
    ImGui::Separator();

    if (g_load2_report.issues.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                           "OK — no SAG-breaking edits detected.");
    } else {
        ImGui::Text("Breaking: %d   Warnings: %d",
                    g_load2_report.break_count,
                    g_load2_report.warn_count);
        ImGui::Spacing();
        ImGui::BeginChild("l2_issues", ImVec2(640, 280), true);
        for (size_t i = 0; i < g_load2_report.issues.size(); i++) {
            auto &iss = g_load2_report.issues[i];
            bool is_sel = ((int)i == g_load2_selected_idx);
            ImVec4 col = iss.sev == L2Severity::Break
                ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f)
                : ImVec4(1.0f, 0.85f, 0.4f,  1.0f);
            ImGui::PushID((int)i);
            char hdr[40];
            snprintf(hdr, sizeof(hdr), "[%4d] %-15s", iss.img_idx, iss.img_name.c_str());
            if (ImGui::Selectable("##row", is_sel, ImGuiSelectableFlags_AllowItemOverlap,
                                  ImVec2(0, 0))) {
                g_load2_selected_idx = (int)i;
                if (iss.img_idx >= 0) g_doc->ilselected = iss.img_idx;
                if (iss.sev == L2Severity::Break) {
                    update_drift_texture(get_img(iss.img_idx));
                }
            }
            ImGui::SameLine();
            ImGui::TextColored(col, "%s", hdr);
            ImGui::SameLine();
            ImGui::TextWrapped("%s", iss.message.c_str());
            ImGui::PopID();
            ImGui::Separator();
        }
        ImGui::EndChild();

        if (g_load2_selected_idx >= 0
            && g_load2_selected_idx < (int)g_load2_report.issues.size())
        {
            auto &sel = g_load2_report.issues[g_load2_selected_idx];
            IMG *si = get_img(sel.img_idx);
            if (sel.sev == L2Severity::Break && si && si->baseline_p) {
                if (!g_load2_drift_tex
                    || g_load2_drift_tex_w != (int)si->w
                    || g_load2_drift_tex_h != (int)si->h)
                {
                    update_drift_texture(si);
                }
                if (g_load2_drift_tex) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("Red rows = zero-shape drift "
                                        "(silhouette differs from baseline)");
                    float scale = (si->w < 64) ? 4.0f : (si->w < 128) ? 3.0f : 2.0f;
                    ImVec2 sz((float)si->w * scale, (float)si->h * scale);
                    ImGui::Image((ImTextureID)(intptr_t)g_load2_drift_tex, sz);
                }
            } else if (sel.sev != L2Severity::Break) {
                ImGui::Spacing();
                ImGui::TextDisabled("(no drift visualization for warnings)");
            }
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Close")) {
        g_show_load2_verify = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* ---- ASM animation viewer implementation ---- */

static std::string asm_trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool asm_is_control_token(const std::string &t)
{
    /* MK2 naming: opcodes/anim refs are lowercase (ani_jump, a_xxx); frame
       data symbols are uppercase (RNSTANCE1, RNSTANCE1A). */
    return !t.empty() && (t[0] == '_' || (t[0] >= 'a' && t[0] <= 'z'));
}

/* Parse a TI-asm integer operand: optional '-', decimal, or hex (trailing 'h',
   often with a leading 0 e.g. "0ah", "-020h"). */
static int asm_parse_int(const std::string &tok)
{
    std::string s = asm_trim(tok);
    if (s.empty()) return 0;
    bool neg = false; size_t i = 0;
    if (s[0] == '-') { neg = true; i = 1; } else if (s[0] == '+') i = 1;
    std::string num = s.substr(i);
    long v = 0;
    if (!num.empty() && (num.back() == 'h' || num.back() == 'H')) {
        v = strtol(num.c_str(), NULL, 16);
    } else {
        char *end = NULL;
        v = strtol(num.c_str(), &end, 10);
        if (end && *end) v = strtol(num.c_str(), NULL, 16); /* bare hex fallback */
    }
    return neg ? -(int)v : (int)v;
}

/* Operand count for the known MK2 animation opcodes (token after the opcode). */
static int asm_opcode_operands(const std::string &op)
{
    if (op == "ani_jump")       return 1;  /* target */
    if (op == "ani_adjustx")    return 1;  /* dx */
    if (op == "ani_adjustxy")   return 2;  /* dx, dy */
    if (op == "ani_calla")      return 1;  /* routine */
    if (op == "ani_sound")      return 1;  /* sound id */
    if (op == "ani_ochar_jump") return 2;  /* cond, target */
    if (op == "ani_flip")       return 0;
    if (op == "ani_flip_v")     return 0;
    if (op == "ani_nosleep")    return 0;
    return -1;                              /* unknown opcode */
}

static void asm_split_operands(const std::string &rest, std::vector<std::string> &out)
{
    std::string cur;
    for (char c : rest) {
        if (c == ',') { std::string t = asm_trim(cur); if (!t.empty()) out.push_back(t); cur.clear(); }
        else cur.push_back(c);
    }
    std::string t = asm_trim(cur);
    if (!t.empty()) out.push_back(t);
}

static void ClearAsmAnimTexture(void)
{
    if (g_asm_anim_tex) { SDL_DestroyTexture(g_asm_anim_tex); g_asm_anim_tex = NULL; }
    g_asm_anim_tex_w = g_asm_anim_tex_h = 0;
    g_asm_anim_last_drawn = -1;
}

/* Build a name->IMG-index map (case-insensitive) for the current document. */
static void AsmBuildNameMap(std::unordered_map<std::string,int> &m)
{
    m.clear();
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        std::string n = img_name_string(img);
        for (char &c : n) c = (char)toupper((unsigned char)c);
        if (!n.empty()) m.emplace(n, idx);
    }
}

static int AsmResolveSym(const std::unordered_map<std::string,int> &m, const std::string &sym)
{
    std::string key = sym;
    /* drop any "+offset" suffix and uppercase */
    size_t plus = key.find('+');
    if (plus != std::string::npos) key = key.substr(0, plus);
    for (char &c : key) c = (char)toupper((unsigned char)c);
    auto it = m.find(key);
    return (it != m.end()) ? it->second : -1;
}

/* Normalize a piece symbol to its IMG-frame name key (drop "+offset", uppercase). */
static std::string AsmSymKey(const std::string &sym)
{
    std::string key = sym;
    size_t plus = key.find('+');
    if (plus != std::string::npos) key = key.substr(0, plus);
    for (char &c : key) c = (char)toupper((unsigned char)c);
    return key;
}

/* Build a name -> (document, local index) map spanning EVERY open tab. A
   character's sprites are split across many IMG files (e.g. CAGE1..CAGE10.IMG),
   so a single animation only resolves fully when its pieces are looked up across
   all loaded documents. The active doc is inserted last so it wins name ties. */
static void AsmBuildGlobalNameMap(
        std::unordered_map<std::string, std::pair<Document*,int>> &m)
{
    m.clear();
    int active = document_active_index();
    int ntabs  = document_tab_count();
    /* pass 0: every non-active doc; pass 1: the active doc (overwrites ties). */
    for (int pass = 0; pass < 2; pass++) {
        for (int t = 0; t < ntabs; t++) {
            bool is_active = (t == active);
            if ((pass == 1) != is_active) continue;
            Document *d = document_get(t);
            if (!d) continue;
            int idx = 0;
            for (IMG *img = (IMG *)d->img_p; img; img = (IMG *)img->nxt_p, idx++) {
                std::string n = img_name_string(img);
                for (char &c : n) c = (char)toupper((unsigned char)c);
                if (!n.empty()) m[n] = std::make_pair(d, idx);
            }
        }
    }
}

/* Resolve every piece of an animation against all open documents, filling both
   the doc-local index (piece_img) and the owning document (piece_doc). */
static void AsmResolveAnimGlobal(AsmAnim &a)
{
    std::unordered_map<std::string, std::pair<Document*,int>> m;
    AsmBuildGlobalNameMap(m);
    a.missing = 0;
    for (auto &fr : a.frames) {
        fr.piece_img.assign(fr.piece_syms.size(), -1);
        fr.piece_doc.assign(fr.piece_syms.size(), (Document*)NULL);
        for (size_t p = 0; p < fr.piece_syms.size(); p++) {
            auto it = m.find(AsmSymKey(fr.piece_syms[p]));
            if (it != m.end()) {
                fr.piece_doc[p] = it->second.first;
                fr.piece_img[p] = it->second.second;
            } else {
                a.missing++;
            }
        }
    }
}

/* Pure parse of a character/exported ASM into a list of animations (no globals,
   no IMG load, no resolution against a specific doc beyond a best-effort first
   pass against the active doc). Shared by the player and opponent loaders. */
static bool ParseAsmAnimFile(const char *path, std::vector<AsmAnim> &out)
{
    out.clear();
    if (!path || !path[0]) return false;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Could not open ASM: %s", path);
        g_restore_msg_timer = 4.0f;
        return false;
    }

    /* Pass 1: gather label bodies (.long token lists) and per-entry comments. */
    std::unordered_map<std::string, std::vector<std::string>> body;
    std::unordered_map<std::string, std::vector<int>> word_body; /* .word ints, for *_local_anipts */
    std::vector<std::string> label_order;               /* labels in file order */
    std::unordered_map<std::string, std::string> comment_for; /* token -> comment */
    std::vector<std::string> anitab_labels;             /* directory tables, in order */

    char line[1024];
    std::string cur_label;
    while (fgets(line, sizeof(line), f)) {
        std::string raw(line);
        /* split off trailing comment */
        std::string comment;
        size_t sc = raw.find(';');
        if (sc != std::string::npos) { comment = asm_trim(raw.substr(sc + 1)); raw = raw.substr(0, sc); }
        /* full-line comment markers */
        std::string lead = asm_trim(raw);
        if (lead.empty()) continue;
        if (lead[0] == '*') continue;

        bool has_label = (line[0] != ' ' && line[0] != '\t');
        std::string label, directive, rest;
        std::string work = raw;
        if (has_label) {
            size_t ws = work.find_first_of(" \t");
            label = asm_trim(work.substr(0, ws == std::string::npos ? work.size() : ws));
            work = (ws == std::string::npos) ? "" : work.substr(ws);
            if (!label.empty()) {
                cur_label = label;
                if (body.find(cur_label) == body.end()) { body[cur_label]; label_order.push_back(cur_label); }
            }
        }
        work = asm_trim(work);
        if (!work.empty()) {
            size_t ws = work.find_first_of(" \t");
            directive = asm_trim(work.substr(0, ws == std::string::npos ? work.size() : ws));
            rest = (ws == std::string::npos) ? "" : asm_trim(work.substr(ws));
        }

        if (directive == ".long" && !cur_label.empty()) {
            std::vector<std::string> ops;
            asm_split_operands(rest, ops);
            for (auto &t : ops) body[cur_label].push_back(t);
            /* capture comment for a single anim-ref entry (anitab rows) */
            if (ops.size() == 1 && !comment.empty()) comment_for[ops[0]] = comment;
        } else if (directive == ".word" && !cur_label.empty()) {
            std::vector<std::string> ops;
            asm_split_operands(rest, ops);
            for (auto &t : ops) word_body[cur_label].push_back(asm_parse_int(t));
        }
        /* directory tables are named "*anitab*" */
        if (has_label && !label.empty()) {
            std::string low = label; for (char &c : low) c = (char)tolower((unsigned char)c);
            if (low.find("anitab") != std::string::npos) anitab_labels.push_back(label);
        }
    }
    fclose(f);

    /* Pass 2: build the ordered animation list. Prefer directory order. */
    std::unordered_map<std::string,int> name_map;
    AsmBuildNameMap(name_map);

    std::vector<std::string> anim_labels;
    std::unordered_map<std::string,bool> seen;
    auto ends_with = [](const std::string &s, const char *suf) {
        size_t n = strlen(suf);
        return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };
    auto add_anim_label = [&](const std::string &lbl) {
        if (lbl.empty() || seen.count(lbl)) return;
        if (body.find(lbl) == body.end()) return;        /* defined here only */
        if (ends_with(lbl, "_local_anipts")) return;     /* data table, not an anim */
        if (ends_with(lbl, "_dual_anipts")) return;      /* data table, not an anim */
        seen[lbl] = true; anim_labels.push_back(lbl);
    };
    for (auto &tab : anitab_labels)
        for (auto &tok : body[tab]) add_anim_label(tok);
    for (auto &lbl : label_order)
        if (lbl.size() > 2 && lbl[0] == 'a' && lbl[1] == '_') add_anim_label(lbl);

    for (auto &lbl : anim_labels) {
        AsmAnim a;
        a.label = lbl;
        auto cit = comment_for.find(lbl);
        a.name = (cit != comment_for.end() && !cit->second.empty()) ? cit->second : lbl;
        a.missing = 0;

        const std::vector<std::string> &toks = body[lbl];
        int cur_dx = 0, cur_dy = 0;
        bool cur_mirror = false, cur_mirror_v = false;
        bool stop = false;
        for (size_t ti = 0; ti < toks.size() && !stop; ti++) {
            const std::string &tok = toks[ti];
            if (tok == "0") break;             /* ani_end terminator */
            if (asm_is_control_token(tok)) {
                int nops = asm_opcode_operands(tok);
                if (tok == "ani_jump") {
                    std::string tgt = (ti + 1 < toks.size()) ? toks[ti + 1] : "";
                    a.control.push_back("loops" + (tgt.empty() ? "" : " to " + tgt));
                    stop = true;               /* loop point — frames captured */
                } else if (tok == "ani_adjustx" && ti + 1 < toks.size()) {
                    cur_dx += asm_parse_int(toks[ti + 1]); ti += 1;
                } else if (tok == "ani_adjustxy" && ti + 2 < toks.size()) {
                    cur_dx += asm_parse_int(toks[ti + 1]);
                    cur_dy += asm_parse_int(toks[ti + 2]); ti += 2;
                } else if (tok == "ani_flip") {
                    cur_mirror = !cur_mirror;
                    if (std::find(a.control.begin(), a.control.end(), "flip") == a.control.end())
                        a.control.push_back("flip");
                } else if (tok == "ani_flip_v") {
                    cur_mirror_v = !cur_mirror_v;
                    a.control.push_back("vflip");
                } else if (nops >= 0) {
                    a.control.push_back(tok);  /* known opcode: note + skip operands */
                    ti += (size_t)nops;
                } else {
                    a.control.push_back(tok + "?");  /* unknown: note and stop safely */
                    stop = true;
                }
                continue;
            }
            /* uppercase token = a frame-group label (or lone piece symbol) */
            AsmAnimFrame fr;
            fr.dx = cur_dx; fr.dy = cur_dy;
            fr.mirror = cur_mirror;
            fr.mirror_v = cur_mirror_v;
            auto bit = body.find(tok);
            if (bit != body.end()) {
                for (auto &p : bit->second) { if (p == "0") break; fr.piece_syms.push_back(p); }
            } else {
                fr.piece_syms.push_back(tok);  /* treat as a lone piece symbol */
            }
            for (auto &p : fr.piece_syms) {
                int ri = AsmResolveSym(name_map, p);
                fr.piece_img.push_back(ri);
                if (ri < 0) a.missing++;
            }
            a.frames.push_back(fr);
        }
        /* Round-trip local anipoints from a paired "<label>_local_anipts" .word
           table (emitted by imgtool's ASM export), one dx,dy pair per frame. */
        auto wit = word_body.find(lbl + "_local_anipts");
        if (wit != word_body.end()) {
            const std::vector<int> &w = wit->second;
            for (size_t fi = 0; fi < a.frames.size() && fi * 2 + 1 < w.size(); fi++) {
                a.frames[fi].dx = w[fi * 2];
                a.frames[fi].dy = w[fi * 2 + 1];
            }
        }
        if (!a.frames.empty() || !a.control.empty())
            out.push_back(std::move(a));
    }
    return !out.empty();
}

/* Build a name->IMG-index map (case-insensitive) for an arbitrary document. */
/* Resolve an animation's piece symbols across every open document. The doc
   argument is kept for call-site compatibility but no longer constrains lookup:
   a character's frames are split across many IMGs, so resolution must span them. */
void AsmResolveAnimAgainstDoc(AsmAnim &a, Document *doc)
{
    (void)doc;
    AsmResolveAnimGlobal(a);
}

/* Open every IMG (as a tab) needed to cover this animation's piece symbols that
   no currently-open document already provides. Scans the ASM folder, sibling
   data/ dirs, every open tab's folder and $IMGDIR. Greedy set-cover, so a
   character whose sprites span several files (CAGE1..CAGE10) gets each opened.
   Returns the number of IMGs opened. */
static int AsmAutoOpenImgsForAnim(const AsmAnim &a, const char *asm_path)
{
    std::unordered_map<std::string,bool> need;
    for (auto &fr : a.frames)
        for (auto &s : fr.piece_syms) {
            std::string u = AsmSymKey(s);
            if (!u.empty()) need[u] = true;
        }
    if (need.empty()) return 0;

    /* Drop symbols any already-open document provides. */
    {
        std::unordered_map<std::string, std::pair<Document*,int>> m;
        AsmBuildGlobalNameMap(m);
        for (auto it = need.begin(); it != need.end(); ) {
            if (m.count(it->first)) it = need.erase(it);
            else ++it;
        }
    }
    if (need.empty()) return 0;

    std::string asmdir = asm_path ? asm_path : "";
    size_t sl = asmdir.find_last_of("\\/");
    asmdir = (sl != std::string::npos) ? asmdir.substr(0, sl) : ".";

    /* Collect every plausible folder an IMG could live in, deduped. ASM files
       commonly sit in a src/ tree while the IMGs live in a sibling data/ dir,
       so probe those relatives plus every open tab's folder and IMGDIR. */
    std::vector<std::string> dirs;
    auto add_dir = [&](const std::string &d) {
        if (d.empty()) return;
        std::string low = d; for (char &c : low) c = (char)tolower((unsigned char)c);
        for (auto &ex : dirs) {
            std::string el = ex; for (char &c : el) c = (char)tolower((unsigned char)c);
            if (el == low) return;
        }
        dirs.push_back(d);
    };
    add_dir(asmdir);
    add_dir(asmdir + "\\data");
    add_dir(asmdir + "\\..\\data");
    add_dir(asmdir + "\\..\\DATA");
    add_dir(asmdir + "\\..");
    add_dir(asmdir + "\\..\\..\\data");
    for (int t = 0; t < document_tab_count(); t++) {
        Document *d = document_get(t);
        if (d && d->fpath_s[0]) add_dir(d->fpath_s);
    }
    if (g_doc->fpath_s[0]) add_dir(g_doc->fpath_s);
    const char *imgdir = getenv("IMGDIR");
    if (imgdir && imgdir[0]) add_dir(imgdir);

    /* Probe every candidate IMG once, recording its uppercased frame names. */
    struct ImgCand { std::string path; std::vector<std::string> names; };
    std::vector<ImgCand> cands;
    for (auto &d : dirs) {
        std::vector<FileEntry> entries;
        GetDirectoryFiles(d, entries, "IMG");
        for (auto &e : entries) {
            if (e.is_dir) continue;
            ImgCand c;
            c.path = PathCombine(d, e.name);
            std::vector<std::string> names;
            ProbeImgFrameNames(c.path.c_str(), names);
            for (auto &nm : names) {
                std::string u = nm; for (char &ch : u) ch = (char)toupper((unsigned char)ch);
                c.names.push_back(u);
            }
            cands.push_back(std::move(c));
        }
    }

    /* Greedy set-cover: repeatedly open the IMG covering the most still-missing
       symbols until everything resolves or no remaining file helps. */
    int opened = 0, guard = 0;
    while (!need.empty() && guard++ < 64) {
        int best = -1, best_hits = 0;
        for (size_t i = 0; i < cands.size(); i++) {
            int hits = 0;
            for (auto &u : cands[i].names) if (need.count(u)) hits++;
            if (hits > best_hits) { best_hits = hits; best = (int)i; }
        }
        if (best < 0) break;
        OpenImgFile(cands[best].path);
        opened++;
        for (auto &u : cands[best].names) need.erase(u);
        cands[best].names.clear();   /* don't pick the same file again */
    }
    return opened;
}

bool LoadAsmAnimations(const char *path)   /* player */
{
    if (!ParseAsmAnimFile(path, g_asm_anims)) {
        if (g_asm_anims.empty())
            snprintf(g_restore_msg, sizeof(g_restore_msg), "No animations found in ASM.");
        g_restore_msg_timer = 4.0f;
        return false;
    }
    g_asm_anim_file = path;
    AsmAnimSelect(g_asm_anims.empty() ? -1 : 0);
    /* Defer IMG loading to the main loop: opening tabs mid-parse is avoided, and
       the handler opens every IMG the selected anim needs, then re-resolves. */
    if (g_asm_anim_sel >= 0) g_request_asm_autoload = true;

    const char *base = (strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path);
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Loaded %d animation%s from %s; opening its sprites...",
             (int)g_asm_anims.size(), g_asm_anims.size() == 1 ? "" : "s", base);
    g_restore_msg_timer = 4.0f;
    return !g_asm_anims.empty();
}

bool LoadAsmOpponent(const char *path)      /* fatality opponent */
{
    if (!ParseAsmAnimFile(path, g_asm_opp_anims)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No animations found in opponent ASM.");
        g_restore_msg_timer = 4.0f;
        return false;
    }
    g_asm_opp_file = path;
    g_asm_opp_doc = g_doc;
    g_asm_opp_doc_idx = document_active_index();
    g_asm_opp_sel = g_asm_opp_anims.empty() ? -1 : 0;
    if (g_asm_opp_sel >= 0)
        AsmResolveAnimGlobal(g_asm_opp_anims[g_asm_opp_sel]);
    g_asm_opp_enabled = true;
    /* Defer opening the opponent's sprite IMGs to the main loop. */
    if (g_asm_opp_sel >= 0) g_request_asm_opp_autoload = true;

    /* Default the opponent to face the player (mirror = opposite of the player
       ASM lane); only set here so the user can still flip it. */
    {
        bool *pf = WorldMarkedMirrorFlag(g_world_marked_state, kWorldAsmSlot);
        bool *of = WorldMarkedMirrorFlag(g_world_marked_state, kWorldAsmOpponentSlot);
        if (of) *of = pf ? !*pf : true;
    }

    const char *base = (strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path);
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Loaded opponent: %d animation%s from %s; opening its sprites...",
             (int)g_asm_opp_anims.size(), g_asm_opp_anims.size() == 1 ? "" : "s", base);
    g_restore_msg_timer = 4.0f;
    return !g_asm_opp_anims.empty();
}

bool AutoLoadDefaultLiuKangOpponent(void)
{
    /* Preserve an explicitly loaded opponent.  Otherwise MK2 character ASM
       files normally live beside one another, so a loaded player ASM gives
       us a reliable project-local MKLK.ASM path without hard-coding a root. */
    if (g_asm_opp_anims.empty()) {
        std::vector<std::string> candidates;
        if (!g_asm_anim_file.empty()) {
            std::string path = g_asm_anim_file;
            size_t slash = path.find_last_of("\\/");
            if (slash != std::string::npos) {
                path.resize(slash + 1);
                candidates.push_back(path + "MKLK.ASM");
            }
        }
        if (g_doc && g_doc->fpath_s[0]) {
            std::string img_dir = g_doc->fpath_s;
            candidates.push_back(img_dir + "\\MKLK.ASM");
            candidates.push_back(img_dir + "\\..\\src\\MKLK.ASM");
        }
        bool loaded = false;
        for (const std::string &path : candidates) {
            FILE *probe = fopen(path.c_str(), "rb");
            if (!probe) continue;
            fclose(probe);
            if (LoadAsmOpponent(path.c_str())) { loaded = true; break; }
        }
        if (!loaded) return false;
    }

    int best = -1;
    int best_score = -1;
    for (int i = 0; i < (int)g_asm_opp_anims.size(); i++) {
        std::string key = g_asm_opp_anims[i].label + " " + g_asm_opp_anims[i].name;
        for (char &c : key) c = (char)tolower((unsigned char)c);
        int score = 0;
        if (key.find("react") != std::string::npos) score += 100;
        if (key.find("dizzy") != std::string::npos) score += 80;
        if (key.find("hit") != std::string::npos) score += 60;
        if (key.find("fall") != std::string::npos) score += 50;
        if (key.find("die") != std::string::npos) score += 40;
        if (key.find("stance") != std::string::npos) score += 10;
        if (score > best_score) { best_score = score; best = i; }
    }
    if (best < 0) return false;
    g_asm_opp_sel = best;
    AsmResolveAnimGlobal(g_asm_opp_anims[best]);
    g_asm_opp_enabled = true;
    g_request_asm_opp_autoload = true;
    return true;
}

/* Re-resolve the selected anim against the current IMG and size the playback
   canvas to the anipoint-anchored union of all its frame pieces. */
void AsmAnimSelect(int i)
{
    g_asm_anim_sel = i;
    g_asm_anim_frame = 0;
    g_asm_anim_timer = 0.0f;
    ClearAsmAnimTexture();
    if (i < 0 || i >= (int)g_asm_anims.size()) return;

    g_asm_anim_doc = g_doc;                         /* representative doc for lane fallback */
    g_asm_anim_doc_idx = document_active_index();

    AsmAnim &a = g_asm_anims[i];
    AsmResolveAnimGlobal(a);                         /* resolve across all open IMGs */
    int minx = 0x3FFFFFFF, miny = 0x3FFFFFFF, maxx = -0x3FFFFFFF, maxy = -0x3FFFFFFF;
    bool any = false;
    for (auto &fr : a.frames) {
        for (size_t p = 0; p < fr.piece_syms.size(); p++) {
            int ri = fr.piece_img[p];
            if (ri < 0) continue;
            IMG *img = doc_get_img(fr.piece_doc[p], ri);
            if (!img) continue;
            int x0 = -(int)(short)img->anix + fr.dx, y0 = -(int)(short)img->aniy + fr.dy;
            int x1 = x0 + img->w, y1 = y0 + img->h;
            if (x0 < minx) minx = x0; if (y0 < miny) miny = y0;
            if (x1 > maxx) maxx = x1; if (y1 > maxy) maxy = y1;
            any = true;
        }
    }
    if (!any) { g_asm_anim_canvas_w = g_asm_anim_canvas_h = 0; return; }
    g_asm_anim_minx = minx; g_asm_anim_miny = miny;
    int cw = maxx - minx, ch = maxy - miny;
    if (cw < 1) cw = 1; if (ch < 1) ch = 1;
    if (cw > 1024) cw = 1024; if (ch > 1024) ch = 1024;
    g_asm_anim_canvas_w = cw; g_asm_anim_canvas_h = ch;
}

/* Deferred (main-loop) handler: open every IMG the selected player anim needs as
   tabs, then re-resolve and re-size against them. If nothing resolves even after
   the scan, fall back to prompting the user to locate an IMG. */
void AsmProcessAutoload(void)

{
    if (g_asm_anim_sel < 0 || g_asm_anim_sel >= (int)g_asm_anims.size()) return;
    AsmAutoOpenImgsForAnim(g_asm_anims[g_asm_anim_sel], g_asm_anim_file.c_str());
    AsmAnimSelect(g_asm_anim_sel);   /* re-resolve against the now-open IMGs */

    int resolved = 0;
    for (auto &fr : g_asm_anims[g_asm_anim_sel].frames)
        for (int ri : fr.piece_img) if (ri >= 0) resolved++;
    if (resolved == 0) g_request_locate_img = true;
}

/* Same as above for the fatality opponent ASM. */
void AsmProcessOppAutoload(void)

{
    if (g_asm_opp_sel < 0 || g_asm_opp_sel >= (int)g_asm_opp_anims.size()) return;
    AsmAutoOpenImgsForAnim(g_asm_opp_anims[g_asm_opp_sel], g_asm_opp_file.c_str());
    AsmResolveAnimGlobal(g_asm_opp_anims[g_asm_opp_sel]);
    g_asm_opp_doc = g_doc;
    g_asm_opp_doc_idx = document_active_index();

    int resolved = 0;
    for (auto &fr : g_asm_opp_anims[g_asm_opp_sel].frames)
        for (int ri : fr.piece_img) if (ri >= 0) resolved++;
    if (resolved == 0) g_request_locate_opp_img = true;
}

static std::string WvpKey(const char *prefix, int idx, const char *field)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s.%d.%s", prefix, idx, field);
    return std::string(buf);
}

static std::string WvpJoinInts(const std::vector<int> &values)
{
    std::string out;
    for (size_t i = 0; i < values.size(); i++) {
        if (i) out += ",";
        out += std::to_string(values[i]);
    }
    return out;
}

static std::vector<int> WvpParseInts(const std::string &text)
{
    std::vector<int> out;
    const char *p = text.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        out.push_back((int)v);
        p = end;
        while (*p && *p != ',') p++;
    }
    return out;
}

static void WvpWriteString(FILE *f, const char *key, const std::string &value)
{
    fprintf(f, "%s=%s\n", key, value.c_str());
}

static void WvpWriteInt(FILE *f, const char *key, int value)
{
    fprintf(f, "%s=%d\n", key, value);
}

static void WvpWriteFloat(FILE *f, const char *key, float value)
{
    fprintf(f, "%s=%.6g\n", key, value);
}

static void WvpWriteBool(FILE *f, const char *key, bool value)
{
    WvpWriteInt(f, key, value ? 1 : 0);
}

static void WvpWriteVec(FILE *f, const std::string &key,
                        const std::vector<int> &values)
{
    WvpWriteString(f, key.c_str(), WvpJoinInts(values));
}

static bool WvpReadFile(const char *path,
                        std::unordered_map<std::string, std::string> &kv)
{
    kv.clear();
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *kend = eq - 1;
        while (kend >= p && (*kend == ' ' || *kend == '\t'))
            *kend-- = '\0';
        kv[std::string(p)] = std::string(eq + 1);
    }
    fclose(f);
    return true;
}

static std::string WvpGetString(
    const std::unordered_map<std::string, std::string> &kv,
    const std::string &key,
    const std::string &fallback = std::string())
{
    auto it = kv.find(key);
    return it == kv.end() ? fallback : it->second;
}

static int WvpGetInt(const std::unordered_map<std::string, std::string> &kv,
                     const std::string &key, int fallback)
{
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) return fallback;
    char *end = NULL;
    long v = strtol(it->second.c_str(), &end, 10);
    return end == it->second.c_str() ? fallback : (int)v;
}

static float WvpGetFloat(const std::unordered_map<std::string, std::string> &kv,
                         const std::string &key, float fallback)
{
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) return fallback;
    char *end = NULL;
    float v = strtof(it->second.c_str(), &end);
    return end == it->second.c_str() ? fallback : v;
}

static bool WvpGetBool(const std::unordered_map<std::string, std::string> &kv,
                       const std::string &key, bool fallback)
{
    return WvpGetInt(kv, key, fallback ? 1 : 0) != 0;
}

static std::vector<int> WvpGetVec(
    const std::unordered_map<std::string, std::string> &kv,
    const std::string &key)
{
    auto it = kv.find(key);
    return it == kv.end() ? std::vector<int>() : WvpParseInts(it->second);
}

static int WvpDocIndexForPointer(Document *doc)
{
    if (!doc) return -1;
    for (int i = 0; i < document_tab_count(); i++)
        if (document_get(i) == doc)
            return i;
    return -1;
}

static std::vector<int> WvpMarkedIndices(Document *doc)
{
    std::vector<int> out;
    int idx = 0;
    for (IMG *img = doc ? (IMG *)doc->img_p : NULL;
         img; img = (IMG *)img->nxt_p, idx++) {
        if (img->flags & 1)
            out.push_back(idx);
    }
    return out;
}

static void WvpApplyMarkedIndices(Document *doc, const std::vector<int> &marked)
{
    if (!doc) return;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p)
        img->flags = (unsigned short)(img->flags & ~1);
    for (int idx : marked) {
        IMG *img = doc_get_img(doc, idx);
        if (img) img->flags = (unsigned short)(img->flags | 1);
    }
}

static int WvpFindAsmByLabel(const std::vector<AsmAnim> &anims,
                             const std::string &label,
                             int fallback)
{
    if (!label.empty()) {
        for (int i = 0; i < (int)anims.size(); i++)
            if (anims[i].label == label)
                return i;
    }
    if (fallback >= 0 && fallback < (int)anims.size()) return fallback;
    return anims.empty() ? -1 : 0;
}

static void WvpClearAsmState(void)
{
    g_asm_anims.clear();
    g_asm_anim_sel = -1;
    g_asm_anim_file.clear();
    g_asm_anim_doc = NULL;
    g_asm_anim_doc_idx = -1;
    g_asm_lane_enabled = false;
    g_asm_opp_anims.clear();
    g_asm_opp_sel = -1;
    g_asm_opp_file.clear();
    g_asm_opp_doc = NULL;
    g_asm_opp_doc_idx = -1;
    g_asm_opp_enabled = false;
    g_request_asm_autoload = false;
    g_request_asm_opp_autoload = false;
    g_request_locate_img = false;
    g_request_locate_opp_img = false;
    ClearAsmAnimTexture();
}

static int WvpResolveDocIndex(int saved_idx,
                              const std::string &path,
                              const std::vector<int> &doc_map)
{
    bool had_path = !path.empty();
    if (!path.empty()) {
        int idx = FindOpenDocumentByPath(path);
        if (idx < 0 && PathReadable(path)) {
            OpenImgFile(path);
            idx = FindOpenDocumentByPath(path);
        }
        if (idx >= 0) return idx;
    }
    if (saved_idx >= 0 && saved_idx < (int)doc_map.size() &&
        doc_map[(size_t)saved_idx] >= 0)
        return doc_map[(size_t)saved_idx];
    if (!had_path && saved_idx >= 0 && saved_idx < document_tab_count())
        return saved_idx;
    return -1;
}

static int WvpSlotFrameCount(const WorldMarkedSequenceState &state, int slot)
{
    int n = (int)state.sequence_frames[slot].size();
    if ((int)state.default_frames[slot].size() > n) n = (int)state.default_frames[slot].size();
    if ((int)state.frame_delays[slot].size() > n) n = (int)state.frame_delays[slot].size();
    if ((int)state.local_dx[slot].size() > n) n = (int)state.local_dx[slot].size();
    if ((int)state.local_dy[slot].size() > n) n = (int)state.local_dy[slot].size();
    if ((int)state.visible_from[slot].size() > n) n = (int)state.visible_from[slot].size();
    if ((int)state.visible_until[slot].size() > n) n = (int)state.visible_until[slot].size();
    if ((int)state.motion_dx[slot].size() > n) n = (int)state.motion_dx[slot].size();
    if ((int)state.motion_dy[slot].size() > n) n = (int)state.motion_dy[slot].size();
    if ((int)state.motion_cap_x[slot].size() > n) n = (int)state.motion_cap_x[slot].size();
    if ((int)state.motion_cap_y[slot].size() > n) n = (int)state.motion_cap_y[slot].size();
    if ((int)state.frame_mirror[slot].size() > n) n = (int)state.frame_mirror[slot].size();
    if ((int)state.frame_z[slot].size() > n) n = (int)state.frame_z[slot].size();
    if ((int)state.dual_on[slot].size() > n) n = (int)state.dual_on[slot].size();
    if ((int)state.dual_dx[slot].size() > n) n = (int)state.dual_dx[slot].size();
    if ((int)state.dual_dy[slot].size() > n) n = (int)state.dual_dy[slot].size();
    if ((int)state.dual_z[slot].size() > n) n = (int)state.dual_z[slot].size();
    if ((int)state.frame_doc[slot].size() > n) n = (int)state.frame_doc[slot].size();
    return n;
}

static void WvpWriteSlot(FILE *f, const WorldMarkedSequenceState &state,
                         int slot)
{
    char key[128];
    int doc_idx = state.sequence_doc_idx[slot];
    if (doc_idx < 0)
        doc_idx = WvpDocIndexForPointer(state.sequence_doc[slot]);
    std::string doc_path = DocFullPath(document_get(doc_idx));

    snprintf(key, sizeof(key), "slot.%d.visible", slot);
    WvpWriteBool(f, key, state.lane_visible[slot]);
    snprintf(key, sizeof(key), "slot.%d.hold_end", slot);
    WvpWriteBool(f, key, state.hold_end[slot]);
    snprintf(key, sizeof(key), "slot.%d.mirror", slot);
    {
        WorldMarkedSequenceState &mutable_state =
            const_cast<WorldMarkedSequenceState &>(state);
        bool *mirror = WorldMarkedMirrorFlag(mutable_state, slot);
        WvpWriteBool(f, key, mirror ? *mirror : false);
    }
    snprintf(key, sizeof(key), "slot.%d.doc_idx", slot);
    WvpWriteInt(f, key, doc_idx);
    snprintf(key, sizeof(key), "slot.%d.doc_path", slot);
    WvpWriteString(f, key, doc_path);
    WvpWriteVec(f, WvpKey("slot", slot, "sequence_frames"), state.sequence_frames[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "default_frames"), state.default_frames[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "frame_delays"), state.frame_delays[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "local_dx"), state.local_dx[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "local_dy"), state.local_dy[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "visible_from"), state.visible_from[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "visible_until"), state.visible_until[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "motion_dx"), state.motion_dx[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "motion_dy"), state.motion_dy[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "motion_cap_x"), state.motion_cap_x[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "motion_cap_y"), state.motion_cap_y[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "frame_mirror"), state.frame_mirror[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "frame_z"), state.frame_z[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "dual_on"), state.dual_on[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "dual_dx"), state.dual_dx[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "dual_dy"), state.dual_dy[slot]);
    WvpWriteVec(f, WvpKey("slot", slot, "dual_z"), state.dual_z[slot]);
    /* Per-entry doc override: -1 means "use this row's own doc" (doc_idx
       above), any other value is a doc.count index — a frame dragged in
       from another row's document. */
    WvpWriteVec(f, WvpKey("slot", slot, "frame_doc_idx"), state.frame_doc[slot]);
    snprintf(key, sizeof(key), "slot.%d.pingpong_delay", slot);
    WvpWriteInt(f, key, state.pingpong_delay[slot]);
    snprintf(key, sizeof(key), "slot.%d.stop_tick", slot);
    WvpWriteInt(f, key, state.stop_tick[slot]);
    snprintf(key, sizeof(key), "slot.%d.auto_step", slot);
    WvpWriteInt(f, key, state.auto_step[slot]);
    snprintf(key, sizeof(key), "slot.%d.auto_life", slot);
    WvpWriteInt(f, key, state.auto_life[slot]);
    snprintf(key, sizeof(key), "slot.%d.auto_vx", slot);
    WvpWriteInt(f, key, state.auto_vx[slot]);
    snprintf(key, sizeof(key), "slot.%d.auto_vy", slot);
    WvpWriteInt(f, key, state.auto_vy[slot]);
    snprintf(key, sizeof(key), "slot.%d.auto_y", slot);
    WvpWriteInt(f, key, state.auto_y[slot]);
    snprintf(key, sizeof(key), "slot.%d.chain_count", slot);
    WvpWriteInt(f, key, state.chain_count[slot]);
    snprintf(key, sizeof(key), "slot.%d.chain_gap", slot);
    WvpWriteInt(f, key, state.chain_gap[slot]);
    snprintf(key, sizeof(key), "slot.%d.chain_delay", slot);
    WvpWriteInt(f, key, state.chain_delay[slot]);
    snprintf(key, sizeof(key), "slot.%d.chain_vy", slot);
    WvpWriteInt(f, key, state.chain_vy[slot]);
    snprintf(key, sizeof(key), "slot.%d.chain_pingpong", slot);
    WvpWriteBool(f, key, state.chain_pingpong[slot]);
    snprintf(key, sizeof(key), "slot.%d.subframe_swap_tick", slot);
    WvpWriteInt(f, key, state.subframe_swap_tick[slot]);
    snprintf(key, sizeof(key), "slot.%d.subframe_waterline_y", slot);
    WvpWriteInt(f, key, state.subframe_waterline_y[slot]);
    snprintf(key, sizeof(key), "slot.%d.subframe_fine_source", slot);
    WvpWriteInt(f, key, state.subframe_fine_source[slot]);
}

static void WvpReadSlot(const std::unordered_map<std::string, std::string> &kv,
                        WorldMarkedSequenceState &state,
                        int slot,
                        const std::vector<int> &doc_map)
{
    std::string prefix = "slot." + std::to_string(slot) + ".";
    state.lane_visible[slot] = WvpGetBool(kv, prefix + "visible", state.lane_visible[slot]);
    state.hold_end[slot] = WvpGetBool(kv, prefix + "hold_end", state.hold_end[slot]);
    bool *mirror = WorldMarkedMirrorFlag(state, slot);
    if (mirror) *mirror = WvpGetBool(kv, prefix + "mirror", *mirror);

    int saved_doc_idx = WvpGetInt(kv, prefix + "doc_idx", -1);
    std::string doc_path = WvpGetString(kv, prefix + "doc_path");
    int doc_idx = WvpResolveDocIndex(saved_doc_idx, doc_path, doc_map);
    state.sequence_doc_idx[slot] = doc_idx;
    state.sequence_doc[slot] = document_get(doc_idx);

    state.sequence_frames[slot] = WvpGetVec(kv, prefix + "sequence_frames");
    state.default_frames[slot] = WvpGetVec(kv, prefix + "default_frames");
    state.frame_delays[slot] = WvpGetVec(kv, prefix + "frame_delays");
    state.local_dx[slot] = WvpGetVec(kv, prefix + "local_dx");
    state.local_dy[slot] = WvpGetVec(kv, prefix + "local_dy");
    state.visible_from[slot] = WvpGetVec(kv, prefix + "visible_from");
    state.visible_until[slot] = WvpGetVec(kv, prefix + "visible_until");
    state.motion_dx[slot] = WvpGetVec(kv, prefix + "motion_dx");
    state.motion_dy[slot] = WvpGetVec(kv, prefix + "motion_dy");
    state.motion_cap_x[slot] = WvpGetVec(kv, prefix + "motion_cap_x");
    state.motion_cap_y[slot] = WvpGetVec(kv, prefix + "motion_cap_y");
    state.frame_mirror[slot] = WvpGetVec(kv, prefix + "frame_mirror");
    state.frame_z[slot] = WvpGetVec(kv, prefix + "frame_z");
    state.dual_on[slot] = WvpGetVec(kv, prefix + "dual_on");
    state.dual_dx[slot] = WvpGetVec(kv, prefix + "dual_dx");
    state.dual_dy[slot] = WvpGetVec(kv, prefix + "dual_dy");
    state.dual_z[slot] = WvpGetVec(kv, prefix + "dual_z");

    /* frame_doc[slot] stores a doc tab index (-1 = this row's own doc), never
       a Document* — the saved index was only ever valid in the saving
       session's own tab order, so remap it the same way doc_idx above is
       remapped; -1 needs no remapping. */
    std::vector<int> fdoc_idx = WvpGetVec(kv, prefix + "frame_doc_idx");
    state.frame_doc[slot].assign(fdoc_idx.size(), -1);
    for (size_t i = 0; i < fdoc_idx.size(); i++) {
        if (fdoc_idx[i] < 0) continue;
        state.frame_doc[slot][i] = WvpResolveDocIndex(fdoc_idx[i], std::string(), doc_map);
    }

    state.pingpong_delay[slot] = WvpGetInt(kv, prefix + "pingpong_delay", state.pingpong_delay[slot]);
    state.stop_tick[slot] = WvpGetInt(kv, prefix + "stop_tick", state.stop_tick[slot]);
    state.auto_step[slot] = WvpGetInt(kv, prefix + "auto_step", state.auto_step[slot]);
    state.auto_life[slot] = WvpGetInt(kv, prefix + "auto_life", state.auto_life[slot]);
    state.auto_vx[slot] = WvpGetInt(kv, prefix + "auto_vx", state.auto_vx[slot]);
    state.auto_vy[slot] = WvpGetInt(kv, prefix + "auto_vy", state.auto_vy[slot]);
    state.auto_y[slot] = WvpGetInt(kv, prefix + "auto_y", state.auto_y[slot]);
    state.chain_count[slot] = WvpGetInt(kv, prefix + "chain_count", state.chain_count[slot]);
    state.chain_gap[slot] = WvpGetInt(kv, prefix + "chain_gap", state.chain_gap[slot]);
    state.chain_delay[slot] = WvpGetInt(kv, prefix + "chain_delay", state.chain_delay[slot]);
    state.chain_vy[slot] = WvpGetInt(kv, prefix + "chain_vy", state.chain_vy[slot]);
    state.chain_pingpong[slot] = WvpGetBool(kv, prefix + "chain_pingpong", state.chain_pingpong[slot]);
    state.subframe_swap_tick[slot] = WvpGetInt(kv, prefix + "subframe_swap_tick", state.subframe_swap_tick[slot]);
    state.subframe_waterline_y[slot] = WvpGetInt(kv, prefix + "subframe_waterline_y", state.subframe_waterline_y[slot]);
    state.subframe_fine_source[slot] = WvpGetInt(kv, prefix + "subframe_fine_source", state.subframe_fine_source[slot]);

    EnsureWorldMarkedFrameDelays(state, slot, WvpSlotFrameCount(state, slot));
}

static bool SaveWorldProjectFile(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Could not write World View project.");
        g_restore_msg_timer = 4.0f;
        return false;
    }

    WorldMarkedSequenceState &state = g_world_marked_state;
    fprintf(f, "format=imgtool_world_project\n");
    WvpWriteInt(f, "version", 1);
    WvpWriteString(f, "app", "midway-imgtool");

    WvpWriteBool(f, "world.enabled", g_world_state.enabled);
    WvpWriteInt(f, "world.w", g_world_state.w);
    WvpWriteInt(f, "world.h", g_world_state.h);
    WvpWriteInt(f, "world.origin_x", g_world_state.origin_x);
    WvpWriteInt(f, "world.origin_y", g_world_state.origin_y);
    WvpWriteBool(f, "world.onion", g_world_state.onion);

    WvpWriteBool(f, "state.marked_play", state.marked_play);
    WvpWriteFloat(f, "state.fps", state.fps);
    WvpWriteFloat(f, "state.timer", state.timer);
    WvpWriteInt(f, "state.frame", state.frame);
    WvpWriteBool(f, "state.paused", state.paused);
    WvpWriteBool(f, "state.dummy_decap_body", state.dummy_decap_body);
    WvpWriteBool(f, "state.dummy_decap_reset", state.dummy_decap_reset);
    WvpWriteBool(f, "state.dummy_decap_manual", state.dummy_decap_manual);
    WvpWriteInt(f, "state.dummy_decap_doc_idx", state.dummy_decap_doc_idx);
    WvpWriteString(f, "state.dummy_decap_doc_path",
                   DocFullPath(document_get(state.dummy_decap_doc_idx)));
    WvpWriteString(f, "state.dummy_decap_prefix", state.dummy_decap_prefix);
    WvpWriteBool(f, "state.draw_sprite_borders", state.draw_sprite_borders);
    WvpWriteBool(f, "state.show_boundary_overlay", state.show_boundary_overlay);
    WvpWriteBool(f, "state.embedded_active", state.embedded_active);
    WvpWriteBool(f, "state.embedded_is_script", state.embedded_is_script);
    WvpWriteBool(f, "state.embedded_show_companions", state.embedded_show_companions);
    WvpWriteInt(f, "state.embedded_record_index", state.embedded_record_index);
    WvpWriteInt(f, "state.embedded_doc_idx", state.embedded_doc_idx);
    WvpWriteString(f, "state.embedded_doc_path",
                   DocFullPath(document_get(state.embedded_doc_idx)));
    WvpWriteString(f, "state.embedded_name", state.embedded_name);
    WvpWriteVec(f, "state.embedded_targets", state.embedded_targets);
    WvpWriteInt(f, "state.embedded_label_count",
                (int)state.embedded_frame_labels.size());
    for (int i = 0; i < (int)state.embedded_frame_labels.size(); i++)
        WvpWriteString(f, WvpKey("embedded_label", i, "text").c_str(),
                       state.embedded_frame_labels[(size_t)i]);

    int doc_count = document_tab_count();
    WvpWriteInt(f, "doc.count", doc_count);
    WvpWriteInt(f, "doc.active", document_active_index());
    for (int i = 0; i < doc_count; i++) {
        Document *doc = document_get(i);
        WvpWriteString(f, WvpKey("doc", i, "path").c_str(), DocFullPath(doc));
        WvpWriteVec(f, WvpKey("doc", i, "marked"), WvpMarkedIndices(doc));
    }

    WvpWriteString(f, "asm.player.path", g_asm_anim_file);
    WvpWriteInt(f, "asm.player.sel", g_asm_anim_sel);
    WvpWriteString(f, "asm.player.label",
                   (g_asm_anim_sel >= 0 && g_asm_anim_sel < (int)g_asm_anims.size())
                       ? g_asm_anims[g_asm_anim_sel].label : std::string());
    WvpWriteBool(f, "asm.player.enabled", g_asm_lane_enabled);
    WvpWriteString(f, "asm.opp.path", g_asm_opp_file);
    WvpWriteInt(f, "asm.opp.sel", g_asm_opp_sel);
    WvpWriteString(f, "asm.opp.label",
                   (g_asm_opp_sel >= 0 && g_asm_opp_sel < (int)g_asm_opp_anims.size())
                       ? g_asm_opp_anims[g_asm_opp_sel].label : std::string());
    WvpWriteBool(f, "asm.opp.enabled", g_asm_opp_enabled);
    WvpWriteBool(f, "asm.window.show", g_show_asm_anim);
    WvpWriteInt(f, "asm.window.frame", g_asm_anim_frame);
    WvpWriteBool(f, "asm.window.play", g_asm_anim_play);
    WvpWriteFloat(f, "asm.window.fps", g_asm_anim_fps);

    WvpWriteInt(f, "slot.count", kWorldMarkedMaxTabs);
    for (int slot = 0; slot < kWorldMarkedMaxTabs; slot++)
        WvpWriteSlot(f, state, slot);

    WvpWriteInt(f, "split.count", (int)state.split_lanes.size());
    for (int i = 0; i < (int)state.split_lanes.size(); i++) {
        const WorldMarkedSplitLane &split = state.split_lanes[(size_t)i];
        WvpWriteInt(f, WvpKey("split", i, "slot").c_str(), split.slot);
        WvpWriteInt(f, WvpKey("split", i, "doc_idx").c_str(), split.doc_idx);
        WvpWriteString(f, WvpKey("split", i, "doc_path").c_str(),
                       DocFullPath(document_get(split.doc_idx)));
    }

    fclose(f);
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Saved World View project.");
    g_restore_msg_timer = 4.0f;
    return true;
}

static bool LoadWorldProjectFile(const char *path)
{
    std::unordered_map<std::string, std::string> kv;
    if (!WvpReadFile(path, kv) ||
        WvpGetString(kv, "format") != "imgtool_world_project") {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Not an imgtool World View project.");
        g_restore_msg_timer = 4.0f;
        return false;
    }

    /* A project is a complete workspace, not an overlay on its caller's
       tabs. Start clean so every saved IMG gets the same document index it
       had when the WVP was written. */
    while (document_tab_count() > 1)
        document_close_tab(document_tab_count() - 1);
    document_set_active(0);
    document_clear_contents(g_doc);
    ResetPerDocumentUiState(false);

    std::string project_path(path ? path : "");
    size_t project_sep = project_path.find_last_of("\\/");
    std::string project_dir = project_sep == std::string::npos
                            ? std::string() : project_path.substr(0, project_sep);
    auto resolve_img_path = [&](const std::string &saved) -> std::string {
        if (saved.empty()) return std::string();
        if (PathReadable(saved)) return saved;
        std::string relative = project_dir.empty() ? saved : PathCombine(project_dir, saved);
        return PathReadable(relative) ? relative : std::string();
    };

    int doc_count = WvpGetInt(kv, "doc.count", 0);
    std::vector<int> doc_map((size_t)(doc_count > 0 ? doc_count : 0), -1);
    int missing_docs = 0;
    for (int i = 0; i < doc_count; i++) {
        std::string saved_path = WvpGetString(kv, WvpKey("doc", i, "path"));
        if (saved_path.empty()) { missing_docs++; continue; }
        std::string doc_path = resolve_img_path(saved_path);
        if (doc_path.empty()) {
            missing_docs++;
            continue;
        }
        OpenImgFile(doc_path);
        int idx = FindOpenDocumentByPath(doc_path);
        Document *doc = document_get(idx);
        if (!doc || doc->imgcnt == 0) {
            missing_docs++;
            continue;
        }
        doc_map[(size_t)i] = idx;
        WvpApplyMarkedIndices(doc, WvpGetVec(kv, WvpKey("doc", i, "marked")));
    }
    g_last_world_project_path = project_path;

    WvpClearAsmState();
    std::string player_path = WvpGetString(kv, "asm.player.path");
    if (!player_path.empty() && PathReadable(player_path) &&
        LoadAsmAnimations(player_path.c_str())) {
        int sel = WvpFindAsmByLabel(g_asm_anims,
                                    WvpGetString(kv, "asm.player.label"),
                                    WvpGetInt(kv, "asm.player.sel", 0));
        AsmAnimSelect(sel);
        g_request_asm_autoload = false;
        AsmProcessAutoload();
    }
    std::string opp_path = WvpGetString(kv, "asm.opp.path");
    if (!opp_path.empty() && PathReadable(opp_path) &&
        LoadAsmOpponent(opp_path.c_str())) {
        g_asm_opp_sel = WvpFindAsmByLabel(g_asm_opp_anims,
                                          WvpGetString(kv, "asm.opp.label"),
                                          WvpGetInt(kv, "asm.opp.sel", 0));
        if (g_asm_opp_sel >= 0 && g_asm_opp_sel < (int)g_asm_opp_anims.size())
            AsmResolveAnimGlobal(g_asm_opp_anims[g_asm_opp_sel]);
        g_request_asm_opp_autoload = false;
        AsmProcessOppAutoload();
    }

    WorldViewState loaded_world;
    loaded_world.enabled = WvpGetBool(kv, "world.enabled", true);
    loaded_world.w = WvpGetInt(kv, "world.w", loaded_world.w);
    loaded_world.h = WvpGetInt(kv, "world.h", loaded_world.h);
    loaded_world.origin_x = WvpGetInt(kv, "world.origin_x", loaded_world.origin_x);
    loaded_world.origin_y = WvpGetInt(kv, "world.origin_y", loaded_world.origin_y);
    loaded_world.onion = WvpGetBool(kv, "world.onion", loaded_world.onion);

    WorldMarkedSequenceState loaded_state;
    loaded_state.marked_play = WvpGetBool(kv, "state.marked_play", true);
    loaded_state.fps = WvpGetFloat(kv, "state.fps", loaded_state.fps);
    loaded_state.timer = WvpGetFloat(kv, "state.timer", 0.0f);
    loaded_state.frame = WvpGetInt(kv, "state.frame", 0);
    loaded_state.paused = WvpGetBool(kv, "state.paused", loaded_state.paused);
    loaded_state.dummy_decap_body = WvpGetBool(kv, "state.dummy_decap_body", false);
    loaded_state.dummy_decap_reset = WvpGetBool(kv, "state.dummy_decap_reset", true);
    loaded_state.dummy_decap_manual = WvpGetBool(kv, "state.dummy_decap_manual", false);
    loaded_state.dummy_decap_doc_idx =
        WvpResolveDocIndex(WvpGetInt(kv, "state.dummy_decap_doc_idx", -1),
                           WvpGetString(kv, "state.dummy_decap_doc_path"),
                           doc_map);
    loaded_state.dummy_decap_prefix = WvpGetString(kv, "state.dummy_decap_prefix");
    loaded_state.draw_sprite_borders = WvpGetBool(kv, "state.draw_sprite_borders", true);
    loaded_state.show_boundary_overlay = WvpGetBool(kv, "state.show_boundary_overlay", true);
    loaded_state.embedded_active = WvpGetBool(kv, "state.embedded_active", false);
    loaded_state.embedded_is_script = WvpGetBool(kv, "state.embedded_is_script", false);
    loaded_state.embedded_show_companions =
        WvpGetBool(kv, "state.embedded_show_companions", false);
    loaded_state.embedded_record_index =
        WvpGetInt(kv, "state.embedded_record_index", -1);
    loaded_state.embedded_doc_idx =
        WvpResolveDocIndex(WvpGetInt(kv, "state.embedded_doc_idx", -1),
                           WvpGetString(kv, "state.embedded_doc_path"),
                           doc_map);
    loaded_state.embedded_name = WvpGetString(kv, "state.embedded_name");
    loaded_state.embedded_targets = WvpGetVec(kv, "state.embedded_targets");
    int label_count = WvpGetInt(kv, "state.embedded_label_count", 0);
    loaded_state.embedded_frame_labels.clear();
    for (int i = 0; i < label_count; i++)
        loaded_state.embedded_frame_labels.push_back(
            WvpGetString(kv, WvpKey("embedded_label", i, "text")));

    int slot_count = WvpGetInt(kv, "slot.count", kWorldMarkedMaxTabs);
    if (slot_count > kWorldMarkedMaxTabs) slot_count = kWorldMarkedMaxTabs;
    for (int slot = 0; slot < slot_count; slot++)
        WvpReadSlot(kv, loaded_state, slot, doc_map);

    int split_count = WvpGetInt(kv, "split.count", 0);
    loaded_state.split_lanes.clear();
    for (int i = 0; i < split_count; i++) {
        WorldMarkedSplitLane split = {};
        split.slot = WvpGetInt(kv, WvpKey("split", i, "slot"), -1);
        split.doc_idx =
            WvpResolveDocIndex(WvpGetInt(kv, WvpKey("split", i, "doc_idx"), -1),
                               WvpGetString(kv, WvpKey("split", i, "doc_path")),
                               doc_map);
        if (split.slot >= 0 && split.slot < kWorldMarkedSourceTabs &&
            document_get(split.doc_idx))
            loaded_state.split_lanes.push_back(split);
    }

    g_world_state = loaded_world;
    g_world_marked_state = loaded_state;
    g_asm_lane_enabled = WvpGetBool(kv, "asm.player.enabled", g_asm_lane_enabled);
    g_asm_opp_enabled = WvpGetBool(kv, "asm.opp.enabled", g_asm_opp_enabled);
    g_show_asm_anim = WvpGetBool(kv, "asm.window.show", g_show_asm_anim);
    g_asm_anim_frame = WvpGetInt(kv, "asm.window.frame", 0);
    g_asm_anim_play = WvpGetBool(kv, "asm.window.play", g_asm_anim_play);
    g_asm_anim_fps = WvpGetFloat(kv, "asm.window.fps", g_asm_anim_fps);
    if (g_asm_anim_fps < 1.0f) g_asm_anim_fps = 1.0f;
    if (g_asm_anim_fps > 30.0f) g_asm_anim_fps = 30.0f;

    int active_saved = WvpGetInt(kv, "doc.active", -1);
    int active_idx = WvpResolveDocIndex(active_saved, std::string(), doc_map);
    if (active_idx >= 0)
        ActivateDocumentTab(active_idx);

    g_img_tex_idx = -2;
    g_zoom_reset = true;
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             missing_docs > 0
                 ? "Loaded World View project (%d missing IMG file%s)."
                 : "Loaded World View project.",
             missing_docs, missing_docs == 1 ? "" : "s");
    g_restore_msg_timer = 5.0f;
    return true;
}

/* (Re)fill the playback texture with the current frame's composited pieces. */
static void AsmAnimRefillTexture(void)
{
    if (g_asm_anim_sel < 0 || g_asm_anim_sel >= (int)g_asm_anims.size()) return;
    if (g_asm_anim_canvas_w <= 0 || g_asm_anim_canvas_h <= 0) return;
    AsmAnim &a = g_asm_anims[g_asm_anim_sel];
    if (a.frames.empty()) return;
    int fi = g_asm_anim_frame % (int)a.frames.size();

    int w = g_asm_anim_canvas_w, h = g_asm_anim_canvas_h;
    if (!g_asm_anim_tex || g_asm_anim_tex_w != w || g_asm_anim_tex_h != h) {
        if (g_asm_anim_tex) SDL_DestroyTexture(g_asm_anim_tex);
        g_asm_anim_tex = SDL_CreateTexture(g_imgui_renderer, SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!g_asm_anim_tex) return;
        SDL_SetTextureBlendMode(g_asm_anim_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_asm_anim_tex, SDL_ScaleModeNearest);
        g_asm_anim_tex_w = w; g_asm_anim_tex_h = h;
    }

    void *pixels; int pitch;
    if (SDL_LockTexture(g_asm_anim_tex, NULL, &pixels, &pitch) != 0) return;
    Uint32 *dst = (Uint32 *)pixels;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            dst[y * (pitch / 4) + x] = 0x00000000u;   /* transparent */

    AsmAnimFrame &fr = a.frames[fi];
    for (size_t p = 0; p < fr.piece_img.size(); p++) {
        int ri = fr.piece_img[p];
        if (ri < 0) continue;
        Document *pdoc = (p < fr.piece_doc.size() && fr.piece_doc[p]) ? fr.piece_doc[p]
                                                                      : g_asm_anim_doc;
        IMG *img = doc_get_img(pdoc, ri);
        if (!img || !img->data_p) continue;
        PAL *pal = doc_get_pal(pdoc, img->palnum);
        const unsigned char *pd = pal ? (const unsigned char *)pal->data_p : NULL;
        int stride = (img->w + 3) & ~3;
        const unsigned char *sp = (const unsigned char *)img->data_p;
        int ox = -(int)(short)img->anix + fr.dx - g_asm_anim_minx;
        int oy = -(int)(short)img->aniy + fr.dy - g_asm_anim_miny;
        for (int y = 0; y < img->h; y++) {
            int dy = oy + y; if (dy < 0 || dy >= h) continue;
            int srcy = fr.mirror_v ? (img->h - 1 - y) : y;
            for (int x = 0; x < img->w; x++) {
                /* ani_flip mirrors horizontally; ani_flip_v mirrors vertically. */
                int srcx = fr.mirror ? (img->w - 1 - x) : x;
                int dx = ox + x; if (dx < 0 || dx >= w) continue;
                unsigned char ci = sp[srcy * stride + srcx];
                if (ci == 0) continue;
                Uint32 r = 200, g = 200, b = 200;
                if (pd) {
                    unsigned short w15 = (unsigned short)(pd[ci*2] | (pd[ci*2+1] << 8));
                    r = ((w15 >> 10) & 0x1F) << 3; g = ((w15 >> 5) & 0x1F) << 3; b = (w15 & 0x1F) << 3;
                }
                dst[dy * (pitch / 4) + dx] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
    SDL_UnlockTexture(g_asm_anim_tex);
    g_asm_anim_last_drawn = fi;
}

void DrawAsmAnimWindow(void)
{
    if (!g_show_asm_anim) return;
    ImGui::SetNextWindowSize(ImVec2(420, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ASM Animations", &g_show_asm_anim)) { ImGui::End(); return; }

    if (ImGui::Button("Load Character ASM...")) OpenFileDialog(FileDialogMode::LoadAsmAnim);
    if (!g_asm_anim_file.empty()) {
        ImGui::SameLine();
        const char *base = strrchr(g_asm_anim_file.c_str(), '\\');
        ImGui::TextDisabled("%s", base ? base + 1 : g_asm_anim_file.c_str());
    }

    if (g_asm_anims.empty()) {
        ImGui::TextWrapped("Load a per-character ASM (e.g. MKRD.ASM for Raiden) to list its "
                           "animations. Selecting one automatically opens every sprite IMG "
                           "it needs (a character's frames are split across several files) "
                           "and plays it composited across them.");
        ImGui::End();
        return;
    }

    /* Animation chooser */
    const char *cur = (g_asm_anim_sel >= 0 && g_asm_anim_sel < (int)g_asm_anims.size())
                    ? g_asm_anims[g_asm_anim_sel].name.c_str() : "(none)";
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##asm_anim_sel", cur)) {
        for (int i = 0; i < (int)g_asm_anims.size(); i++) {
            bool sel = (i == g_asm_anim_sel);
            char lbl[96];
            snprintf(lbl, sizeof(lbl), "%s  (%s)", g_asm_anims[i].name.c_str(), g_asm_anims[i].label.c_str());
            if (ImGui::Selectable(lbl, sel)) { AsmAnimSelect(i); g_request_asm_autoload = true; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (g_asm_anim_sel < 0) { ImGui::End(); return; }
    AsmAnim &a = g_asm_anims[g_asm_anim_sel];

    if (ImGui::Checkbox("Play in World View lane", &g_asm_lane_enabled) && g_asm_lane_enabled) {
        g_world_state.enabled = true;
        g_world_marked_state.marked_play = true;
        WorldMarkedRestart(g_world_marked_state);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Render this animation as a lane in World View, using the script's\n"
                          "ticks, local anipoints, anipoint placement and loop.");
    ImGui::Separator();

    ImGui::Checkbox("Play", &g_asm_anim_play);
    ImGui::SameLine(); ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("fps", &g_asm_anim_fps, 1.0f, 30.0f, "%.0f");
    int nframes = (int)a.frames.size();
    if (nframes > 0) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
        int disp = g_asm_anim_frame % nframes + 1;
        if (ImGui::SliderInt("##asm_frame", &disp, 1, nframes, "frame %d")) {
            g_asm_anim_frame = disp - 1; g_asm_anim_play = false;
        }
    }

    /* advance playback */
    if (g_asm_anim_play && nframes > 0 && g_asm_anim_fps > 0.0f) {
        g_asm_anim_timer += ImGui::GetIO().DeltaTime;
        float step = 1.0f / g_asm_anim_fps;
        while (g_asm_anim_timer >= step) { g_asm_anim_timer -= step; g_asm_anim_frame = (g_asm_anim_frame + 1) % nframes; }
    }
    if (nframes > 0 && (g_asm_anim_frame % nframes) != g_asm_anim_last_drawn)
        AsmAnimRefillTexture();

    /* preview */
    if (g_asm_anim_tex && g_asm_anim_canvas_w > 0) {
        float avail = ImGui::GetContentRegionAvail().x;
        float scale = (g_asm_anim_canvas_w > 0) ? (avail / (float)g_asm_anim_canvas_w) : 1.0f;
        if (scale > 4.0f) scale = 4.0f; if (scale < 0.25f) scale = 0.25f;
        ImVec2 sz((float)g_asm_anim_canvas_w * scale, (float)g_asm_anim_canvas_h * scale);
        ImGui::Image((ImTextureID)(intptr_t)g_asm_anim_tex, sz);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                           "No frames of this animation resolve to the loaded IMG.");
    }

    /* missing-data report */
    ImGui::Separator();
    ImGui::Text("Frames: %d   Pieces missing in IMG: %d", nframes, a.missing);
    if (!a.control.empty()) {
        std::string ctl;
        for (auto &c : a.control) { if (!ctl.empty()) ctl += ", "; ctl += c; }
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "Control / opcodes: %s", ctl.c_str());
    }
    if (a.missing > 0 && ImGui::TreeNode("Unresolved symbols")) {
        for (auto &fr : a.frames)
            for (size_t p = 0; p < fr.piece_syms.size(); p++)
                if (p < fr.piece_img.size() && fr.piece_img[p] < 0)
                    ImGui::BulletText("%s", fr.piece_syms[p].c_str());
        ImGui::TreePop();
    }

    /* ---- Fatality opponent (second ASM, drawn in the opponent lane) ---- */
    ImGui::SeparatorText("Fatality opponent");
    if (ImGui::Button("Load Johnny Cage")) {
        /* Default opponent: MKJC.ASM in the same folder as the player ASM. */
        std::string dir = g_asm_anim_file;
        size_t sl = dir.find_last_of("\\/");
        dir = (sl != std::string::npos) ? dir.substr(0, sl) : ".";
        std::string jc = dir + "\\MKJC.ASM";
        FILE *probe = fopen(jc.c_str(), "rb");
        if (probe) { fclose(probe); LoadAsmOpponent(jc.c_str()); }
        else       { g_request_load_opp_asm = true; }   /* not found -> pick manually */
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Load MKJC.ASM (Johnny Cage) from the player ASM's folder as the opponent.");
    ImGui::SameLine();
    if (ImGui::Button("Load Opponent ASM...")) g_request_load_opp_asm = true;

    if (!g_asm_opp_anims.empty()) {
        if (!g_asm_opp_file.empty()) {
            const char *ob = strrchr(g_asm_opp_file.c_str(), '\\');
            ImGui::SameLine(); ImGui::TextDisabled("%s", ob ? ob + 1 : g_asm_opp_file.c_str());
        }
        ImGui::Checkbox("Play opponent in World View lane", &g_asm_opp_enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw the opponent animation in its own lane, facing the player.");
        if (g_asm_opp_enabled) { g_world_state.enabled = true; g_world_marked_state.marked_play = true; }

        const char *ocur = (g_asm_opp_sel >= 0 && g_asm_opp_sel < (int)g_asm_opp_anims.size())
                         ? g_asm_opp_anims[g_asm_opp_sel].name.c_str() : "(none)";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##asm_opp_sel", ocur)) {
            for (int i = 0; i < (int)g_asm_opp_anims.size(); i++) {
                bool seld = (i == g_asm_opp_sel);
                char lbl[96];
                snprintf(lbl, sizeof(lbl), "%s  (%s)", g_asm_opp_anims[i].name.c_str(),
                         g_asm_opp_anims[i].label.c_str());
                if (ImGui::Selectable(lbl, seld)) {
                    g_asm_opp_sel = i;
                    AsmResolveAnimGlobal(g_asm_opp_anims[i]);
                    g_request_asm_opp_autoload = true;
                    WorldMarkedRestart(g_world_marked_state);
                }
                if (seld) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (g_asm_opp_sel >= 0 && g_asm_opp_sel < (int)g_asm_opp_anims.size())
            ImGui::Text("Opponent frames: %d   missing: %d",
                        (int)g_asm_opp_anims[g_asm_opp_sel].frames.size(),
                        g_asm_opp_anims[g_asm_opp_sel].missing);
    }

    ImGui::End();
}

/* Controls for the selected sprite's non-destructive overlay layer. Only shown
   when the current sprite actually has a layer (created via Drop Paste to
   Layer). Move/flip/flatten/delete; the layer bakes onto the sprite on save. */
void DrawSpriteLayerPanel(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    SpriteLayer *L = img_layer(img);
    if (!L) return;

    ImGui::SetNextWindowSize(ImVec2(270, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Sprite Layer")) {
        ImGui::Text("On %.15s   layer %dx%d", img->n_s, L->w, L->h);
        bool vis = L->visible != 0;
        if (ImGui::Checkbox("Visible", &vis)) { L->visible = vis ? 1 : 0; mark_dirty(); g_img_tex_idx = -2; }
        ImGui::SameLine();
        ImGui::TextDisabled("(bakes onto sprite on save)");

        ImGui::Separator();
        int pos[2] = { L->x, L->y };
        if (ImGui::DragInt2("Offset", pos, 0.5f)) { L->x = pos[0]; L->y = pos[1]; mark_dirty(); g_img_tex_idx = -2; }
        if (ImGui::Button("Left"))  { L->x--; mark_dirty(); g_img_tex_idx = -2; } ImGui::SameLine();
        if (ImGui::Button("Right")) { L->x++; mark_dirty(); g_img_tex_idx = -2; } ImGui::SameLine();
        if (ImGui::Button("Up"))    { L->y--; mark_dirty(); g_img_tex_idx = -2; } ImGui::SameLine();
        if (ImGui::Button("Down"))  { L->y++; mark_dirty(); g_img_tex_idx = -2; }

        ImGui::Separator();
        if (ImGui::Button("Flip H")) { doc_undo_push(); flip_layer_horizontal(L); g_img_tex_idx = -2; }
        ImGui::SameLine();
        if (ImGui::Button("Flip V")) { doc_undo_push(); flip_layer_vertical(L); g_img_tex_idx = -2; }

        ImGui::Separator();
        if (ImGui::Button("Flatten Now")) { doc_undo_push(); flatten_img_layer(img); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Merge the layer into the sprite now (permanent).");
        ImGui::SameLine();
        if (ImGui::Button("Delete Layer")) { doc_undo_push(); delete_img_layer(img); }
    }
    ImGui::End();
}


/* MK2 strike-table (hitbox) editor. Reads/writes mk2-main/src/MKSTK.ASM
   directly — the source-of-truth for the strike tables. The MAME stk.bin
   path is intentionally not used here; rebuilding through the ASM is the
   permanent route. */
void DrawMk2HitboxWindow(void)
{
    if (!g_show_mk2) return;

    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("MK2 Hitboxes (MKSTK.ASM)", &g_show_mk2)) {
        ImGui::End();
        return;
    }

    /* Auto-clear the "Loaded N moves" / "Saved" success message once the
       user has started editing again. Errors keep their sticky flag so
       they stay visible until the next action explicitly resolves them. */
    if (!g_mk2_status_sticky && g_mk2_doc.dirty && !g_mk2_status.empty())
        g_mk2_status.clear();

    /* Window-scoped shortcuts. RouteFocused makes these fire only when
       the MK2 panel (or one of its child widgets) holds focus, so they
       don't hijack the pixel-undo path on the main canvas. */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteFocused)) {
        int rec = mk2::undo_pop(&g_mk2_doc);
        if (rec >= 0) Mk2SelectRecord(rec);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteFocused)) {
        int rec = mk2::redo_pop(&g_mk2_doc);
        if (rec >= 0) Mk2SelectRecord(rec);
    }

    /* --- Path / Browse / Load / Save row --- */
    ImGui::SetNextItemWidth(-280);
    ImGui::InputTextWithHint("##mk2path", "path to MKSTK.ASM (use Browse...)", g_mk2_path, sizeof(g_mk2_path));
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
#ifdef _WIN32
        char buf[1024];
        /* Seed the filename buffer with the current path so the dialog
           opens at the last location. lpstrFile doubles as input on
           open-mode. */
        size_t cur = strlen(g_mk2_path);
        if (cur >= sizeof(buf)) cur = sizeof(buf) - 1;
        memcpy(buf, g_mk2_path, cur); buf[cur] = '\0';
        /* Initial directory from the persisted last-dir, used only when
           lpstrFile doesn't already contain a directory component. */
        char init_dir[MAX_PATH] = "";
        load_last_dir_cat(init_dir, sizeof(init_dir), "mk2");
        OPENFILENAMEA ofn = {};
        ofn.lStructSize     = sizeof(ofn);
        ofn.lpstrFilter     = "ASM source\0*.ASM;*.asm\0All files\0*.*\0";
        ofn.lpstrFile       = buf;
        ofn.nMaxFile        = sizeof(buf);
        ofn.lpstrInitialDir = init_dir[0] ? init_dir : NULL;
        ofn.lpstrTitle      = "Select MKSTK.ASM";
        ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&ofn)) {
            strncpy(g_mk2_path, buf, sizeof(g_mk2_path) - 1);
            g_mk2_path[sizeof(g_mk2_path) - 1] = '\0';
            /* Persist the directory (everything up to the last separator). */
            const char *last_sep = NULL;
            for (const char *p = buf; *p; p++)
                if (*p == '\\' || *p == '/') last_sep = p;
            if (last_sep && last_sep > buf) {
                char dir[MAX_PATH];
                size_t n = (size_t)(last_sep - buf);
                if (n >= sizeof(dir)) n = sizeof(dir) - 1;
                memcpy(dir, buf, n); dir[n] = '\0';
                save_last_dir_cat(dir, "mk2");
            }
        }
#else
        g_mk2_status = "Browse not implemented on this platform - type the path manually";
        g_mk2_status_sticky = true;
#endif
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pick MKSTK.ASM from disk");
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        std::string err;
        if (mk2::load(&g_mk2_doc, g_mk2_path, &err)) {
            char buf[160];
            snprintf(buf, sizeof(buf), "Loaded %d moves, %d char tables",
                     (int)g_mk2_doc.records.size(), (int)g_mk2_doc.char_tables.size());
            g_mk2_status = buf;
            g_mk2_status_sticky = false;
            g_mk2_char_idx = 0;
            g_mk2_move_idx = 0;
            g_mk2_search[0] = '\0';
            /* Fresh load wipes any prior undo/redo history — those entries
               referenced records that may no longer match the new doc. */
            g_mk2_doc.undo_stack.clear();
            g_mk2_doc.redo_stack.clear();
            /* If an IMG is already loaded, pre-select the matching
               character so the panel comes up pointing at the right
               fighter without an extra click. */
            Mk2AutoSelectFromImg();
        } else {
            g_mk2_status = std::string("Load failed: ") + err;
            g_mk2_status_sticky = true;
        }
    }
    ImGui::SameLine();
    bool can_save = g_mk2_doc.dirty && !g_mk2_doc.source_path.empty();
    if (!can_save) ImGui::BeginDisabled();
    if (ImGui::Button("Save")) {
        std::string err;
        if (mk2::save(&g_mk2_doc, &err)) { g_mk2_status = "Saved MKSTK.ASM"; g_mk2_status_sticky = false; }
        else { g_mk2_status = std::string("Save failed: ") + err; g_mk2_status_sticky = true; }
    }
    if (!can_save) ImGui::EndDisabled();
    ImGui::SameLine();
    bool can_reload = !g_mk2_doc.source_path.empty();
    if (!can_reload) ImGui::BeginDisabled();
    if (ImGui::Button("Reload")) {
        /* Re-read MKSTK.ASM from disk, discarding any in-memory edits.
           Uses the previously-resolved source_path rather than the input
           box content so a stray edit there can't redirect the reload. */
        std::string err;
        std::string path = g_mk2_doc.source_path;
        if (mk2::load(&g_mk2_doc, path.c_str(), &err)) {
            char buf[160];
            snprintf(buf, sizeof(buf), "Reloaded %d moves, %d char tables",
                     (int)g_mk2_doc.records.size(), (int)g_mk2_doc.char_tables.size());
            g_mk2_status = buf;
            g_mk2_status_sticky = false;
            /* Keep the selection if the labels still resolve, otherwise
               fall back to the first move. */
            int new_char = g_mk2_char_idx;
            if (new_char >= (int)g_mk2_doc.char_tables.size()) new_char = 0;
            g_mk2_char_idx = new_char;
            if (g_mk2_move_idx >= (int)g_mk2_doc.char_tables[new_char].moves.size())
                g_mk2_move_idx = 0;
        } else {
            g_mk2_status = std::string("Reload failed: ") + err;
            g_mk2_status_sticky = true;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-read MKSTK.ASM from disk, discarding unsaved edits");
    if (!can_reload) ImGui::EndDisabled();

    ImGui::SameLine();
    bool can_undo_mk2 = mk2::can_undo(&g_mk2_doc);
    if (!can_undo_mk2) ImGui::BeginDisabled();
    if (ImGui::Button("Undo")) {
        int rec = mk2::undo_pop(&g_mk2_doc);
        if (rec >= 0) Mk2SelectRecord(rec);
    }
    if (!can_undo_mk2) ImGui::EndDisabled();

    ImGui::SameLine();
    bool can_redo_mk2 = mk2::can_redo(&g_mk2_doc);
    if (!can_redo_mk2) ImGui::BeginDisabled();
    if (ImGui::Button("Redo")) {
        int rec = mk2::redo_pop(&g_mk2_doc);
        if (rec >= 0) Mk2SelectRecord(rec);
    }
    if (!can_redo_mk2) ImGui::EndDisabled();

    if (!g_mk2_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_mk2_status.c_str());
    }

    /* If no document loaded yet, stop here. */
    if (g_mk2_doc.char_tables.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("Click Browse... to pick MKSTK.ASM, then click Load.");
        ImGui::End();
        return;
    }

    ImGui::Separator();

    /* --- Three-pane layout: chars | moves | fields --- */
    const float row_h = ImGui::GetContentRegionAvail().y - 8.0f;
    ImGui::BeginChild("##mk2_chars", ImVec2(120, row_h), true);
    ImGui::TextDisabled("Character");
    for (int i = 0; i < (int)g_mk2_doc.char_tables.size(); i++) {
        const auto &t = g_mk2_doc.char_tables[i];
        char label[40];
        snprintf(label, sizeof(label), "%s (%d)", t.name.c_str(), (int)t.moves.size());
        if (ImGui::Selectable(label, g_mk2_char_idx == i)) {
            g_mk2_char_idx = i;
            g_mk2_move_idx = 0;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##mk2_moves", ImVec2(260, row_h), true);
    ImGui::TextDisabled("Move");
    /* Substring filter — case-insensitive. Empty box matches everything. */
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mk2_search", "filter...", g_mk2_search, sizeof(g_mk2_search));
    auto match_filter = [](const std::string &s, const char *needle) {
        if (!needle || !needle[0]) return true;
        std::string a = s; for (auto &c : a) c = (char)std::tolower((unsigned char)c);
        std::string b = needle; for (auto &c : b) c = (char)std::tolower((unsigned char)c);
        return a.find(b) != std::string::npos;
    };
    if (g_mk2_char_idx >= 0 && g_mk2_char_idx < (int)g_mk2_doc.char_tables.size()) {
        const auto &moves = g_mk2_doc.char_tables[g_mk2_char_idx].moves;
        for (int i = 0; i < (int)moves.size(); i++) {
            if (!match_filter(moves[i], g_mk2_search)) continue;
            char label[80];
            snprintf(label, sizeof(label), "%2d  %s", i, moves[i].c_str());
            if (ImGui::Selectable(label, g_mk2_move_idx == i))
                g_mk2_move_idx = i;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##mk2_fields", ImVec2(0, row_h), true);
    /* Resolve selected move to a record index. */
    int rec_idx = -1;
    if (g_mk2_char_idx >= 0 && g_mk2_char_idx < (int)g_mk2_doc.char_tables.size()) {
        const auto &moves = g_mk2_doc.char_tables[g_mk2_char_idx].moves;
        if (g_mk2_move_idx >= 0 && g_mk2_move_idx < (int)moves.size())
            rec_idx = mk2::find_record(&g_mk2_doc, moves[g_mk2_move_idx].c_str());
    }
    if (rec_idx < 0) {
        ImGui::TextDisabled("Select a move on the left.");
    } else {
        const mk2::StrikeRecord &rec = g_mk2_doc.records[rec_idx];
        ImGui::Text("%s", rec.label.c_str());
        ImGui::TextDisabled("MKSTK.ASM line %d", rec.label_line);
        ImGui::Separator();
        ImGui::Spacing();

        /* x_offset / y_offset / x_size / y_size — int editors. */
        for (int fi = mk2::F_X_OFFSET; fi <= mk2::F_Y_SIZE; fi++) {
            int v = rec.fields[fi].has_value ? (int)rec.fields[fi].value : 0;
            ImGui::SetNextItemWidth(140);
            char id[40]; snprintf(id, sizeof(id), "%s##mk2_%d", mk2::kFieldNames[fi], fi);
            if (ImGui::InputInt(id, &v, 1, 8)) {
                if (v < -0x8000) v = -0x8000;
                if (v > 0x7FFF)  v = 0x7FFF;
                mk2::undo_push(&g_mk2_doc, rec_idx, false);
                mk2::set_value(&g_mk2_doc, rec_idx, fi, (int32_t)v);
            }
        }

        ImGui::Spacing();
        /* Strike routine and sound — raw token editors (may be symbolic). */
        for (int fi : { (int)mk2::F_STRIKE, (int)mk2::F_SOUND }) {
            char buf[64];
            const std::string &raw = rec.fields[fi].raw;
            size_t n = raw.size() < sizeof(buf) - 1 ? raw.size() : sizeof(buf) - 1;
            memcpy(buf, raw.data(), n); buf[n] = '\0';
            ImGui::SetNextItemWidth(180);
            char id[40]; snprintf(id, sizeof(id), "%s##mk2_%d", mk2::kFieldNames[fi], fi);
            if (ImGui::InputText(id, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                mk2::undo_push(&g_mk2_doc, rec_idx, false);
                mk2::set_raw(&g_mk2_doc, rec_idx, fi, buf);
            }
        }

        ImGui::Spacing();
        /* Damage word: split into hit (hi byte) and block (lo byte). */
        int dmg = rec.fields[mk2::F_DAMAGE].has_value ? (int)rec.fields[mk2::F_DAMAGE].value : 0;
        int hit = mk2::damage_hit(dmg), blk = mk2::damage_block(dmg);
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputInt("damage_hit##mk2", &hit, 1, 4)) {
            if (hit < 0) hit = 0; if (hit > 255) hit = 255;
            mk2::undo_push(&g_mk2_doc, rec_idx, false);
            mk2::set_value(&g_mk2_doc, rec_idx, mk2::F_DAMAGE, mk2::pack_damage(hit, blk));
        }
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputInt("damage_block##mk2", &blk, 1, 4)) {
            if (blk < 0) blk = 0; if (blk > 255) blk = 255;
            mk2::undo_push(&g_mk2_doc, rec_idx, false);
            mk2::set_value(&g_mk2_doc, rec_idx, mk2::F_DAMAGE, mk2::pack_damage(hit, blk));
        }
        ImGui::TextDisabled("damage word = 0x%04X", dmg & 0xFFFF);

        ImGui::Spacing();
        /* Score — 32-bit. */
        int score = rec.fields[mk2::F_SCORE].has_value ? (int)rec.fields[mk2::F_SCORE].value : 0;
        ImGui::SetNextItemWidth(160);
        if (ImGui::InputInt("score##mk2", &score, 100, 1000)) {
            mk2::undo_push(&g_mk2_doc, rec_idx, false);
            mk2::set_value(&g_mk2_doc, rec_idx, mk2::F_SCORE, (int32_t)score);
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

static bool Mk2FatalityFilterMatch(const std::string &text, const char *filter)
{
    if (!filter || !filter[0]) return true;
    std::string a = text;
    std::string b = filter;
    for (char &c : a) c = (char)std::tolower((unsigned char)c);
    for (char &c : b) c = (char)std::tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
}

struct Mk2FatalityFighterDef {
    const char *name;
    const char *source_file;
    const char *command_prefix[4];
    const char *img_files[24];
    const char *fatal_anims[10];
    const char *db1_anim;
    const char *db2_anim;
    const char *db1_victim;
    const char *db2_victim;
};

static const char *g_mk2_fatality_cage_deaths[] = {
    "a_torso_ripped", "a_decapfall", "a_head", "a_headhole", "a_swipe_torso",
    "a_nutcrunched", "a_bike_kicked", "a_drained", "a_banged", "a_impaled",
    "a_back_broke", "a_jc_arms_ripped", NULL
};

static const Mk2FatalityFighterDef g_mk2_fatality_fighters[] = {
    { "Johnny Cage", "MKJC.ASM", { "jc_", NULL },
      { "data/CAGE1.IMG", "data/CAGE2.IMG", "data/CAGE3.IMG", "data/CAGE4.IMG", "data/CAGE5.IMG",
        "data/CAGE6.IMG", "data/CAGE7.IMG", "data/CAGE8.IMG", "data/CAGE9.IMG", "data/CAGE10.IMG", NULL },
      { "a_jcrip", "a_jc_pp", "a_jc_headhole", "a_splits", NULL },
      "a_jcrip", "a_jc_pp", "a_torso_ripped", "a_head" },

    { "Liu Kang", "MKLK.ASM", { "lk_", NULL },
      { "data/KANG1.IMG", "data/KANG2.IMG", "data/KANG3.IMG", "data/KANG4.IMG", "data/KANG5.IMG",
        "data/KANG6.IMG", "data/KANG7.IMG", "data/KANG8.IMG", "data/KANG9.IMG", "data/KANG10.IMG",
        "data/LKBFIST.IMG", NULL },
      { "a_lkdragon", "a_lkwheel", "a_lkbike", NULL },
      "a_lkdragon", "a_lkwheel", "a_torso_ripped", "a_decapfall" },

    { "Raiden", "MKRD.ASM", { "rd_", NULL },
      { "data/RAID1.IMG", "data/RAID2.IMG", "data/RAID3.IMG", "data/RAID4.IMG", "data/RAID5.IMG",
        "data/RAID6.IMG", "data/RAID7.IMG", "data/RAID8.IMG", "data/RAID9.IMG", "data/RAIDWALK.IMG",
        "data/RADBOLT1.IMG", "data/RADBOLT2.IMG", NULL },
      { "a_death_zap1", "a_death_bolt1", "a_death_zap2", "a_death_shock", NULL },
      "a_death_zap1", "a_death_zap2", "a_torso_ripped", "a_decapfall" },

    { "Shang Tsung", "MKST.ASM", { "st_", NULL },
      { "data/TSUNG1.IMG", "data/TSUNG2.IMG", "data/TSUNG3.IMG", "data/TSUNG4.IMG", "data/TSUNG5.IMG",
        "data/TSUNG6.IMG", "data/TSUNG7.IMG", "data/TSUNG8.IMG", "data/TSUNG9.IMG", "data/TSUNG10.IMG",
        "data/TSUNG1G.IMG", "data/OLDSHNG.IMG", NULL },
      { "a_st_kano_morph", "a_st_kano_roll", "a_st_kano_back", "a_st_2_jc", NULL },
      "a_st_kano_roll", "a_st_kano_morph", "a_decapfall", "a_drained" },

    { "Baraka", "MKSA.ASM", { "sa_", NULL },
      { "data/UGMO1.IMG", "data/UGMO2.IMG", "data/UGMO3.IMG", "data/UGMO4.IMG", "data/UGMO5.IMG",
        "data/UGMO6.IMG", "data/UGMO7.IMG", "data/UGMO8.IMG", "data/UGMO9.IMG", "data/UGMO10.IMG",
        "data/UGMO1SHO.IMG", NULL },
      { "a_sashred", "a_sastab", "a_swipe", NULL },
      "a_sashred", "a_sastab", "a_decapfall", "a_impaled" },

    { "Kitana", "MKFN.ASM", { "fn1_", NULL },
      { "data/KAT1.IMG", "data/KAT2.IMG", "data/KAT3.IMG", "data/KAT4.IMG", "data/KAT5.IMG",
        "data/KAT6.IMG", "data/KAT7.IMG", "data/KAT8.IMG", "data/KAT9.IMG", "data/KAT10.IMG",
        "data/KAT11.IMG", NULL },
      { "a_death_kiss1", "a_fan_swipe", NULL },
      "a_death_kiss1", "a_fan_swipe", "a_drained", "a_decapfall" },

    { "Mileena", "MKFN.ASM", { "fn2_", NULL },
      { "data/KAT1.IMG", "data/KAT2.IMG", "data/KAT3.IMG", "data/KAT4.IMG", "data/KAT5.IMG",
        "data/KAT6.IMG", "data/KAT7.IMG", "data/KAT8.IMG", "data/KAT9.IMG", "data/KAT10.IMG",
        "data/KAT11.IMG", NULL },
      { "a_fn2_stab", "a_death_kiss2", NULL },
      "a_fn2_stab", "a_death_kiss2", "a_impaled", "a_drained" },

    { "Sub-Zero", "MKNJ.ASM", { "sz_", NULL },
      { "data/NINJAS1.IMG", "data/NINJAS2.IMG", "data/NINJAS3.IMG", "data/NINJAS4.IMG", "data/NINJAS5.IMG",
        "data/NINJAS6.IMG", "data/NINJAS7.IMG", "data/NINJAS8.IMG", "data/NINJAS9.IMG", "data/NINJAS10.IMG",
        "data/NINJAS11.IMG", "data/NINJAS12.IMG", "data/FREEZE1.IMG", "data/FROZEN.IMG", "data/SNOBALL.IMG", NULL },
      { "a_sz_tornado", "a_pitch", "a_ice_ball", NULL },
      "a_sz_tornado", "a_pitch", "a_torso_ripped", "a_decapfall" },

    { "Scorpion", "MKNJ.ASM", { "sc_", NULL },
      { "data/NINJAS1.IMG", "data/NINJAS2.IMG", "data/NINJAS3.IMG", "data/NINJAS4.IMG", "data/NINJAS5.IMG",
        "data/NINJAS6.IMG", "data/NINJAS7.IMG", "data/NINJAS8.IMG", "data/NINJAS9.IMG", "data/NINJAS10.IMG",
        "data/NINJAS11.IMG", "data/NINJAS12.IMG", "data/NEWROPE.IMG", NULL },
      { "a_scortch", "a_scorpion_skull", "a_sc_swipe", "a_death_spear", NULL },
      "a_scortch", "a_sc_swipe", "a_torso_ripped", "a_swipe_torso" },

    { "Reptile", "MKNJ.ASM", { "rp_", NULL },
      { "data/NINJAS1.IMG", "data/NINJAS2.IMG", "data/NINJAS3.IMG", "data/NINJAS4.IMG", "data/NINJAS5.IMG",
        "data/NINJAS6.IMG", "data/NINJAS7.IMG", "data/NINJAS8.IMG", "data/NINJAS9.IMG", "data/NINJAS10.IMG",
        "data/NINJAS11.IMG", "data/NINJAS12.IMG", "data/ACID1.IMG", NULL },
      { "a_eat_head", "a_spit", "a_slow_proj", NULL },
      "a_eat_head", "a_eat_head", "a_head", "a_head" },

    { "Jax", "MKJX.ASM", { "jx_", NULL },
      { "data/NUJAX1.IMG", "data/NUJAX2.IMG", "data/NUJAX3.IMG", "data/NUJAX4.IMG", "data/NUJAX5.IMG",
        "data/NUJAX6.IMG", "data/NUJAX7.IMG", "data/NUJAX8.IMG", "data/NUJAX9.IMG", "data/NUJAX10.IMG",
        "data/JAXPRO.IMG", "data/MKJXARMS.IMG", NULL },
      { "a_clap", "a_back_breaker", "a_arm_rip", NULL },
      "a_clap", "a_arm_rip", "a_head", "a_jc_arms_ripped" },

    { "Kung Lao", "MKHH.ASM", { "hh_", NULL },
      { "data/HATHED1.IMG", "data/HATHED2.IMG", "data/HATHED3.IMG", "data/HATHED4.IMG", "data/HATHED5.IMG",
        "data/HATHED6.IMG", "data/HATHED7.IMG", "data/HATHED8.IMG", "data/HATHED9.IMG", "data/HATHED10.IMG",
        "data/HATHED11.IMG", "data/HATHED12.IMG", NULL },
      { "a_spin", "a_hh_hat_swipe", NULL },
      "a_spin", "a_hh_hat_swipe", "a_torso_ripped", "a_decapfall" },
};

static const int kMk2FatalityFighterCount =
    (int)(sizeof(g_mk2_fatality_fighters) / sizeof(g_mk2_fatality_fighters[0]));

struct Mk2FatalityReactionPair {
    const char *attacker = NULL;
    const char *victim = NULL;
    const char *reason = NULL;
};

static void Mk2FatalityClampSelections(void)
{
    if (g_mk2_fatality_command_idx < 0) g_mk2_fatality_command_idx = 0;
    if (g_mk2_fatality_combo_idx < 0) g_mk2_fatality_combo_idx = 0;
    if (g_mk2_fatality_anim_idx < 0) g_mk2_fatality_anim_idx = 0;
    if (g_mk2_fatality_command_idx >= (int)g_mk2_fatality_doc.commands.size())
        g_mk2_fatality_command_idx = (int)g_mk2_fatality_doc.commands.size() - 1;
    if (g_mk2_fatality_combo_idx >= (int)g_mk2_fatality_doc.combos.size())
        g_mk2_fatality_combo_idx = (int)g_mk2_fatality_doc.combos.size() - 1;
    if (g_mk2_fatality_anim_idx >= (int)g_mk2_fatality_doc.animations.size())
        g_mk2_fatality_anim_idx = (int)g_mk2_fatality_doc.animations.size() - 1;
    if (g_mk2_fatality_command_idx < 0) g_mk2_fatality_command_idx = 0;
    if (g_mk2_fatality_combo_idx < 0) g_mk2_fatality_combo_idx = 0;
    if (g_mk2_fatality_anim_idx < 0) g_mk2_fatality_anim_idx = 0;
}

static void Mk2FatalityLoadRoot(const char *root)
{
    std::string err;
    if (mk2fatal::load(&g_mk2_fatality_doc, root, &err)) {
        char buf[192];
        snprintf(buf, sizeof(buf), "Loaded %d command blocks, %d combos, %d animation blocks",
                 (int)g_mk2_fatality_doc.commands.size(),
                 (int)g_mk2_fatality_doc.combos.size(),
                 (int)g_mk2_fatality_doc.animations.size());
        g_mk2_fatality_status = buf;
        g_mk2_fatality_status_sticky = false;
        g_mk2_fatality_command_idx = 0;
        g_mk2_fatality_combo_idx = 0;
        g_mk2_fatality_anim_idx = 0;
        g_mk2_fatality_selected_line = 0;
        g_mk2_fatality_filter[0] = '\0';
    } else {
        g_mk2_fatality_status = std::string("Load failed: ") + err;
        g_mk2_fatality_status_sticky = true;
    }
}

static bool DrawMk2FatalitySourceEditor(const char *id, int file_idx, int start_line, int end_line,
                                        int *selected_line, char *insert_buf,
                                        size_t insert_buf_size, bool allow_insert_delete)
{
    const mk2fatal::SourceFile *sf = mk2fatal::get_file(&g_mk2_fatality_doc, file_idx);
    if (!sf) {
        ImGui::TextDisabled("Source file unavailable.");
        return false;
    }
    if (start_line <= 0) start_line = 1;
    if (end_line > (int)sf->lines.size()) end_line = (int)sf->lines.size();
    if (end_line < start_line) {
        ImGui::TextDisabled("No source lines in this block.");
        return false;
    }
    if (*selected_line < start_line || *selected_line > end_line) *selected_line = start_line;

    ImGui::TextDisabled("%s  lines %d-%d", sf->rel_path.c_str(), start_line, end_line);
    const float footer_h = allow_insert_delete ? 64.0f : 0.0f;
    bool changed = false;
    bool structural = false;

    ImGui::BeginChild(id, ImVec2(0, -footer_h), true);
    if (ImGui::BeginTable("##mk2fatal_src_table", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 54.0f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
        for (int line = start_line; line <= end_line; line++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(line);
            char lbuf[24];
            snprintf(lbuf, sizeof(lbuf), "%d", line);
            if (ImGui::Selectable(lbuf, *selected_line == line))
                *selected_line = line;
            ImGui::TableSetColumnIndex(1);
            char buf[1024];
            const std::string &src = sf->lines[line - 1];
            size_t n = src.size() < sizeof(buf) - 1 ? src.size() : sizeof(buf) - 1;
            memcpy(buf, src.data(), n);
            buf[n] = '\0';
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##src", buf, sizeof(buf))) {
                if (mk2fatal::set_line(&g_mk2_fatality_doc, file_idx, line, buf))
                    changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (allow_insert_delete) {
        ImGui::SetNextItemWidth(-220);
        ImGui::InputTextWithHint("##mk2fatal_insert", "assembly line to insert", insert_buf, insert_buf_size);
        ImGui::SameLine();
        bool can_insert = insert_buf && insert_buf[0] && *selected_line >= start_line && *selected_line <= end_line;
        if (!can_insert) ImGui::BeginDisabled();
        if (ImGui::Button("Insert Before")) {
            if (mk2fatal::insert_line(&g_mk2_fatality_doc, file_idx, *selected_line, insert_buf)) {
                structural = true;
                end_line++;
            }
        }
        if (!can_insert) ImGui::EndDisabled();
        ImGui::SameLine();
        bool can_delete = *selected_line > start_line && *selected_line <= end_line;
        if (!can_delete) ImGui::BeginDisabled();
        if (ImGui::Button("Delete Line")) {
            if (mk2fatal::delete_line(&g_mk2_fatality_doc, file_idx, *selected_line)) {
                structural = true;
                if (*selected_line > start_line) (*selected_line)--;
            }
        }
        if (!can_delete) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The label line is protected; select a .long/.word line to delete.");
    }

    if (changed || structural) {
        std::string err;
        mk2fatal::reparse(&g_mk2_fatality_doc, &err);
        if (!g_mk2_fatality_status_sticky && !g_mk2_fatality_status.empty())
            g_mk2_fatality_status.clear();
        Mk2FatalityClampSelections();
    }
    return changed || structural;
}

static bool Mk2FatalityFileExists(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static std::string Mk2FatalityResolveProjectAsset(const std::string &rel_path)
{
    std::string root = g_mk2_fatality_doc.root_path.empty()
                     ? std::string(g_mk2_fatality_root)
                     : g_mk2_fatality_doc.root_path;
    std::string p = PathCombine(root, rel_path);
    if (Mk2FatalityFileExists(p)) return p;

    std::string parent = GetParentDirectory(root);
    p = PathCombine(parent, rel_path);
    if (Mk2FatalityFileExists(p)) return p;

    return PathCombine(root, rel_path);
}

static bool Mk2FatalityNameEquals(const std::string &a, const std::string &b)
{
    size_t na = a.size();
    size_t nb = b.size();
    while (na > 0 && a[na - 1] == '\0') na--;
    while (nb > 0 && b[nb - 1] == '\0') nb--;
    if (na != nb) return false;
    for (size_t i = 0; i < na; i++)
        if (std::toupper((unsigned char)a[i]) != std::toupper((unsigned char)b[i]))
            return false;
    return true;
}

static bool Mk2FatalityVectorHas(const std::vector<std::string> &items, const std::string &value)
{
    for (const std::string &item : items)
        if (Mk2FatalityNameEquals(item, value)) return true;
    return false;
}

static void Mk2FatalityPushUnique(std::vector<std::string> *items, const std::string &value)
{
    if (value.empty()) return;
    if (Mk2FatalityVectorHas(*items, value)) return;
    items->push_back(value);
}

static void Mk2FatalityMergePlan(mk2fatal::AssetPlan *dst, const mk2fatal::AssetPlan &src)
{
    if (!dst) return;
    if (dst->root_label.empty()) dst->root_label = src.root_label;
    if (dst->resolved_label.empty()) dst->resolved_label = src.resolved_label;
    if (dst->preferred_file.empty()) dst->preferred_file = src.preferred_file;
    for (const std::string &s : src.animation_labels) Mk2FatalityPushUnique(&dst->animation_labels, s);
    for (const std::string &s : src.sprite_labels) Mk2FatalityPushUnique(&dst->sprite_labels, s);
    for (const std::string &s : src.missing_labels) Mk2FatalityPushUnique(&dst->missing_labels, s);
    for (const std::string &s : src.img_files) Mk2FatalityPushUnique(&dst->img_files, s);
}

static int Mk2FatalityFindImageBySpriteLabel(Document *doc, const std::string &label)
{
    if (!doc) return -1;
    for (int i = 0; i < (int)doc->imgcnt; i++) {
        IMG *img = doc_get_img(doc, i);
        if (!img) continue;
        if (Mk2FatalityNameEquals(img_name_string(img), label)) return i;
    }
    return -1;
}

static int Mk2FatalityFindImageBySpriteLabel(const std::string &label)
{
    return Mk2FatalityFindImageBySpriteLabel(g_doc, label);
}

static mk2fatal::AssetPlan Mk2FatalityBuildPlanForLabels(const std::vector<std::string> &labels,
                                                         const char *preferred_file,
                                                         const char *const *img_files)
{
    mk2fatal::AssetPlan plan;
    plan.root_label = labels.empty() ? "" : labels[0];
    plan.preferred_file = preferred_file ? preferred_file : "";
    for (const std::string &label : labels) {
        mk2fatal::AssetPlan part;
        std::string err;
        if (mk2fatal::build_asset_plan(&g_mk2_fatality_doc, label.c_str(), preferred_file, &part, &err))
            Mk2FatalityMergePlan(&plan, part);
        else
            Mk2FatalityPushUnique(&plan.missing_labels, label);
    }
    if (img_files) {
        for (int i = 0; img_files[i]; i++)
            Mk2FatalityPushUnique(&plan.img_files, img_files[i]);
    }
    return plan;
}

static bool Mk2FatalityCommandMatchesFighter(const mk2fatal::CommandBlock &cmd,
                                             const Mk2FatalityFighterDef &fighter)
{
    for (int i = 0; i < 4 && fighter.command_prefix[i]; i++) {
        const char *p = fighter.command_prefix[i];
        size_t n = strlen(p);
        if (cmd.label.size() >= n) {
            bool match = true;
            for (size_t j = 0; j < n; j++) {
                if (std::tolower((unsigned char)cmd.label[j]) !=
                    std::tolower((unsigned char)p[j])) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        if (!cmd.combo_label.empty() && Mk2FatalityFilterMatch(cmd.combo_label, p))
            return true;
    }
    return false;
}

static std::vector<int> Mk2FatalityFighterCommandIndices(const Mk2FatalityFighterDef &fighter)
{
    std::vector<int> out;
    for (int i = 0; i < (int)g_mk2_fatality_doc.commands.size(); i++) {
        const mk2fatal::CommandBlock &cmd = g_mk2_fatality_doc.commands[i];
        if (Mk2FatalityCommandMatchesFighter(cmd, fighter))
            out.push_back(i);
    }
    return out;
}

static int Mk2FatalityAnimListIndex(const char *const *items, const char *label)
{
    if (!items || !label || !label[0]) return 0;
    for (int i = 0; items[i]; i++)
        if (Mk2FatalityNameEquals(items[i], label)) return i;
    return 0;
}

static const char *Mk2FatalitySelectedAttackerAnim(const Mk2FatalityFighterDef &fighter)
{
    int count = 0;
    while (count < 10 && fighter.fatal_anims[count]) count++;
    if (count == 0) return NULL;
    if (g_mk2_fatality_attacker_anim_idx < 0) g_mk2_fatality_attacker_anim_idx = 0;
    if (g_mk2_fatality_attacker_anim_idx >= count) g_mk2_fatality_attacker_anim_idx = count - 1;
    return fighter.fatal_anims[g_mk2_fatality_attacker_anim_idx];
}

static const char *Mk2FatalitySelectedVictimAnim(void)
{
    int count = 0;
    while (g_mk2_fatality_cage_deaths[count]) count++;
    if (g_mk2_fatality_victim_anim_idx < 0) g_mk2_fatality_victim_anim_idx = 0;
    if (g_mk2_fatality_victim_anim_idx >= count) g_mk2_fatality_victim_anim_idx = count - 1;
    return g_mk2_fatality_cage_deaths[g_mk2_fatality_victim_anim_idx];
}

static std::string Mk2FatalityInferAnimationFromRoutine(const std::string &routine)
{
    if (routine.size() <= 3) return std::string();
    std::string lower = routine;
    for (char &c : lower) c = (char)std::tolower((unsigned char)c);
    if (lower.find("do_") != 0) return std::string();

    std::string candidate = std::string("a_") + routine.substr(3);
    std::string err;
    mk2fatal::AssetPlan tmp;
    if (mk2fatal::build_asset_plan(&g_mk2_fatality_doc, candidate.c_str(), "MKJC.ASM", &tmp, &err))
        return candidate;
    return std::string();
}

static void Mk2FatalityApplyTimelineFromPlan(const mk2fatal::AssetPlan &plan,
                                             int *matched_sprites,
                                             int *missing_sprites)
{
    if (matched_sprites) *matched_sprites = 0;
    if (missing_sprites) *missing_sprites = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p)
        img->flags &= ~1u;

    TimelineClearFrames();
    g_timeline_play_dir = 1;
    ClearTimelineCompositeSelection();

    for (const std::string &sprite : plan.sprite_labels) {
        int idx = Mk2FatalityFindImageBySpriteLabel(sprite);
        if (idx >= 0) {
            IMG *img = get_img(idx);
            if (img) img->flags |= 1u;
            if (std::find(g_timeline_frames.begin(), g_timeline_frames.end(), idx) == g_timeline_frames.end())
                TimelinePushFrame(idx);
            if (matched_sprites) (*matched_sprites)++;
        } else if (missing_sprites) {
            (*missing_sprites)++;
        }
    }

    g_timeline_built_for_imgcnt = g_doc->imgcnt;
    if (!g_timeline_frames.empty()) {
        g_doc->ilselected = g_timeline_frames[0];
        g_is_playing = true;
        g_play_speed = 8.0f;
        g_zoom_reset = true;
    }
}

static void Mk2FatalityLoadPlanImagesIntoActiveDoc(const mk2fatal::AssetPlan &plan,
                                                   int *loaded_files,
                                                   int *missing_files)
{
    if (loaded_files) *loaded_files = 0;
    if (missing_files) *missing_files = 0;
    for (const std::string &rel : plan.img_files) {
        std::string full = Mk2FatalityResolveProjectAsset(rel);
        if (!Mk2FatalityFileExists(full)) {
            if (missing_files) (*missing_files)++;
            continue;
        }
        unsigned int before = g_doc->imgcnt;
        SetActiveDocumentPath(full);
        LoadImgFile();
        if (g_doc->imgcnt > before) {
            if (loaded_files) (*loaded_files)++;
            RecentAdd(full);
        }
    }
    g_dirty = false;
}

static void Mk2FatalityMarkPlanInDoc(Document *doc, const mk2fatal::AssetPlan &plan,
                                     std::vector<int> *marked_indices,
                                     int *matched_sprites,
                                     int *missing_sprites)
{
    if (marked_indices) marked_indices->clear();
    if (matched_sprites) *matched_sprites = 0;
    if (missing_sprites) *missing_sprites = 0;
    if (!doc) return;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p)
        img->flags &= ~1u;

    for (const std::string &sprite : plan.sprite_labels) {
        int idx = Mk2FatalityFindImageBySpriteLabel(doc, sprite);
        if (idx >= 0) {
            IMG *img = doc_get_img(doc, idx);
            if (img) img->flags |= 1u;
            if (marked_indices &&
                std::find(marked_indices->begin(), marked_indices->end(), idx) == marked_indices->end())
                marked_indices->push_back(idx);
            if (matched_sprites) (*matched_sprites)++;
        } else if (missing_sprites) {
            (*missing_sprites)++;
        }
    }
}

static void Mk2FatalityStageDualPlans(const Mk2FatalityFighterDef &fighter,
                                      const mk2fatal::AssetPlan &attacker_plan,
                                      const mk2fatal::AssetPlan &victim_plan)
{
    PrepareDocumentForOpenedFile();
    int attacker_doc_idx = document_active_index();
    int attacker_loaded = 0, attacker_missing_files = 0;
    Mk2FatalityLoadPlanImagesIntoActiveDoc(attacker_plan, &attacker_loaded, &attacker_missing_files);
    Document *attacker_doc = document_get(attacker_doc_idx);

    std::vector<int> attacker_marked;
    int attacker_matched = 0, attacker_missing_sprites = 0;
    Mk2FatalityMarkPlanInDoc(attacker_doc, attacker_plan, &attacker_marked,
                             &attacker_matched, &attacker_missing_sprites);
    if (attacker_doc) attacker_doc->dirty = 0;

    int victim_doc_idx = document_new_tab();
    ClearAll();
    int victim_loaded = 0, victim_missing_files = 0;
    Mk2FatalityLoadPlanImagesIntoActiveDoc(victim_plan, &victim_loaded, &victim_missing_files);
    Document *victim_doc = document_get(victim_doc_idx);

    std::vector<int> victim_marked;
    int victim_matched = 0, victim_missing_sprites = 0;
    Mk2FatalityMarkPlanInDoc(victim_doc, victim_plan, &victim_marked,
                             &victim_matched, &victim_missing_sprites);
    if (victim_doc) victim_doc->dirty = 0;

    document_set_active(attacker_doc_idx);
    TimelineSetFrames(attacker_marked);
    g_timeline_play_idx = 0;
    g_timeline_play_dir = 1;
    g_timeline_built_for_imgcnt = g_doc->imgcnt;
    if (!g_timeline_frames.empty())
        g_doc->ilselected = g_timeline_frames[0];

    g_world_state.enabled = true;
    g_world_marked_state.marked_play = true;
    g_world_marked_state.fps = g_mk2_fatality_preview_fps;
    g_play_speed = g_mk2_fatality_preview_fps;
    g_is_playing = true;
    g_world_marked_state.mirror_active = false;
    g_world_marked_state.mirror_other = true;
    for (bool &mirror : g_world_marked_state.mirror_extra)
        mirror = false;
    WorldMarkedClearSplitLanes(g_world_marked_state);
    for (int i = 0; i < kWorldMarkedMaxTabs; i++) {
        g_world_marked_state.hold_end[i] = false;
        g_world_marked_state.lane_visible[i] = true;
    }
    g_world_marked_state.draw_sprite_borders = true;
    g_world_marked_state.show_boundary_overlay = true;
    g_world_marked_state.hold_end[kWorldDummyDecapSlot] = true;
    g_world_marked_state.dummy_decap_body = false;
    g_world_marked_state.dummy_decap_reset = true;
    g_world_marked_state.dummy_decap_manual = false;
    g_world_marked_state.dummy_decap_doc_idx = -1;
    g_world_marked_state.dummy_decap_prefix.clear();
    g_world_marked_state.paused = false;
    WorldMarkedRestart(g_world_marked_state);
    g_zoom_reset = true;

    char buf[320];
    snprintf(buf, sizeof(buf),
             "%s staged: attacker %d IMG/%d sprite%s, Cage victim %d IMG/%d sprite%s%s%s.",
             fighter.name,
             attacker_loaded, attacker_matched, attacker_matched == 1 ? "" : "s",
             victim_loaded, victim_matched, victim_matched == 1 ? "" : "s",
             (attacker_missing_sprites || victim_missing_sprites) ? " (some sprite refs missing)" : "",
             (attacker_missing_files || victim_missing_files) ? " (some IMG files missing)" : "");
    g_mk2_fatality_stage_status = buf;
}

static mk2fatal::AssetPlan Mk2FatalityBuildCageDeathPlan(const std::vector<std::string> &labels)
{
    static const char *kCageImgs[] = {
        "data/CAGE1.IMG", "data/CAGE2.IMG", "data/CAGE3.IMG", "data/CAGE4.IMG", "data/CAGE5.IMG",
        "data/CAGE6.IMG", "data/CAGE7.IMG", "data/CAGE8.IMG", "data/CAGE9.IMG", "data/CAGE10.IMG", NULL
    };
    return Mk2FatalityBuildPlanForLabels(labels, "MKJC.ASM", kCageImgs);
}

static void Mk2FatalityStageFighterWorkspace(void)
{
    if (g_mk2_fatality_fighter_idx < 0 || g_mk2_fatality_fighter_idx >= kMk2FatalityFighterCount)
        return;
    const Mk2FatalityFighterDef &fighter = g_mk2_fatality_fighters[g_mk2_fatality_fighter_idx];

    std::vector<std::string> attacker_labels;
    for (int i = 0; i < 10 && fighter.fatal_anims[i]; i++)
        attacker_labels.push_back(fighter.fatal_anims[i]);
    mk2fatal::AssetPlan attacker_plan =
        Mk2FatalityBuildPlanForLabels(attacker_labels, fighter.source_file, fighter.img_files);

    std::vector<std::string> victim_labels;
    for (int i = 0; g_mk2_fatality_cage_deaths[i]; i++)
        victim_labels.push_back(g_mk2_fatality_cage_deaths[i]);
    mk2fatal::AssetPlan victim_plan = Mk2FatalityBuildCageDeathPlan(victim_labels);

    g_mk2_fatality_plan = victim_plan;
    Mk2FatalityStageDualPlans(fighter, attacker_plan, victim_plan);
}

static Mk2FatalityReactionPair Mk2FatalityInferReactionPair(
    const mk2fatal::CommandBlock &cmd,
    const Mk2FatalityFighterDef &fighter)
{
    Mk2FatalityReactionPair pair = {};
    const char *attacker = fighter.fatal_anims[0];
    const char *victim = "a_torso_ripped";
    const char *reason = "fallback body ending";
    if (cmd.routine == "do_fatality_1") {
        attacker = fighter.db1_anim ? fighter.db1_anim : attacker;
        victim = fighter.db1_victim ? fighter.db1_victim : victim;
        reason = "fighter fatality 1 default";
    } else if (cmd.routine == "do_fatality_2") {
        attacker = fighter.db2_anim ? fighter.db2_anim : attacker;
        victim = fighter.db2_victim ? fighter.db2_victim : victim;
        reason = "fighter fatality 2 default";
    } else if (Mk2FatalityFilterMatch(cmd.routine, "headhole")) {
        attacker = "a_jc_headhole";
        victim = "a_headhole";
        reason = "routine name contains headhole";
    } else if (Mk2FatalityFilterMatch(cmd.routine, "raiden_lift")) {
        attacker = "a_death_zap1";
        victim = "a_torso_ripped";
        reason = "routine name contains raiden_lift";
    } else if (Mk2FatalityFilterMatch(cmd.routine, "decap")) {
        victim = "a_decapfall";
        reason = "routine name contains decap";
    } else if (Mk2FatalityFilterMatch(cmd.routine, "rip")) {
        victim = "a_torso_ripped";
        reason = "routine name contains rip";
    } else if (Mk2FatalityFilterMatch(cmd.routine, "head")) {
        victim = "a_head";
        reason = "routine name contains head";
    }

    pair.attacker = attacker;
    pair.victim = victim;
    pair.reason = reason;
    return pair;
}

static void Mk2FatalityApplyFatalityDefaults(const mk2fatal::CommandBlock &cmd,
                                             const Mk2FatalityFighterDef &fighter)
{
    Mk2FatalityReactionPair pair = Mk2FatalityInferReactionPair(cmd, fighter);
    g_mk2_fatality_attacker_anim_idx =
        Mk2FatalityAnimListIndex(fighter.fatal_anims, pair.attacker);
    g_mk2_fatality_victim_anim_idx =
        Mk2FatalityAnimListIndex(g_mk2_fatality_cage_deaths, pair.victim);
}

static void Mk2FatalityStageSelectedFatality(void)
{
    if (g_mk2_fatality_fighter_idx < 0 || g_mk2_fatality_fighter_idx >= kMk2FatalityFighterCount)
        return;
    const Mk2FatalityFighterDef &fighter = g_mk2_fatality_fighters[g_mk2_fatality_fighter_idx];

    std::vector<std::string> attacker_labels;
    const char *attacker_anim = Mk2FatalitySelectedAttackerAnim(fighter);
    if (attacker_anim) attacker_labels.push_back(attacker_anim);
    mk2fatal::AssetPlan attacker_plan =
        Mk2FatalityBuildPlanForLabels(attacker_labels, fighter.source_file, fighter.img_files);

    std::vector<std::string> victim_labels;
    const char *victim_anim = Mk2FatalitySelectedVictimAnim();
    if (victim_anim) victim_labels.push_back(victim_anim);
    mk2fatal::AssetPlan victim_plan = Mk2FatalityBuildCageDeathPlan(victim_labels);

    g_mk2_fatality_plan = victim_plan;
    Mk2FatalityStageDualPlans(fighter, attacker_plan, victim_plan);
}

static void Mk2FatalityStageAssetPlan(const mk2fatal::AssetPlan &plan)
{
    if (plan.img_files.empty()) {
        g_mk2_fatality_stage_status = "No IMG libraries were resolved for this plan.";
        return;
    }

    PrepareDocumentForOpenedFile();
    int loaded_files = 0;
    int missing_files = 0;
    for (const std::string &rel : plan.img_files) {
        std::string full = Mk2FatalityResolveProjectAsset(rel);
        if (!Mk2FatalityFileExists(full)) {
            missing_files++;
            continue;
        }
        unsigned int before = g_doc->imgcnt;
        SetActiveDocumentPath(full);
        LoadImgFile();
        if (g_doc->imgcnt > before) {
            loaded_files++;
            RecentAdd(full);
        }
    }

    int matched = 0;
    int missing_sprites = 0;
    Mk2FatalityApplyTimelineFromPlan(plan, &matched, &missing_sprites);
    g_dirty = false;
    g_img_tex_idx = -2;

    char buf[256];
    snprintf(buf, sizeof(buf),
             "Staged %d IMG file%s, %d/%d referenced sprite%s matched%s%s.",
             loaded_files, loaded_files == 1 ? "" : "s",
             matched, (int)plan.sprite_labels.size(),
             plan.sprite_labels.size() == 1 ? "" : "s",
             missing_sprites ? " (some missing)" : "",
             missing_files ? " (some IMG files missing)" : "");
    g_mk2_fatality_stage_status = buf;
}

static void Mk2FatalityBuildAndStage(const char *animation_label)
{
    if (!animation_label || !animation_label[0]) return;
    std::string err;
    mk2fatal::AssetPlan plan;
    if (!mk2fatal::build_asset_plan(&g_mk2_fatality_doc, animation_label, "MKJC.ASM", &plan, &err)) {
        g_mk2_fatality_stage_status = std::string("Stage failed: ") + err;
        return;
    }
    g_mk2_fatality_plan = plan;
    Mk2FatalityStageAssetPlan(g_mk2_fatality_plan);
}

static void DrawMk2FatalityPlanSummary(void)
{
    if (g_mk2_fatality_plan.root_label.empty()) return;
    ImGui::Separator();
    ImGui::TextDisabled("Staged plan: %s via %s",
                        g_mk2_fatality_plan.resolved_label.empty()
                            ? g_mk2_fatality_plan.root_label.c_str()
                            : g_mk2_fatality_plan.resolved_label.c_str(),
                        g_mk2_fatality_plan.preferred_file.empty()
                            ? "source" : g_mk2_fatality_plan.preferred_file.c_str());
    ImGui::TextDisabled("%d animation label%s, %d sprite label%s, %d IMG librar%s",
                        (int)g_mk2_fatality_plan.animation_labels.size(),
                        g_mk2_fatality_plan.animation_labels.size() == 1 ? "" : "s",
                        (int)g_mk2_fatality_plan.sprite_labels.size(),
                        g_mk2_fatality_plan.sprite_labels.size() == 1 ? "" : "s",
                        (int)g_mk2_fatality_plan.img_files.size(),
                        g_mk2_fatality_plan.img_files.size() == 1 ? "y" : "ies");
    if (!g_mk2_fatality_stage_status.empty())
        ImGui::TextDisabled("%s", g_mk2_fatality_stage_status.c_str());
}

static void DrawMk2FatalityReactionPairPanel(
    const Mk2FatalityFighterDef &fighter,
    const mk2fatal::CommandBlock *cmd)
{
    if (!cmd) return;

    Mk2FatalityReactionPair inferred =
        Mk2FatalityInferReactionPair(*cmd, fighter);
    const char *selected_attacker = Mk2FatalitySelectedAttackerAnim(fighter);
    const char *selected_victim = Mk2FatalitySelectedVictimAnim();

    if (!ImGui::CollapsingHeader("Reaction Pair",
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::TextDisabled("Fatality routine drives the pair: attacker animation plus victim reaction/body ending.");
    if (ImGui::BeginTable("##mk2fatal_reaction_pair", 3,
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableSetupColumn("Selected", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Inferred", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Routine");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(cmd->routine.empty() ? "(not detected)"
                                                    : cmd->routine.c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", inferred.reason ? inferred.reason : "manual");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Attacker");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(selected_attacker ? selected_attacker : "(none)");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(inferred.attacker ? inferred.attacker : "(none)");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Victim");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(selected_victim ? selected_victim : "(none)");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(inferred.victim ? inferred.victim : "(none)");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Source");
        ImGui::TableNextColumn();
        ImGui::Text("%s / %s", fighter.source_file, "MKJC.ASM");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("opponent reactions currently use Cage body-ending labels");

        ImGui::EndTable();
    }

    if (ImGui::SmallButton("Use Inferred Pair##mk2fatal_pair_infer")) {
        Mk2FatalityApplyFatalityDefaults(*cmd, fighter);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Set the attacker and victim selectors from the routine/default mapping.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Stage Pair##mk2fatal_pair_stage")) {
        Mk2FatalityStageSelectedFatality();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Load the selected attacker and victim reaction into World View lanes.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy Pair##mk2fatal_pair_copy")) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "routine=%s attacker=%s victim=%s reason=%s",
                 cmd->routine.empty() ? "(not detected)" : cmd->routine.c_str(),
                 selected_attacker ? selected_attacker : "(none)",
                 selected_victim ? selected_victim : "(none)",
                 inferred.reason ? inferred.reason : "manual");
        ImGui::SetClipboardText(buf);
    }
}

void DrawMk2FatalityWindow(void)
{
    if (!g_show_mk2_fatality) return;

    ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("MK2 Fatality Lab", &g_show_mk2_fatality)) {
        ImGui::End();
        return;
    }

    if (!g_mk2_fatality_status_sticky && g_mk2_fatality_doc.dirty && !g_mk2_fatality_status.empty())
        g_mk2_fatality_status.clear();

    ImGui::SetNextItemWidth(-360);
    ImGui::InputTextWithHint("##mk2fatal_root", "path to mk2-main or its src folder", g_mk2_fatality_root, sizeof(g_mk2_fatality_root));
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
#ifdef _WIN32
        char path[MAX_PATH] = "";
        BROWSEINFOA bi = {};
        bi.lpszTitle = "Select mk2-main folder";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (pidl) {
            if (SHGetPathFromIDListA(pidl, path) && path[0]) {
                strncpy(g_mk2_fatality_root, path, sizeof(g_mk2_fatality_root) - 1);
                g_mk2_fatality_root[sizeof(g_mk2_fatality_root) - 1] = '\0';
                save_last_dir_cat(path, "mk2fatal");
            }
            CoTaskMemFree(pidl);
        }
#else
        g_mk2_fatality_status = "Browse not implemented on this platform - type the path manually";
        g_mk2_fatality_status_sticky = true;
#endif
    }
    ImGui::SameLine();
    if (ImGui::Button("Use ../mk2-main")) {
        strncpy(g_mk2_fatality_root, "..\\mk2-main", sizeof(g_mk2_fatality_root) - 1);
        g_mk2_fatality_root[sizeof(g_mk2_fatality_root) - 1] = '\0';
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        Mk2FatalityLoadRoot(g_mk2_fatality_root);
    }
    ImGui::SameLine();
    bool can_save = g_mk2_fatality_doc.dirty && !g_mk2_fatality_doc.files.empty();
    if (!can_save) ImGui::BeginDisabled();
    if (ImGui::Button("Save")) {
        std::string err;
        if (mk2fatal::save(&g_mk2_fatality_doc, &err)) {
            g_mk2_fatality_status = "Saved MK2 fatality source edits";
            g_mk2_fatality_status_sticky = false;
        } else {
            g_mk2_fatality_status = std::string("Save failed: ") + err;
            g_mk2_fatality_status_sticky = true;
        }
    }
    if (!can_save) ImGui::EndDisabled();
    ImGui::SameLine();
    bool can_reload = !g_mk2_fatality_doc.root_path.empty();
    if (!can_reload) ImGui::BeginDisabled();
    if (ImGui::Button("Reload")) {
        std::string root = g_mk2_fatality_doc.root_path;
        Mk2FatalityLoadRoot(root.c_str());
    }
    if (!can_reload) ImGui::EndDisabled();

    if (!g_mk2_fatality_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_mk2_fatality_status.c_str());
    }

    if (g_mk2_fatality_doc.files.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("Load an MK2 source root to browse fatality command blocks, controller combo tables, and body-ending animation sequences.");
        ImGui::End();
        return;
    }

    Mk2FatalityClampSelections();
    ImGui::Separator();
    ImGui::TextDisabled("%d files loaded. Dirty files save back to the same ASM paths.",
                        (int)g_mk2_fatality_doc.files.size());

    if (g_mk2_fatality_fighter_idx < 0) g_mk2_fatality_fighter_idx = 0;
    if (g_mk2_fatality_fighter_idx >= kMk2FatalityFighterCount)
        g_mk2_fatality_fighter_idx = kMk2FatalityFighterCount - 1;
    const Mk2FatalityFighterDef &fighter = g_mk2_fatality_fighters[g_mk2_fatality_fighter_idx];
    std::vector<int> fighter_cmds = Mk2FatalityFighterCommandIndices(fighter);
    if (g_mk2_fatality_selected_fatality < 0) g_mk2_fatality_selected_fatality = 0;
    if (g_mk2_fatality_selected_fatality >= (int)fighter_cmds.size())
        g_mk2_fatality_selected_fatality = (int)fighter_cmds.size() - 1;
    if (g_mk2_fatality_selected_fatality < 0) g_mk2_fatality_selected_fatality = 0;
    if (!fighter_cmds.empty() &&
        std::find(fighter_cmds.begin(), fighter_cmds.end(), g_mk2_fatality_command_idx) == fighter_cmds.end())
        g_mk2_fatality_command_idx = fighter_cmds[g_mk2_fatality_selected_fatality];

    ImGui::SetNextItemWidth(210);
    if (ImGui::BeginCombo("Fighter##mk2fatal_fighter", fighter.name)) {
        for (int i = 0; i < kMk2FatalityFighterCount; i++) {
            bool selected = (g_mk2_fatality_fighter_idx == i);
            if (ImGui::Selectable(g_mk2_fatality_fighters[i].name, selected)) {
                g_mk2_fatality_fighter_idx = i;
                g_mk2_fatality_selected_fatality = 0;
                g_mk2_fatality_attacker_anim_idx = 0;
                g_mk2_fatality_victim_anim_idx = 0;
                std::vector<int> new_cmds = Mk2FatalityFighterCommandIndices(g_mk2_fatality_fighters[i]);
                if (!new_cmds.empty())
                    Mk2FatalityApplyFatalityDefaults(g_mk2_fatality_doc.commands[new_cmds[0]],
                                                     g_mk2_fatality_fighters[i]);
                Mk2FatalityStageFighterWorkspace();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Fighter")) {
        Mk2FatalityStageFighterWorkspace();
    }
    ImGui::SameLine();
    if (g_mk2_fatality_preview_fps < 1.0f) g_mk2_fatality_preview_fps = 1.0f;
    if (g_mk2_fatality_preview_fps > 30.0f) g_mk2_fatality_preview_fps = 30.0f;
    ImGui::SetNextItemWidth(86);
    if (ImGui::InputFloat("FPS##mk2fatal_fps", &g_mk2_fatality_preview_fps, 1.0f, 4.0f, "%.1f")) {
        if (g_mk2_fatality_preview_fps < 1.0f) g_mk2_fatality_preview_fps = 1.0f;
        if (g_mk2_fatality_preview_fps > 30.0f) g_mk2_fatality_preview_fps = 30.0f;
        g_world_marked_state.fps = g_mk2_fatality_preview_fps;
        g_play_speed = g_mk2_fatality_preview_fps;
    }

    const char *fatality_preview = fighter_cmds.empty()
        ? "(none found)"
        : g_mk2_fatality_doc.commands[fighter_cmds[g_mk2_fatality_selected_fatality]].label.c_str();
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("Fatality##mk2fatal_pick", fatality_preview)) {
        for (int i = 0; i < (int)fighter_cmds.size(); i++) {
            const mk2fatal::CommandBlock &cmd = g_mk2_fatality_doc.commands[fighter_cmds[i]];
            char label[192];
            snprintf(label, sizeof(label), "%s  %s", cmd.label.c_str(),
                     cmd.routine.empty() ? "" : cmd.routine.c_str());
            bool selected = (g_mk2_fatality_selected_fatality == i);
            if (ImGui::Selectable(label, selected)) {
                g_mk2_fatality_selected_fatality = i;
                Mk2FatalityApplyFatalityDefaults(cmd, fighter);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    const mk2fatal::CommandBlock *selected_cmd = NULL;
    if (!fighter_cmds.empty()) {
        selected_cmd =
            &g_mk2_fatality_doc.commands[fighter_cmds[g_mk2_fatality_selected_fatality]];
    }
    if (!fighter_cmds.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s  %s",
                            selected_cmd->combo_label.empty() ? "combo?" : selected_cmd->combo_label.c_str(),
                            selected_cmd->range_note.empty() ? "" : selected_cmd->range_note.c_str());
    }

    ImGui::SetNextItemWidth(220);
    const char *attacker_preview = Mk2FatalitySelectedAttackerAnim(fighter);
    if (ImGui::BeginCombo("Attacker Animation##mk2fatal_attacker_anim",
                          attacker_preview ? attacker_preview : "(none)")) {
        for (int i = 0; i < 10 && fighter.fatal_anims[i]; i++) {
            bool selected = (g_mk2_fatality_attacker_anim_idx == i);
            if (ImGui::Selectable(fighter.fatal_anims[i], selected))
                g_mk2_fatality_attacker_anim_idx = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    const char *victim_preview = Mk2FatalitySelectedVictimAnim();
    if (ImGui::BeginCombo("Cage Victim Animation##mk2fatal_victim_anim", victim_preview)) {
        for (int i = 0; g_mk2_fatality_cage_deaths[i]; i++) {
            bool selected = (g_mk2_fatality_victim_anim_idx == i);
            if (ImGui::Selectable(g_mk2_fatality_cage_deaths[i], selected))
                g_mk2_fatality_victim_anim_idx = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (fighter_cmds.empty()) ImGui::BeginDisabled();
    if (ImGui::Button("Animate Fatality")) {
        Mk2FatalityStageSelectedFatality();
    }
    if (fighter_cmds.empty()) ImGui::EndDisabled();
    if (!g_mk2_fatality_stage_status.empty())
        ImGui::TextDisabled("%s", g_mk2_fatality_stage_status.c_str());
    DrawMk2FatalityReactionPairPanel(fighter, selected_cmd);
    ImGui::Separator();

    if (ImGui::BeginTabBar("##mk2fatal_tabs")) {
        if (ImGui::BeginTabItem("Fatalities")) {
            const float h = ImGui::GetContentRegionAvail().y - 4.0f;
            ImGui::BeginChild("##mk2fatal_cmd_list", ImVec2(300, h), true);
            ImGui::TextDisabled("Command Blocks");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##mk2fatal_filter_cmd", "filter...", g_mk2_fatality_filter, sizeof(g_mk2_fatality_filter));
            for (int ci = 0; ci < (int)fighter_cmds.size(); ci++) {
                int i = fighter_cmds[ci];
                const auto &cmd = g_mk2_fatality_doc.commands[i];
                std::string hay = cmd.label + " " + cmd.routine + " " + cmd.combo_label + " " + cmd.trigger;
                if (!Mk2FatalityFilterMatch(hay, g_mk2_fatality_filter)) continue;
                char label[192];
                snprintf(label, sizeof(label), "%s  %s", cmd.label.c_str(),
                         cmd.routine.empty() ? "(routine?)" : cmd.routine.c_str());
                if (ImGui::Selectable(label, g_mk2_fatality_command_idx == i)) {
                    g_mk2_fatality_command_idx = i;
                    g_mk2_fatality_selected_fatality = ci;
                    Mk2FatalityApplyFatalityDefaults(cmd, fighter);
                    g_mk2_fatality_selected_line = 0;
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##mk2fatal_cmd_detail", ImVec2(0, h), true);
            if (fighter_cmds.empty()) {
                ImGui::TextDisabled("No fatality command blocks found for this fighter.");
            } else {
                if (g_mk2_fatality_command_idx < 0 ||
                    g_mk2_fatality_command_idx >= (int)g_mk2_fatality_doc.commands.size())
                    g_mk2_fatality_command_idx = fighter_cmds[0];
                mk2fatal::CommandBlock cmd = g_mk2_fatality_doc.commands[g_mk2_fatality_command_idx];
                ImGui::Text("%s", cmd.label.c_str());
                ImGui::TextDisabled("Routine: %s   Transfer: %s   Finish Him: %s",
                                    cmd.routine.empty() ? "(not detected)" : cmd.routine.c_str(),
                                    cmd.transfer.empty() ? "(not detected)" : cmd.transfer.c_str(),
                                    cmd.finish_him_only ? "yes" : "no");
                ImGui::TextDisabled("Combo: %s   Trigger: %s",
                                    cmd.combo_label.empty() ? "(direct / timing)" : cmd.combo_label.c_str(),
                                    cmd.trigger.empty() ? "(not detected)" : cmd.trigger.c_str());
                if (!cmd.range_note.empty()) ImGui::TextDisabled("%s", cmd.range_note.c_str());
                std::string inferred_anim = Mk2FatalityInferAnimationFromRoutine(cmd.routine);
                if (!inferred_anim.empty()) {
                    if (ImGui::Button("Use Routine Animation")) {
                        g_mk2_fatality_attacker_anim_idx =
                            Mk2FatalityAnimListIndex(fighter.fatal_anims, inferred_anim.c_str());
                        Mk2FatalityStageSelectedFatality();
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", inferred_anim.c_str());
                } else {
                    ImGui::TextDisabled("No direct victim animation mapping.");
                }
                DrawMk2FatalityPlanSummary();
                ImGui::Separator();
                if (ImGui::CollapsingHeader("Command Source", ImGuiTreeNodeFlags_DefaultOpen)) {
                    DrawMk2FatalitySourceEditor("##mk2fatal_cmd_src", cmd.file_idx, cmd.start_line, cmd.end_line,
                                                &g_mk2_fatality_selected_line, NULL, 0, false);
                }
                int combo_idx = cmd.combo_label.empty() ? -1 : mk2fatal::find_combo(&g_mk2_fatality_doc, cmd.combo_label.c_str());
                if (combo_idx >= 0) {
                    mk2fatal::ComboBlock combo = g_mk2_fatality_doc.combos[combo_idx];
                    if (ImGui::CollapsingHeader("Controller Combo", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::TextDisabled("%s   time %s   %d words",
                                            combo.label.c_str(),
                                            combo.time_token.empty() ? "?" : combo.time_token.c_str(),
                                            (int)combo.words.size());
                        DrawMk2FatalitySourceEditor("##mk2fatal_cmd_combo_src", combo.file_idx, combo.start_line, combo.end_line,
                                                    &g_mk2_fatality_selected_line,
                                                    g_mk2_fatality_insert_combo, sizeof(g_mk2_fatality_insert_combo), true);
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Animations")) {
            const float h = ImGui::GetContentRegionAvail().y - 4.0f;
            ImGui::BeginChild("##mk2fatal_anim_list", ImVec2(320, h), true);
            ImGui::TextDisabled("Animation / Body Blocks");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##mk2fatal_filter_anim", "filter...", g_mk2_fatality_filter, sizeof(g_mk2_fatality_filter));
            ImGui::Checkbox("Body endings only", &g_mk2_fatality_body_only);
            for (int i = 0; i < (int)g_mk2_fatality_doc.animations.size(); i++) {
                const auto &anim = g_mk2_fatality_doc.animations[i];
                if (!Mk2FatalityFilterMatch(anim.file_rel, fighter.source_file)) continue;
                if (g_mk2_fatality_body_only && !anim.body_ending) continue;
                std::string hay = anim.label + " " + anim.file_rel;
                if (!Mk2FatalityFilterMatch(hay, g_mk2_fatality_filter)) continue;
                char label[224];
                snprintf(label, sizeof(label), "%s  [%s]", anim.label.c_str(), anim.file_rel.c_str());
                if (ImGui::Selectable(label, g_mk2_fatality_anim_idx == i)) {
                    g_mk2_fatality_anim_idx = i;
                    g_mk2_fatality_selected_line = 0;
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##mk2fatal_anim_detail", ImVec2(0, h), true);
            if (g_mk2_fatality_doc.animations.empty()) {
                ImGui::TextDisabled("No animation blocks found.");
            } else {
                mk2fatal::AnimationBlock anim = g_mk2_fatality_doc.animations[g_mk2_fatality_anim_idx];
                ImGui::Text("%s", anim.label.c_str());
                ImGui::TextDisabled("%s   .long tokens %d   .word tokens %d   adjustxy %d",
                                    anim.file_rel.c_str(), anim.long_count, anim.word_count, anim.adjust_count);
                if (anim.body_ending) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("body ending");
                }
                if (ImGui::Button("Animate With Cage")) {
                    std::vector<std::string> attacker_labels;
                    attacker_labels.push_back(anim.label);
                    mk2fatal::AssetPlan attacker_plan =
                        Mk2FatalityBuildPlanForLabels(attacker_labels, fighter.source_file, fighter.img_files);
                    std::vector<std::string> victim_labels;
                    victim_labels.push_back(Mk2FatalitySelectedVictimAnim());
                    mk2fatal::AssetPlan victim_plan = Mk2FatalityBuildCageDeathPlan(victim_labels);
                    g_mk2_fatality_plan = victim_plan;
                    Mk2FatalityStageDualPlans(fighter, attacker_plan, victim_plan);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Johnny Cage");
                DrawMk2FatalityPlanSummary();
                ImGui::Separator();
                DrawMk2FatalitySourceEditor("##mk2fatal_anim_src", anim.file_idx, anim.start_line, anim.end_line,
                                            &g_mk2_fatality_selected_line,
                                            g_mk2_fatality_insert_anim, sizeof(g_mk2_fatality_insert_anim), true);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Controller")) {
            const float h = ImGui::GetContentRegionAvail().y - 4.0f;
            ImGui::BeginChild("##mk2fatal_combo_list", ImVec2(300, h), true);
            ImGui::TextDisabled("scom_* Tables");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##mk2fatal_filter_combo", "filter...", g_mk2_fatality_filter, sizeof(g_mk2_fatality_filter));
            for (int i = 0; i < (int)g_mk2_fatality_doc.combos.size(); i++) {
                const auto &combo = g_mk2_fatality_doc.combos[i];
                std::string hay = combo.label + " " + combo.time_token;
                if (!Mk2FatalityFilterMatch(hay, g_mk2_fatality_filter)) continue;
                char label[160];
                snprintf(label, sizeof(label), "%s  (%s)", combo.label.c_str(),
                         combo.time_token.empty() ? "time?" : combo.time_token.c_str());
                if (ImGui::Selectable(label, g_mk2_fatality_combo_idx == i)) {
                    g_mk2_fatality_combo_idx = i;
                    g_mk2_fatality_selected_line = 0;
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##mk2fatal_combo_detail", ImVec2(0, h), true);
            if (g_mk2_fatality_doc.combos.empty()) {
                ImGui::TextDisabled("No controller combo tables found.");
            } else {
                mk2fatal::ComboBlock combo = g_mk2_fatality_doc.combos[g_mk2_fatality_combo_idx];
                ImGui::Text("%s", combo.label.c_str());
                ImGui::TextDisabled("time %s   %d words",
                                    combo.time_token.empty() ? "?" : combo.time_token.c_str(),
                                    (int)combo.words.size());
                ImGui::Separator();
                DrawMk2FatalitySourceEditor("##mk2fatal_combo_src", combo.file_idx, combo.start_line, combo.end_line,
                                            &g_mk2_fatality_selected_line,
                                            g_mk2_fatality_insert_combo, sizeof(g_mk2_fatality_insert_combo), true);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}


static void DrawAutoSplitSummaryLine(const char *label,
                                     const AutoSplitTargetSummary &summary)
{
    if (summary.split_count <= 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.66f, 0.30f, 1.0f),
                           "%s: no legal split", label);
        return;
    }

    long long delta = summary.src_zcom_bits - summary.split_zcom_bits;
    double pct = summary.src_zcom_bits > 0
        ? (double)delta * 100.0 / (double)summary.src_zcom_bits
        : 0.0;
    ImVec4 col = delta >= 0
        ? ImVec4(0.42f, 0.90f, 0.55f, 1.0f)
        : ImVec4(1.0f, 0.66f, 0.30f, 1.0f);
    ImGui::TextColored(col, "%s: %lld -> %lld bits (%+.1f%%)",
                       label, summary.src_zcom_bits,
                       summary.split_zcom_bits, pct);
    if (summary.selected_preview.best_split_valid &&
        summary.selected_preview.target_count > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("selected cut %s=%d",
                            summary.selected_preview.best_split_vertical ? "x" : "y",
                            summary.selected_preview.best_split_pos);
    }
    ImGui::TextDisabled("%d split-ready target%s, %d skipped below/empty threshold",
                        summary.split_count,
                        summary.split_count == 1 ? "" : "s",
                        summary.skipped_count);
}

void DrawAutoChopDialog(void)
{
    if (g_show_auto_chop) ImGui::OpenPopup("Break into Subframes");
    if (!ImGui::BeginPopupModal("Break into Subframes", &g_show_auto_chop, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextWrapped("Breaks marked sprites, or the selected sprite if none are marked,\n"
                       "into parent-aware subframes and recalculates ANIX/ANIY.");
    ImGui::Spacing();

    AutoSplitTargetSummary horizontal_summary;
    AutoSplitTargetSummary vertical_summary;
    BuildAutoSplitTargetSummary(false, &horizontal_summary);
    BuildAutoSplitTargetSummary(true, &vertical_summary);

    ImGui::RadioButton("Best Horizontal Cut", &g_chop_mode,
                       AutoChopMode_BestHorizontal);
    ImGui::SameLine();
    ImGui::RadioButton("Best Vertical Cut", &g_chop_mode,
                       AutoChopMode_BestVertical);
    ImGui::SameLine();
    ImGui::RadioButton("Manual Grid", &g_chop_mode, AutoChopMode_ManualGrid);
    ImGui::Checkbox("Trim empty space (Highly recommended)", &g_chop_trim);

    ImGui::Spacing();
    ImGui::TextDisabled("Best cuts require both sides to be greater than %dpx.",
                        k_auto_split_min_side);
    DrawAutoSplitSummaryLine("Horizontal", horizontal_summary);
    DrawAutoSplitSummaryLine("Vertical", vertical_summary);

    if (g_chop_mode == AutoChopMode_ManualGrid) {
        ImGui::Spacing();
        if (ImGui::Button("Auto 3 Subframes", ImVec2(140, 0))) AutoChopSetThreeBandSize();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputInt("Piece Width", &g_chop_w)) { if (g_chop_w < 1) g_chop_w = 1; }
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputInt("Piece Height", &g_chop_h)) { if (g_chop_h < 1) g_chop_h = 1; }

        AutoChopPreview summary;
        BuildAutoChopTargetSummary(&summary);
        if (summary.target_count > 0) {
            if (summary.pieces.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.66f, 0.30f, 1.0f),
                                   "LOAD2 ZCOM: no non-empty pieces");
            } else {
                long long delta = summary.src_zcom_bits - summary.split_zcom_bits;
                double pct = summary.src_zcom_bits > 0
                    ? (double)delta * 100.0 / (double)summary.src_zcom_bits
                    : 0.0;
                ImVec4 col = delta >= 0
                    ? ImVec4(0.42f, 0.90f, 0.55f, 1.0f)
                    : ImVec4(1.0f, 0.66f, 0.30f, 1.0f);
                ImGui::TextColored(col, "Manual grid LOAD2 ZCOM: %lld -> %lld bits (%+.1f%%)",
                                   summary.src_zcom_bits, summary.split_zcom_bits, pct);
            }
            char bpp_buf[32];
            if (summary.bpp > 0) snprintf(bpp_buf, sizeof(bpp_buf), "%d bpp", summary.bpp);
            else snprintf(bpp_buf, sizeof(bpp_buf), "mixed bpp");
            ImGui::TextDisabled("%d target%s, %d piece%s, %d empty cell%s skipped, %s",
                                summary.target_count,
                                summary.target_count == 1 ? "" : "s",
                                (int)summary.pieces.size(),
                                summary.pieces.size() == 1 ? "" : "s",
                                summary.empty_cells,
                                summary.empty_cells == 1 ? "" : "s",
                                bpp_buf);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool best_horizontal = (g_chop_mode == AutoChopMode_BestHorizontal);
    bool best_vertical = (g_chop_mode == AutoChopMode_BestVertical);
    bool can_best = best_horizontal ? (horizontal_summary.split_count > 0)
                                    : (vertical_summary.split_count > 0);
    if (g_chop_mode == AutoChopMode_ManualGrid) can_best = true;
    ImGui::BeginDisabled(!can_best);
    if (ImGui::Button("Break", ImVec2(100, 0))) {
        int count = 0;
        if (best_horizontal || best_vertical)
            count = ApplyBestAutoSplitToTargets(best_vertical);
        else
            count = ChopMarkedImages(g_chop_w, g_chop_h, g_chop_trim);
        if (count > 0) {
            snprintf(g_restore_msg, sizeof(g_restore_msg), "Broke into %d subframe piece(s).", count);
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg), "No pieces generated (mark or select a sprite).");
        }
        g_restore_msg_timer = 4.0f;
        g_show_auto_chop = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_auto_chop = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawBulkRestoreRegexDialog(void)
{
    if (g_show_restore_regex) ImGui::OpenPopup("Bulk Restore via Regex");
    if (!ImGui::BeginPopupModal("Bulk Restore via Regex", &g_show_restore_regex, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextWrapped("Uses a regex to map child names to parent names across the entire file.\n"
                       "Capture group 1 (\\1) is used as the parent name.\n"
                       "Example: ^(.+)[A-Z]$ maps JCJUMPFLIP1A -> JCJUMPFLIP1");
    ImGui::Spacing();

    ImGui::Text("Mode:");
    ImGui::SameLine();
    ImGui::RadioButton("Diff (preserve hand-tuning)", &g_restore_diff_mode, 1);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Only the pixels you EDITED on the master are\n"
                          "propagated into children. Every untouched\n"
                          "pixel in each child stays as-is.\n"
                          "(Right choice for adding a logo, edge tweak, etc.)");
    ImGui::SameLine();
    ImGui::RadioButton("Replace (overwrite child bbox)", &g_restore_diff_mode, 0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Wipes each child to zero, then fills its bbox\n"
                          "with parent pixels. Clobbers hand-tuned\n"
                          "per-piece details.");
    ImGui::SameLine();
    ImGui::RadioButton("Reconstruct from Parent", &g_restore_diff_mode, 2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Treats parent as ground truth. For every child\n"
                          "pixel that diverges from the parent (after\n"
                          "anipoint-relative shift), copies the parent's\n"
                          "value into the child.\n"
                          "Use to restore censored / blacked-out regions\n"
                          "where the master still has the original detail.");
    ImGui::Spacing();

    bool pattern_changed = ImGui::InputText("Regex Pattern", g_restore_regex_buf, sizeof(g_restore_regex_buf));
    if (pattern_changed) {
        g_restore_regex_tested = false;
        g_restore_regex_error = false;
        g_restore_matches.clear();
    }

    if (!g_restore_regex_tested) {
        if (ImGui::Button("Preview Matches", ImVec2(120, 0))) {
            g_restore_matches.clear();
            g_restore_regex_error = false;
            std::regex re;
            try {
                re = std::regex(g_restore_regex_buf);
                for (IMG *child = (IMG *)g_doc->img_p; child; child = (IMG *)child->nxt_p) {
                    if (!child->data_p || child->w == 0 || child->h == 0) continue;
                    std::string name(child->n_s);
                    std::smatch match;
                    if (std::regex_match(name, match, re) && match.size() > 1) {
                        std::string parent_name = match[1].str();
                        IMG *parent = NULL;
                        for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
                            if (parent_name == p->n_s) {
                                parent = p;
                                break;
                            }
                        }
                        if (parent && parent->data_p && parent->w > 0 && parent->h > 0 && parent != child
                            && parent->palnum == child->palnum) {
                            g_restore_matches.push_back({child, parent, true, 0, 0});
                        }
                    }
                }
                std::sort(g_restore_matches.begin(), g_restore_matches.end(), [](const BulkRestoreMatch& a, const BulkRestoreMatch& b) {
                    int cmp = strcmp(a.parent->n_s, b.parent->n_s);
                    if (cmp != 0) return cmp < 0;
                    return strcmp(a.child->n_s, b.child->n_s) < 0;
                });
                ComputeBulkRestoreCoverage(g_restore_matches);
                g_restore_regex_tested = true;
            } catch (const std::regex_error&) {
                g_restore_regex_error = true;
            }
        }
        if (g_restore_regex_error) {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Regex Error: invalid pattern");
        }
    } else {
        int partial_count = 0;
        for (auto& m : g_restore_matches) {
            if (m.total_pixels > 0 && m.covered_pixels < m.total_pixels) partial_count++;
        }
        ImGui::Text("Found %d match(es). Select items to restore:", (int)g_restore_matches.size());
        if (partial_count > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                "%d match%s with partial coverage (anipoint shift pushes parent rect "
                "out of bounds). Pairs mode will zero-fill the uncovered area.",
                partial_count, partial_count == 1 ? "" : "es");
        }
        ImGui::BeginChild("MatchesList", ImVec2(520, 220), true);
        std::string last_parent = "";
        for (size_t i = 0; i < g_restore_matches.size(); i++) {
            BulkRestoreMatch& m = g_restore_matches[i];
            std::string current_parent = m.parent->n_s;
            if (current_parent != last_parent) {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", current_parent.c_str());
                last_parent = current_parent;
            }
            ImGui::Indent(16.0f);
            char label[128];
            snprintf(label, sizeof(label), "%s##%zu", m.child->n_s, i);
            bool partial = m.total_pixels > 0 && m.covered_pixels < m.total_pixels;
            if (partial) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.55f, 1.0f));
            ImGui::Checkbox(label, &m.selected);
            if (partial) ImGui::PopStyleColor();
            if (m.total_pixels > 0) {
                ImGui::SameLine();
                int pct = (int)((100.0 * m.covered_pixels) / m.total_pixels + 0.5);
                if (partial) {
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.55f, 1.0f),
                                       "(%d%% covered)", pct);
                } else {
                    ImGui::TextDisabled("(100%%)");
                }
            }
            ImGui::Unindent(16.0f);
        }
        ImGui::EndChild();

        if (ImGui::Button("Select All")) {
            for (auto& m : g_restore_matches) m.selected = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Deselect All")) {
            for (auto& m : g_restore_matches) m.selected = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Deselect Partial")) {
            for (auto& m : g_restore_matches) {
                if (m.total_pixels > 0 && m.covered_pixels < m.total_pixels) m.selected = false;
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::BeginDisabled(!g_restore_regex_tested || g_restore_matches.empty());
    if (ImGui::Button("Start Restore", ImVec2(120, 0))) {
        int n = 0;
        const char *verb_done = "Restored";
        const char *verb_zero = "restored";
        switch (g_restore_diff_mode) {
            case 1:
                n = ExecuteBulkRestoreDiff(g_restore_matches);
                verb_done = "Diff-restored"; verb_zero = "diffed";
                break;
            case 2:
                n = ExecuteBulkRestoreReconstruct(g_restore_matches);
                verb_done = "Reconstructed"; verb_zero = "reconstructed";
                break;
            default:
                n = ExecuteBulkRestorePairs(g_restore_matches);
                break;
        }
        if (n > 0) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "%s %d child image(s) from their parents.",
                     verb_done, n);
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "0 images %s.", verb_zero);
        }
        g_restore_msg_timer = 6.0f;

        g_show_restore_regex = false;
        g_restore_regex_tested = false;
        g_restore_matches.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_restore_regex = false;
        g_restore_regex_tested = false;
        g_restore_matches.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawDeleteImagesConfirm(void)
{
    if (g_show_delete_images_confirm) ImGui::OpenPopup("Delete Sprite Subframes");
    if (!ImGui::BeginPopupModal("Delete Sprite Subframes", &g_show_delete_images_confirm,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    std::vector<int> base = g_pending_delete_base_indices;
    std::vector<int> extra = g_pending_delete_subframe_indices;
    NormalizeImageDeleteIndices(&base);
    NormalizeImageDeleteIndices(&extra);

    int base_count = (int)base.size();
    int extra_count = (int)extra.size();
    bool bulk = base_count > 1 || g_pending_delete_parent_name[0] == '\0';

    if (bulk) {
        ImGui::TextWrapped("Delete %d marked sprite%s?", base_count,
                           base_count == 1 ? "" : "s");
        ImGui::TextWrapped("%d subframe%s belong to marked parent sprite%s.",
                           extra_count,
                           extra_count == 1 ? "" : "s",
                           base_count == 1 ? "" : "s");
    } else {
        ImGui::TextWrapped("\"%s\" has %d subframe%s.",
                           g_pending_delete_parent_name,
                           extra_count,
                           extra_count == 1 ? "" : "s");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const char *base_label = bulk ? "Delete Marked Only" : "Delete Parent Only";
    if (ImGui::Button(base_label, ImVec2(150, 0))) {
        int deleted = DeleteImagesByIndices(base);
        if (g_last_delete_removed_palettes > 0) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Deleted %d sprite%s and %d now-unused palette%s.",
                     deleted, deleted == 1 ? "" : "s",
                     g_last_delete_removed_palettes,
                     g_last_delete_removed_palettes == 1 ? "" : "s");
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Deleted %d sprite%s.", deleted, deleted == 1 ? "" : "s");
        }
        g_restore_msg_timer = 4.0f;
        ClearPendingImageDelete();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();

    std::vector<int> all = base;
    all.insert(all.end(), extra.begin(), extra.end());
    const char *all_label = bulk ? "Delete Marked + Subframes" : "Delete Parent + Subframes";
    if (ImGui::Button(all_label, ImVec2(210, 0))) {
        int deleted = DeleteImagesByIndices(all);
        if (g_last_delete_removed_palettes > 0) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Deleted %d sprite%s and %d now-unused palette%s.",
                     deleted, deleted == 1 ? "" : "s",
                     g_last_delete_removed_palettes,
                     g_last_delete_removed_palettes == 1 ? "" : "s");
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Deleted %d sprite%s.", deleted, deleted == 1 ? "" : "s");
        }
        g_restore_msg_timer = 4.0f;
        ClearPendingImageDelete();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();

    if (ImGui::Button("Cancel", ImVec2(90, 0))) {
        ClearPendingImageDelete();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

SeqScrLayoutInfo SeqScrLayout(void)
{
    SeqScrLayoutInfo li = {};
    li.far_model = g_doc->fileversion >= 0x0634;
    li.record_size = li.far_model ? 98 : 58;
    li.entry_size = li.far_model ? 18 : 16;
    li.entry_index_off = li.far_model ? 4 : 2;
    li.entry_ticks_off = li.far_model ? 6 : 4;
    li.entry_dx_off = li.far_model ? 8 : 6;
    li.entry_dy_off = li.far_model ? 10 : 8;
    li.entry_spare1_off = li.far_model ? 12 : 10;
    li.startx_off = li.far_model ? 84 : 52;
    li.starty_off = li.far_model ? 86 : 54;
    return li;
}

unsigned short SeqScrReadU16(const unsigned char *p)
{
    return (unsigned short)(p[0] | (p[1] << 8));
}

short SeqScrReadI16(const unsigned char *p)
{
    return (short)SeqScrReadU16(p);
}

static void SeqScrWriteU16(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void SeqScrCopyName(char dst[17], const unsigned char *src)
{
    memcpy(dst, src, 16);
    dst[16] = '\0';
    for (int i = 0; i < 16; i++) {
        unsigned char c = (unsigned char)dst[i];
        if (c == '\0') break;
        if (c < 32 || c >= 127) dst[i] = '.';
    }
}

bool SeqScrBuildRecords(std::vector<SeqScrRecordView> &records,
                        bool *truncated_out)
{
    records.clear();
    if (truncated_out) *truncated_out = false;
    if (!g_doc->scrseqmem_p || g_doc->scrseqbytes == 0)
        return false;

    const SeqScrLayoutInfo li = SeqScrLayout();
    const unsigned char *blob = (const unsigned char *)g_doc->scrseqmem_p;
    size_t bytes = (size_t)g_doc->scrseqbytes;
    size_t off = 0;
    unsigned int total = g_doc->seqcnt + g_doc->scrcnt;
    records.reserve(total);

    for (unsigned int i = 0; i < total; i++) {
        SeqScrRecordView rec = {};
        rec.index = (int)i;
        rec.script = i >= g_doc->seqcnt;
        rec.offset = off;
        rec.truncated = off + (size_t)li.record_size > bytes;
        if (rec.truncated) {
            if (truncated_out) *truncated_out = true;
            records.push_back(rec);
            return true;
        }

        const unsigned char *base = blob + off;
        SeqScrCopyName(rec.name, base);
        rec.flags = (int)SeqScrReadI16(base + 16);
        rec.num = (int)SeqScrReadU16(base + 18);
        rec.startx = (int)SeqScrReadI16(base + li.startx_off);
        rec.starty = (int)SeqScrReadI16(base + li.starty_off);
        rec.entries_offset = off + (size_t)li.record_size;

        size_t entry_bytes = (size_t)rec.num * (size_t)li.entry_size;
        if (rec.entries_offset + entry_bytes > bytes) {
            rec.truncated = true;
            if (truncated_out) *truncated_out = true;
            records.push_back(rec);
            return true;
        }

        records.push_back(rec);
        off = rec.entries_offset + entry_bytes;
    }

    if (off > bytes && truncated_out)
        *truncated_out = true;
    return true;
}

const char *SeqScrRecordTypeLabel(const SeqScrRecordView &rec)
{
    return rec.script ? "Script" : "Seq";
}

/* Append a new, empty (zero-entry) sequence or script record to the SEQSCR
   blob. A new sequence is inserted at the end of the sequence block so the
   indices of existing sequences (which scripts reference) stay valid; a new
   script is appended at the very end. Grows the blob, bumps seqcnt/scrcnt, and
   leaves it for the editor to fill in. Returns false on a malformed blob or
   allocation failure. */
bool SeqScrAddRecord(bool script)
{
    const SeqScrLayoutInfo li = SeqScrLayout();
    const unsigned char *old = (const unsigned char *)g_doc->scrseqmem_p;
    size_t blob_bytes = old ? (size_t)g_doc->scrseqbytes : 0;

    size_t insert_off = blob_bytes;   /* append (new script, or empty blob) */
    if (blob_bytes > 0) {
        std::vector<SeqScrRecordView> records;
        bool truncated = false;
        SeqScrBuildRecords(records, &truncated);
        if (truncated) return false;  /* never rewrite a malformed blob */
        if (!script) {
            /* End of the sequence block = offset of the first script record. */
            for (const SeqScrRecordView &rec : records) {
                if (rec.index == (int)g_doc->seqcnt) { insert_off = rec.offset; break; }
            }
        }
    }

    size_t new_bytes = blob_bytes + (size_t)li.record_size;
    unsigned char *nb = (unsigned char *)malloc(new_bytes);
    if (!nb) return false;

    if (insert_off > 0) memcpy(nb, old, insert_off);
    memset(nb + insert_off, 0, (size_t)li.record_size);

    const char *defname = script ? "NEWSCRIPT" : "NEWSEQ";
    size_t namelen = strlen(defname);
    if (namelen > 16) namelen = 16;
    memcpy(nb + insert_off, defname, namelen);
    /* num (offset 18) and flags (offset 16) stay zero. Far-pointer files use
       -1 (0xFF bytes) to mean "no damage table ref"; default to that. */
    if (li.far_model)
        memset(nb + insert_off + 88, 0xFF, 6);

    if (insert_off < blob_bytes)
        memcpy(nb + insert_off + li.record_size, old + insert_off,
               blob_bytes - insert_off);

    doc_undo_push();
    if (g_doc->scrseqmem_p) free(g_doc->scrseqmem_p);
    g_doc->scrseqmem_p = nb;
    g_doc->scrseqbytes = (unsigned int)new_bytes;
    if (script) g_doc->scrcnt++;
    else        g_doc->seqcnt++;
    mark_dirty();
    return true;
}

bool SeqScrAppendEntry(int record_index, int target_index)
{
    std::vector<int> targets(1, target_index);
    return SeqScrAppendEntries(record_index, targets);
}

bool SeqScrAppendEntries(int record_index, const std::vector<int> &target_indices)
{
    if (target_indices.empty()) return false;
    std::vector<SeqScrRecordView> records;
    bool truncated = false;
    if (!SeqScrBuildRecords(records, &truncated) || truncated ||
        record_index < 0 || record_index >= (int)records.size())
        return false;
    const SeqScrRecordView &rec = records[(size_t)record_index];
    if (rec.truncated) return false;
    for (int target_index : target_indices)
        if (target_index < 0 ||
            (!rec.script && target_index >= (int)g_doc->imgcnt) ||
            (rec.script && target_index >= (int)g_doc->seqcnt))
            return false;
    const SeqScrLayoutInfo li = SeqScrLayout();
    size_t old_bytes = (size_t)g_doc->scrseqbytes;
    size_t insert_at = rec.entries_offset + (size_t)rec.num * (size_t)li.entry_size;
    size_t added_bytes = (size_t)li.entry_size * target_indices.size();
    size_t new_bytes = old_bytes + added_bytes;
    unsigned char *nb = (unsigned char *)malloc(new_bytes);
    if (!nb) return false;
    memcpy(nb, g_doc->scrseqmem_p, insert_at);
    memset(nb + insert_at, 0, added_bytes);
    for (size_t i = 0; i < target_indices.size(); i++) {
        unsigned char *entry = nb + insert_at + i * (size_t)li.entry_size;
        SeqScrWriteU16(entry + li.entry_index_off,
                       (unsigned short)(short)target_indices[i]);
        entry[li.entry_ticks_off] = 1;
    }
    memcpy(nb + insert_at + added_bytes,
           (unsigned char *)g_doc->scrseqmem_p + insert_at,
           old_bytes - insert_at);
    SeqScrWriteU16(nb + rec.offset + 18,
                   (unsigned short)(rec.num + target_indices.size()));
    doc_undo_push();
    free(g_doc->scrseqmem_p);
    g_doc->scrseqmem_p = nb;
    g_doc->scrseqbytes = (unsigned int)new_bytes;
    mark_dirty();
    return true;
}

bool SeqScrMoveEntry(int record_index, int entry_index, int delta)
{
    std::vector<SeqScrRecordView> records;
    bool truncated = false;
    if (!SeqScrBuildRecords(records, &truncated) || truncated ||
        record_index < 0 || record_index >= (int)records.size())
        return false;
    const SeqScrRecordView &rec = records[(size_t)record_index];
    int other = entry_index + delta;
    if (rec.truncated || entry_index < 0 || entry_index >= rec.num ||
        other < 0 || other >= rec.num)
        return false;

    const SeqScrLayoutInfo li = SeqScrLayout();
    doc_undo_push();
    unsigned char *blob = (unsigned char *)g_doc->scrseqmem_p;
    unsigned char *a = blob + rec.entries_offset +
                       (size_t)entry_index * (size_t)li.entry_size;
    unsigned char *b = blob + rec.entries_offset +
                       (size_t)other * (size_t)li.entry_size;
    std::vector<unsigned char> tmp((size_t)li.entry_size);
    memcpy(tmp.data(), a, (size_t)li.entry_size);
    memcpy(a, b, (size_t)li.entry_size);
    memcpy(b, tmp.data(), (size_t)li.entry_size);
    mark_dirty();
    return true;
}

bool SeqScrDeleteEntry(int record_index, int entry_index)
{
    std::vector<SeqScrRecordView> records;
    bool truncated = false;
    if (!SeqScrBuildRecords(records, &truncated) || truncated ||
        record_index < 0 || record_index >= (int)records.size())
        return false;
    const SeqScrRecordView &rec = records[(size_t)record_index];
    if (rec.truncated || entry_index < 0 || entry_index >= rec.num)
        return false;
    const SeqScrLayoutInfo li = SeqScrLayout();
    size_t old_bytes = (size_t)g_doc->scrseqbytes;
    size_t erase_at = rec.entries_offset + (size_t)entry_index * (size_t)li.entry_size;
    size_t new_bytes = old_bytes - (size_t)li.entry_size;
    unsigned char *nb = (unsigned char *)malloc(new_bytes ? new_bytes : 1);
    if (!nb) return false;
    memcpy(nb, g_doc->scrseqmem_p, erase_at);
    memcpy(nb + erase_at, (unsigned char *)g_doc->scrseqmem_p + erase_at + li.entry_size,
           old_bytes - erase_at - li.entry_size);
    SeqScrWriteU16(nb + rec.offset + 18, (unsigned short)(rec.num - 1));
    doc_undo_push();
    free(g_doc->scrseqmem_p);
    g_doc->scrseqmem_p = nb;
    g_doc->scrseqbytes = (unsigned int)new_bytes;
    mark_dirty();
    return true;
}

const char *SeqScrEntryTargetName(const SeqScrRecordView &rec,
                                  int entry_index,
                                  const std::vector<SeqScrRecordView> &records)
{
    static char label[64];
    if (entry_index < 0) return "NULL";

    if (rec.script) {
        if (entry_index >= 0 && entry_index < (int)g_doc->seqcnt &&
            entry_index < (int)records.size()) {
            snprintf(label, sizeof(label), "%s", records[(size_t)entry_index].name);
            return label;
        }
        snprintf(label, sizeof(label), "seq[%d]?", entry_index);
        return label;
    }

    IMG *img = get_img(entry_index);
    if (img) {
        snprintf(label, sizeof(label), "%.15s", img->n_s);
        return label;
    }
    snprintf(label, sizeof(label), "img[%d]?", entry_index);
    return label;
}

static bool SeqScrBeginBlobEdit(void)
{
    if (!g_doc->scrseqmem_p || g_doc->scrseqbytes == 0)
        return false;
    return doc_undo_push();
}

static bool SeqScrInputI16(const char *label, unsigned char *base, int off,
                           bool editing, int width = 74)
{
    int v = (int)SeqScrReadI16(base + off);
    int old_v = v;
    if (!editing) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth((float)width);
    bool changed = ImGui::InputInt(label, &v, 0, 0);
    if (!editing) ImGui::EndDisabled();
    if (!changed || v == old_v)
        return false;
    if (v < -32768) v = -32768;
    if (v > 32767) v = 32767;
    if (!SeqScrBeginBlobEdit())
        return false;
    unsigned char *edit_blob = (unsigned char *)g_doc->scrseqmem_p;
    size_t base_off = (size_t)(base - (unsigned char *)g_doc->scrseqmem_p);
    SeqScrWriteU16(edit_blob + base_off + (size_t)off,
                   (unsigned short)(short)v);
    mark_dirty();
    return true;
}

static bool SeqScrInputU8(const char *label, unsigned char *base, int off,
                          bool editing, int width = 58)
{
    int v = (int)base[off];
    int old_v = v;
    if (!editing) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth((float)width);
    bool changed = ImGui::InputInt(label, &v, 0, 0);
    if (!editing) ImGui::EndDisabled();
    if (!changed || v == old_v)
        return false;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    if (!SeqScrBeginBlobEdit())
        return false;
    unsigned char *edit_blob = (unsigned char *)g_doc->scrseqmem_p;
    size_t base_off = (size_t)(base - (unsigned char *)g_doc->scrseqmem_p);
    edit_blob[base_off + (size_t)off] = (unsigned char)v;
    mark_dirty();
    return true;
}

static bool SeqScrInputI8(const char *label, unsigned char *base, int off,
                          bool editing, int width = 52)
{
    int v = base[off] >= 128 ? (int)base[off] - 256 : (int)base[off];
    int old_v = v;
    if (!editing) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth((float)width);
    bool changed = ImGui::InputInt(label, &v, 0, 0);
    if (!editing) ImGui::EndDisabled();
    if (!changed || v == old_v)
        return false;
    if (v < -128) v = -128;
    if (v > 127) v = 127;
    if (!SeqScrBeginBlobEdit())
        return false;
    unsigned char *edit_blob = (unsigned char *)g_doc->scrseqmem_p;
    size_t base_off = (size_t)(base - (unsigned char *)g_doc->scrseqmem_p);
    edit_blob[base_off + (size_t)off] = (unsigned char)(signed char)v;
    mark_dirty();
    return true;
}

/* Blob-resizing operations are deferred until the record list has finished
   drawing; record views point into that blob and must not be invalidated
   halfway through the ImGui pass. */
static int s_seqscr_pending_append_record = -1;
static int s_seqscr_pending_append_target = -1;
static int s_seqscr_pending_delete_record = -1;
static int s_seqscr_pending_delete_entry = -1;
static int s_seqscr_pending_move_record = -1;
static int s_seqscr_pending_move_entry = -1;
static int s_seqscr_pending_move_delta = 0;
static int s_seqscr_pending_append_marked_record = -1;

static void SeqScrDrawRecordEditor(const SeqScrRecordView &rec,
                                   const SeqScrLayoutInfo &li,
                                   const std::vector<SeqScrRecordView> &records,
                                   bool editing)
{
    if (rec.truncated) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                           "Record is truncated; editing disabled.");
        return;
    }

    unsigned char *blob = (unsigned char *)g_doc->scrseqmem_p;
    unsigned char *base = blob + rec.offset;

    ImGui::TextDisabled("%s %d  offset=0x%X  entries=%d  flags=0x%04X",
                        SeqScrRecordTypeLabel(rec),
                        rec.script ? (rec.index - (int)g_doc->seqcnt) : rec.index,
                        (unsigned)rec.offset, rec.num, rec.flags & 0xFFFF);

    static int s_name_record = -1;
    static char s_name_edit[17] = {};
    if (s_name_record == rec.index) {
        if (!editing) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("Name##seqscr_name_edit", s_name_edit,
                         sizeof(s_name_edit));
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply##seqscr_name_apply")) {
            if (SeqScrBeginBlobEdit()) {
                unsigned char *edit_blob = (unsigned char *)g_doc->scrseqmem_p;
                unsigned char *edit_base = edit_blob + rec.offset;
                memset(edit_base, 0, 16);
                strncpy((char *)edit_base, s_name_edit, 15);
                mark_dirty();
            }
            s_name_record = -1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel##seqscr_name_cancel"))
            s_name_record = -1;
        if (!editing) ImGui::EndDisabled();
    } else {
        ImGui::Text("Name: %.16s", rec.name);
        ImGui::SameLine();
        if (!editing) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Rename##seqscr_name_start")) {
            s_name_record = rec.index;
            memset(s_name_edit, 0, sizeof(s_name_edit));
            strncpy(s_name_edit, rec.name, sizeof(s_name_edit) - 1);
        }
        if (!editing) ImGui::EndDisabled();
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Flags");
    ImGui::SameLine();
    SeqScrInputI16("##seqscr_flags", base, 16, editing);
    ImGui::SameLine();
    ImGui::TextDisabled("Num is read-only because changing it would resize/reframe the blob.");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Start X");
    ImGui::SameLine();
    SeqScrInputI16("##seqscr_startx", base, li.startx_off, editing);
    ImGui::SameLine();
    ImGui::TextUnformatted("Start Y");
    ImGui::SameLine();
    SeqScrInputI16("##seqscr_starty", base, li.starty_off, editing);

    ImGui::Separator();
    if (rec.script) {
        static int s_script_target_seq = 0;
        if (s_script_target_seq < 0) s_script_target_seq = 0;
        if (s_script_target_seq >= (int)g_doc->seqcnt)
            s_script_target_seq = (int)g_doc->seqcnt - 1;
        ImGui::TextDisabled("Add a sequence call to this script:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputInt("Seq##seqscr_add_script_target", &s_script_target_seq, 0, 0);
        bool can_add = g_doc->seqcnt > 0 && s_script_target_seq >= 0 &&
                       s_script_target_seq < (int)g_doc->seqcnt;
        ImGui::SameLine();
        ImGui::BeginDisabled(!can_add);
        if (ImGui::SmallButton("Add Sequence##seqscr_append_script")) {
            s_seqscr_pending_append_record = rec.index;
            s_seqscr_pending_append_target = s_script_target_seq;
        }
        ImGui::EndDisabled();
    } else {
        int selected = g_doc->ilselected;
        IMG *selected_img = get_img(selected);
        ImGui::TextDisabled("Add the selected sprite as the next frame:");
        ImGui::SameLine();
        ImGui::BeginDisabled(!selected_img);
        if (ImGui::SmallButton("Add Selected Sprite##seqscr_append_sprite")) {
            s_seqscr_pending_append_record = rec.index;
            s_seqscr_pending_append_target = selected;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && selected_img)
            ImGui::SetTooltip("Adds [%d] %.16s with a 1-tick hold. Edit Ticks/dX/dY below afterwards.",
                              selected, selected_img->n_s);
        int marked = CountMarkedImages();
        ImGui::SameLine();
        ImGui::BeginDisabled(marked <= 0);
        if (ImGui::SmallButton("Add Marked Frames##seqscr_append_marked"))
            s_seqscr_pending_append_marked_record = rec.index;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && marked > 0)
            ImGui::SetTooltip("Appends all %d marked sprites in image-list order, each with a 1-tick hold.", marked);
    }

    if (li.far_model) {
        ImGui::TextDisabled("Damage table refs");
        for (int i = 0; i < 6; i++) {
            if (i > 0) ImGui::SameLine();
            char id[32];
            snprintf(id, sizeof(id), "D%d##seqscr_dam_%d", i, i);
            SeqScrInputI8(id, base, 88 + i, editing, 52);
        }
    }

    if (rec.num <= 0) {
        ImGui::TextDisabled("No entries.");
        return;
    }

    if (ImGui::BeginTable("##seqscr_entries", 8,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0, 240))) {
        ImGui::TableSetupColumn("# / Order", ImGuiTableColumnFlags_WidthFixed, 112);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 74);
        ImGui::TableSetupColumn("Ticks", ImGuiTableColumnFlags_WidthFixed, 58);
        ImGui::TableSetupColumn("dX", ImGuiTableColumnFlags_WidthFixed, 74);
        ImGui::TableSetupColumn("dY", ImGuiTableColumnFlags_WidthFixed, 74);
        ImGui::TableSetupColumn("Spare1", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Spare2/3", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableHeadersRow();

        for (int e = 0; e < rec.num; e++) {
            unsigned char *entry = blob + rec.entries_offset +
                                   (size_t)e * (size_t)li.entry_size;
            int item_index = (int)SeqScrReadI16(entry + li.entry_index_off);

            ImGui::PushID(e);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", e);
            ImGui::SameLine();
            if (ImGui::SmallButton("x##seqscr_delete_entry")) {
                s_seqscr_pending_delete_record = rec.index;
                s_seqscr_pending_delete_entry = e;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this entry from the record.");
            ImGui::SameLine();
            ImGui::BeginDisabled(e == 0);
            if (ImGui::SmallButton("^##seqscr_move_up")) {
                s_seqscr_pending_move_record = rec.index;
                s_seqscr_pending_move_entry = e;
                s_seqscr_pending_move_delta = -1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(e + 1 >= rec.num);
            if (ImGui::SmallButton("v##seqscr_move_down")) {
                s_seqscr_pending_move_record = rec.index;
                s_seqscr_pending_move_entry = e;
                s_seqscr_pending_move_delta = 1;
            }
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(SeqScrEntryTargetName(rec, item_index, records));

            ImGui::TableSetColumnIndex(2);
            SeqScrInputI16("##idx", entry, li.entry_index_off, editing);

            ImGui::TableSetColumnIndex(3);
            SeqScrInputU8("##ticks", entry, li.entry_ticks_off, editing);

            ImGui::TableSetColumnIndex(4);
            SeqScrInputI16("##dx", entry, li.entry_dx_off, editing);

            ImGui::TableSetColumnIndex(5);
            SeqScrInputI16("##dy", entry, li.entry_dy_off, editing);

            ImGui::TableSetColumnIndex(6);
            SeqScrInputI16("##s1", entry, li.entry_spare1_off, editing, 66);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Raw signed spare word preserved from the ENTRY record.");

            ImGui::TableSetColumnIndex(7);
            int spare2_off = li.entry_spare1_off + 2;
            int spare3_off = li.entry_spare1_off + 4;
            SeqScrInputI16("##s2", entry, spare2_off, editing, 66);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Raw signed spare word preserved from the ENTRY record.");
            ImGui::SameLine();
            SeqScrInputI16("##s3", entry, spare3_off, editing, 66);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Raw signed spare word preserved from the ENTRY record.");
            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

void DrawSeqScrEditorWindow(void)
{
    if (!g_show_seqscr_editor) return;

    ImGui::SetNextWindowSize(ImVec2(900, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Anim Scripts / Seqs", &g_show_seqscr_editor)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Sequences: %u   Scripts: %u   Blob: %u bytes   Layout: %s",
                g_doc->seqcnt, g_doc->scrcnt, g_doc->scrseqbytes,
                g_doc->fileversion >= 0x0634 ? "far pointer" : "near pointer");
    ImGui::TextDisabled("Edits are fixed-size only and save back into the IMG's SEQSCR/ENTRY blob.");
    if (ImGui::Button("New Sequence")) SeqScrAddRecord(false);
    ImGui::SameLine();
    if (ImGui::Button("New Script")) SeqScrAddRecord(true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scripts call sequences; create sequences first, then add those sequences to a script.");
    if (ImGui::CollapsingHeader("How to read this", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("Sequences are the simple frame lists: each entry usually targets an IMG sprite, with Ticks as the frame hold and dX/dY as local animation offsets.");
        ImGui::TextWrapped("Scripts are higher-level animation lists: each entry targets a sequence, so a script can chain multiple sprite sequences together.");
        ImGui::TextWrapped("Index is the raw target number saved in the IMG. The Target column resolves it to a sprite or sequence name when this editor can match it.");
        ImGui::TextWrapped("Spare1/2/3 are the three raw signed spare words at the end of each ENTRY record. They are preserved Midway/WIMP metadata and may be routine-specific.");
        ImGui::TextWrapped("Start X/Y and record-level damage refs are preserved Midway/WIMP metadata. Damage refs use -1 for none in newer far-pointer files.");
    }

    bool has_blob = g_doc->scrseqmem_p && g_doc->scrseqbytes > 0;
    if (!has_blob) {
        ImGui::Spacing();
        ImGui::TextDisabled("This IMG has no embedded sequence/script blob yet. Use New Sequence or New Script above to create one.");
        ImGui::End();
        return;
    }

    static bool s_editing = false;
    ImGui::Checkbox("Enable in-place editing", &s_editing);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Allows editing names, flags, start positions, damage refs,\n"
                          "entry indices, ticks, deltas, and spare fields.\n"
                          "Entry counts are intentionally read-only.");
    }

    std::vector<SeqScrRecordView> records;
    bool truncated = false;
    SeqScrBuildRecords(records, &truncated);
    if (truncated) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                           "Warning: the sequence/script blob ended before all declared records were parsed.");
    }

    const SeqScrLayoutInfo li = SeqScrLayout();
    ImGui::Separator();

    if (ImGui::BeginChild("##seqscr_records", ImVec2(0, 0), true)) {
        for (const SeqScrRecordView &rec : records) {
            char header[128];
            int local_index = rec.script ? rec.index - (int)g_doc->seqcnt
                                         : rec.index;
            snprintf(header, sizeof(header), "%s %d  %.16s  entries=%d%s",
                     SeqScrRecordTypeLabel(rec), local_index, rec.name,
                     rec.num, rec.truncated ? "  TRUNCATED" : "");
            ImGui::PushID(rec.index);
            if (ImGui::CollapsingHeader(header)) {
                SeqScrDrawRecordEditor(rec, li, records, s_editing);
                ImGui::Spacing();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (s_seqscr_pending_append_record >= 0) {
        SeqScrAppendEntry(s_seqscr_pending_append_record,
                          s_seqscr_pending_append_target);
        s_seqscr_pending_append_record = -1;
        s_seqscr_pending_append_target = -1;
    } else if (s_seqscr_pending_append_marked_record >= 0) {
        std::vector<int> targets;
        int idx = 0;
        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++)
            if (img->flags & 1) targets.push_back(idx);
        SeqScrAppendEntries(s_seqscr_pending_append_marked_record, targets);
        s_seqscr_pending_append_marked_record = -1;
    } else if (s_seqscr_pending_delete_record >= 0) {
        SeqScrDeleteEntry(s_seqscr_pending_delete_record,
                          s_seqscr_pending_delete_entry);
        s_seqscr_pending_delete_record = -1;
        s_seqscr_pending_delete_entry = -1;
    } else if (s_seqscr_pending_move_record >= 0) {
        SeqScrMoveEntry(s_seqscr_pending_move_record,
                        s_seqscr_pending_move_entry,
                        s_seqscr_pending_move_delta);
        s_seqscr_pending_move_record = -1;
        s_seqscr_pending_move_entry = -1;
        s_seqscr_pending_move_delta = 0;
    }

    ImGui::End();
}

static void DebugHexBytes(const char *label, const unsigned char *data, int len)
{
    char hex[256];
    char ascii[64];
    int hp = 0;
    int ap = 0;
    if (!data || len <= 0) {
        ImGui::TextDisabled("%s: <none>", label);
        return;
    }
    if (len > 32) len = 32;
    for (int i = 0; i < len && hp < (int)sizeof(hex) - 4; i++) {
        hp += snprintf(hex + hp, sizeof(hex) - (size_t)hp, "%02X%s",
                       data[i], (i + 1 < len) ? " " : "");
        ascii[ap++] = (data[i] >= 32 && data[i] < 127) ? (char)data[i] : '.';
    }
    hex[hp] = '\0';
    ascii[ap] = '\0';
    ImGui::Text("%s: %s  |%s|", label, hex, ascii);
}

static void DebugDrawPointTable(const IMG *img)
{
    if (!img || !img->pttbl_p) {
        ImGui::TextDisabled("PTTBL:   none");
        return;
    }

    ImGui::Text("PTTBL_p:  %p", img->pttbl_p);
    const unsigned char *p = (const unsigned char *)img->pttbl_p;
    if (ImGui::BeginTable("##debug_pttbl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("X");
        ImGui::TableSetupColumn("Y");
        ImGui::TableHeadersRow();
        for (int i = 0; i < 10; i++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", (int)SeqScrReadI16(p + i * 4));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", (int)SeqScrReadI16(p + i * 4 + 2));
        }
        ImGui::EndTable();
    }
}

static void DebugDrawAltPaletteTable(const IMG *img)
{
    if (!img || !img->opaltbl_p) {
        ImGui::TextDisabled("OPALTBL: none");
        return;
    }

    const unsigned char *alt = (const unsigned char *)img->opaltbl_p;
    ImGui::Text("OPALTBL_p: %p", img->opaltbl_p);
    DebugHexBytes("OPALTBL", alt, 16);
    if (ImGui::BeginTable("##debug_opaltbl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Slot");
        ImGui::TableSetupColumn("Byte");
        ImGui::TableSetupColumn("Best-effort palette");
        ImGui::TableHeadersRow();
        for (int i = 0; i < 16; i++) {
            unsigned int v = alt[i];
            PAL *pal = get_pal((int)v);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%02d", i);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%02X / %u", v, v);
            ImGui::TableSetColumnIndex(2);
            if (pal) ImGui::Text("%u %.9s", v, pal->n_s);
            else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
}

static void DebugDrawDamageTableRefs()
{
    if (!g_doc->damtbl_p || g_doc->damtblbytes == 0 || g_doc->damcnt == 0) {
        ImGui::TextDisabled("DAMAGE TABLE REFS: none");
        return;
    }

    std::vector<SeqScrRecordView> records;
    bool truncated = false;
    SeqScrBuildRecords(records, &truncated);

    unsigned int count = g_doc->damtblbytes / 4u;
    if (g_doc->damcnt < count) count = g_doc->damcnt;
    const unsigned char *p = (const unsigned char *)g_doc->damtbl_p;
    ImGui::Text("DAMTBLBYTES: %u bytes", g_doc->damtblbytes);
    if (truncated) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.25f, 1.0f),
                                      "SEQSCR blob truncated; names may be incomplete.");

    if (ImGui::BeginTable("##debug_damtbl", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Flag");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Index");
        ImGui::TableSetupColumn("Target");
        ImGui::TableHeadersRow();
        for (unsigned int i = 0; i < count; i++) {
            unsigned short flag = SeqScrReadU16(p + i * 4);
            short index = SeqScrReadI16(p + i * 4 + 2);
            const char *kind = (flag == 0x0040) ? "Seq"
                              : (flag == 0x0000) ? "Script" : "?";
            int rec_index = -1;
            if (flag == 0x0040 && index >= 0) rec_index = index;
            else if (flag == 0x0000 && index >= 0) rec_index = (int)g_doc->seqcnt + index;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", i);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%04X", flag);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(kind);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", (int)index);
            ImGui::TableSetColumnIndex(4);
            if (rec_index >= 0 && rec_index < (int)records.size())
                ImGui::Text("%s %.16s", kind, records[(size_t)rec_index].name);
            else
                ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
}

void DrawDebugInfoModal(void)
{
    if (g_show_debug) ImGui::OpenPopup("Debug Info");
    ImGui::SetNextWindowSize(ImVec2(720, 680), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Debug Info", &g_show_debug, ImGuiWindowFlags_NoMove)) return;

    if (ImGui::CollapsingHeader("LIB_HDR", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("IMGCNT:  %u",     g_doc->imgcnt);
        ImGui::Text("PALCNT:  %u",     g_doc->palcnt);
        ImGui::Text("SEQCNT:  %u",     g_doc->seqcnt);
        ImGui::Text("SCRCNT:  %u",     g_doc->scrcnt);
        ImGui::Text("DAMCNT:  %u",     g_doc->damcnt);
        ImGui::Text("VERSION: 0x%04X", g_doc->fileversion);
        DebugHexBytes("BUFSCR", g_doc->file_bufscr, 4);
        ImGui::Text("SPARE1/2/3: 0x%04X 0x%04X 0x%04X",
                    (unsigned)g_doc->file_spare1,
                    (unsigned)g_doc->file_spare2,
                    (unsigned)g_doc->file_spare3);
        ImGui::Separator();
        ImGui::TextDisabled("SEQSCR/ENTRY blob (load-time, round-trips on save):");
        if (g_doc->scrseqmem_p && g_doc->scrseqbytes > 0) {
            ImGui::Text("SCRSEQBYTES:  %u bytes", g_doc->scrseqbytes);
        } else {
            ImGui::TextDisabled("SCRSEQBYTES:  0  (no seq/scr in file)");
        }
        DebugDrawDamageTableRefs();
    }

    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (ImGui::CollapsingHeader("IMAGE (runtime)", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (img) {
            ImGui::Text("NXT_p:    %p",       img->nxt_p);
            ImGui::Text("N_s:      %.15s",    img->n_s);
            ImGui::Text("FLAGS:    0x%04X",   (int)img->flags);
            ImGui::Text("ANIX:     %d",       (int)(short)img->anix);
            ImGui::Text("ANIY:     %d",       (int)(short)img->aniy);
            ImGui::Text("W:        %d",       (int)img->w);
            ImGui::Text("H:        %d",       (int)img->h);
            ImGui::Text("PALNUM:   %d",       (int)img->palnum);
            ImGui::Text("DATA_p:   %p",       img->data_p);
            ImGui::Text("ANIX2:    %d",       (int)(short)img->anix2);
            ImGui::Text("ANIY2:    %d",       (int)(short)img->aniy2);
            ImGui::Text("ANIZ2:    %d",       (int)(short)img->aniz2);
            ImGui::Text("OPALS:    0x%04X",   (int)img->opals);
            DebugDrawAltPaletteTable(img);
            DebugDrawPointTable(img);
            ImGui::Text("TEMP:     %p",       img->temp);
        } else {
            ImGui::TextDisabled("No image selected");
        }
    }
    if (ImGui::CollapsingHeader("IMAGE_disk (load-time)", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (img) {
            DebugHexBytes("RAW_NAME", img->file_name_raw, 16);
            ImGui::Text("FILE_OSET:    0x%X (%u)", img->file_oset, img->file_oset);
            ImGui::Text("FILE_DATA:    0x%X (%u)", img->file_data, img->file_data);
            ImGui::Text("FILE_LIB:     %u",        (unsigned)img->file_lib);
            ImGui::Text("FILE_FRM:     %u",        (unsigned)img->file_frm);
            if (img->file_pttblnum == 0xFFFF)
                ImGui::TextDisabled("FILE_PTTBLNUM: 0xFFFF (none)");
            else
                ImGui::Text("FILE_PTTBLNUM: %u",  (unsigned)img->file_pttblnum);
            if (!(img->flags & 0x0080)) {
                unsigned int stride = ((unsigned int)img->w + 3u) & ~3u;
                ImGui::Text("PIX_SIZE:     %u bytes (uncompressed, %ux%u)",
                            stride * (unsigned)img->h,
                            (unsigned)img->w, (unsigned)img->h);
            } else {
                ImGui::TextDisabled("PIX_SIZE:     CMP — variable per row");
            }
        } else {
            ImGui::TextDisabled("No image selected");
        }
    }

    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (ImGui::CollapsingHeader("PALETTE (runtime)", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (pal) {
            ImGui::Text("NXT_p:    %p",       pal->nxt_p);
            ImGui::Text("N_s:      %.9s",     pal->n_s);
            ImGui::Text("FLAGS:    0x%02X",   pal->flags);
            ImGui::Text("BITSPIX:  %u",       pal->bitspix);
            if (pal->numc > 0) {
                int want_bpp = PaletteBppForColorCount((int)pal->numc);
                if ((int)pal->bitspix != want_bpp) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.66f, 0.30f, 1.0f),
                                       "(should be %d for %u colors)",
                                       want_bpp, pal->numc);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Palette Operations > Recalculate BPP fixes this.\n"
                                          "TBL/IRW/LOAD2 exports pack pixels at BITSPIX.");
                }
            }
            ImGui::Text("NUMC:     %u",       pal->numc);
            ImGui::Text("PAD:      0x%04X",   pal->pad);
            ImGui::Text("DATA_p:   %p",       pal->data_p);
            ImGui::Text("TEMP:     %p",       pal->temp);
            ImGui::Separator();
            DebugHexBytes("RAW_NAME", pal->file_name_raw, 10);
            ImGui::Text("FILE_DATA:  0x%04X", (unsigned)pal->file_data);
            ImGui::Text("FILE_LIB:   %u",     (unsigned)pal->file_lib);
            ImGui::Text("FILE_COLIND:%u",     (unsigned)pal->file_colind);
            ImGui::Text("FILE_CMAP:  %u",     (unsigned)pal->file_cmap);
            ImGui::Text("FILE_SPARE: 0x%04X", (unsigned)pal->file_spare);
        } else {
            ImGui::TextDisabled("No palette selected");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0))) {
        g_show_debug = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("F9 to toggle");
    ImGui::EndPopup();
}

struct OpacityGradientStats {
    int images = 0;
    int opaque_pixels = 0;
    int cleared_pixels = 0;
};

static int OpacityGradientClamp(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static unsigned int OpacityGradientHash(int x, int y, unsigned char ci,
                                        int image_idx, int seed)
{
    unsigned int h = 2166136261u;
    h ^= (unsigned int)(x + seed * 17); h *= 16777619u;
    h ^= (unsigned int)(y + seed * 31); h *= 16777619u;
    h ^= (unsigned int)ci;              h *= 16777619u;
    h ^= (unsigned int)(image_idx + 1); h *= 16777619u;
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return h;
}

static bool OpacityGradientOpaqueBounds(const IMG *img,
                                        int *min_x, int *min_y,
                                        int *max_x, int *max_y)
{
    if (min_x) *min_x = 0;
    if (min_y) *min_y = 0;
    if (max_x) *max_x = -1;
    if (max_y) *max_y = -1;
    if (!img || !img->data_p || img->w == 0 || img->h == 0)
        return false;

    int w = (int)img->w;
    int h = (int)img->h;
    int stride = (w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)img->data_p;
    int lx = w, ly = h, rx = -1, by = -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (pix[(size_t)y * stride + x] == 0) continue;
            if (x < lx) lx = x;
            if (y < ly) ly = y;
            if (x > rx) rx = x;
            if (y > by) by = y;
        }
    }
    if (rx < 0) return false;
    if (min_x) *min_x = lx;
    if (min_y) *min_y = ly;
    if (max_x) *max_x = rx;
    if (max_y) *max_y = by;
    return true;
}

static float OpacityGradientT(int direction,
                              int x, int y,
                              int min_x, int min_y,
                              int max_x, int max_y)
{
    int w_span = max_x - min_x;
    int h_span = max_y - min_y;
    switch (direction) {
    case 0:
        return w_span > 0 ? (float)(x - min_x) / (float)w_span : 0.0f;
    case 1:
        return w_span > 0 ? (float)(max_x - x) / (float)w_span : 0.0f;
    case 2:
        return h_span > 0 ? (float)(y - min_y) / (float)h_span : 0.0f;
    case 3:
        return h_span > 0 ? (float)(max_y - y) / (float)h_span : 0.0f;
    case 4:
    case 5: {
        float cx = ((float)min_x + (float)max_x) * 0.5f;
        float cy = ((float)min_y + (float)max_y) * 0.5f;
        float dx = (float)x - cx;
        float dy = (float)y - cy;
        float d = sqrtf(dx * dx + dy * dy);
        float corners[4][2] = {
            { (float)min_x - cx, (float)min_y - cy },
            { (float)max_x - cx, (float)min_y - cy },
            { (float)min_x - cx, (float)max_y - cy },
            { (float)max_x - cx, (float)max_y - cy }
        };
        float max_d = 0.0f;
        for (int i = 0; i < 4; i++) {
            float cd = sqrtf(corners[i][0] * corners[i][0] +
                             corners[i][1] * corners[i][1]);
            if (cd > max_d) max_d = cd;
        }
        float t = max_d > 0.0f ? d / max_d : 0.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        return direction == 4 ? t : 1.0f - t;
    }
    default:
        return 0.0f;
    }
}

static int ApplyOpacityGradientOne(IMG *img, int image_idx, bool apply)
{
    if (!img || !img->data_p || img->w == 0 || img->h == 0)
        return 0;

    int min_x = 0, min_y = 0, max_x = (int)img->w - 1, max_y = (int)img->h - 1;
    if (g_opacity_gradient_content_bounds) {
        if (!OpacityGradientOpaqueBounds(img, &min_x, &min_y, &max_x, &max_y))
            return 0;
    }

    int w = (int)img->w;
    int h = (int)img->h;
    int stride = (w + 3) & ~3;
    unsigned char *pix = (unsigned char *)img->data_p;
    int changed = 0;
    int start = OpacityGradientClamp(g_opacity_gradient_start, 0, 100);
    int end = OpacityGradientClamp(g_opacity_gradient_end, 0, 100);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char *p = pix + (size_t)y * stride + x;
            unsigned char ci = *p;
            if (ci == 0) continue;
            float t = OpacityGradientT(g_opacity_gradient_direction,
                                       x, y, min_x, min_y, max_x, max_y);
            int keep_pct = (int)floorf((float)start +
                                       ((float)end - (float)start) * t +
                                       0.5f);
            keep_pct = OpacityGradientClamp(keep_pct, 0, 100);
            bool keep = keep_pct >= 100 ||
                        (keep_pct > 0 &&
                         (int)(OpacityGradientHash(x, y, ci, image_idx,
                                                   g_opacity_gradient_seed) % 100u) < keep_pct);
            if (!keep) {
                changed++;
                if (apply) *p = 0;
            }
        }
    }
    return changed;
}

static OpacityGradientStats OpacityGradientScan(bool apply,
                                                std::vector<int> *changed_indices)
{
    OpacityGradientStats stats = {};
    if (!g_doc) return stats;

    auto visit = [&](int idx, IMG *img) {
        if (!img || !img->data_p || img->w == 0 || img->h == 0) return;
        int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
        if (!OpacityGradientOpaqueBounds(img, &min_x, &min_y, &max_x, &max_y))
            return;

        int stride = ((int)img->w + 3) & ~3;
        const unsigned char *pix = (const unsigned char *)img->data_p;
        int opaque = 0;
        for (int y = 0; y < (int)img->h; y++)
            for (int x = 0; x < (int)img->w; x++)
                if (pix[(size_t)y * stride + x] != 0) opaque++;

        int changed = ApplyOpacityGradientOne(img, idx, apply);
        stats.images++;
        stats.opaque_pixels += opaque;
        stats.cleared_pixels += changed;
        if (apply && changed > 0 && changed_indices)
            changed_indices->push_back(idx);
    };

    if (g_opacity_gradient_marked) {
        int idx = 0;
        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
            if (img->flags & 1) visit(idx, img);
        }
    } else {
        IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
        visit(g_doc->ilselected, img);
    }
    return stats;
}

/* Live preview texture for the opacity gradient dialog. Rebuilt each frame the
   preview is shown (sprites are small, so a per-frame relock is cheap); the
   texture is recreated only when the sprite dimensions change. Cleared pixels
   are written transparent so the dissolve shows over the checkerboard host. */
static SDL_Texture *g_opacity_gradient_preview_tex = NULL;
static int          g_opacity_gradient_preview_w = 0;
static int          g_opacity_gradient_preview_h = 0;

static void OpacityGradientFreePreview(void)
{
    if (g_opacity_gradient_preview_tex) {
        SDL_DestroyTexture(g_opacity_gradient_preview_tex);
        g_opacity_gradient_preview_tex = NULL;
    }
    g_opacity_gradient_preview_w = 0;
    g_opacity_gradient_preview_h = 0;
}

static SDL_Texture *OpacityGradientBuildPreview(IMG *img, int image_idx)
{
    if (!img || !img->data_p || img->w == 0 || img->h == 0 || !g_imgui_renderer)
        return NULL;

    int w = (int)img->w;
    int h = (int)img->h;
    if (!g_opacity_gradient_preview_tex ||
        g_opacity_gradient_preview_w != w ||
        g_opacity_gradient_preview_h != h) {
        OpacityGradientFreePreview();
        g_opacity_gradient_preview_tex =
            SDL_CreateTexture(g_imgui_renderer, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!g_opacity_gradient_preview_tex) return NULL;
        SDL_SetTextureBlendMode(g_opacity_gradient_preview_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_opacity_gradient_preview_tex, SDL_ScaleModeNearest);
        g_opacity_gradient_preview_w = w;
        g_opacity_gradient_preview_h = h;
    }

    int min_x = 0, min_y = 0, max_x = w - 1, max_y = h - 1;
    if (g_opacity_gradient_content_bounds)
        OpacityGradientOpaqueBounds(img, &min_x, &min_y, &max_x, &max_y);

    int start = OpacityGradientClamp(g_opacity_gradient_start, 0, 100);
    int end = OpacityGradientClamp(g_opacity_gradient_end, 0, 100);
    int stride = (w + 3) & ~3;
    const unsigned char *sp = (const unsigned char *)img->data_p;
    PAL *pal = get_pal(img->palnum);
    const unsigned char *pd = pal ? (const unsigned char *)pal->data_p : NULL;

    void *pixels; int pitch;
    if (SDL_LockTexture(g_opacity_gradient_preview_tex, NULL, &pixels, &pitch) != 0)
        return NULL;
    Uint32 *dst = (Uint32 *)pixels;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char ci = sp[(size_t)y * stride + x];
            Uint32 out = 0; /* transparent by default */
            if (ci != 0) {
                float t = OpacityGradientT(g_opacity_gradient_direction,
                                           x, y, min_x, min_y, max_x, max_y);
                int keep_pct = (int)floorf((float)start +
                                           ((float)end - (float)start) * t + 0.5f);
                keep_pct = OpacityGradientClamp(keep_pct, 0, 100);
                bool keep = keep_pct >= 100 ||
                            (keep_pct > 0 &&
                             (int)(OpacityGradientHash(x, y, ci, image_idx,
                                       g_opacity_gradient_seed) % 100u) < keep_pct);
                if (keep) {
                    Uint32 r = 200, g = 200, b = 200;
                    if (pd) {
                        unsigned short w15 =
                            (unsigned short)(pd[ci*2] | (pd[ci*2+1] << 8));
                        r = (((w15 >> 10) & 0x1F) << 3);
                        g = (((w15 >>  5) & 0x1F) << 3);
                        b = (( w15        & 0x1F) << 3);
                    }
                    out = (0xFFu << 24) | (r << 16) | (g << 8) | b;
                }
            }
            dst[y * (pitch / 4) + x] = out;
        }
    }
    SDL_UnlockTexture(g_opacity_gradient_preview_tex);
    return g_opacity_gradient_preview_tex;
}

static void SpriteCleanupPreviewClear(void)
{
    if (g_sprite_cleanup_preview.current_tex) {
        SDL_DestroyTexture(g_sprite_cleanup_preview.current_tex);
        g_sprite_cleanup_preview.current_tex = NULL;
    }
    if (g_sprite_cleanup_preview.updated_tex) {
        SDL_DestroyTexture(g_sprite_cleanup_preview.updated_tex);
        g_sprite_cleanup_preview.updated_tex = NULL;
    }
    g_sprite_cleanup_preview.current_w = 0;
    g_sprite_cleanup_preview.current_h = 0;
    g_sprite_cleanup_preview.updated_w = 0;
    g_sprite_cleanup_preview.updated_h = 0;
    g_sprite_cleanup_preview.image_idx = -1;
    g_sprite_cleanup_preview.changed_pixels = 0;
    g_sprite_cleanup_preview.key.clear();
}

static SpriteCleanupOptions SpriteCleanupOptionsFromUi(void)
{
    SpriteCleanupOptions opt;
    opt.search_radius = g_sprite_cleanup_radius;
    opt.similarity_distance = g_sprite_cleanup_similarity;
    opt.min_similar_neighbors = g_sprite_cleanup_min_similar;
    opt.min_replacement_neighbors = g_sprite_cleanup_replacement_support;
    opt.outlier_distance = g_sprite_cleanup_outlier;
    opt.allow_transparent_replacement = g_sprite_cleanup_transparent;
    return opt;
}

static int SpriteCleanupPreviewTargetIndex(int marked)
{
    if (!g_doc) return -1;
    if (marked > 0) {
        if (g_doc->ilselected >= 0) {
            IMG *selected = get_img(g_doc->ilselected);
            if (selected && (selected->flags & 1))
                return g_doc->ilselected;
        }
        int idx = 0;
        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
            if (img->flags & 1) return idx;
        }
        return -1;
    }
    return g_doc->ilselected;
}

static unsigned int SpriteCleanupPixelHash(const unsigned char *pixels, size_t bytes)
{
    unsigned int h = 2166136261u;
    for (size_t i = 0; i < bytes; i++) {
        h ^= (unsigned int)pixels[i];
        h *= 16777619u;
    }
    return h;
}

static bool SpriteCleanupPixelsToRgba(const unsigned char *pixels,
                                      int w, int h, int stride, PAL *pal,
                                      std::vector<unsigned char> &rgba)
{
    if (!pixels || w <= 0 || h <= 0 || stride < w) return false;
    rgba.assign((size_t)w * h * 4, 0);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char ci = pixels[(size_t)y * stride + x];
            unsigned char *dst = &rgba[((size_t)y * w + x) * 4];
            if (ci == 0) {
                dst[3] = 0;
                continue;
            }

            unsigned int r = ci, g = ci, b = ci;
            if (pal && pal->data_p && ci < pal->numc) {
                unsigned short w15 = pal_word_or_black(pal, ci);
                r = (unsigned int)(((w15 >> 10) & 0x1F) * 255 / 31);
                g = (unsigned int)(((w15 >>  5) & 0x1F) * 255 / 31);
                b = (unsigned int)(( w15        & 0x1F) * 255 / 31);
            }
            dst[0] = (unsigned char)r;
            dst[1] = (unsigned char)g;
            dst[2] = (unsigned char)b;
            dst[3] = 255;
        }
    }
    return true;
}

static void SpriteCleanupPreviewRefresh(int marked)
{
    int image_idx = SpriteCleanupPreviewTargetIndex(marked);
    IMG *img = (image_idx >= 0) ? get_img(image_idx) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) {
        SpriteCleanupPreviewClear();
        return;
    }

    SpriteCleanupOptions opt = SpriteCleanupOptionsFromUi();
    int w = (int)img->w;
    int h = (int)img->h;
    int stride = (w + 3) & ~3;
    size_t bytes = (size_t)stride * h;
    unsigned char *pixels = (unsigned char *)img->data_p;
    unsigned int hash = SpriteCleanupPixelHash(pixels, bytes);

    char key[256];
    snprintf(key, sizeof(key), "%p:%d:%p:%u:%u:%u:%u:%u:%d:%d:%d:%d:%d:%d",
             (void *)g_doc, image_idx, img->data_p,
             (unsigned)img->w, (unsigned)img->h, (unsigned)img->palnum,
             g_palette_sync_serial, hash,
             opt.search_radius, opt.similarity_distance,
             opt.min_similar_neighbors, opt.min_replacement_neighbors,
             opt.outlier_distance, opt.allow_transparent_replacement ? 1 : 0);
    if (g_sprite_cleanup_preview.current_tex &&
        g_sprite_cleanup_preview.updated_tex &&
        g_sprite_cleanup_preview.key == key)
        return;

    PAL *pal = get_pal((int)img->palnum);
    std::vector<unsigned char> current_rgba;
    std::vector<unsigned char> updated_rgba;
    std::vector<unsigned char> work(bytes);
    memcpy(work.data(), pixels, bytes);

    int changed = CleanupSpriteArtifacts(work.data(), w, h, stride, pal, &opt, true);
    SDL_Texture *current_tex = NULL;
    SDL_Texture *updated_tex = NULL;
    if (SpriteCleanupPixelsToRgba(pixels, w, h, stride, pal, current_rgba))
        current_tex = make_preview_texture(current_rgba.data(), w, h, 192);
    if (SpriteCleanupPixelsToRgba(work.data(), w, h, stride, pal, updated_rgba))
        updated_tex = make_preview_texture(updated_rgba.data(), w, h, 192);

    SpriteCleanupPreviewClear();
    if (!current_tex || !updated_tex) {
        if (current_tex) SDL_DestroyTexture(current_tex);
        if (updated_tex) SDL_DestroyTexture(updated_tex);
        return;
    }

    SDL_QueryTexture(current_tex, NULL, NULL,
                     &g_sprite_cleanup_preview.current_w,
                     &g_sprite_cleanup_preview.current_h);
    SDL_QueryTexture(updated_tex, NULL, NULL,
                     &g_sprite_cleanup_preview.updated_w,
                     &g_sprite_cleanup_preview.updated_h);
    g_sprite_cleanup_preview.current_tex = current_tex;
    g_sprite_cleanup_preview.updated_tex = updated_tex;
    g_sprite_cleanup_preview.image_idx = image_idx;
    g_sprite_cleanup_preview.changed_pixels = changed;
    g_sprite_cleanup_preview.key = key;
}

void OpenSpriteCleanupDialog(void)
{
    if (!g_doc) return;
    if (CountMarkedImages() == 0 && g_doc->ilselected < 0) return;
    SpriteCleanupPreviewClear();
    g_show_sprite_cleanup = true;
}

void DrawSpriteCleanupDialog(void)
{
    if (g_show_sprite_cleanup)
        ImGui::OpenPopup("Sprite Artifact Cleanup");
    if (!ImGui::BeginPopupModal("Sprite Artifact Cleanup",
                                &g_show_sprite_cleanup,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!g_show_sprite_cleanup) SpriteCleanupPreviewClear();
        return;
    }

    int marked = CountMarkedImages();
    bool have_target = marked > 0 || (g_doc && g_doc->ilselected >= 0);
    if (marked > 0) {
        ImGui::Text("Target: %d marked sprite%s", marked, marked == 1 ? "" : "s");
    } else if (g_doc && g_doc->ilselected >= 0) {
        IMG *img = get_img(g_doc->ilselected);
        ImGui::Text("Target: %s", img ? img->n_s : "selected sprite");
    } else {
        ImGui::TextDisabled("No sprite selected");
    }

    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt("Radius", &g_sprite_cleanup_radius, 1, 8);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Neighbor search radius in pixels.");

    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt("Color tolerance", &g_sprite_cleanup_similarity, 0, 32);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Higher values treat nearby palette colors as the same family.");

    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt("Similar neighbors", &g_sprite_cleanup_min_similar, 1, 8);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pixels with at least this much same-family support are kept.");

    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt("Replacement support", &g_sprite_cleanup_replacement_support, 1, 16);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Local support required before repainting with a non-transparent color.");

    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt("Outlier difference", &g_sprite_cleanup_outlier, 0, 32);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Minimum color difference from the chosen replacement.");

    ImGui::Checkbox("Dust to #0", &g_sprite_cleanup_transparent);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Surrounded one-off pixels become transparent index #0.");

    ImGui::Separator();
    SpriteCleanupPreviewRefresh(marked);
    if (g_sprite_cleanup_preview.current_tex &&
        g_sprite_cleanup_preview.updated_tex) {
        IMG *preview_img = get_img(g_sprite_cleanup_preview.image_idx);
        ImGui::Text("Preview: %s (%d px)",
                    preview_img ? preview_img->n_s : "sprite",
                    g_sprite_cleanup_preview.changed_pixels);

        ImGui::BeginGroup();
        ImGui::TextUnformatted("Current");
        ImGui::Image((ImTextureID)(intptr_t)g_sprite_cleanup_preview.current_tex,
                     ImVec2((float)g_sprite_cleanup_preview.current_w,
                            (float)g_sprite_cleanup_preview.current_h));
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Updated");
        ImGui::Image((ImTextureID)(intptr_t)g_sprite_cleanup_preview.updated_tex,
                     ImVec2((float)g_sprite_cleanup_preview.updated_w,
                            (float)g_sprite_cleanup_preview.updated_h));
        ImGui::EndGroup();
    } else {
        ImGui::TextDisabled("Preview unavailable");
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!have_target);
    if (ImGui::Button("Clean", ImVec2(100, 0))) {
        SpriteCleanupOptions opt = SpriteCleanupOptionsFromUi();
        CleanSpriteArtifactsInTargets(&opt);
        SpriteCleanupPreviewClear();
        g_show_sprite_cleanup = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        SpriteCleanupPreviewClear();
        g_show_sprite_cleanup = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void OpenOpacityGradientDialog(void)
{
    if (!g_doc || g_doc->ilselected < 0) return;
    g_show_opacity_gradient = true;
}

void DrawOpacityGradientDialog(void)
{
    if (g_show_opacity_gradient)
        ImGui::OpenPopup("Opacity Gradient");
    if (!ImGui::BeginPopupModal("Opacity Gradient",
                                &g_show_opacity_gradient,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        OpacityGradientFreePreview();
        return;
    }

    IMG *selected = (g_doc && g_doc->ilselected >= 0)
                 ? get_img(g_doc->ilselected) : NULL;
    int marked_count = CountMarkedImages();
    bool have_selected = selected && selected->data_p &&
                         selected->w > 0 && selected->h > 0;
    if (!have_selected) {
        ImGui::TextDisabled("Select a sprite with pixels first.");
        if (ImGui::Button("Close", ImVec2(90, 0)))
            g_show_opacity_gradient = false;
        ImGui::EndPopup();
        return;
    }

    ImGui::Text("%s  %dx%d", selected->n_s, selected->w, selected->h);
    ImGui::TextWrapped("IMG sprites do not store true per-pixel alpha. This writes transparent index #0 with a stable dissolve pattern.");

    const char *directions[] = {
        "Left to Right",
        "Right to Left",
        "Top to Bottom",
        "Bottom to Top",
        "Center to Edge",
        "Edge to Center"
    };
    ImGui::Combo("Direction", &g_opacity_gradient_direction,
                 directions, (int)(sizeof(directions) / sizeof(directions[0])));

    ImGui::SetNextItemWidth(190.0f);
    ImGui::SliderInt("Start opacity", &g_opacity_gradient_start, 0, 100, "%d%%");
    ImGui::SetNextItemWidth(190.0f);
    ImGui::SliderInt("End opacity", &g_opacity_gradient_end, 0, 100, "%d%%");
    if (ImGui::SmallButton("Swap Start/End")) {
        int t = g_opacity_gradient_start;
        g_opacity_gradient_start = g_opacity_gradient_end;
        g_opacity_gradient_end = t;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset Fade Out")) {
        g_opacity_gradient_start = 100;
        g_opacity_gradient_end = 0;
    }

    ImGui::Checkbox("Use opaque content bounds", &g_opacity_gradient_content_bounds);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Span the gradient over the non-transparent sprite bounds instead of the padded canvas.");
    ImGui::Checkbox("Trim transparent bounds after apply", &g_opacity_gradient_trim);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Pattern seed", &g_opacity_gradient_seed, 1, 17);

    bool can_marked = marked_count > 0;
    if (!can_marked && g_opacity_gradient_marked)
        g_opacity_gradient_marked = false;
    if (!can_marked) ImGui::BeginDisabled();
    ImGui::Checkbox("Apply to marked sprites", &g_opacity_gradient_marked);
    if (!can_marked) ImGui::EndDisabled();

    ImGui::Checkbox("Live preview", &g_opacity_gradient_preview);
    if (g_opacity_gradient_preview) {
        SDL_Texture *tex =
            OpacityGradientBuildPreview(selected, g_doc->ilselected);
        if (tex) {
            /* Fit the sprite into a preview box, scaled up with nearest
               filtering, over a checkerboard so cleared (transparent) pixels
               read clearly. */
            const float BOX = 200.0f;
            float sw_px = (float)selected->w;
            float sh_px = (float)selected->h;
            float scale = BOX / (sw_px > sh_px ? sw_px : sh_px);
            ImVec2 draw_sz(sw_px * scale, sh_px * scale);

            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float CHK = 8.0f;
            for (float yy = 0; yy < draw_sz.y; yy += CHK) {
                for (float xx = 0; xx < draw_sz.x; xx += CHK) {
                    bool dark = ((int)(xx / CHK) + (int)(yy / CHK)) & 1;
                    ImVec2 a(p0.x + xx, p0.y + yy);
                    ImVec2 b(p0.x + (xx + CHK > draw_sz.x ? draw_sz.x : xx + CHK),
                             p0.y + (yy + CHK > draw_sz.y ? draw_sz.y : yy + CHK));
                    dl->AddRectFilled(a, b,
                        dark ? IM_COL32(60, 60, 60, 255)
                             : IM_COL32(90, 90, 90, 255));
                }
            }
            ImGui::Image((ImTextureID)(intptr_t)tex, draw_sz);
        }
    } else {
        OpacityGradientFreePreview();
    }

    OpacityGradientStats preview = OpacityGradientScan(false, NULL);
    ImGui::Separator();
    ImGui::Text("Preview: %d sprite%s, %d opaque px, %d px become transparent",
                preview.images, preview.images == 1 ? "" : "s",
                preview.opaque_pixels, preview.cleared_pixels);

    bool can_apply = preview.cleared_pixels > 0;
    if (!can_apply) ImGui::BeginDisabled();
    if (ImGui::Button("Apply", ImVec2(110, 0))) {
        std::vector<int> changed_indices;
        if (doc_undo_push()) {
            OpacityGradientStats applied =
                OpacityGradientScan(true, &changed_indices);
            int crops = 0;
            if (applied.cleared_pixels > 0 && g_opacity_gradient_trim) {
                if (g_opacity_gradient_marked)
                    crops = CropMarkedImagesToContent();
                else
                    crops = CropSelectedImageToContent();
            }
            if (applied.cleared_pixels > 0 || crops > 0) {
                mark_dirty();
                g_img_tex_idx = -2;
                for (int idx : changed_indices)
                    InvalidateThumb(idx);
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Opacity gradient cleared %d px on %d sprite%s%s.",
                         applied.cleared_pixels,
                         applied.images,
                         applied.images == 1 ? "" : "s",
                         crops > 0 ? " and trimmed bounds" : "");
                g_restore_msg_timer = 5.0f;
            }
        }
        g_show_opacity_gradient = false;
    }
    if (!can_apply) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0)))
        g_show_opacity_gradient = false;

    ImGui::EndPopup();
}

void OpenInnerStrokeDialog(void)
{
    if (g_doc && g_doc->ilselected >= 0) g_show_inner_stroke = true;
}

void DrawInnerStrokeDialog(void)
{
    if (g_show_inner_stroke) ImGui::OpenPopup("3-Tone Inner Stroke");
    if (!ImGui::BeginPopupModal("3-Tone Inner Stroke", &g_show_inner_stroke,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    IMG *img = (g_doc && g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    PAL *pal = img ? get_pal((int)img->palnum) : NULL;
    bool valid = img && img->data_p && pal && pal->data_p && pal->bitspix >= 2 &&
                 g_sel_color > 0 && g_sel_color < (1 << (pal->bitspix > 8 ? 8 : pal->bitspix));
    if (!valid) {
        ImGui::TextWrapped("Select a sprite with a 2bpp-or-higher palette and choose an opaque palette swatch for the fill.");
    } else {
        ImGui::Text("%s  %dx%d  (%dbpp)", img->n_s, img->w, img->h, pal->bitspix);
        ImGui::Text("Fill index: #%d", g_sel_color);
        ImGui::ColorEdit3("Inner stroke hue", g_inner_stroke_rgb,
                          ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoInputs);
        ImGui::TextWrapped("The silhouette becomes the selected fill index. Its first three inside pixel bands become dark, medium, and light versions of this hue.");
        if (pal->bitspix == 2)
            ImGui::TextWrapped("2bpp note: only three opaque indices exist, so the selected fill is used as the lightest third band.");
        ImGui::TextDisabled("The sprite gets its own palette copy; every opaque palette entry becomes the fill, then the three stroke entries are applied.");
    }

    ImGui::Separator();
    if (!valid) ImGui::BeginDisabled();
    if (ImGui::Button("Apply", ImVec2(110, 0))) {
        int changed = ApplySelectedInnerStroke(
            (unsigned char)(g_inner_stroke_rgb[0] * 255.0f + 0.5f),
            (unsigned char)(g_inner_stroke_rgb[1] * 255.0f + 0.5f),
            (unsigned char)(g_inner_stroke_rgb[2] * 255.0f + 0.5f));
        if (changed > 0) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Applied 3-tone inner stroke to %d pixel%s.",
                     changed, changed == 1 ? "" : "s");
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Could not create the sprite's inner-stroke palette.");
        }
        g_restore_msg_timer = 4.0f;
        g_show_inner_stroke = false;
    }
    if (!valid) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0))) g_show_inner_stroke = false;
    ImGui::EndPopup();
}

void DrawNewImgConfirm(void)
{
    if (g_show_new_img_confirm) ImGui::OpenPopup("New IMG");
    if (!ImGui::BeginPopupModal("New IMG", &g_show_new_img_confirm, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("Create a fresh IMG tab?");
    ImGui::Text("Starts with one blank palette and one 32x32 image.");
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("New", ImVec2(80, 0))) {
        PrepareDocumentForOpenedFile();
        g_doc->fileversion = 0x0634;
        g_doc->fname_s[0]  = 0;
        /* Bootstrap: a fresh doc with zero palettes/images is unusable —
           the user can't paint, can't import a TGA target, can't even
           see the editor properly. Seed one default palette and one
           blank image so the UI is immediately functional. AddNewPalette
           handles undo+dirty internally; clear those again so the new
           doc starts pristine. */
        AddNewPalette();
        AddNewBlankImage();
        g_dirty = false;
        ResetPerDocumentUiState(false);
        g_show_new_img_confirm = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        g_show_new_img_confirm = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* "Add new blank image" — small modal that prompts for width and height
   before allocating, replacing the previous fixed 32x32 path. */
void DrawNewBlankImageDialog(void)
{
    if (g_show_new_blank_dialog) ImGui::OpenPopup("Add Blank Image");
    if (!ImGui::BeginPopupModal("Add Blank Image", &g_show_new_blank_dialog,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Pick the size of the new image.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Width",  &g_new_blank_w)) {
        if (g_new_blank_w < 1)    g_new_blank_w = 1;
        if (g_new_blank_w > 1024) g_new_blank_w = 1024;
    }
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Height", &g_new_blank_h)) {
        if (g_new_blank_h < 1)    g_new_blank_h = 1;
        if (g_new_blank_h > 1024) g_new_blank_h = 1024;
    }
    ImGui::Spacing();
    ImGui::Separator();
    /* Enter commits, Esc cancels — matches the rest of the modals. */
    bool commit = ImGui::Button("Add", ImVec2(80, 0))
               || ImGui::IsKeyPressed(ImGuiKey_Enter);
    if (commit) {
        AddNewBlankImage(g_new_blank_w, g_new_blank_h);
        g_show_new_blank_dialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        g_show_new_blank_dialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* Run whatever action queued the unsaved-changes confirm. Called once the
   user has chosen Save or Discard. After running, g_pending_action is reset
   to None so the dialog never re-fires. */
static void RunPendingAction(void)
{
    PendingAction act = g_pending_action;
    std::string   path = g_pending_action_path;
    int           tab_idx = g_pending_tab_index;
    g_pending_action = PendingAction::None;
    g_pending_action_path.clear();
    g_pending_tab_index = -1;
    switch (act) {
        case PendingAction::Quit: {
            int dirty_idx = FindDirtyDocumentIndex();
            if (dirty_idx >= 0) {
                ActivateDocumentTab(dirty_idx);
                g_pending_action = PendingAction::Quit;
                g_show_unsaved_confirm = true;
            }
            break;
        }
        case PendingAction::OpenDialog:     OpenFileDialog(FileDialogMode::OpenImg); break;
        case PendingAction::OpenPath:       OpenImgFile(path); break;
        case PendingAction::OpenLodDialog:  OpenFileDialog(FileDialogMode::OpenLod); break;
        case PendingAction::CloseTab:
            if (tab_idx < 0) tab_idx = document_active_index();
            document_close_tab(tab_idx);
            ResetPerDocumentUiState(false);
            g_doc_tab_select_request = document_active_index();
            break;
        case PendingAction::None: default:  break;
    }
}

void DrawUnsavedChangesConfirm(void)

{
    /* Legacy: g_pending_quit is set by Esc/window-close; treat it as the
       Quit pending action if nothing else queued. */
    if (g_pending_quit && g_pending_action == PendingAction::None && !g_show_unsaved_confirm) {
        int dirty_idx = FindDirtyDocumentIndex();
        if (dirty_idx >= 0) {
            ActivateDocumentTab(dirty_idx);
            g_pending_action = PendingAction::Quit;
            g_show_unsaved_confirm = true;
        }
    }
    if (g_show_unsaved_confirm) ImGui::OpenPopup("Unsaved Changes");
    if (!ImGui::BeginPopupModal("Unsaved Changes", &g_show_unsaved_confirm, ImGuiWindowFlags_AlwaysAutoResize)) return;

    const char *verb =
        (g_pending_action == PendingAction::OpenDialog ||
         g_pending_action == PendingAction::OpenPath  ||
         g_pending_action == PendingAction::OpenLodDialog) ? "before opening another file"
        : (g_pending_action == PendingAction::CloseTab) ? "before closing this tab"
                                                        : "before quitting";
    ImGui::Text("You have unsaved changes.");
    ImGui::Text("Do you want to save %s?", verb);
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(80, 0))) {
        g_show_unsaved_confirm = false;
        ImGui::CloseCurrentPopup();
        if (g_doc->fname_s[0] != '\0') {
            SaveImgFile();
            g_dirty = false;
            RunPendingAction();
        } else {
            /* No filename yet — fall through to Save dialog, and discard the
               pending action since the user needs to drive that flow manually.
               (Avoids racing a fresh Save dialog against an Open dialog.) */
            g_pending_quit = false;
            g_pending_action = PendingAction::None;
            g_pending_action_path.clear();
            g_pending_tab_index = -1;
            OpenFileDialog(FileDialogMode::SaveImg);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(80, 0))) {
        g_show_unsaved_confirm = false;
        ImGui::CloseCurrentPopup();
        g_dirty = false; /* user chose to throw the edits away */
        RunPendingAction();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        g_show_unsaved_confirm = false;
        if (g_pending_action == PendingAction::Quit) g_pending_quit = false;
        g_pending_action = PendingAction::None;
        g_pending_action_path.clear();
        g_pending_tab_index = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* MK2-side unsaved-changes confirm. Fires when the user tries to quit
   with edits pending in MKSTK.ASM. Independent of the IMG dirty flow
   so the two doc types each get their own prompt. */
void DrawMk2UnsavedChangesConfirm(void)

{
    if (g_show_mk2_unsaved_confirm) ImGui::OpenPopup("MK2 Hitboxes - Unsaved");
    if (!ImGui::BeginPopupModal("MK2 Hitboxes - Unsaved", &g_show_mk2_unsaved_confirm,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("You have unsaved edits in MKSTK.ASM.");
    ImGui::Text("Do you want to save them before quitting?");
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(80, 0))) {
        std::string err;
        if (mk2::save(&g_mk2_doc, &err)) {
            g_mk2_status = "Saved MKSTK.ASM";
            g_mk2_status_sticky = false;
            g_show_mk2_unsaved_confirm = false;
            ImGui::CloseCurrentPopup();
        } else {
            g_mk2_status = std::string("Save failed: ") + err;
            g_mk2_status_sticky = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(80, 0))) {
        g_mk2_doc.dirty = false; /* user chose to throw the MK2 edits away */
        g_show_mk2_unsaved_confirm = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        g_show_mk2_unsaved_confirm = false;
        g_pending_quit = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawMk2FatalityUnsavedChangesConfirm(void)

{
    if (g_show_mk2_fatality_unsaved_confirm) ImGui::OpenPopup("MK2 Fatality Lab - Unsaved");
    if (!ImGui::BeginPopupModal("MK2 Fatality Lab - Unsaved", &g_show_mk2_fatality_unsaved_confirm,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("You have unsaved edits in MK2 fatality source files.");
    ImGui::Text("Do you want to save them before quitting?");
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(80, 0))) {
        std::string err;
        if (mk2fatal::save(&g_mk2_fatality_doc, &err)) {
            g_mk2_fatality_status = "Saved MK2 fatality source edits";
            g_mk2_fatality_status_sticky = false;
            g_show_mk2_fatality_unsaved_confirm = false;
            ImGui::CloseCurrentPopup();
        } else {
            g_mk2_fatality_status = std::string("Save failed: ") + err;
            g_mk2_fatality_status_sticky = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(80, 0))) {
        g_mk2_fatality_doc.dirty = false;
        for (auto &sf : g_mk2_fatality_doc.files) sf.dirty = false;
        g_show_mk2_fatality_unsaved_confirm = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        g_show_mk2_fatality_unsaved_confirm = false;
        g_pending_quit = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawHelpModal(void)
{
    if (g_show_help) ImGui::OpenPopup("Help");
    if (!ImGui::BeginPopupModal("Help", &g_show_help,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) return;
    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_Always);
    if (ImGui::BeginChild("##helpscroll", ImVec2(680, 420), true)) {
        ImGui::TextUnformatted(g_help_text);
        ImGui::EndChild();
    }
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0))) {
        g_show_help = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawAboutModal(void)
{
    if (g_show_about) ImGui::OpenPopup("About IMGTOOL");
    if (ImGui::BeginPopupModal("About IMGTOOL", &g_show_about, ImGuiWindowFlags_AlwaysAutoResize)) {
        /* Headline: name + version in a slightly larger font weight. */
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
        ImGui::Text("IMGTOOL  v%s", IMGTOOL_VERSION);
        ImGui::PopStyleColor();
        ImGui::Separator();

        ImGui::TextWrapped("A modern port of the 1992 Midway Image Tool. "
                           "Sprite + palette editor for the .IMG container files "
                           "shipped with Mortal Kombat, NBA Jam, NBA Hangtime, "
                           "and other Williams/Midway arcade titles of the era.");
        ImGui::Spacing();

        /* Two-column key/value table so the values line up regardless of
           the proportional-font widths of the labels. ImGui::Text uses a
           variable-width font, so space-padding inside the format string
           can't be relied on for alignment. */
        SDL_version sdlv;
        SDL_GetVersion(&sdlv);
        if (ImGui::BeginTable("##about_kv", 2,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody)) {
            auto row = [](const char *k, const char *fmt, ...) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(k);
                ImGui::TableSetColumnIndex(1);
                va_list ap; va_start(ap, fmt);
                char buf[256];
                vsnprintf(buf, sizeof(buf), fmt, ap);
                va_end(ap);
                ImGui::TextUnformatted(buf);
            };
            row("Version",    "%s",          IMGTOOL_VERSION);
            row("Built",      "%s %s",       __DATE__, __TIME__);
#ifdef IMGTOOL_GIT_REV
            row("Commit",     "%s",          IMGTOOL_GIT_REV);
#endif
            row("Dear ImGui", "%s",          IMGUI_VERSION);
            row("SDL2",       "%d.%d.%d",    sdlv.major, sdlv.minor, sdlv.patch);
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Original tool: Shawn Liptak, Williams Electronics, 1992");
        ImGui::TextDisabled("SDL/ImGui modernization & feature work: see git history");

        ImGui::Spacing();
        ImGui::TextDisabled("Portions based on agentic AI reverse engineering by");
        ImGui::SameLine();
        ImGui::TextLinkOpenURL("Asure007", "https://github.com/Asure/");
        ImGui::TextLinkOpenURL("https://github.com/junkwax/midway-imgtool");
        ImGui::SameLine();
        ImGui::TextDisabled(" (issues + releases)");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            g_show_about = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawVerboseLogWindow(void)
{
    if (!g_verbose) return;
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Verbose Logging", &g_verbose)) {
        if (ImGui::Button("Clear")) { g_log_lines.clear(); }
        ImGui::SameLine();
        if (ImGui::Button("Copy to Clipboard")) {
            std::string all_logs;
            for (const auto& s : g_log_lines) all_logs += s + "\n";
            ImGui::SetClipboardText(all_logs.c_str());
        }
        ImGui::Separator();
        ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& s : g_log_lines) {
            ImGui::TextUnformatted(s.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::End();
}

void DrawTransientToast(float dt)
{
    if (g_restore_msg_timer <= 0.0f) return;
    g_restore_msg_timer -= dt;
    ImGuiIO &io = ImGui::GetIO();
    float sw = io.DisplaySize.x;
    float sh = io.DisplaySize.y;
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowPos(ImVec2(sw * 0.5f, sh - 60), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    if (ImGui::Begin("##toast", NULL,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::TextUnformatted(g_restore_msg);
    }
    ImGui::End();
}


/* ---- Extracted g_help_text ---- */
const char *g_help_text =
    R"IMA(IMAGE TOOL HELP
================================================================================

QUICKSTART
----------
What is an IMG file?  An IMG file is a container of multiple sprites (images)
plus one or more palettes that colorize them.  A single IMG can hold hundreds
of frames -- e.g. NINJAS10.IMG probably holds every animation frame for a
ninja character.  Think of imgtool as a sprite-sheet editor.

Launch: Double-click imgtool.exe.  You'll see a mostly-black window waiting
for you to load something.

Step 1 -- Open a file:  Ctrl+O opens the file browser.
  Up/Down - scroll through files
  Enter   - open directory or load the selected file
  Backspace - go up one directory
  Esc     - cancel

Step 2 -- Read the screen:
  +----------------------------------+-------------------+
  |         MAIN IMAGE VIEW          |                   |
  |     current sprite drawn here    |   PROPERTIES      |
  |                                  |   (anim points,   |
  |                                  |    hitbox sliders)|
  +-----------------+----------------+-------------------+
  |  PALETTE LIST   |        IMAGE LIST                  |
  |  (pal names)    |   (all sprites in this IMG)        |
  +-----------------+------------------------------------+

  - Main view (center) -- the currently-selected sprite
  - Image list (right) -- every sprite in the file. "*" = marked
  - Palette list (top-right panel) -- palettes in this IMG
  - Bottom palette bar -- 256 color swatches for current palette

Step 3 -- Browse: Up/Down moves one sprite. PgUp/PgDn jumps a page.

Step 4 -- Mark sprites: Space toggles mark. M marks all, m clears all.
  Marking is how you select sprites for batch operations.

Step 5 -- Zoom: toolbar Z+/Z-, Ctrl+= / Ctrl+-, or Ctrl+mouse wheel zooms.
  Ctrl+0 fits the sprite. Mouse wheel scrolls vertically. Middle-mouse drag
  pans. Space + left drag also pans (Photoshop hand-tool style).

Step 6 -- Palettes: click a palette in the Palette list to set it on the
  active sprite. [ sets palette for marked sprites, ] for the current one
  (when no paint tool is active - Pencil reassigns [ and ] to brush size).

Step 7 -- Two IMGs at once: Tab swaps between list 1 and list 2. Open a
  second IMG after pressing Tab, then swap back with Tab.

Step 8 -- Save: Ctrl+S saves. Pre-2.x IMGs are auto-converted on load.


================================================================================
KEYBOARD REFERENCE
------------------

File:
  Ctrl+O               Open IMG
  Ctrl+S               Save IMG
  Alt+L  / Alt+S       Load / Save LBM
  Ctrl+L               Load TGA
  Ctrl+B               Build TGA from marked images
  Esc                  Quit (prompts on unsaved changes)

Edit:
  Ctrl+Z               Undo (paint stroke, anipoint, hitbox, palette ops)
  Ctrl+Y               Redo
  Ctrl+C / Ctrl+X / Ctrl+V   Copy / Cut / Paste
  Ctrl+Shift+X         Cut selection to a new sprite
  Ctrl+Shift+V         Paste clipboard as a new sprite
  Ctrl+A               Select all
  Ctrl+D               Deselect
  Ctrl+Shift+I         Invert selection
  Ctrl+J               Duplicate image (or duplicate floating paste)
  Ctrl+E               Merge Down: commit floating paste in place
  Ctrl+T               Free Transform floating paste (scale / rotate)
  H / V                Flip floating paste horizontally / vertically
  L                    Drop floating paste to a non-destructive sprite layer

Image list:
  Space                Mark / Unmark current image
  Shift+M              Set all marks (typed as "M")
  M                    Clear all marks (typed as "m")
  Del                  Delete image when image list is active
  Shift+Del            Delete image from anywhere
  Ctrl+R               Rename current image
  Ctrl+P               Add / Remove point table on current image
  Alt+PgUp / PgDn      Move current image up / down in the list
  Tab                  Swap image lists (lists 1 and 2)
  ;                    Least-squares size reduce on marked

Tools (toolbar shortcuts):
  P                    Pencil   - paint at current color
  G                    Paint Bucket - fill contiguous area
  V                    Variant Paint - target-palette-only color over opaque pixels
  R                    Marquee  - rectangular selection
  W                    Magic Wand (Ctrl-click adds)
  L                    Lasso
  I                    Eyedropper - pick color from canvas
  (no shortcut)        Smart Eraser, Clone Stamp, Smart Remap (toolbar buttons)

Brush-specific (only fire while Pencil or Variant Paint is active):
  [ / ]                Shrink / grow brush radius (1..16)

Palette (only fire when no paint tool is active):
  [                    Set palette for marked images
  ]                    Set palette for current image
  Shift+8 (`*`)        Merge marked palettes
  Shift+R              Rename selected palette
  Del                  Delete selected palette when palette list is active

Timeline / Anim:
  K                    Toggle timeline play / stop
  Left / Right         Step prev / next animation frame
  Ctrl+Left / Right    Move current timeline frame earlier / later
  Hold                 Set extra base ticks before the current frame advances
  Ctrl-click frames    Pair two frames; Play/Left/Right advances both
  Drag paired sprite   Move sprite by editing its anipoint; lock Back/Front to protect it
  Auto Anipts          With a paired frame locked, align the sequence by sprite sizes
  World Marked         Play marked animations from up to ten IMG rows together
  World Sequence       On-canvas Pause/Refresh plus per-frame delay thumbnails
  Split Row            Move the selected World Sequence entry onward to a new row
  Auto Y               Bulk Show@/Hide@ plus per-entry vX/vY until a Y line is crossed
  Eye / Borders        Hide individual World rows, or all sprite bounds
  Bounds               Green 400x254 safe, yellow X-clipped, red vertical/bad anchor
  Dummy Body           Adds stock *DECAP1-7 body fall as an editable sync lane
  World Left / Right   Pause and scrub all marked-row sequences together

View / Help:
  Ctrl+= / Ctrl+-      Zoom in / out
  Ctrl+0               Fit sprite to canvas
  H                    Show this help
  F9                   Debug info popup

Mouse on canvas:
  Mouse wheel          Scroll vertically
  Ctrl + mouse wheel   Zoom in / out from canvas center
  Middle drag          Pan
  Space + left drag    Pan (Photoshop hand-tool style)
  Right-click          Eyedrop (any tool mode)
  Blank left-drag      Starts marquee selection from transparent pixels
  Shift + left-click   Flood fill (when no select tool active)


================================================================================
MK2 FRAME ORIGIN, ANIPOINTS, AND FRAME CONNECTIONS
--------------------------------------------------
Frame origin:
  The origin of an IMG frame is local pixel coordinate (0,0), the top-left
  corner of its rectangular pixel buffer. Transparent padding is part of that
  rectangle. X increases right and Y increases down. The origin is not the
  top-left opaque pixel unless the artwork happens to touch both edges.

Primary animation point (AX/AY):
  AX/AY is a signed offset measured from the frame origin. The white crosshair
  is drawn at local coordinate (AX,AY), so it may appear outside the rectangle.
  For example AY=-14 places the anchor 14 pixels above the frame origin.

  When the game/editor places a frame at world anchor (WX,WY):
      frame top-left = (WX - AX, WY - AY)
      local pixel (x,y) appears at (WX + x - AX, WY + y - AY)

  Consecutive frames look stable when AX/AY identifies the same physical
  feature in each drawing (feet, body center, hand, weapon pivot, etc.). Edit
  X1/Y1 in Sprite > Anipts Tools, drag the white crosshair, use World View, or
  use Operations > Set Marked Anipoints to X/Y / Align Marked Anipoints.
  The group X/Y dialog can change either axis independently. Its first marked
  frame is the reference; Left/Top preserves the entered coordinate, Center
  adds half each frame-size difference, and Right/Bottom adds the full size
  difference so the anchor keeps the same distance from that edge.

Secondary animation point (AX2/AY2/AZ2):
  This is another anchor inside the SAME frame; it does not link one animation
  frame to the next. An active point is cyan with a yellow line to the primary.
  -1/-1/-1 (stored as FFFF/FFFF/FFFF) means unused. Use Clear 2nd Point to
  restore that sentinel; 0/0/0 is an active point at the frame origin.

Connecting frames -- two different jobs:
  1. Visual alignment: put matching physical features at a common world anchor.
     Mark the frames and set/align their AX/AY, or enable World View and step
     Left/Right. Onion skin and Ctrl-click paired timeline frames help compare
     adjacent poses; dragging a paired sprite adjusts its anipoint.
  2. Playback/runtime order: anipoints do not create a frame-to-frame link.
     Embedded IMG relationships live in the SEQSCR/ENTRY blob. A Sequence is
     an ordered list of sprite indices; each ENTRY also stores Ticks, dX/dY and
     three preserved spare words. A Script is a higher-level ordered list that
     calls Sequences. External MK2 game ASM may also define runtime animation.

Editing embedded SEQSCR data now:
  - Open the Animation tab. Expand Sequences or Scripts.
  - Select a record and click Edit..., or use View/Edit Anim Data.
  - New Sequence creates an empty frame list. Select a sprite in the normal
    image list, then use Add Selected Sprite to append it as the next frame.
  - Enable in-place editing to change the record name/flags/start position and
    each ENTRY's Index, Ticks, dX/dY and spare words. The x button removes an
    entry. New Script creates a record whose entries call sequence indices.
  - Load a Sequence/Script in World View to preview it with its saved timing
    and offsets. Save the IMG to persist edits to the SEQSCR/ENTRY blob.

  Add Marked Frames appends the marked sprite group in image-list order. The
  ^/v controls reorder entries. This remains a fairly low-level editor: there
  is not yet a thumbnail/drag sequence builder or visual sprite-name picker.
  The bottom Animation Timeline is convenient for preview and temporary order,
  but its order is not automatically written into SEQSCR. Merely changing
  AX/AY also does not alter SEQSCR animation order.


================================================================================
MK2 STRIKE-TABLE EDITOR
-----------------------
Tools - MK2 Hitboxes (MKSTK.ASM)... opens an editor for the strike-table
source file used by the MK2 source tree. Imgtool parses MKSTK.ASM directly
and writes back in place, preserving comments, indentation, and symbolic
literals (e.g. sf_squeeze).

Workflow:
  Browse... - native file picker, remembers last directory
  Load      - parse the .asm into memory
  Save      - write the in-memory edits back to disk
  Reload    - re-read from disk, dropping unsaved edits
  Undo / Redo (and Ctrl+Z / Ctrl+Shift+Z when the panel has focus)

3-pane navigator:
  Left  - character codes (jc, lk, hh, nj, etc. - the two-letter labels
          MKSTK.ASM itself uses). The panel auto-jumps to the matching
          character when you open a sprite IMG with a recognized prefix
          (CAGE -> jc, KANG -> lk, HATHED -> hh, NINJAS -> nj, etc.).
  Mid   - moves for the selected character. Filter box at the top.
  Right - fields for the selected move: x/y/w/h, strike_routine (raw
          token), damage split into hit/block bytes, score (32-bit),
          sound (raw token).

Canvas overlay:
  The selected move's collision box draws on the active sprite as a
  magenta rectangle with drag-to-resize corner handles. The IMG-embedded
  hitbox overlay auto-hides while an MK2 move is selected so the two
  systems don't pile on top of each other.


================================================================================
FILE FORMATS
------------

IMG (Image Library) -- Primary format. Binary, little-endian, packed structs.
  LIB_HDR (28 bytes): g_doc->imgcnt, g_doc->palcnt, version (0x634+)
  IMAGE records (50 bytes each): name[16], flags, anix/y, w, h, palnum
  PALETTE records (26 bytes each): name[10], flags, bitspix, numc
  BLOB: raw pixel data (stride = (w+3)&~3) + 15-bit packed RGB palettes

TGA (TrueVision Targa) -- 8-bit color-mapped, bottom-up by default.
  Loads as new image+palette. Saves current image with its palette.

LBM (IFF/ILBM) -- Chunk-based format. CMAP (palette) + BODY (bitmap).
  Supports RLE-compressed body chunks.

PNG -- Import/Export via stb_image. Auto-quantizes colors to nearest
  15-bit palette on import. Exports RGBA with transparency for color 0.

Pre-2.x IMG files (version < 0x500) are auto-converted on open.
Max sprite size: 640x400. Max 2000 images/palettes per file.


================================================================================
DMA2 HARDWARE REFERENCE
-----------------------
The Williams DMA #2 (January 1992, Rev 1.5) handles pixel transfers between
image memory and the bitmap. This is the hardware that MK/NBA-era games used
to blit sprites to screen.  The following is the original document text:

)IMA"
R"dma2(
		THE BRAND SPANKING NEW DMA (#2)

			KEEP ENTERPRISES, INC.

			JANUARY 1, 1992

			DOCUMENT REV. 1.5


	DMA # 2 - GENERAL INFORMATION

	- THE NEW DMA WILL INCORPORATE BACKWARD COMPATIBILITY TO THE OLD DMA
	IN BOTH PINOUT AND FUNCTIONALITY.

	- THE NEW FEATURES IN ADDITION TO THE OLD ARE AS FOLLOWS:

		1) VARIABLE PIXEL SIZE PROCESSING.  THE NEW DMA CAN PROCESS
		   1 TO 8 BIT PIXELS THAT ARE STORED IN IMAGE MEMORY IN A
		   SERIAL FASHION.

		   EXAMPLE:  5 BIT PIXELS STORED INTO 8 BIT EPROM
		   +---+---+---+---+---+---+---+---+
		   |P1 |P1 |P1 |P1 |P1 |P2 |P2 |P2 |
		   +---+---+---+---+---+---+---+---+
		   |P2 |P2 |P3 |P3 |P3 |P3 |P3 |P4 |
		   +---+---+---+---+---+---+---+---+
		   |P4 |P4 |P4 |P4 |P5 |P5 |P5 |P5 |
		   +---+---+---+---+---+---+---+---+
		   |P5 |P6 |P6 |P6 |P6 |P6 |P7 |P7 |
		   +---+---+---+---+---+---+---+---+

		2) THE NEW DMA CAN BE HALTED IN THE MIDDLE OF A TRANSFER
		   AND THEN BE RESTARTED TO RESUME THE TRANSFER.  THIS IS
		   ACCOMPLISHED BY WRITING A ZERO TO THE DMA GO BIT (BIT 15)
		   IN THE CONTROL REGISTER.  IN THE OLD DMA, THIS WOULD KILL
		   THE TRANSFER SO THAT IT COULD NOT BE RESTARTED.  TO KILL
		   A TRANSFER IN THE NEW DMA, WRITE A ZERO TO THE DMA GO BIT 2
		   TIMES IN A ROW.  TO RESTART A TRANSFER AFTER HALTING,
		   WRITE A ONE TO THE DMA GO BIT IN THE CONTROL REGISTER.

		3) IN ADDITION TO THE CLIPPING ACHIEVED BY MANIPULATING THE
		   OFFSET REGISTER, IN THE NEW DMA, A METHOD OF CLIPPING
		   USING REGISTERED CLIP VALUES IS AVAILABLE. THE HOST CAN
		   SPECIFY CLIP AMOUNTS TO THE DMA AND THE MATH NEEDED TO
		   IMPLEMENT A TRANSFER IS DONE INTERNAL TO THE DMA.

		4) THE NEW DMA CAN DO A TRANSFER FROM THE IMAGE MEMORY TO THE
		   BIT MAP WITH A SCALING EFFECT, I.E. THE IMAGE CAN BE
		   SHRUNK OR ENLARGED.

		5) THE NEW DMA IMPLEMENTS A COMPRESSION MODE IN WHICH LEADING
		   AND TRAILING ZERO DATA PIXELS CAN BE ENCODED IN A RUN LENGTH
		   FASHION TO SAVE ON IMAGE MEMORY.

		6) OFF SCREEN CLIPPING CAN BE AUTOMATIC.  THERE ARE FOUR
		   REGISTERS THAT SPECIFY THE WINDOW TO WHICH THE DMA CAN
		   TRANSFER DATA.

		7) NOTE THAT THE CONTROL REGISTER AND THE OFFSET REGISTER
		   HAVE BEEN SWAPPED SO THAT THE MOVE MULTIPLE INSTRUCTION
		   CAN BE USED TO DOWNLOAD THE REGISTERS AND SET DMA GO
		   EFFICIENTLY.

	DMA # 2 - INTERNAL REGISTERS R5-R0

	    REGISTER # 7 - SOURCE VERTICAL SIZE REGISTER
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    | 0 | 0 | 0 | 0 | 0 | 0 |           VERTICAL SIZE              |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 6 - SOURCE HORIZONTAL SIZE REGISTER
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    | 0 | 0 | 0 | 0 | 0 | 0 |          HORIZONTAL SIZE             |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 5 - DESTINATION ADDRESS - Y
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    | 0 | 0 | 0 | 0 | 0 | 0 | 0 |      DESTINATION Y COORDINATE     |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 4 - DESTINATION ADDRESS - X
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    | 0 | 0 | 0 | 0 | 0 | 0 | 0 |      DESTINATION X COORDINATE     |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 3 - SOURCE ADDRESS - HIGH ORDER
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    |               SOURCE ADDRESS UPPER 16 BITS                   |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 2 - SOURCE ADDRESS - LOW ORDER
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    |               SOURCE ADDRESS LOWER 16 BITS                   |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 1 - CONTROL REGISTER   ** SEE NOTE 1 BELOW
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    |DGO|  PIX SIZE |TM1|TM0|LM1|LM0|CMP|CLP|VFL|HFL|   PIXEL OPS   |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 0 - OFFSET REGISTER / RCLIP-LCLIP VALUES
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    |            OFFSET VALUE FOR OLD STYLE CLIPPING               |
	    |   LEFT CLIP PIXELS VALUE     |    RIGHT CLIP PIXELS VALUE    |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    ** NOTE 1:
	    DGO      - BIT 15     - DMA GO / DMA HALT
	    PIX      - BITS 14-12 - PIXEL SIZE (0 = 8 BITS)
	    TM1      - BIT 11     - DMA COMPRESS TRAIL PIX MULT BIT 1
	    TM2      - BIT 10     - DMA COMPRESS TRAIL PIX MULT BIT 0
	    LM1      - BIT 9      - DMA COMPRESS LEAD  PIX MULT BIT 1
	    LM0      - BIT 8      - DMA COMPRESS LEAD  PIX MULT BIT 0
	    CMP      - BIT 7      - DMA COMPRESS MODE
	    CLP      - BIT 6      - DMA CLIP ON = 1 (USING U,D,L,R METHOD)
	    VFL      - BIT 5      - VERTICAL FLIP (FLIP ABOUT X AXIS)
	    HFL      - BIT 4      - HORIZONTAL FLIP (FLIP ABOUT Y AXIS)
	    OPS      - BITS 3-0   - PIXEL CONSTANT/SUBSTITUTION OPS

	    ** NOTE 2: IN COMPRESSION MODE, SCALING IS INHIBITED AND
	               CLIPPING IS INHIBITED.


	DMA # 2 - CLIPPING AN IMAGE

	OVERVIEW: AN IMAGE CAN BE TRANSFERRED TO THE BIT MAP IN ITS
	ENTIRETY OR A PORTION CAN BE "CLIPPED" BY SPECIFYING LEFT AND
	RIGHT CLIP AMOUNTS IN REGISTER 0, WITH BIT 6 (CLP) SET.

	IMPLEMENTATION:
	        - THE OFFSET METHOD (OLD STYLE): REGISTER 0 UPPER BYTE =
	          LEFT CLIP, LOWER BYTE = RIGHT CLIP.
	        - THE REGISTER CLIP METHOD (NEW FEATURE): SET CLP BIT TO 1,
	          USE REGISTERS 12 AND 13 FOR WINDOW BORDERS.


	DMA # 2 - TRANSFERRING A SCALED IMAGE

	OVERVIEW: AN IMAGE CAN BE SCALED BY TRAVERSING EACH LINE WITH A
	PREDETERMINED SAMPLE RATE.  RATIO IS 1:(INT + FRAC/256).
	REGISTER 11 = Y SCALE, REGISTER 10 = X SCALE.
	SCALE FACTORS: UPPER BYTE = INTEGER, LOWER BYTE = FRACTION.

	MAXIMUM SCALE FACTOR FOR SHRINK IN X DIRECTION:
	#BITS/PIXEL    INT  FRACTION
	-----------    ---- --------
	     1          1F    FF
	     2          10    00
	     3          0A    AA
	     4          08    00
	     5          06    66
	     6          05    55
	     7          04    92
	     8          04    00


	DMA # 2 - COMPRESSION OF LEADING AND TRAILING ZEROS

	TO SAVE IMAGE SPACE, LEADING AND TRAILING ZERO PIXELS ARE RUN-
	LENGTH ENCODED. THE FIRST BYTE OF EACH COMPRESSED LINE CONTAINS:
	UPPER NIBBLE = TRAILING ZERO COUNT, LOWER NIBBLE = LEADING ZERO
	COUNT. TMx/LMx BITS IN CONTROL REGISTER MULTIPLY THESE VALUES BY
	1, 2, 4, OR 8.

	IN COMPRESSION MODE, THE DMA DECODES THIS ON THE FLY IF:
	  CMP (BIT 7) = 1, TMx, LMx BITS = 0.


	DMA # 2 - OFF SCREEN CLIPPING (WINDOWING)

	FOUR REGISTERS SPECIFY WINDOW BOUNDARIES (0-511).  SET CONFIG
	REGISTER BIT 5 = 0 FOR LEFT/RIGHT, = 1 FOR UPPER/LOWER.
	REGISTERS 12/13 HOLD LEFT/TOP AND RIGHT/BOTTOM LIMITS.


	DMA # 2 - LIMITATIONS

	SCALING: THERE ARE SIZE LIMITS. SEE TABLE ABOVE FOR SHRINK/GROW
	MAX/MIN IN X DIRECTION. 1-BIT PIXELS CAN SHRINK TO 1F.FF (1:31.996).
	COMPRESSION: CLIPPING AND SCALING ARE DISABLED IN COMPRESS MODE.
)dma2"
R"IMA(
================================================================================
ABOUT
-----
midway-imgtool -- Editor for Midway arcade IMG container files (MK2/MK3,
NBA Jam, NBA Hangtime, etc.).  Originally a 1992 DOS tool by Shawn Liptak
(Williams Electronics), now a pure C/C++ + SDL2 + Dear ImGui port.
SDL-main branch -- 64-bit build.  github.com/junkwax/midway-imgtool
)IMA";


/* =========================================================
   Sprite Resizing Dialog Modals
   ========================================================= */

void OpenResizeSpriteDialog(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;
    g_resize_source_idx = g_doc->ilselected;
    g_resize_source_w = img->w;
    g_resize_source_h = img->h;
    g_resize_w = img->w;
    g_resize_h = img->h;
    g_resize_scale_x = 100;
    g_resize_scale_y = 100;
    g_show_resize_sprite = true;
}

void OpenBulkResizeDialog(void)

{
    g_bulk_resize_scale_x = 100;
    g_bulk_resize_scale_y = 100;
    g_bulk_resize_lock_aspect = true;
    g_bulk_resize_mode = (int)SpriteResizeMode::IndexNearest;
    g_bulk_resize_trim_bounds = false;
    g_show_bulk_resize = true;
}

void DrawResizeSpriteDialog(void)
{
    if (g_show_resize_sprite) ImGui::OpenPopup("Resize Sprite");
    if (!ImGui::BeginPopupModal("Resize Sprite", &g_show_resize_sprite,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    IMG *img = (g_resize_source_idx >= 0) ? get_img(g_resize_source_idx) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) {
        ImGui::TextDisabled("No sprite selected");
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            g_show_resize_sprite = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    if (g_resize_source_w <= 0 || g_resize_source_h <= 0 ||
        g_resize_source_idx != g_doc->ilselected) {
        g_resize_source_idx = g_doc->ilselected;
        g_resize_source_w = img->w;
        g_resize_source_h = img->h;
        g_resize_w = img->w;
        g_resize_h = img->h;
        g_resize_scale_x = g_resize_scale_y = 100;
    }

    ImGui::Text("%s  %dx%d", img->n_s, g_resize_source_w, g_resize_source_h);
    ImGui::Separator();

    ImGui::Checkbox("Constrain Aspect Ratio", &g_resize_lock_aspect);
    ImGui::SetNextItemWidth(110);
    int w = g_resize_w;
    if (ImGui::InputInt("Width", &w, 1, 16)) {
        g_resize_w = clamp_int(w, 1, 4096);
        if (g_resize_lock_aspect && g_resize_source_w > 0)
            g_resize_h = clamp_int(round_to_int((double)g_resize_w * (double)g_resize_source_h / (double)g_resize_source_w), 1, 4096);
        resize_sync_scale_from_dims();
    }
    ImGui::SetNextItemWidth(110);
    int h = g_resize_h;
    if (ImGui::InputInt("Height", &h, 1, 16)) {
        g_resize_h = clamp_int(h, 1, 4096);
        if (g_resize_lock_aspect && g_resize_source_h > 0)
            g_resize_w = clamp_int(round_to_int((double)g_resize_h * (double)g_resize_source_w / (double)g_resize_source_h), 1, 4096);
        resize_sync_scale_from_dims();
    }

    ImGui::SetNextItemWidth(110);
    if (g_resize_lock_aspect) {
        int pct = g_resize_scale_x;
        if (ImGui::InputInt("Scale %", &pct, 1, 10)) {
            g_resize_scale_x = g_resize_scale_y = clamp_int(pct, 1, 3200);
            resize_sync_dims_from_scale();
        }
    } else {
        int sx = g_resize_scale_x;
        if (ImGui::InputInt("Scale X %", &sx, 1, 10)) {
            g_resize_scale_x = clamp_int(sx, 1, 3200);
            resize_sync_dims_from_scale();
        }
        ImGui::SetNextItemWidth(110);
        int sy = g_resize_scale_y;
        if (ImGui::InputInt("Scale Y %", &sy, 1, 10)) {
            g_resize_scale_y = clamp_int(sy, 1, 3200);
            resize_sync_dims_from_scale();
        }
    }

    const char *mode_names[] = {
        "Lossless Palette IDs",
        "Max Quality",
        "Quality + Smallest Bytes"
    };
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("Mode", &g_resize_mode, mode_names, 3);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Lossless Palette IDs keeps existing indices exact with nearest-neighbor.\n"
                          "Max Quality resamples RGB from the active palette and remaps.\n"
                          "Quality + Smallest Bytes also trims transparent bounds.");
    }

    bool force_trim = (g_resize_mode == (int)SpriteResizeMode::QualitySmallBytes);
    bool trim_box = force_trim ? true : g_resize_trim_bounds;
    if (force_trim) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Trim Transparent Bounds", &trim_box) && !force_trim)
        g_resize_trim_bounds = trim_box;
    if (force_trim) ImGui::EndDisabled();

    ImGui::Spacing();
    int old_bytes = ((g_resize_source_w + 3) & ~3) * g_resize_source_h;
    int new_bytes = ((g_resize_w + 3) & ~3) * g_resize_h;
    ImGui::TextDisabled("IMG data: %d B -> %d B", old_bytes, new_bytes);

    bool same_size = (g_resize_w == g_resize_source_w && g_resize_h == g_resize_source_h);
    bool can_apply = !same_size || g_resize_trim_bounds || force_trim;
    ImGui::BeginDisabled(!can_apply);
    if (ImGui::Button("Resize", ImVec2(100, 0))) {
        SpriteResizeMode mode = (SpriteResizeMode)clamp_int(g_resize_mode, 0, 2);
        if (ResizeSelectedSprite(g_resize_w, g_resize_h, mode, g_resize_trim_bounds)) {
            g_show_resize_sprite = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_resize_sprite = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawBulkResizeDialog(void)
{
    if (g_show_bulk_resize) ImGui::OpenPopup("Bulk Resize Marked");
    if (!ImGui::BeginPopupModal("Bulk Resize Marked", &g_show_bulk_resize,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    int marked = CountMarkedImages();
    if (marked <= 0) {
        ImGui::TextDisabled("No marked sprites");
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            g_show_bulk_resize = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    ImGui::Text("%d marked sprite%s", marked, marked == 1 ? "" : "s");
    ImGui::Separator();

    ImGui::Checkbox("Constrain Aspect Ratio", &g_bulk_resize_lock_aspect);
    ImGui::SetNextItemWidth(110);
    if (g_bulk_resize_lock_aspect) {
        int pct = g_bulk_resize_scale_x;
        if (ImGui::InputInt("Scale %", &pct, 1, 10)) {
            g_bulk_resize_scale_x = g_bulk_resize_scale_y = clamp_int(pct, 1, 3200);
        }
    } else {
        int sx = g_bulk_resize_scale_x;
        if (ImGui::InputInt("Scale X %", &sx, 1, 10))
            g_bulk_resize_scale_x = clamp_int(sx, 1, 3200);
        ImGui::SetNextItemWidth(110);
        int sy = g_bulk_resize_scale_y;
        if (ImGui::InputInt("Scale Y %", &sy, 1, 10))
            g_bulk_resize_scale_y = clamp_int(sy, 1, 3200);
    }

    const char *mode_names[] = {
        "Lossless Palette IDs",
        "Max Quality",
        "Quality + Smallest Bytes"
    };
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("Mode", &g_bulk_resize_mode, mode_names, 3);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Lossless Palette IDs keeps existing indices exact with nearest-neighbor.\n"
                          "Max Quality resamples RGB from each sprite palette and remaps.\n"
                          "Quality + Smallest Bytes also trims transparent bounds.");
    }

    bool force_trim = (g_bulk_resize_mode == (int)SpriteResizeMode::QualitySmallBytes);
    bool trim_box = force_trim ? true : g_bulk_resize_trim_bounds;
    if (force_trim) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Trim Transparent Bounds", &trim_box) && !force_trim)
        g_bulk_resize_trim_bounds = trim_box;
    if (force_trim) ImGui::EndDisabled();

    int preview_changed = 0;
    long long old_bytes = 0;
    long long new_bytes = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if (!(img->flags & 1) || !img->data_p || img->w == 0 || img->h == 0)
            continue;
        int nw = clamp_int(round_to_int((double)img->w * (double)g_bulk_resize_scale_x / 100.0), 1, 4096);
        int nh = clamp_int(round_to_int((double)img->h * (double)g_bulk_resize_scale_y / 100.0), 1, 4096);
        old_bytes += (long long)(((int)img->w + 3) & ~3) * (long long)img->h;
        new_bytes += (long long)((nw + 3) & ~3) * (long long)nh;
        if (nw != (int)img->w || nh != (int)img->h || g_bulk_resize_trim_bounds || force_trim)
            preview_changed++;
    }
    ImGui::TextDisabled("IMG data before trim: %lld B -> %lld B", old_bytes, new_bytes);

    ImGui::Spacing();
    ImGui::BeginDisabled(preview_changed <= 0);
    if (ImGui::Button("Resize Marked", ImVec2(120, 0))) {
        SpriteResizeMode mode = (SpriteResizeMode)clamp_int(g_bulk_resize_mode, 0, 2);
        int n = BulkResizeMarkedSprites(g_bulk_resize_scale_x,
                                        g_bulk_resize_scale_y,
                                        mode,
                                        g_bulk_resize_trim_bounds);
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 n > 0 ? "Bulk resized %d marked sprite%s."
                       : "No marked sprites resized.",
                 n, n == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
        if (n > 0) {
            g_show_bulk_resize = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_bulk_resize = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

