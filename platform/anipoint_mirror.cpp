/*************************************************************
 * platform/anipoint_mirror.cpp
 * Mirror-aware anipoint math declared in anipoint_mirror.h.
 *************************************************************/
#include "anipoint_mirror.h"

#include <cctype>
#include <cstring>

const char *mirror_convention_label(MirrorConvention conv)
{
    return conv == MirrorConvention_Ganiof ? "ganiof (single-part)"
                                           : "ani2 (multipart)";
}

const char *mirror_convention_source(MirrorConvention conv)
{
    return conv == MirrorConvention_Ganiof
               ? "MKDISP.ASM ganiof: anix_eff = (sizex - 1) - anix"
               : "MKUTIL.ASM ani2: anix_eff = sizex - anix";
}

int mirror_anipoint_axis(int a, int size, MirrorConvention conv)
{
    if (size < 1) size = 1;
    return (conv == MirrorConvention_Ganiof) ? (size - 1 - a) : (size - a);
}

int anipoint_effective(int a, int size, bool flipped, MirrorConvention conv)
{
    return flipped ? mirror_anipoint_axis(a, size, conv) : a;
}

float anipoint_center_offset(int a, int size)
{
    if (size < 1) size = 1;
    return (float)a - (float)(size - 1) * 0.5f;
}

float anipoint_drawn_center_from_anchor(int a, int size, bool flipped,
                                        MirrorConvention conv)
{
    /* left_edge = anchor - anix_eff, so centre - anchor = (size-1)/2 - anix_eff. */
    if (size < 1) size = 1;
    int eff = anipoint_effective(a, size, flipped, conv);
    return (float)(size - 1) * 0.5f - (float)eff;
}

bool anipoint_axis_outside(int a, int size)
{
    if (size < 1) size = 1;
    return a < 0 || a > size - 1;
}

int anipoint_axis_slack(int a, int size)
{
    if (size < 1) size = 1;
    if (a < 0) return -a;
    if (a > size - 1) return a - (size - 1);
    return 0;
}

AnipointBoundsReport anipoint_bounds_report(int anix, int aniy,
                                            int sizex, int sizey)
{
    AnipointBoundsReport r;
    r.x_outside = anipoint_axis_outside(anix, sizex);
    r.y_outside = anipoint_axis_outside(aniy, sizey);
    r.x_slack   = anipoint_axis_slack(anix, sizex);
    r.y_slack   = anipoint_axis_slack(aniy, sizey);
    return r;
}

/* Recursive backtracking glob. Sprite names are <= 15 chars and patterns are
   short, so the worst case here is trivially bounded. */
static bool glob_match(const char *n, const char *p)
{
    if (*p == '\0') return *n == '\0';
    if (*p == '*') {
        /* Collapse runs of '*' so "A**B" costs no more than "A*B". */
        while (p[1] == '*') p++;
        for (const char *scan = n;; scan++) {
            if (glob_match(scan, p + 1)) return true;
            if (*scan == '\0') return false;
        }
    }
    if (*n == '\0') return false;
    if (*p != '?' &&
        std::tolower((unsigned char)*p) != std::tolower((unsigned char)*n))
        return false;
    return glob_match(n + 1, p + 1);
}

bool sprite_name_matches_glob(const char *name, const char *pattern)
{
    if (!name || !pattern || !*pattern) return false;

    /* IMG name fields are space-padded and may not be NUL-terminated at the
       field width; the caller passes a bounded buffer, so trim here. */
    char trimmed[64];
    size_t n = 0;
    while (n < sizeof(trimmed) - 1 && name[n] != '\0') {
        trimmed[n] = name[n];
        n++;
    }
    trimmed[n] = '\0';
    while (n > 0 && (trimmed[n - 1] == ' ' || trimmed[n - 1] == '\t'))
        trimmed[--n] = '\0';

    char pat[64];
    size_t m = 0;
    while (m < sizeof(pat) - 1 && pattern[m] != '\0') {
        pat[m] = pattern[m];
        m++;
    }
    pat[m] = '\0';
    while (m > 0 && (pat[m - 1] == ' ' || pat[m - 1] == '\t'))
        pat[--m] = '\0';
    if (m == 0) return false;

    return glob_match(trimmed, pat);
}
