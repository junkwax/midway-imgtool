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
