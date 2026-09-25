/*************************************************************
 * platform/img_util.cpp
 * Small pure IMG helpers declared in img_util.h.
 *************************************************************/
#include "img_util.h"

std::string img_name_string(const IMG *img)
{
    if (!img) return std::string();
    size_t n = 0;
    while (n < sizeof(img->n_s) && img->n_s[n] != '\0') n++;
    return std::string(img->n_s, img->n_s + n);
}

unsigned short signed_to_img_word(int v)
{
    if (v < -32768) v = -32768;
    if (v >  32767) v =  32767;
    return (unsigned short)(short)v;
}

std::string next_sequential_img_name(const std::string &name, int step)
{
    if (step < 1) step = 1;
    size_t base_off = name.find_last_of("\\/:");
    base_off = (base_off == std::string::npos) ? 0 : base_off + 1;
    std::string dir = name.substr(0, base_off);
    std::string base = name.substr(base_off);

    size_t dot = base.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? std::string(".IMG") : base.substr(dot);
    std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);

    size_t digits_at = stem.size();
    while (digits_at > 0 && stem[digits_at - 1] >= '0' && stem[digits_at - 1] <= '9')
        digits_at--;
    std::string prefix = stem.substr(0, digits_at);
    std::string digits = stem.substr(digits_at);

    /* An unnumbered file is implicitly #1 of its series, so its next is 2. */
    long long n = digits.empty() ? 1 : 0;
    for (char c : digits) n = n * 10 + (c - '0');
    n += step;

    std::string next = std::to_string(n);
    /* Keep zero padding: 09 -> 10, 007 -> 008. */
    while (next.size() < digits.size()) next.insert(next.begin(), '0');

    const size_t kMaxStem = 8;
    if (prefix.size() + next.size() > kMaxStem) {
        size_t keep = next.size() < kMaxStem ? kMaxStem - next.size() : 0;
        prefix.resize(keep < prefix.size() ? keep : prefix.size());
    }
    return dir + prefix + next + ext;
}
