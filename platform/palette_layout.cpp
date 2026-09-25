/*************************************************************
 * platform/palette_layout.cpp
 * Saved palette layouts; see palette_layout.h.
 *************************************************************/
#include "palette_layout.h"

#include <cstdlib>

namespace {

char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

} /* namespace */

std::string CleanLayoutField(const std::string &s, size_t max_len)
{
    std::string out;
    for (char c : s)
        if (c != '|' && c != ';' && c != '=' && c != ',' && c != '\r' && c != '\n') out += c;
    const size_t a = out.find_first_not_of(" \t");
    if (a == std::string::npos) return std::string();
    out = out.substr(a, out.find_last_not_of(" \t") - a + 1);
    if (out.size() > max_len) out.resize(max_len);
    return out;
}

bool ParsePaletteLayoutLine(const char *line, PaletteLayout *out)
{
    if (!line || !out) return false;
    std::string s(line);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    const std::vector<std::string> f = split(s, '|');
    if (f.size() != 4) return false;
    PaletteLayout l;
    l.name = CleanLayoutField(f[0], 31);
    if (l.name.empty()) return false;
    l.numc = std::atoi(f[1].c_str());
    for (const std::string &p : split(f[2], ',')) {
        const std::string n = CleanLayoutField(p, 9);
        if (!n.empty()) l.palettes.push_back(n);
    }
    for (const std::string &sec : split(f[3], ';')) {
        const size_t eq = sec.find('=');
        if (eq == std::string::npos) continue;
        PaletteLayoutSection ps;
        ps.name = CleanLayoutField(sec.substr(0, eq), 15);
        ps.ranges = sec.substr(eq + 1);
        const size_t a = ps.ranges.find_first_not_of(' ');
        ps.ranges = a == std::string::npos ? std::string() : ps.ranges.substr(a);
        if (!ps.name.empty()) l.sections.push_back(ps);
    }
    *out = l;
    return true;
}

std::string FormatPaletteLayoutLine(const PaletteLayout &layout)
{
    std::string s = CleanLayoutField(layout.name, 31) + "|" + std::to_string(layout.numc) + "|";
    bool first = true;
    for (const std::string &p : layout.palettes) {
        const std::string n = CleanLayoutField(p, 9);
        if (n.empty()) continue;
        s += (first ? "" : ",") + n;
        first = false;
    }
    s += "|";
    first = true;
    for (const PaletteLayoutSection &sec : layout.sections) {
        const std::string n = CleanLayoutField(sec.name, 15);
        if (n.empty()) continue;
        std::string r;
        for (char c : sec.ranges)            /* ranges keep ',' but never '|' ';' '=' */
            if (c != '|' && c != ';' && c != '=' && c != '\r' && c != '\n') r += c;
        s += (first ? "" : ";") + n + "=" + r;
        first = false;
    }
    return s;
}

bool SameLayoutName(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (up(a[i]) != up(b[i])) return false;
    return true;
}

int FindLayoutForPalette(const std::vector<PaletteLayout> &layouts, const char *palette, int numc)
{
    if (!palette || !palette[0]) return -1;
    for (size_t i = 0; i < layouts.size(); i++) {
        if (layouts[i].numc > 0 && numc > 0 && layouts[i].numc != numc) continue;
        for (const std::string &p : layouts[i].palettes)
            if (SameLayoutName(p, palette)) return (int)i;
    }
    return -1;
}
