/*****************************************************************************
 * platform/main.cpp
 *
 * Program entry point: SDL2 init, window/renderer creation, and the event
 * loop. Everything else (UI, file I/O, palette/sprite editing) is driven by
 * the ImGui overlay module.
 *
 * Lineage: This is the modernized C++ descendant of the original 1992 DOS
 * IT.EXE entry point by Shawn Liptak / Williams Electronics. The DOS asm
 * entry, Watcom DOS/4GW wrapper, and gadget-based UI are gone — only the
 * shape of "init SDL, run loop, shut down" survives.
 *****************************************************************************/

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "compat.h"
#include <SDL.h>
#include "shim_file.h"
#include "imgui_overlay.h"
#include "document.h"
#include "img_io.h"
#include "img_format.h"
#include "load2_verify.h"
#include "lod_parser.h"
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#else
#  include <unistd.h>
#  ifdef __APPLE__
#    include <mach-o/dyld.h>
#  endif
#endif

static void headless_mark_all() {
    IMG* p = (IMG*)g_doc->img_p;
    while (p) {
        p->flags |= 1; // Mark image
        p = (IMG*)p->nxt_p;
    }
}

static void headless_load_img(const char* filepath) {
    std::string path(filepath);
    size_t sep = path.find_last_of("\\/");
    std::string dir = (sep == std::string::npos) ? std::string(".") : path.substr(0, sep);
    std::string file = (sep == std::string::npos) ? path : path.substr(sep + 1);

    size_t n_dir = dir.size();
    if (n_dir > 63) n_dir = 63;
    memset(g_doc->fpath_s, 0, 64);
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
#ifdef _WIN32
    _chdir(dir.c_str());
#else
    chdir(dir.c_str());
#endif

    LoadImgFile();
}

static int run_headless_cli(int argc, char *argv[]) {
    if (argc < 3) {
        std::printf("Error: Missing arguments for headless command.\n");
        return 1;
    }
    const char* cmd = argv[1];
    document_init();

    if (std::strcmp(cmd, "--verify-load2") == 0) {
        const char* input_img = argv[2];
        headless_load_img(input_img);
        L2Report report = VerifyLoad2Packing(g_load2_ppp, g_load2_limit_scales_to_3);
        std::printf("LOAD2 Verification Report for %s:\n", input_img);
        std::printf("  Checked %u sprites.\n", report.imgs_checked);
        if (report.break_count > 0 || report.warn_count > 0) {
            std::printf("  Found %d break(s) and %d warning(s).\n", report.break_count, report.warn_count);
            for (const auto& issue : report.issues) {
                std::printf("  - [%s] %s: %s\n", issue.sev == L2Severity::Warn ? "WARN" : "BREAK", issue.img_name.c_str(), issue.message.c_str());
            }
            return (report.break_count > 0) ? 1 : 0;
        }
        std::printf("  No issues found.\n");
        return 0;
    }

    if (std::strcmp(cmd, "--build-lod") == 0) {
        if (argc < 4) { std::printf("Error: Missing output file.\n"); return 1; }
        const char* lod_file = argv[2];
        const char* output_file = argv[3];

        LodManifest manifest = ParseLodFile(lod_file);
        if (manifest.parse_error) {
            std::printf("Error parsing LOD: %s\n", manifest.error_msg.c_str());
            return 1;
        }

        if (manifest.ppp_value > 0) g_load2_ppp = manifest.ppp_value;
        std::string lod_dir = ".";
        std::string lod_path(lod_file);
        size_t sep = lod_path.find_last_of("\\/");
        if (sep != std::string::npos) lod_dir = lod_path.substr(0, sep);

        for (const auto& entry : manifest.entries) {
            std::string rpath = entry.resolved_path;
            size_t path_sep = rpath.find_last_of("\\/");
            std::string dir = (path_sep != std::string::npos) ? rpath.substr(0, path_sep) : ".";
            std::string file = (path_sep != std::string::npos) ? rpath.substr(path_sep + 1) : rpath;

            auto try_load = [&](const std::string &d) -> bool {
                size_t nd = d.length();
                if (nd > 63) nd = 63;
                memset(g_doc->fpath_s, 0, 64);
                memcpy(g_doc->fpath_s, d.c_str(), nd);

                size_t n_file = file.length();
                if (n_file > 12) n_file = 12;
                memset(g_doc->fname_s, 0, 13);
                memcpy(g_doc->fname_s, file.c_str(), n_file);
                for (size_t j = 0; j < n_file; j++)
                    g_doc->fname_s[j] = (char)toupper((unsigned char)g_doc->fname_s[j]);

                unsigned int prev = g_doc->imgcnt;
#ifdef _WIN32
                _chdir(d.c_str());
#else
                chdir(d.c_str());
#endif
                LoadImgFile();
                return g_doc->imgcnt > prev;
            };

            if (!try_load(dir) && !try_load(lod_dir)) {
                const char *imgdir = getenv("IMGDIR");
                if (imgdir) try_load(imgdir);
            }
        }

        std::string out_path(output_file);
        sep = out_path.find_last_of("\\/");
        std::string out_dir = (sep == std::string::npos) ? std::string(".") : out_path.substr(0, sep);
        std::string out_file = (sep == std::string::npos) ? out_path : out_path.substr(sep + 1);

        size_t n_dir = out_dir.size();
        if (n_dir > 63) n_dir = 63;
        memset(g_doc->fpath_s, 0, 64);
        memcpy(g_doc->fpath_s, out_dir.data(), n_dir);

        size_t n_file = out_file.size();
        if (n_file > 12) n_file = 12;
        memset(g_doc->fname_s, 0, 13);
        memcpy(g_doc->fname_s, out_file.data(), n_file);
        for (size_t i = 0; i < n_file; i++)
            g_doc->fname_s[i] = (char)toupper((unsigned char)g_doc->fname_s[i]);

#ifdef _WIN32
        _chdir(out_dir.c_str());
#else
        chdir(out_dir.c_str());
#endif
        SaveImgFile();
        std::printf("Built IMG from LOD and saved to %s\n", output_file);
        return 0;
    }

    if (argc < 4) {
        std::printf("Error: Missing arguments for headless export.\n");
        return 1;
    }
    const char* input_img = argv[2];
    const char* output_file = argv[3];

    headless_load_img(input_img);
    headless_mark_all();

    if (std::strcmp(cmd, "--export-anilst") == 0) {
        WriteAnilstFromMarked(output_file);
        std::printf("Exported ANILST to %s\n", output_file);
        return 0;
    }

    if (std::strcmp(cmd, "--export-tbl") == 0) {
        bool mk3 = false, include_pal = false, pad_4bit = false, align_16bit = false, dual_bank = false;
        int bank = 0;
        unsigned int base_address = 0x02000000;

        for (int i = 4; i < argc; i++) {
            if (std::strcmp(argv[i], "--mk3") == 0) mk3 = true;
            else if (std::strcmp(argv[i], "--include-pal") == 0) include_pal = true;
            else if (std::strcmp(argv[i], "--padding") == 0) pad_4bit = true;
            else if (std::strcmp(argv[i], "--align-16") == 0) align_16bit = true;
            else if (std::strcmp(argv[i], "--dual-bank") == 0) dual_bank = true;
            else if (std::strncmp(argv[i], "--bank=", 7) == 0) bank = std::atoi(argv[i] + 7);
            else if (std::strncmp(argv[i], "--base=", 7) == 0) base_address = std::strtoul(argv[i] + 7, nullptr, 16);
        }
        WriteTblFromMarked(output_file, base_address, mk3, include_pal, pad_4bit, align_16bit, dual_bank, bank);
        std::printf("Exported TBL to %s\n", output_file);
        return 0;
    }

    if (std::strcmp(cmd, "--export-irw") == 0) {
        int bpp = 8;
        bool align_16bit = true;
        unsigned int base_address = 0x02000000;

        for (int i = 4; i < argc; i++) {
            if (std::strncmp(argv[i], "--bpp=", 6) == 0) bpp = std::atoi(argv[i] + 6);
            else if (std::strcmp(argv[i], "--no-align") == 0) align_16bit = false;
            else if (std::strncmp(argv[i], "--base=", 7) == 0) base_address = std::strtoul(argv[i] + 7, nullptr, 16);
        }
        WriteIrwFromMarked(output_file, base_address, bpp, align_16bit);
        std::printf("Exported IRW to %s\n", output_file);
        return 0;
    }

    if (std::strcmp(cmd, "--export-png") == 0) {
#ifdef _WIN32
        _mkdir(output_file);
#else
        mkdir(output_file, 0755);
#endif
        IMG* p = (IMG*)g_doc->img_p;
        int idx = 0;
        while (p) {
            g_doc->ilselected = idx;
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/%.15s.png", output_file, p->n_s);
            ExportPng(path);
            p = (IMG*)p->nxt_p;
            idx++;
        }
        std::printf("Exported PNGs to directory: %s\n", output_file);
        return 0;
    }

    if (std::strcmp(cmd, "--build-tga") == 0) {
        BuildTgaFromMarked(output_file);
        std::printf("Built TGA sprite sheet to %s\n", output_file);
        return 0;
    }

    std::printf("Error: Unknown command %s\n", cmd);
    return 1;
}

/* Populate shim_file's exe_dir so legacy "c:\bin\" path remapping resolves
   against the directory the executable lives in. */
static void capture_exe_dir(int argc, char **argv)
{
    char full[MAX_PATH] = {0};
#ifdef _WIN32
    (void)argc; (void)argv;
    if (GetModuleFileNameA(NULL, full, sizeof(full))) {
        char *slash = strrchr(full, '\\');
        if (slash) { *slash = '\0'; strncpy(exe_dir, full, MAX_PATH - 1); }
    }
#else
    ssize_t len = -1;
#  ifdef __APPLE__
    /* macOS: /proc/self/exe doesn't exist; use the dyld API instead. */
    uint32_t sz = sizeof(full);
    if (_NSGetExecutablePath(full, &sz) == 0) {
        len = (ssize_t)strlen(full);
    }
#  else
    len = readlink("/proc/self/exe", full, sizeof(full) - 1);
#  endif
    if (len < 0 && argc > 0) {  /* fallback to argv[0] when the OS lookup fails */
        strncpy(full, argv[0], sizeof(full) - 1);
        len = (ssize_t)strlen(full);
    }
    if (len > 0) {
        full[len] = '\0';
        char *slash = strrchr(full, '/');
        if (slash) { *slash = '\0'; strncpy(exe_dir, full, MAX_PATH - 1); }
    }
#endif
}

int main(int argc, char *argv[])
{
    capture_exe_dir(argc, argv);

    /* ---- Check for headless CLI commands ---- */
    if (argc > 1) {
        if (std::strcmp(argv[1], "--export-anilst") == 0 ||
            std::strcmp(argv[1], "--export-tbl") == 0 ||
            std::strcmp(argv[1], "--export-irw") == 0 ||
            std::strcmp(argv[1], "--export-png") == 0 ||
            std::strcmp(argv[1], "--build-tga") == 0 ||
            std::strcmp(argv[1], "--build-lod") == 0 ||
            std::strcmp(argv[1], "--verify-load2") == 0) {
            return run_headless_cli(argc, argv);
        }
    }

    /* ---- Init document storage (must precede any IMG access) ---- */
    document_init();

    /* ---- Init SDL2 ---- */
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        MessageBoxA(NULL, SDL_GetError(), "SDL_Init Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Title bakes in the version so users can tell builds apart at a glance
       (e.g. when running multiple checkouts side by side). */
    char title[64];
    snprintf(title, sizeof(title), "IMGTOOL v%s", IMGTOOL_VERSION);
    SDL_Window *window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1024, 768, SDL_WINDOW_RESIZABLE);
    if (!window) { SDL_Quit(); return 1; }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!renderer) { SDL_DestroyWindow(window); SDL_Quit(); return 1; }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    /* 640x480 canvas matches the original VGA resolution the editor was
       built around; the ImGui overlay zooms this into the visible canvas. */
    SDL_Texture *canvas_tex = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480);

    imgui_overlay_init(window, renderer, canvas_tex);

    if (argc > 1) {
        if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "/?") == 0) {
            char help_msg[256];
            std::snprintf(help_msg, sizeof(help_msg),
                "Usage: %s [file]\n\n"
                "  file    Path to an .img, .png, .gif, .tga, or .lbm file to open on launch.", argv[0]);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "IMGTOOL Usage", help_msg, window);
            SDL_DestroyTexture(canvas_tex);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 0;
        }
        imgui_overlay_open_path(argv[1]);
    }

    /* ---- Main loop ----
     *
     * Idle-friendly: SDL_WaitEventTimeout blocks when nothing's happening
     * instead of spinning a core at 100%. The 16 ms timeout is one frame at
     * 60 Hz, so reactive elements (tooltip fades, mouse-button-held
     * dragging, popup animations) catch up within a single frame.
     *
     * We unconditionally re-render after the wait — ImGui needs at least
     * one frame to settle after every state change. */
    /* Subscribe to OS file-drop events. SDL emits SDL_DROPFILE per file
       dragged onto the window; the string is heap-allocated and we own it. */
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    bool running = true;
    while (running) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 16)) {
            do {
                imgui_overlay_process_event(&e);
                if (e.type == SDL_QUIT)
                    imgui_overlay_request_quit();
                else if (e.type == SDL_DROPFILE) {
                    imgui_overlay_open_path(e.drop.file);
                    SDL_free(e.drop.file);
                }
            } while (SDL_PollEvent(&e));
        }

        SDL_SetRenderDrawColor(renderer, 0x06, 0x06, 0x06, 0xFF);
        SDL_RenderClear(renderer);

        imgui_overlay_newframe();
        imgui_overlay_render();
        imgui_overlay_present();

        SDL_RenderPresent(renderer);

        if (imgui_overlay_should_quit())
            running = false;
    }

    imgui_overlay_shutdown();
    SDL_DestroyTexture(canvas_tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
