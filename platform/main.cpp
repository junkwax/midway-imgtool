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
#include <cerrno>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#else
#  include <unistd.h>
#  ifdef __APPLE__
#    include <mach-o/dyld.h>
#  endif
#endif

static bool is_help_arg(const char *arg)
{
    return arg &&
        (std::strcmp(arg, "--help") == 0 ||
         std::strcmp(arg, "-h") == 0 ||
         std::strcmp(arg, "/?") == 0);
}

static bool is_headless_command(const char *arg)
{
    return arg &&
        (std::strcmp(arg, "--export-anilst") == 0 ||
         std::strcmp(arg, "--export-tbl") == 0 ||
         std::strcmp(arg, "--compare-tbl") == 0 ||
         std::strcmp(arg, "--export-irw") == 0 ||
         std::strcmp(arg, "--export-png") == 0 ||
         std::strcmp(arg, "--build-tga") == 0 ||
         std::strcmp(arg, "--debug-spritesheet") == 0 ||
         std::strcmp(arg, "--build-lod") == 0 ||
         std::strcmp(arg, "--verify-load2") == 0);
}

static void print_cli_help(FILE *out, const char *exe)
{
    if (!exe || !*exe) exe = "imgtool";
    std::fprintf(out,
        "IMGTOOL command line\n"
        "\n"
#ifdef IMGTOOL_CLI_ONLY
        "GUI executable:\n"
        "  imgtool [file]\n"
#else
        "GUI:\n"
        "  %s [file]\n"
#endif
        "\n"
        "Headless commands:\n"
        "  %s --verify-load2 <input.img> [--ppp=N] [--limit-scales-to-3]\n"
        "  %s --export-anilst <input.img> <output.asm>\n"
        "  %s --export-tbl <input.img> <output.tbl> [options]\n"
        "  %s --compare-tbl <input.img> <existing.tbl>\n"
        "  %s --export-irw <input.img> <output.irw> [options]\n"
        "  %s --export-png <input.img> <output_dir>\n"
        "  %s --build-tga <input.img> <output.tga>\n"
        "  %s --debug-spritesheet <sheet.png|jpg|tga> <output_dir> [options]\n"
        "  %s --build-lod <manifest.lod> <output.img> [--override-dir=DIR]\n"
        "\n"
        "Compare exits 1 when the table and the IMG disagree, so a build can\n"
        "gate on it. SAG is never compared: LOAD2 assigns ROM addresses, the\n"
        ".IMG does not carry them.\n"
        "\n"
        "TBL options:\n"
        "  --mk3              write MK3 7-value headers\n"
        "  --include-pal      include palette label pointers\n"
        "  --padding          pad packed data to 4-bit boundary (/P)\n"
        "  --align-16         align sprite starts to 16-bit boundary (/L)\n"
        "  --dual-bank        emit dual-bank addresses (/E)\n"
        "  --bank=N           bank number for dual-bank output (0 or 1)\n"
        "  --base=HEX         ROM base address (default 02000000)\n"
        "\n"
        "IRW options:\n"
        "  --bpp=N            fixed bits per pixel, 1..8\n"
        "  --bpp=auto         infer bpp from image data\n"
        "  --bpp=palette      infer bpp from palette size (/B)\n"
        "  --no-align         disable 16-bit alignment\n"
        "  --base=HEX         ROM base address (default 02000000)\n"
        "\n"
        "Sprite sheet debug options:\n"
        "  --background=N     background cutoff, 180..255 (default 245)\n"
        "  --min-pixels=N     minimum accepted frame pixels (default 160)\n"
        "  --padding=N        extra transparent padding around crops (default 2)\n"
        "  --no-crop          keep candidate box padding instead of tight crop\n"
        "  --prefix=NAME      debug crop naming prefix (default FRAME)\n"
        "\n"
        "Exit status: 0 on success; non-zero on invalid args, failed loads,\n"
        "or LOAD2 breaking issues.\n",
#ifdef IMGTOOL_CLI_ONLY
        exe, exe, exe, exe, exe, exe, exe, exe, exe);
#else
        exe, exe, exe, exe, exe, exe, exe, exe, exe, exe);
#endif
}

static bool parse_u32_hex_arg(const char *arg, const char *prefix, unsigned int *out)
{
    size_t n = std::strlen(prefix);
    if (std::strncmp(arg, prefix, n) != 0) return false;
    const char *s = arg + n;
    if (!*s) return false;
    char *end = NULL;
    unsigned long v = std::strtoul(s, &end, 16);
    if (!end || *end != '\0' || v > 0xFFFFFFFFUL) return false;
    *out = (unsigned int)v;
    return true;
}

static bool parse_int_arg(const char *arg, const char *prefix, int min_v, int max_v, int *out)
{
    size_t n = std::strlen(prefix);
    if (std::strncmp(arg, prefix, n) != 0) return false;
    const char *s = arg + n;
    if (!*s) return false;
    char *end = NULL;
    long v = std::strtol(s, &end, 10);
    if (!end || *end != '\0' || v < min_v || v > max_v) return false;
    *out = (int)v;
    return true;
}

static bool path_exists(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0;
}

static bool path_is_dir(const char *path)
{
    struct stat st;
    if (!path || !*path || stat(path, &st) != 0) return false;
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

static std::string path_join(const std::string &dir, const std::string &file)
{
    if (dir.empty() || dir == ".") return file;
    char last = dir[dir.size() - 1];
    if (last == '\\' || last == '/') return dir + file;
#ifdef _WIN32
    return dir + "\\" + file;
#else
    return dir + "/" + file;
#endif
}

static std::string current_dir_string(void)
{
    char buf[MAX_PATH];
#ifdef _WIN32
    if (_getcwd(buf, sizeof(buf))) return std::string(buf);
#else
    if (getcwd(buf, sizeof(buf))) return std::string(buf);
#endif
    return std::string(".");
}

static bool path_is_absolute(const std::string &path)
{
    if (path.empty()) return false;
#ifdef _WIN32
    if (path.size() >= 2 && path[1] == ':') return true;
    return path.size() >= 2 &&
        ((path[0] == '\\' && path[1] == '\\') ||
         (path[0] == '/'  && path[1] == '/'));
#else
    return path[0] == '/';
#endif
}

static std::string make_absolute_path(const std::string &path, const std::string &base)
{
    if (path_is_absolute(path)) return path;
    return path_join(base.empty() ? std::string(".") : base, path);
}

static bool ensure_dir_exists(const char *path)
{
#ifdef _WIN32
    if (_mkdir(path) == 0) return true;
#else
    if (mkdir(path, 0755) == 0) return true;
#endif
    if (errno == EEXIST) return path_is_dir(path);
    return false;
}

static void set_doc_path_for_file(const char *filepath)
{
    std::string path(filepath ? filepath : "");
    size_t sep = path.find_last_of("\\/");
    std::string dir = (sep == std::string::npos) ? std::string(".") : path.substr(0, sep);
    std::string file = (sep == std::string::npos) ? path : path.substr(sep + 1);

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

#ifdef _WIN32
    _chdir(dir.c_str());
#else
    chdir(dir.c_str());
#endif
}

static void headless_mark_all() {
    IMG* p = (IMG*)g_doc->img_p;
    while (p) {
        p->flags |= 1; // Mark image
        p = (IMG*)p->nxt_p;
    }
}

static bool headless_load_img(const char* filepath) {
    if (!path_exists(filepath)) {
        std::fprintf(stderr, "Error: input IMG not found: %s\n", filepath ? filepath : "(null)");
        return false;
    }
    unsigned int prev = g_doc->imgcnt;
    set_doc_path_for_file(filepath);
    LoadImgFile();
    if (g_doc->imgcnt <= prev) {
        std::fprintf(stderr, "Error: failed to load IMG: %s\n", filepath);
        return false;
    }
    return true;
}

static int run_headless_cli(int argc, char *argv[]) {
    const char *exe = (argc > 0) ? argv[0] : "imgtool";
    if (argc < 2 || is_help_arg(argv[1])) {
        print_cli_help(stdout, exe);
        return 0;
    }

    const char* cmd = argv[1];
    if (!is_headless_command(cmd)) {
        std::fprintf(stderr, "Error: unknown command: %s\n\n", cmd);
        print_cli_help(stderr, exe);
        return 2;
    }

    for (int i = 2; i < argc; i++) {
        if (is_help_arg(argv[i])) {
            print_cli_help(stdout, exe);
            return 0;
        }
    }

    document_init();

    if (std::strcmp(cmd, "--verify-load2") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "Error: --verify-load2 requires <input.img>\n");
            return 2;
        }
        const char* input_img = argv[2];
        for (int i = 3; i < argc; i++) {
            if (std::strncmp(argv[i], "--ppp=", 6) == 0) {
                if (!parse_int_arg(argv[i], "--ppp=", 0, 8, &g_load2_ppp)) {
                    std::fprintf(stderr, "Error: invalid --ppp value: %s\n", argv[i]);
                    return 2;
                }
            } else if (std::strcmp(argv[i], "--limit-scales-to-3") == 0) {
                g_load2_limit_scales_to_3 = true;
            } else if (std::strcmp(argv[i], "--no-limit-scales") == 0) {
                g_load2_limit_scales_to_3 = false;
            } else {
                std::fprintf(stderr, "Error: unknown --verify-load2 option: %s\n", argv[i]);
                return 2;
            }
        }
        if (!headless_load_img(input_img)) return 1;

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

    if (std::strcmp(cmd, "--debug-spritesheet") == 0) {
        if (argc < 4) {
            std::fprintf(stderr, "Error: --debug-spritesheet requires <sheet image> <output_dir>\n");
            return 2;
        }
        const char* sheet_file = argv[2];
        const char* output_dir = argv[3];
        SpriteSheetImportOptions opts = {};
        opts.detect_mode = SpriteSheetDetect_Auto;
        opts.background_threshold = 245;
        opts.min_pixels = 160;
        opts.padding = 2;
        opts.crop = true;
        std::strncpy(opts.name_prefix, "FRAME", sizeof(opts.name_prefix) - 1);

        for (int i = 4; i < argc; i++) {
            if (std::strncmp(argv[i], "--background=", 13) == 0) {
                if (!parse_int_arg(argv[i], "--background=", 180, 255, &opts.background_threshold)) {
                    std::fprintf(stderr, "Error: invalid --background value: %s\n", argv[i]);
                    return 2;
                }
            } else if (std::strncmp(argv[i], "--min-pixels=", 13) == 0) {
                if (!parse_int_arg(argv[i], "--min-pixels=", 1, 10000000, &opts.min_pixels)) {
                    std::fprintf(stderr, "Error: invalid --min-pixels value: %s\n", argv[i]);
                    return 2;
                }
            } else if (std::strncmp(argv[i], "--padding=", 10) == 0) {
                if (!parse_int_arg(argv[i], "--padding=", 0, 256, &opts.padding)) {
                    std::fprintf(stderr, "Error: invalid --padding value: %s\n", argv[i]);
                    return 2;
                }
            } else if (std::strcmp(argv[i], "--no-crop") == 0) {
                opts.crop = false;
            } else if (std::strncmp(argv[i], "--prefix=", 9) == 0) {
                std::strncpy(opts.name_prefix, argv[i] + 9, sizeof(opts.name_prefix) - 1);
                opts.name_prefix[sizeof(opts.name_prefix) - 1] = '\0';
            } else {
                std::fprintf(stderr, "Error: unknown --debug-spritesheet option: %s\n", argv[i]);
                return 2;
            }
        }

        if (!path_exists(sheet_file)) {
            std::fprintf(stderr, "Error: sprite sheet not found: %s\n", sheet_file);
            return 1;
        }
        SpriteSheetDebugReport report = {};
        int frames = DebugSpriteSheetImport(sheet_file, output_dir, &opts, &report);
        std::printf("Sprite sheet debug for %s:\n", sheet_file);
        std::printf("  Size: %dx%d\n", report.sheet_w, report.sheet_h);
        std::printf("  Raw islands: %d\n", report.raw_islands);
        std::printf("  Removed separator rows/cols: %d/%d\n", report.line_rows, report.line_cols);
        std::printf("  Accepted frames: %d\n", report.accepted_frames);
        std::printf("  Wrote debug artifacts to: %s\n", output_dir);
        return frames > 0 ? 0 : 1;
    }

    if (std::strcmp(cmd, "--build-lod") == 0) {
        if (argc < 4) {
            std::fprintf(stderr, "Error: --build-lod requires <manifest.lod> <output.img>\n");
            return 2;
        }
        const char* lod_file = argv[2];
        const char* output_file = argv[3];
        const char* override_dir = NULL;
        std::string start_dir = current_dir_string();
        std::string override_dir_storage;

        for (int i = 4; i < argc; i++) {
            if (std::strncmp(argv[i], "--override-dir=", 15) == 0) {
                override_dir = argv[i] + 15;
                if (!*override_dir) {
                    std::fprintf(stderr, "Error: --override-dir needs a directory\n");
                    return 2;
                }
                override_dir_storage = make_absolute_path(override_dir, start_dir);
                override_dir = override_dir_storage.c_str();
            } else {
                std::fprintf(stderr, "Error: unknown --build-lod option: %s\n", argv[i]);
                return 2;
            }
        }

        if (!path_exists(lod_file)) {
            std::fprintf(stderr, "Error: LOD manifest not found: %s\n", lod_file);
            return 1;
        }

        LodManifest manifest = ParseLodFile(lod_file, override_dir);
        if (manifest.parse_error) {
            std::fprintf(stderr, "Error parsing LOD: %s\n", manifest.error_msg.c_str());
            return 1;
        }
        if (manifest.entries.empty()) {
            std::fprintf(stderr, "Error: LOD did not reference any IMG files: %s\n", lod_file);
            return 1;
        }

        if (manifest.has_ppp_value) g_load2_ppp = manifest.ppp_value;
        std::string lod_dir = ".";
        std::string lod_path(lod_file);
        size_t sep = lod_path.find_last_of("\\/");
        if (sep != std::string::npos) lod_dir = lod_path.substr(0, sep);
        lod_dir = make_absolute_path(lod_dir, start_dir);

        int loaded = 0;
        std::string first_missing;
        for (const auto& entry : manifest.entries) {
            std::string rpath = entry.resolved_path;
            size_t path_sep = rpath.find_last_of("\\/");
            std::string dir = (path_sep != std::string::npos) ? rpath.substr(0, path_sep) : ".";
            std::string file = (path_sep != std::string::npos) ? rpath.substr(path_sep + 1) : rpath;

            auto try_load = [&](const std::string &d) -> bool {
                std::string candidate = path_join(d, file);
                if (!path_exists(candidate.c_str())) return false;
                size_t nd = d.length();
                if (nd > sizeof(g_doc->fpath_s) - 1) nd = sizeof(g_doc->fpath_s) - 1;
                memset(g_doc->fpath_s, 0, sizeof(g_doc->fpath_s));
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

            bool ok = try_load(dir) || (lod_dir != dir && try_load(lod_dir));
            if (!ok) {
                const char *imgdir = getenv("IMGDIR");
                if (imgdir && imgdir[0] && std::string(imgdir) != dir && std::string(imgdir) != lod_dir)
                    ok = try_load(imgdir);
            }
            if (ok) {
                loaded++;
            } else if (first_missing.empty()) {
                first_missing = rpath;
            }
        }
        if (loaded != (int)manifest.entries.size()) {
            std::fprintf(stderr, "Error: loaded %d/%zu IMG files from LOD. First missing: %s\n",
                         loaded, manifest.entries.size(), first_missing.c_str());
            return 1;
        }

        std::string output_full = make_absolute_path(output_file, start_dir);
        set_doc_path_for_file(output_full.c_str());
        SaveImgFile();
        std::printf("Built IMG from LOD (%d sprites from %zu file(s)) and saved to %s\n",
                    (int)g_doc->imgcnt, manifest.entries.size(), output_full.c_str());
        return 0;
    }

    if (argc < 4) {
        std::fprintf(stderr, "Error: %s requires <input.img> <output>\n", cmd);
        return 2;
    }
    const char* input_img = argv[2];
    const char* output_file = argv[3];

    if (std::strcmp(cmd, "--export-anilst") == 0) {
        if (argc > 4) {
            std::fprintf(stderr, "Error: unknown --export-anilst option: %s\n", argv[4]);
            return 2;
        }
        if (!headless_load_img(input_img)) return 1;
        headless_mark_all();
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
            else if (std::strncmp(argv[i], "--bank=", 7) == 0) {
                if (!parse_int_arg(argv[i], "--bank=", 0, 1, &bank)) {
                    std::fprintf(stderr, "Error: invalid --bank value: %s\n", argv[i]);
                    return 2;
                }
            } else if (std::strncmp(argv[i], "--base=", 7) == 0) {
                if (!parse_u32_hex_arg(argv[i], "--base=", &base_address)) {
                    std::fprintf(stderr, "Error: invalid --base value: %s\n", argv[i]);
                    return 2;
                }
            } else {
                std::fprintf(stderr, "Error: unknown --export-tbl option: %s\n", argv[i]);
                return 2;
            }
        }
        if (!headless_load_img(input_img)) return 1;
        headless_mark_all();
        WriteTblFromMarked(output_file, base_address, mk3, include_pal, pad_4bit, align_16bit, dual_bank, bank);
        std::printf("Exported TBL to %s\n", output_file);
        return 0;
    }

    /* Diff a checked-in .TBL against the IMG instead of overwriting it.
       MK2's src/*.TBL files are hand-maintained — nothing regenerates them —
       so a re-export that moved an anipoint would desync art from table
       silently. Exits 1 on drift so a build step can gate on it. */
    if (std::strcmp(cmd, "--compare-tbl") == 0) {
        if (argc < 4) {
            std::fprintf(stderr,
                "Error: --compare-tbl requires <input.img> <existing.tbl>\n");
            return 2;
        }
        const char *input_img = argv[2];
        const char *tbl_path = argv[3];
        for (int i = 4; i < argc; i++) {
            std::fprintf(stderr, "Error: unknown --compare-tbl option: %s\n", argv[i]);
            return 2;
        }

        FILE *tf = std::fopen(tbl_path, "rb");
        if (!tf) {
            std::fprintf(stderr, "Error: could not open TBL: %s\n", tbl_path);
            return 1;
        }
        std::string text;
        char rbuf[4096];
        size_t rn;
        while ((rn = std::fread(rbuf, 1, sizeof(rbuf), tf)) > 0)
            text.append(rbuf, rn);
        std::fclose(tf);

        std::vector<TblEntry> table;
        std::vector<std::string> warnings;
        std::string err;
        if (!ParseTblText(text, table, &warnings, &err)) {
            std::fprintf(stderr, "Error: %s (%s)\n", err.c_str(), tbl_path);
            return 1;
        }
        for (size_t w = 0; w < warnings.size(); w++)
            std::fprintf(stderr, "Warning: %s\n", warnings[w].c_str());

        if (!headless_load_img(input_img)) return 1;
        headless_mark_all();

        std::vector<TblEntry> img_entries;
        BuildTblEntriesFromDoc(true, img_entries);

        std::vector<TblDiffRow> rows;
        TblDiffSummary summary;
        DiffTblEntries(table, img_entries, rows, &summary);

        std::string report = FormatTblDiffReport(tbl_path, rows, summary);
        std::fputs(report.c_str(), stdout);

        bool drift = summary.differing > 0 || summary.only_in_tbl > 0 ||
                     summary.only_in_img > 0;
        return drift ? 1 : 0;
    }

    if (std::strcmp(cmd, "--export-irw") == 0) {
        int bpp = 0;
        bool align_16bit = true;
        unsigned int base_address = 0x02000000;

        for (int i = 4; i < argc; i++) {
            if (std::strcmp(argv[i], "--bpp=auto") == 0) bpp = 0;
            else if (std::strcmp(argv[i], "--bpp=palette") == 0) bpp = -1;
            else if (std::strncmp(argv[i], "--bpp=", 6) == 0) {
                if (!parse_int_arg(argv[i], "--bpp=", 1, 8, &bpp)) {
                    std::fprintf(stderr, "Error: invalid --bpp value: %s\n", argv[i]);
                    return 2;
                }
            }
            else if (std::strcmp(argv[i], "--no-align") == 0) align_16bit = false;
            else if (std::strncmp(argv[i], "--base=", 7) == 0) {
                if (!parse_u32_hex_arg(argv[i], "--base=", &base_address)) {
                    std::fprintf(stderr, "Error: invalid --base value: %s\n", argv[i]);
                    return 2;
                }
            } else {
                std::fprintf(stderr, "Error: unknown --export-irw option: %s\n", argv[i]);
                return 2;
            }
        }
        if (!headless_load_img(input_img)) return 1;
        headless_mark_all();
        WriteIrwFromMarked(output_file, base_address, bpp, align_16bit);
        std::printf("Exported IRW to %s\n", output_file);
        return 0;
    }

    if (std::strcmp(cmd, "--export-png") == 0) {
        if (argc > 4) {
            std::fprintf(stderr, "Error: unknown --export-png option: %s\n", argv[4]);
            return 2;
        }
        if (!headless_load_img(input_img)) return 1;
        headless_mark_all();
        if (!ensure_dir_exists(output_file)) {
            std::fprintf(stderr, "Error: could not create output directory: %s\n", output_file);
            return 1;
        }
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
        if (argc > 4) {
            std::fprintf(stderr, "Error: unknown --build-tga option: %s\n", argv[4]);
            return 2;
        }
        if (!headless_load_img(input_img)) return 1;
        headless_mark_all();
        BuildTgaFromMarked(output_file);
        std::printf("Built TGA sprite sheet to %s\n", output_file);
        return 0;
    }

    std::fprintf(stderr, "Error: unknown command: %s\n", cmd);
    return 2;
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

#ifdef IMGTOOL_CLI_ONLY
    if (argc <= 1 || is_help_arg(argv[1])) {
        print_cli_help(stdout, argc > 0 ? argv[0] : "imgtool-cli");
        return 0;
    }
    return run_headless_cli(argc, argv);
#else
    /* ---- Check for headless CLI commands ---- */
    if (argc > 1) {
        if (is_headless_command(argv[1])) {
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
    if (!window) {
        MessageBoxA(NULL, SDL_GetError(), "SDL_CreateWindow Error", MB_OK | MB_ICONERROR);
        SDL_Quit(); return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        MessageBoxA(NULL, SDL_GetError(), "SDL_CreateRenderer Error", MB_OK | MB_ICONERROR);
        SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    /* 640x480 canvas matches the original VGA resolution the editor was
       built around; the ImGui overlay zooms this into the visible canvas. */
    SDL_Texture *canvas_tex = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480);

    imgui_overlay_init(window, renderer, canvas_tex);

    if (argc > 1) {
        if (is_help_arg(argv[1])) {
            char help_msg[512];
            std::snprintf(help_msg, sizeof(help_msg),
                "Usage: %s [file]\n\n"
                "  file    Path to an .img, .png, .gif, .tga, or .lbm file to open on launch.\n\n"
                "For headless automation, use imgtool-cli --help.", argv[0]);
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
#endif
}
