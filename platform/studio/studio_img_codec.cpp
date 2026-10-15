#include "studio_img_codec.h"
#include <algorithm>
#include <array>
#include <new>

namespace studio {
namespace {
bool near_version(unsigned v) { return v >= 0x60a && v < 0x632; }
bool range(const std::vector<unsigned char> &b, size_t p, size_t n) {
    return p <= b.size() && n <= b.size() - p;
}
template<class T> T at(const std::vector<unsigned char> &b, size_t p) {
    T v{}; memcpy(&v, b.data()+p, sizeof v); return v;
}
template<class T> void put(std::vector<unsigned char> &b, size_t p, const T &v) {
    memcpy(b.data()+p, &v, sizeof v);
}
size_t table_end(const LIB_HDR &h) {
    return (size_t)h.oset + h.imgcnt*sizeof(IMAGE_disk) + (h.palcnt-3)*sizeof(PALETTE_disk);
}
void append(std::vector<unsigned char> &to, const unsigned char *p, size_t n) {
    to.insert(to.end(), p, p+n);
}
void *owned(const unsigned char *p, size_t n) {
    void *v=malloc(n); if (!v) throw std::bad_alloc(); memcpy(v,p,n); return v;
}
}
bool inspect_img(const std::vector<unsigned char> &b, LIB_HDR &h) {
    if (!range(b,0,sizeof h)) return false;
    h=at<LIB_HDR>(b,0); bool near=near_version(h.version);
    if (h.temp!=0xabcd || (!near && (h.version<0x634 || h.version>0x654)) ||
        !h.imgcnt || h.imgcnt>2000 || h.palcnt<3 || h.palcnt>4096 || h.oset<sizeof h) return false;
    size_t records=h.imgcnt*sizeof(IMAGE_disk)+(h.palcnt-3)*sizeof(PALETTE_disk);
    if (!range(b,h.oset,records)) return false;
    size_t points=0,alts=0,decoded=0;
    for (unsigned i=0;i<h.imgcnt;++i) {
        auto d=at<IMAGE_disk>(b,h.oset+i*sizeof(IMAGE_disk));
        if (!d.w || !d.h || d.palnum<3 || d.palnum>=h.palcnt) return false;
        size_t stride=((size_t)d.w+3)&~size_t(3); decoded+=stride*d.h;
        if (decoded>128*1024*1024) return false;
        if (d.flags&0x80) {
            size_t p=d.oset;
            for (unsigned y=0;y<d.h;++y) {
                if (!range(b,p,1)) return false;
                unsigned c=b[p++],skip=(c&15)*(1u<<((d.flags>>8)&3))+(c>>4)*(1u<<((d.flags>>10)&3));
                if (skip>d.w || !range(b,p,d.w-skip)) return false;
                p+=d.w-skip;
            }
        } else if (!range(b,d.oset,stride*d.h)) return false;
        // Near: FRM at +42, Z at +44, point at +46, other palettes at +48.
        // The later port's empirical far layout has the last two fields reversed.
        if ((short)(near?d.opals:d.pttblnum)>=0) ++points;
        if (h.version>=0x61d && (short)(near?d.pttblnum:d.opals)>=0) ++alts;
    }
    for (unsigned i=0;i<h.palcnt-3;++i) {
        auto d=at<PALETTE_disk>(b,h.oset+h.imgcnt*sizeof(IMAGE_disk)+i*sizeof(PALETTE_disk));
        if (!d.numc || d.numc>256 || !range(b,d.oset,d.numc*2)) return false;
    }
    size_t pos=table_end(h);
    const size_t sequence_start=pos;
    int count=std::max(0,(int)(short)h.seqcnt)+std::max(0,(int)(short)h.scrcnt);
    for (int i=0;i<count;++i) {
        if (!range(b,pos,20)) return false;
        auto n=at<unsigned short>(b,pos+18);
        size_t nbytes=(near?58:98)+(size_t)n*(near?16:18);
        if (!range(b,pos,nbytes)) return false;
        pos+=nbytes;
        if (pos-sequence_start>32*1024*1024) return false;
    }
    size_t damage=near?0:std::max(0,(int)(short)h.damcnt)*4;
    size_t tail=points*40+alts*16+damage;
    // A near adapter must account for every byte; an unrecognized tail cannot
    // safely be moved past resized animation records.
    return range(b,pos,tail) && (!near || pos+tail==b.size());
}
bool normalize_img(const std::vector<unsigned char> &source,std::vector<unsigned char> &modern,std::string &error) {
    LIB_HDR h{};
    if (!inspect_img(source,h)) { error="This IMG is truncated, invalid, or uses an unsupported layout (supported: 0x60a–0x631 and 0x634–0x654)."; return false; }
    modern=source;
    if (!near_version(h.version)) return true;
    size_t end=table_end(h),pos=end;
    modern.resize(end);
    for (unsigned i=0;i<h.imgcnt;++i) {
        size_t p=h.oset+i*sizeof(IMAGE_disk); auto d=at<IMAGE_disk>(source,p);
        std::swap(d.aniz2,d.frm); std::swap(d.opals,d.pttblnum);
        if (h.version<0x61d) d.opals=0xffff;
        put(modern,p,d);
    }
    int count=std::max(0,(int)(short)h.seqcnt)+std::max(0,(int)(short)h.scrcnt);
    for (int i=0;i<count;++i) {
        const auto *p=source.data()+pos; unsigned n=p[18]|(p[19]<<8);
        std::array<unsigned char,98> record{}; memcpy(record.data(),p,20);
        for (int e=0;e<16;++e) memcpy(record.data()+20+e*4,p+20+e*2,2);
        memcpy(record.data()+84,p+52,4); memset(record.data()+88,0xff,6);
        memcpy(record.data()+96,p+56,2); append(modern,record.data(),record.size()); pos+=58;
        for (unsigned e=0;e<n;++e) {
            std::array<unsigned char,18> entry{}; memcpy(entry.data(),source.data()+pos,2);
            memcpy(entry.data()+4,source.data()+pos+2,14); append(modern,entry.data(),entry.size()); pos+=16;
        }
    }
    append(modern,source.data()+pos,source.size()-pos);
    h.version=0x634; h.damcnt=0;
    h.seqcnt=(unsigned short)std::max(0,(int)(short)h.seqcnt);h.scrcnt=(unsigned short)std::max(0,(int)(short)h.scrcnt);
    put(modern,0,h);
    LIB_HDR checked{};
    if (!inspect_img(modern,checked)) { modern.clear(); error="The legacy IMG conversion did not pass validation."; return false; }
    error.clear(); return true;
}
bool project_img_layout(std::vector<unsigned char> &modern,unsigned version,std::string &error) {
    if(!near_version(version)&&(version<0x634||version>0x654)){error="Unsupported IMG output layout.";return false;}
    LIB_HDR h{};
    if (!inspect_img(modern,h) || h.version<0x634) {error="The saved IMG did not pass validation.";return false;}
    if (!near_version(version)) return true;
    if (h.damcnt && (short)h.damcnt>0) {error="Damage tables cannot be stored in this IMG's original format. Remove them before saving.";return false;}
    auto output=modern;size_t end=table_end(h),pos=end;output.resize(end);
    for (unsigned i=0;i<h.imgcnt;++i) {
        size_t p=h.oset+i*sizeof(IMAGE_disk); auto d=at<IMAGE_disk>(modern,p);
        if (version<0x61d && (short)d.opals>=0) {error="Alternate palettes cannot be stored in this IMG's original format. Remove them before saving.";return false;}
        std::swap(d.aniz2,d.frm); std::swap(d.opals,d.pttblnum); put(output,p,d);
    }
    int count=std::max(0,(int)(short)h.seqcnt)+std::max(0,(int)(short)h.scrcnt);
    for (int i=0;i<count;++i) {
        const auto *p=modern.data()+pos;unsigned n=p[18]|(p[19]<<8);
        std::array<unsigned char,58> record{};memcpy(record.data(),p,20);
        for (int e=0;e<16;++e) {
            if (p[22+e*4] || p[23+e*4]) {error="A far animation pointer cannot fit the original IMG layout. This IMG must retain its original format.";return false;}
            memcpy(record.data()+20+e*2,p+20+e*4,2);
        }
        if (std::any_of(p+88,p+94,[](unsigned char v){return v!=0xff;}) || p[94] || p[95]) {
            error="Animation damage metadata cannot fit the original IMG layout. This IMG must retain its original format.";return false;
        }
        memcpy(record.data()+52,p+84,4);memcpy(record.data()+56,p+96,2);
        append(output,record.data(),record.size());pos+=98;
        for (unsigned e=0;e<n;++e) {
            p=modern.data()+pos;
            if (p[2] || p[3]) {error="A far entry pointer cannot fit the original IMG layout. This IMG must retain its original format.";return false;}
            append(output,p,2);append(output,p+4,14);pos+=18;
        }
    }
    append(output,modern.data()+pos,modern.size()-pos);h.version=(unsigned short)version;put(output,0,h);
    LIB_HDR checked{};
    if (!inspect_img(output,checked)) {error="The original-layout save did not pass validation.";return false;}
    modern=std::move(output);error.clear();return true;
}
bool load_img_snapshot(const std::vector<unsigned char> &b,Document &out,std::string &error) {
    LIB_HDR h{};
    if (!inspect_img(b,h) || h.version<0x634 || out.img_p || out.pal_p || out.scrseqmem_p || out.damtbl_p) {error="Invalid IMG snapshot or nonempty destination.";return false;}
    struct Scope {Document *old;Scope(Document &d):old(g_doc){g_doc=&d;}~Scope(){g_doc=old;}} scope(out);
    out.fileversion=h.version;memcpy(out.file_bufscr,h.bufscr,4);
    out.file_spare1=h.spare1;out.file_spare2=h.spare2;out.file_spare3=h.spare3;
    out.seqcnt=std::max(0,(int)(short)h.seqcnt);out.scrcnt=std::max(0,(int)(short)h.scrcnt);
    size_t pos=table_end(h),start=pos;
    for (unsigned i=0;i<out.seqcnt+out.scrcnt;++i) pos+=98+(size_t)at<unsigned short>(b,pos+18)*18;
    out.scrseqbytes=(unsigned)(pos-start);
    if (out.scrseqbytes) out.scrseqmem_p=owned(b.data()+start,out.scrseqbytes);
    size_t points=0;
    for (unsigned i=0;i<h.imgcnt;++i) if ((short)at<IMAGE_disk>(b,h.oset+i*sizeof(IMAGE_disk)).pttblnum>=0) ++points;
    size_t ptpos=pos,altpos=pos+points*40;
    for (unsigned i=0;i<h.imgcnt;++i) {
        auto d=at<IMAGE_disk>(b,h.oset+i*sizeof(IMAGE_disk)); auto im=AllocImg();if(!im)throw std::bad_alloc();
        memcpy(im->n_s,d.n_s,15);im->n_s[15]=0;memcpy(im->file_name_raw,d.n_s,16);
        im->flags=d.flags;im->anix=d.anix;im->aniy=d.aniy;im->w=d.w;im->h=d.h;im->palnum=d.palnum-3;
        im->anix2=d.anix2;im->aniy2=d.aniy2;im->aniz2=d.aniz2;im->opals=d.opals;
        im->file_oset=d.oset;im->file_data=d.data;im->file_lib=d.lib;im->file_frm=d.frm;im->file_pttblnum=d.pttblnum;
        size_t stride=((size_t)d.w+3)&~size_t(3),sz=stride*d.h;
        im->data_p=calloc(1,sz);if(!im->data_p)throw std::bad_alloc();
        if (d.flags&0x80) {
            size_t p=d.oset;auto dst=(unsigned char *)im->data_p;
            for (unsigned y=0;y<d.h;++y) {
                unsigned c=b[p++],left=(c&15)*(1u<<((d.flags>>8)&3)),right=(c>>4)*(1u<<((d.flags>>10)&3));
                memcpy(dst+y*stride+left,b.data()+p,d.w-left-right);p+=d.w-left-right;
            }
        } else memcpy(im->data_p,b.data()+d.oset,sz);
        im->baseline_p=owned((unsigned char *)im->data_p,sz);im->baseline_w=d.w;im->baseline_h=d.h;
        if ((short)d.pttblnum>=0) {im->pttbl_p=owned(b.data()+ptpos,40);ptpos+=40;}
        if ((short)d.opals>=0) {im->opaltbl_p=owned(b.data()+altpos,16);altpos+=16;}
    }
    for (unsigned i=0;i<h.palcnt-3;++i) {
        auto d=at<PALETTE_disk>(b,h.oset+h.imgcnt*sizeof(IMAGE_disk)+i*sizeof(PALETTE_disk));
        auto pal=AllocPal();if(!pal)throw std::bad_alloc();memcpy(pal->n_s,d.n_s,9);pal->n_s[9]=0;
        memcpy(pal->file_name_raw,d.n_s,10);pal->flags=d.flags;pal->bitspix=d.bitspix;pal->numc=d.numc;
        pal->file_data=d.data;pal->file_lib=d.lib;pal->file_colind=d.colind;pal->file_cmap=d.cmap;pal->file_spare=d.spare;
        pal->data_p=owned(b.data()+d.oset,d.numc*2);
    }
    out.damcnt=std::max(0,(int)(short)h.damcnt);out.damtblbytes=out.damcnt*4;
    if (out.damtblbytes)out.damtbl_p=owned(b.data()+altpos,out.damtblbytes);
    out.ilselected=0;out.ilpalloaded=-1;error.clear();return true;
}
}
