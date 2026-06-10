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
static WorldMarkedSequenceState &g_world_marked_state = WorldMarkedState();

/* ---- ImGui Native File Dialog ---- */
bool g_show_file_dialog = false;
static FileDialogMode g_file_dialog_mode = FileDialogMode::OpenImg;
static char g_file_dialog_dir[1024] = "";
static char g_file_dialog_file[256] = "";
static std::vector<std::string> g_file_dialog_multi_files;
static std::string g_file_dialog_anchor_file;
static char g_lod_override_dir[1024] = "";

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
static int  g_sheet_bg_threshold  = 245;
static int  g_sheet_min_pixels    = 160;
static int  g_sheet_padding       = 2;
static bool g_sheet_crop          = true;
static char g_sheet_prefix[12]    = "FRAME";

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
        case FileDialogMode::ExportPng:       return "png";
        case FileDialogMode::ImportGif:       return "gif";
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

static void file_preview_clear(void)
{
    if (g_file_preview.tex) {
        SDL_DestroyTexture(g_file_preview.tex);
        g_file_preview.tex = NULL;
    }
    g_file_preview.w = g_file_preview.h = 0;
    g_file_preview.path.clear();
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
        case FileDialogMode::ExportPng: return "PNG";
        case FileDialogMode::ImportSpriteSheetMatch: return "";
        case FileDialogMode::ImportGif: return "GIF";
        case FileDialogMode::ExportPalette: return g_palette_export_act ? "ACT" : "PAL";
        case FileDialogMode::ImportPalette: return "PAL";
        case FileDialogMode::WriteAniLst: return "ASM";
        case FileDialogMode::WriteTbl:  return "TBL";
        case FileDialogMode::WriteIrw:  return "IRW";
        case FileDialogMode::LoadAsmAnim:
        case FileDialogMode::SaveAsmAnim: return "ASM";
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
                      mode == FileDialogMode::ExportPng || mode == FileDialogMode::ExportPalette ||
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
    } else if (g_doc->fname_s[0] != '\0') {
        size_t n = 0;
        while (n < 12 && g_doc->fname_s[n] != '\0') n++;
        memcpy(g_file_dialog_file, g_doc->fname_s, n);
        g_file_dialog_file[n] = '\0';
    } else {
        g_file_dialog_file[0] = '\0';
    }
    FileDialogClearMultiSelection();
    if (FileDialogSupportsMultiSelect(mode) && g_file_dialog_file[0]) {
        g_file_dialog_multi_files.push_back(g_file_dialog_file);
        g_file_dialog_anchor_file = g_file_dialog_file;
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
        ImportPng(p.c_str());
        mark_dirty();
        g_img_tex_idx = -2;
    } else if (ext == "gif") {
        ensure_new_doc_if_empty();
        ImportGif(p.c_str(), g_gif_blend_mode, g_gif_opacity_percent, g_gif_import_all);
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
    else if (g_file_dialog_mode == FileDialogMode::ExportPalette) title = "Export Palette";
    else if (g_file_dialog_mode == FileDialogMode::ImportPalette) title = "Import Palette";
    else if (g_file_dialog_mode == FileDialogMode::WriteAniLst) title = "Write ANILST";
    else if (g_file_dialog_mode == FileDialogMode::WriteTbl) title = "Write TBL";
    else if (g_file_dialog_mode == FileDialogMode::WriteIrw) title = "Write IRW";
    else if (g_file_dialog_mode == FileDialogMode::LoadAsmAnim) title = "Load Character ASM";
    else if (g_file_dialog_mode == FileDialogMode::SaveAsmAnim) title = "Save World View ASM";

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
                unsigned int before_count = g_doc->imgcnt;
                for (const std::string &file : selected_files) {
                    std::string path = PathCombine(g_file_dialog_dir, file);
                    ImportPng(path.c_str());
                }
                if (selected_files.size() > 1) {
                    unsigned int added = g_doc->imgcnt - before_count;
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             added ? "Imported %u image(s) from %d PNG file(s)." : "No PNG images imported.",
                             added, (int)selected_files.size());
                    g_restore_msg_timer = 4.0f;
                }
                mark_dirty();
            } else if (g_file_dialog_mode == FileDialogMode::ImportPngMatch) {
                unsigned int before_count = g_doc->imgcnt;
                for (const std::string &file : selected_files) {
                    std::string path = PathCombine(g_file_dialog_dir, file);
                    ImportPngMatch(path.c_str());
                }
                if (selected_files.size() > 1) {
                    unsigned int added = g_doc->imgcnt - before_count;
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             added ? "Imported %u image(s) from %d PNG file(s)." : "No PNG images imported.",
                             added, (int)selected_files.size());
                    g_restore_msg_timer = 4.0f;
                }
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
                    ImportGif(path.c_str(), g_gif_blend_mode, g_gif_opacity_percent, g_gif_import_all);
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
            g_show_file_dialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            file_preview_clear();
            g_show_file_dialog = false;
            ImGui::CloseCurrentPopup();
        }
        if (FileDialogSupportsMultiSelect(g_file_dialog_mode) && g_file_dialog_multi_files.size() > 1)
            ImGui::TextDisabled("%d files selected", (int)g_file_dialog_multi_files.size());
        ImGui::EndPopup();
    }
}

/* Help modal */
