/*************************************************************
 * platform/lod_parser.cpp
 * LOD manifest parser for LOAD2 .lod files.
 *************************************************************/
#include "lod_parser.h"
#include "compat.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

#ifdef _WIN32
#include <direct.h>
#define getcwd_impl _getcwd
#else
#include <unistd.h>
#define getcwd_impl getcwd
#endif

static bool has_dir_separator(const char *path)
{
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') return true;
    }
    return false;
}

static std::string get_imgdir(void)
{
    const char *env = getenv("IMGDIR");
    if (env && env[0]) return std::string(env);
    return std::string();
}

static std::string get_cwd(void)
{
    char buf[MAX_PATH];
    if (getcwd_impl(buf, sizeof(buf))) return std::string(buf);
    return std::string(".");
}

/* Strip trailing carriage return (Windows line endings) */
static void rtrim_cr(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
        line[--len] = '\0';
}

static bool is_keyword_line(const char *line, const char *kw)
{
    return strncmp(line, kw, 4) == 0;
}

static std::string resolve_img_path(const char *raw, const char *override_dir)
{
    std::string path(raw);

    /* If an override directory is provided, force it (ignoring original path dirs) */
    if (override_dir && override_dir[0] != '\0') {
        std::string over_str(override_dir);
        /* Extract just the filename from the raw path */
        size_t last_slash = path.find_last_of("\\/");
        if (last_slash != std::string::npos) {
            path = path.substr(last_slash + 1);
        }
        if (over_str.back() == '\\' || over_str.back() == '/')
            return over_str + path;
        else
            return over_str + "\\" + path;
    }

    if (has_dir_separator(path.c_str()))
        return path;

    std::string imgdir = get_imgdir();
    if (!imgdir.empty()) {
        if (imgdir.back() == '\\' || imgdir.back() == '/')
            return imgdir + path;
        else
            return imgdir + "\\" + path;
    }

    return get_cwd() + "\\" + path;
}

LodManifest ParseLodFile(const char *lod_path, const char *override_dir)
{
    LodManifest manifest;

    FILE *f = fopen(lod_path, "r");
    if (!f) {
        manifest.parse_error = true;
        manifest.error_msg = "Could not open .lod file: ";
        manifest.error_msg += lod_path;
        return manifest;
    }

    char line[512];
    int line_no = 0;

    while (fgets(line, sizeof(line), f)) {
        line_no++;
        rtrim_cr(line);

        if (line[0] == '\0') continue;

        if (line[0] == ';' || line[0] == '/') continue;

        if (is_keyword_line(line, "PPP>")) {
            int val = 0;
            if (sscanf(line + 4, "%d", &val) == 1) {
                if (val >= 0 && val <= 8)
                    manifest.ppp_value = val;
            }
            continue;
        }

        /* Recognised keywords we don't act on: skip them */
        if (is_keyword_line(line, "--->") ||
            is_keyword_line(line, "***>") ||
            is_keyword_line(line, "GLO>") ||
            is_keyword_line(line, "ASM>") ||
            is_keyword_line(line, "ROM>") ||
            is_keyword_line(line, "ZON>") ||
            is_keyword_line(line, "ZOF>") ||
            is_keyword_line(line, "CON>") ||
            is_keyword_line(line, "COF>") ||
            is_keyword_line(line, "PON>") ||
            is_keyword_line(line, "POF>") ||
            is_keyword_line(line, "XON>") ||
            is_keyword_line(line, "XOF>") ||
            is_keyword_line(line, "FRM>") ||
            is_keyword_line(line, "BBB>") ||
            is_keyword_line(line, "UFN>") ||
            is_keyword_line(line, "UGL>") ||
            is_keyword_line(line, "UNI>") ||
            is_keyword_line(line, "IHDR") ||
            is_keyword_line(line, "MON>") ||
            is_keyword_line(line, "MOF>") ||
            is_keyword_line(line, "BON>") ||
            is_keyword_line(line, "BOF>") ||
            is_keyword_line(line, "ZAL>"))
            continue;

        /* Not a keyword: it's an IMG file path */
        LodManifest::Entry entry;
        entry.resolved_path = resolve_img_path(line, override_dir);
        manifest.entries.push_back(entry);
    }

    fclose(f);

    if (manifest.entries.empty()) {
        manifest.parse_error = true;
        manifest.error_msg = "No IMG files found in .lod file.";
    }

    return manifest;
}
