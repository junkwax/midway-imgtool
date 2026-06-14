/*************************************************************
 * platform/anipoint.cpp
 * Anipoint predicates + sprite-name sequence parsing declared in anipoint.h.
 *************************************************************/
#include "anipoint.h"

#include <cctype>

bool secondary_anipoint_words_in_use(unsigned short x,
                                     unsigned short y,
                                     unsigned short z)
{
    if ((short)x < 0 || (short)y < 0) return false;
    return (short)z != -1;
}

bool secondary_anipoint_in_use(const IMG *img)
{
    if (!img) return false;
    return secondary_anipoint_words_in_use(img->anix2, img->aniy2, img->aniz2);
}

void clear_secondary_anipoint(IMG *img)
{
    if (!img) return;
    img->anix2 = (unsigned short)-1;
    img->aniy2 = (unsigned short)-1;
    img->aniz2 = (unsigned short)-1;
}

void activate_secondary_anipoint(IMG *img)
{
    if (!img) return;
    if ((short)img->anix2 < 0) img->anix2 = 0;
    if ((short)img->aniy2 < 0) img->aniy2 = 0;
    if ((short)img->aniz2 == -1) img->aniz2 = 0;
}

std::string trim_sprite_name(std::string s)
{
    while (!s.empty() && std::isspace((unsigned char)s.back()))
        s.pop_back();
    while (!s.empty() && std::isspace((unsigned char)s.front()))
        s.erase(s.begin());
    return s;
}

bool ascii_iequals(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower((unsigned char)a[i]) !=
            std::tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

bool strip_trailing_sequence_digits(const std::string &name,
                                    std::string *stem_out)
{
    std::string s = trim_sprite_name(name);
    size_t pos = s.size();
    while (pos > 0 && std::isdigit((unsigned char)s[pos - 1]))
        pos--;
    if (pos == s.size() || pos == 0) return false;
    if (stem_out) *stem_out = s.substr(0, pos);
    return true;
}

std::string InferSubframeParentName(const char *name)
{
    if (!name || !*name) return std::string();
    std::string s(name);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.size() < 2) return std::string();

    size_t end = s.size();
    size_t suffix = end;
    while (suffix > 0 && std::isalpha((unsigned char)s[suffix - 1]))
        suffix--;
    size_t suffix_len = end - suffix;
    if (suffix_len > 0 && suffix > 0 && std::isdigit((unsigned char)s[suffix - 1])) {
        size_t digits = suffix;
        while (digits > 0 && std::isdigit((unsigned char)s[digits - 1]))
            digits--;

        /* Original Midway libraries overwhelmingly use BASE1A/BASE1B/etc.
           Tool-generated grid chops may use BASE_1A. Keep FLIP and other
           word suffixes out of the visual subframe folders. */
        if (digits > 0 && (s[digits - 1] == '_' || s[digits - 1] == '-'))
            return s.substr(0, digits - 1);
        if (suffix_len == 1)
            return s.substr(0, suffix);
    }

    size_t digits = end;
    while (digits > 0 && std::isdigit((unsigned char)s[digits - 1]))
        digits--;
    if (digits < end && digits > 0) {
        if (s[digits - 1] == '_' || s[digits - 1] == '-')
            return s.substr(0, digits - 1);
        return s.substr(0, digits);
    }

    return std::string();
}
