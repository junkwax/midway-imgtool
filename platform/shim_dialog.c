/*************************************************************
 * platform/shim_dialog.c
 * Native OS open/save file dialog for imgtool.
 *
 * Replaces the in-app modal file browser (filereq_open) when
 * the user presses 'l', 'a', or 's'. After the dialog returns
 * OK we write the full directory into g_doc->fpath_s, the chosen
 * basename into g_doc->fnametmp_s, and chdir so the downstream asm
 * callback can open the file via I21OPENR (which uses the
 * current working directory).
 *
 * Windows: Win32 GetOpenFileNameA / GetSaveFileNameA.
 * Linux:   stub that returns cancel — the asm in-app browser
 *          is still reachable via the old filereq_open path
 *          if a future build wires it up for non-Win32.
 *************************************************************/

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include "compat.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <commdlg.h>
#  include <direct.h>
#  include <shlobj.h>
#  pragma comment(lib, "comdlg32.lib")
#  pragma comment(lib, "shell32.lib")
#endif

/* Document-state file paths (g_doc->fname_s, g_doc->fpath_s, g_doc->fnametmp_s) live on
 * Document; pull them in through document.h. The /alternatename
 * pragmas are obsolete now that no ASM defines these symbols. */
#include "document.h"

/* Relay output: shim_carry 0=OK (file chosen), 1=cancel/error.
 * Thunk reads this to set CF for the asm caller. */
extern unsigned int shim_carry;

/* Called by the asm thunk.
 *   shim_eax = filter string pointer (e.g. "*.IMG" or "*.BIN")
 *   shim_esi = title pointer ("LOAD", "SAVE", "APPEND")
 * We just look at the first character of the title to pick
 * open-vs-save mode (L/A = open, S = save). */
extern unsigned int shim_eax;
extern unsigned int shim_esi;

/* ---- Directory persistence (save/load last opened folder) ---- */

static const char *get_config_path(void)
{
    static char path[MAX_PATH] = "";
    if (!path[0]) {
#ifdef _WIN32
        char appdata[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
            _snprintf(path, sizeof(path), "%s\\imgtool\\last_dir.txt", appdata);
        } else {
            _snprintf(path, sizeof(path), "last_dir.txt");
        }
#else
        _snprintf(path, sizeof(path), "%s/.imgtool_last_dir", getenv("HOME") ? getenv("HOME") : ".");
#endif
    }
    return path;
}

static void save_last_directory(const char *dir)
{
    if (!dir || !*dir) return;
    const char *config = get_config_path();
#ifdef _WIN32
    char parent[MAX_PATH];
    _snprintf(parent, sizeof(parent), "%s\\imgtool", getenv("APPDATA") ? getenv("APPDATA") : ".");
    CreateDirectoryA(parent, NULL);  /* create dir if missing */
#endif
    FILE *f = fopen(config, "w");
    if (f) {
        fprintf(f, "%s", dir);
        fclose(f);
    }
}

static void load_last_directory(char *dir, size_t dirsz)
{
    if (!dir || !dirsz) return;
    dir[0] = '\0';
    const char *config = get_config_path();
    FILE *f = fopen(config, "r");
    if (f) {
        if (fgets(dir, (int)dirsz, f)) {
            /* trim trailing newline */
            size_t len = strlen(dir);
            if (len > 0 && dir[len - 1] == '\n') dir[len - 1] = '\0';
        }
        fclose(f);
    }
}

#ifdef _WIN32

/* Convert a Win32 filter pattern like "*.IMG" into the
 * double-null-terminated pair form GetOpenFileName expects:
 *    "IMG files (*.IMG)\0*.IMG\0All files (*.*)\0*.*\0"
 * Returns pointer to a static buffer. */
static const char *build_filter(const char *pat)
{
    static char buf[128];
    if (!pat || !*pat) pat = "*.*";

    /* Derive an uppercase extension label, e.g. "IMG" from "*.IMG" */
    const char *dot = strchr(pat, '.');
    char label[16] = "Files";
    if (dot && dot[1]) {
        size_t n = strlen(dot + 1);
        if (n > sizeof(label) - 1) n = sizeof(label) - 1;
        for (size_t i = 0; i < n; i++) label[i] = (char)toupper((unsigned char)dot[1 + i]);
        label[n] = '\0';
    }

    int w = 0;
    w += _snprintf(buf + w, sizeof(buf) - w, "%s files (%s)", label, pat);
    buf[w++] = '\0';
    w += _snprintf(buf + w, sizeof(buf) - w, "%s", pat);
    buf[w++] = '\0';
    const char *tail = "All files (*.*)";
    w += _snprintf(buf + w, sizeof(buf) - w, "%s", tail);
    buf[w++] = '\0';
    w += _snprintf(buf + w, sizeof(buf) - w, "*.*");
    buf[w++] = '\0';
    buf[w]   = '\0';  /* terminating double-null */
    return buf;
}

/* Uppercase a filename in place (DOS convention — itimg.asm compares
 * extensions against "*.IMG" etc as uppercase). */
static void upcase(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/* Given a full path like "C:\DOSGames\NINJAS10.IMG", split into
 *   dir  = "C:\DOSGames"   (no trailing backslash)
 *   base = "NINJAS10.IMG"  (must fit in 12 chars + NUL for 8.3)
 * Returns 0 on success, 1 if basename too long.
 */
static int split_path(const char *full, char *dir, size_t dirsz,
                                           char *base, size_t basesz)
{
    const char *slash = strrchr(full, '\\');
    if (!slash) slash = strrchr(full, '/');
    if (!slash) {
        /* no directory component — unusual from GetOpenFileName but handle */
        dir[0] = '\0';
        if (strlen(full) >= basesz) return 1;
        strcpy(base, full);
        return 0;
    }
    size_t dlen = (size_t)(slash - full);
    if (dlen >= dirsz) dlen = dirsz - 1;
    memcpy(dir, full, dlen);
    dir[dlen] = '\0';
    /* Preserve a trailing backslash when the path is a drive root
     * ("C:" → we stored ""; needs "C:\"). GetOpenFileName returns
     * "C:\file.img" in that case, where slash points at index 2. */
    if (dlen == 2 && dir[1] == ':') {
        if (dlen + 1 < dirsz) { dir[dlen] = '\\'; dir[dlen + 1] = '\0'; }
    }
    const char *b = slash + 1;
    if (strlen(b) >= basesz) return 1;
    strcpy(base, b);
    return 0;
}

static void show_error(const char *msg)
{
    MessageBoxA(NULL, msg, "IMGTOOL", MB_OK | MB_ICONWARNING);
}

void shim_filereq_impl(void)
{
    const char *pat   = (const char *)(UINT_PTR)shim_eax;
    const char *title = (const char *)(UINT_PTR)shim_esi;
    int is_save       = (title && (title[0] == 'S' || title[0] == 's'));

    char full[MAX_PATH] = "";
    /* Pre-fill with current g_doc->fname_s if set — matches asm's fmode.0 behavior */
    if (!is_save && g_doc->fname_s[0]) {
        _snprintf(full, sizeof(full), "%s", g_doc->fname_s);
    } else if (is_save && g_doc->fname_s[0]) {
        _snprintf(full, sizeof(full), "%s", g_doc->fname_s);
    }

    /* Start the dialog in g_doc->fpath_s if it looks valid, otherwise load last saved dir */
    char initdir[MAX_PATH] = "";
    if (g_doc->fpath_s[0]) {
        _snprintf(initdir, sizeof(initdir), "%s", g_doc->fpath_s);
    } else {
        load_last_directory(initdir, sizeof(initdir));
    }

    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = NULL;
    ofn.lpstrFilter = build_filter(pat);
    ofn.lpstrFile   = full;
    ofn.nMaxFile    = sizeof(full);
    ofn.lpstrTitle  = title;
    ofn.lpstrInitialDir = initdir[0] ? initdir : NULL;
    ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    if (is_save) {
        ofn.Flags |= OFN_OVERWRITEPROMPT;
    } else {
        ofn.Flags |= OFN_FILEMUSTEXIST;
    }

    BOOL ok = is_save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
    if (!ok) {
        shim_carry = 1;   /* cancel */
        return;
    }

    char dir[MAX_PATH], base[MAX_PATH];
    if (split_path(full, dir, sizeof(dir), base, sizeof(base)) != 0) {
        show_error("Filename too long for the IMG format (max 12 chars, 8.3).");
        shim_carry = 1;
        return;
    }
    if (strlen(base) > 12) {
        show_error("Filename too long for the IMG format (max 12 chars, 8.3).");
        shim_carry = 1;
        return;
    }

    upcase(base);

    /* Write g_doc->fpath_s (64 bytes, zero-padded), g_doc->fname_s + g_doc->fnametmp_s (13 bytes). */
    {
        size_t n = strlen(dir);
        if (n > 63) n = 63;
        memset(g_doc->fpath_s, 0, 64);
        memcpy(g_doc->fpath_s, dir, n);
    }
    {
        size_t n = strlen(base);
        memset(g_doc->fname_s,    0, 13);
        memset(g_doc->fnametmp_s, 0, 13);
        memcpy(g_doc->fname_s,    base, n);
        memcpy(g_doc->fnametmp_s, base, n);
    }

    /* Save the directory for next time */
    save_last_directory(dir);

    /* Chdir into the selected folder so I21OPENR ("g_doc->fname_s") finds it. */
    _chdir(dir);

    shim_carry = 0;   /* OK */
}

#else  /* non-Windows: return cancel, let caller fall back to in-app browser */

void shim_filereq_impl(void)
{
    shim_carry = 1;
}

#endif
