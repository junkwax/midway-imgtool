#include "studio_document.h"
#include "studio_layers.h"
#include "studio_selection.h"
#include "studio_img_codec.h"
#include "img_io.h"
#include "palette_math.h"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <chrono>
#include "anipoint.h"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

// Only the format adapter borrows the classic active-document pointer. It is
// called synchronously on the UI thread and always restores its previous value.
namespace {
struct CodecScope {
    Document *previous;
    explicit CodecScope(Document &doc) : previous(g_doc) { g_doc = &doc; }
    ~CodecScope() { g_doc = previous; }
};
// The shared writer uses these fields as scratch. Saving (including a failed
// layout serialization) must not introduce untracked changes into Studio history.
struct WriterScratch {
    struct Image { IMG *p; void *temp; unsigned short alternate,point; };
    struct Palette { PAL *p; void *temp; };
    std::vector<Image> images;
    std::vector<Palette> palettes;
    Document &document;
    unsigned original_version;
    explicit WriterScratch(Document &d, unsigned source_version) : document(d), original_version(d.fileversion) {
        for(auto p=(IMG *)d.img_p;p;p=(IMG *)p->nxt_p)images.push_back({p,p->temp,p->opals,p->file_pttblnum});
        for(auto p=(PAL *)d.pal_p;p;p=(PAL *)p->nxt_p)palettes.push_back({p,p->temp});
        d.fileversion=source_version<0x634?0x634:source_version;
    }
    ~WriterScratch() {
        document.fileversion=original_version;
        for(auto &v:images){v.p->temp=v.temp;v.p->opals=v.alternate;v.p->file_pttblnum=v.point;}
        for(auto &v:palettes)v.p->temp=v.temp;
    }
};
bool read_bytes(const std::string &path, std::vector<unsigned char> &out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    auto n = f.tellg();
    if (n < 0 || n > 256 * 1024 * 1024) return false;
    out.resize((size_t)n);
    f.seekg(0);
    return out.empty() || bool(f.read((char *)out.data(), out.size()));
}
template<class T> bool read_at(const std::vector<unsigned char> &b, size_t p, T &v) {
    if (p > b.size() || sizeof(T) > b.size() - p) return false;
    memcpy(&v, b.data() + p, sizeof(T)); return true;
}
bool reserve_temp(const std::filesystem::path &p) {
#ifdef _WIN32
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h); return true;
#else
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    close(fd); return true;
#endif
}
}

// Legacy import/mutation entry points are deliberately unavailable in Studio.
// Load/save do not invoke these hooks. Fail closed if future code calls one.
void undo_push() { throw std::logic_error("Legacy mutation requires a Studio command"); }
bool doc_undo_push() { return false; }

namespace studio {
Artwork::Artwork() { data.fileversion = 0x634; data.ilselected = -1; memset(data.file_bufscr,0xff,sizeof data.file_bufscr); }
Artwork::~Artwork() { document_clear_contents(&data); }
IMG *Artwork::sprite(int index) const {
    if (index < 0) return nullptr;
    IMG *p = (IMG *)data.img_p;
    while (p && index--) p = (IMG *)p->nxt_p;
    return p;
}
PAL *Artwork::palette(int index) const {
    if (index < 0) return nullptr;
    PAL *p = (PAL *)data.pal_p;
    while (p && index--) p = (PAL *)p->nxt_p;
    return p;
}
std::unique_ptr<Artwork> Artwork::open(const std::string &path, std::string &error) {
    error.clear();
    auto d = std::make_unique<Artwork>();
    LIB_HDR header{};
    if (!read_bytes(path, d->disk_baseline)) { error = "Could not read this file (limit: 256 MiB)."; return {}; }
    std::vector<unsigned char> normalized;
    if (!normalize_img(d->disk_baseline, normalized, error)) return {};
    read_at(d->disk_baseline,0,header);
    d->source_version=header.version;
    d->source_header=header;
    if (!load_img_snapshot(normalized,d->data,error)) return {};
    if (d->data.imgcnt != header.imgcnt || d->data.palcnt != (unsigned)(header.palcnt - 3)) {
        error = "IMG loading did not complete. No open document was changed."; return {};
    }
    for (IMG *im = (IMG *)d->data.img_p; im; im = (IMG *)im->nxt_p) {
        if (!im->data_p || !im->baseline_p) { error = "Not enough memory to load this IMG."; return {}; }
    }
    d->file_path = std::filesystem::absolute(std::filesystem::u8path(path)).lexically_normal().u8string();
    d->assign_initial_ids();
    if(header.version<0x61d)for(unsigned i=0;i<header.imgcnt;++i){
        IMAGE_disk record{};read_at(d->disk_baseline,header.oset+i*sizeof record,record);
        d->legacy_reserved_alternates[d->sprite_id(i)]=record.pttblnum;
    }
    return d;
}
bool Artwork::save(const std::string &path, std::string &error) {
    commit(); error.clear();
    std::error_code ec;
    auto dest = std::filesystem::absolute(std::filesystem::u8path(path), ec).lexically_normal();
    if (ec || dest.filename().empty()) { error = "Choose an IMG filename."; return false; }
    if (!file_path.empty() && std::filesystem::equivalent(dest, std::filesystem::u8path(file_path), ec)) {
        std::vector<unsigned char> current;
        if (!read_bytes(file_path, current) || current != disk_baseline) {
            error = "This file changed on disk. Save As a new file, or reopen it before overwriting."; return false;
        }
    }
    static unsigned counter = 0;
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temp = dest;
    temp += ".studio-" + std::to_string(stamp) + "-" + std::to_string(++counter) + ".tmp";
    if (!reserve_temp(temp)) { error = "Cannot create a temporary file in the destination folder."; return false; }
    bool ok=false;
    try {
        LayerCompositeScope layers(data);
        CodecScope scope(data);
        WriterScratch scratch(data,source_version);
        ok = SaveImgFileToPath(temp.u8string().c_str(), &error);
    } catch(const std::exception &e) { error=std::string("Could not prepare IMG save: ")+e.what(); }
    std::vector<unsigned char> written;
    LIB_HDR h{};
    if (ok && (!read_bytes(temp.u8string(), written) || !inspect_img(written, h))) {
        ok = false; error = "The saved IMG did not pass validation. The destination was left intact.";
    }
    if (ok && source_version<0x634) {
        ok=project_img_layout(written,source_version,error);
        if(ok){
            LIB_HDR original{};read_at(written,0,original);
            // Before damage/alternate support these words are opaque reserves.
            // Empty sequence/script pools also commonly use negative sentinels.
            original.damcnt=source_header.damcnt;
            if(!original.seqcnt&&(short)source_header.seqcnt<0)original.seqcnt=source_header.seqcnt;
            if(!original.scrcnt&&(short)source_header.scrcnt<0)original.scrcnt=source_header.scrcnt;
            memcpy(written.data(),&original,sizeof original);
            if(source_version<0x61d)for(int i=0;i<count();++i){
                auto found=legacy_reserved_alternates.find(sprite_id(i));
                if(found!=legacy_reserved_alternates.end())memcpy(written.data()+original.oset+i*sizeof(IMAGE_disk)+48,&found->second,2);
            }
        }
        if(ok) { std::ofstream f(temp,std::ios::binary|std::ios::trunc);
            ok=bool(f.write((const char *)written.data(),written.size()));f.close();ok=ok&&!f.fail();
            if(!ok)error="Could not write the original IMG layout. The destination was left intact.";
        }
    }
    if(ok&&(!inspect_img(written,h)||h.version!=source_version)){
        ok=false;error="Saving would change this IMG's original format. The destination was left intact.";
    }
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok) error = "Could not replace the destination file (it may be open or read-only).";
#else
        std::filesystem::rename(temp, dest, ec);
        ok = !ec; if (!ok) error = ec.message();
#endif
    }
    if (!ok) { std::filesystem::remove(temp, ec); return false; }
    file_path = dest.u8string(); disk_baseline = std::move(written); saved_revision = revision;
    read_at(disk_baseline,0,source_header);
    return true;
}
bool Artwork::disk_matches() const {std::vector<unsigned char> current;return !file_path.empty()&&read_bytes(file_path,current)&&current==disk_baseline;}
Artwork::State Artwork::capture() const {
    State s; s.revision = revision;s.selection=selected_sprite_id;
    for (int i = 0; i < count(); ++i) {
        IMG *im = sprite(i); auto p = (unsigned char *)im->data_p;
        State::Sprite item;item.fields=*im;item.id=sprite_id(i);
        item.fields.nxt_p=item.fields.data_p=item.fields.baseline_p=item.fields.pttbl_p=item.fields.opaltbl_p=item.fields.temp=item.fields.layer_p=nullptr;
        item.pixels.assign(p,p+(((size_t)im->w+3)&~size_t(3))*im->h);
        auto copy_blob=[](void *from,size_t bytes,std::vector<unsigned char> &out){if(from)out.assign((unsigned char *)from,(unsigned char *)from+bytes);};
        copy_blob(im->baseline_p,(((size_t)im->baseline_w+3)&~size_t(3))*im->baseline_h,item.baseline);
        copy_blob(im->pttbl_p,40,item.points);copy_blob(im->opaltbl_p,16,item.alternate);copy_blob(im->layer_p,checked_layer_bytes(*im),item.layer);
        s.sprites.push_back(std::move(item));
    }
    for (int i = 0; i < palette_count(); ++i) {
        PAL *pal = palette(i); auto p = (unsigned char *)pal->data_p;
        State::Palette item;item.fields=*pal;item.fields.nxt_p=item.fields.data_p=item.fields.temp=nullptr;
        item.colors.assign(p,p+pal->numc*2);s.palettes.push_back(std::move(item));
    }
    s.seqcnt=data.seqcnt;s.scrcnt=data.scrcnt;
    if(data.scrseqmem_p)s.sequences.assign((unsigned char *)data.scrseqmem_p,(unsigned char *)data.scrseqmem_p+data.scrseqbytes);
    return s;
}
size_t Artwork::State::bytes() const {
    size_t n=sequences.size();for(auto &v:sprites)n+=sizeof(v.fields)+v.pixels.size()+v.baseline.size()+v.points.size()+v.alternate.size()+v.layer.size();
    for(auto &v:palettes)n+=sizeof(v.fields)+v.colors.size();return n;
}
bool Artwork::State::same(const State &s) const {
    if(sprites.size()!=s.sprites.size()||palettes.size()!=s.palettes.size()||sequences!=s.sequences||seqcnt!=s.seqcnt||scrcnt!=s.scrcnt)return false;
    for(size_t i=0;i<sprites.size();++i){auto &a=sprites[i];auto &b=s.sprites[i];
        if(a.id!=b.id||memcmp(&a.fields,&b.fields,sizeof(IMG))||a.pixels!=b.pixels||a.points!=b.points||a.alternate!=b.alternate||a.layer!=b.layer)return false;
    }
    for(size_t i=0;i<palettes.size();++i)if(memcmp(&palettes[i].fields,&s.palettes[i].fields,sizeof(PAL))||palettes[i].colors!=s.palettes[i].colors)return false;
    return true;
}
void Artwork::restore(const State &s) {
    bool structural=s.sprites.size()!=sprite_ids.size()||s.palettes.size()!=(size_t)palette_count();
    if(!structural)for(size_t i=0;i<s.sprites.size();++i){auto im=sprite((int)i);auto &item=s.sprites[i];
        if(item.id!=sprite_ids[i]||item.fields.w!=im->w||item.fields.h!=im->h||memcmp(item.fields.n_s,im->n_s,16)){structural=true;break;}
    }
    // Build a complete replacement first. Allocation failure leaves live data intact.
    auto replacement=std::make_unique<Artwork>();
    auto allocate=[](const std::vector<unsigned char> &bytes)->void *{if(bytes.empty())return nullptr;void *p=malloc(bytes.size());if(!p)throw std::bad_alloc();memcpy(p,bytes.data(),bytes.size());return p;};
    {CodecScope scope(replacement->data);
        for(auto &item:s.sprites){IMG *im=AllocImg();if(!im)throw std::bad_alloc();void *next=im->nxt_p;*im=item.fields;im->nxt_p=next;
            im->data_p=allocate(item.pixels);im->baseline_p=allocate(item.baseline);im->pttbl_p=allocate(item.points);im->opaltbl_p=allocate(item.alternate);im->layer_p=allocate(item.layer);
        }
        for(auto &item:s.palettes){PAL *pal=AllocPal();if(!pal)throw std::bad_alloc();void *next=pal->nxt_p;*pal=item.fields;pal->nxt_p=next;pal->data_p=allocate(item.colors);}
        replacement->data.scrseqmem_p=allocate(s.sequences);replacement->data.scrseqbytes=(unsigned)s.sequences.size();
    }
    std::swap(data.img_p,replacement->data.img_p);std::swap(data.imgcnt,replacement->data.imgcnt);
    std::swap(data.pal_p,replacement->data.pal_p);std::swap(data.palcnt,replacement->data.palcnt);
    std::swap(data.scrseqmem_p,replacement->data.scrseqmem_p);std::swap(data.scrseqbytes,replacement->data.scrseqbytes);
    data.seqcnt=s.seqcnt;data.scrcnt=s.scrcnt;
    sprite_ids.clear();for(auto &item:s.sprites)sprite_ids.push_back(item.id);
    revision = s.revision;selected_sprite_id=s.selection; ++serial;if(structural)++structure_serial;
}
void Artwork::begin(const std::string &label) {
    if (!pending) { pending = std::make_unique<State>(capture()); pending->label = label; }
}
void Artwork::commit() {
    if (!pending) return;
    State now = capture();
    if (!now.same(*pending)) {
        past.push_back(std::move(*pending)); future.clear(); revision = next_revision++; ++serial;
        trim_history();
    }
    pending.reset();
}
void Artwork::trim_history(){
    size_t bytes=0;for(auto &s:past)bytes+=s.bytes();
    while(past.size()>1&&(past.size()>64||bytes>128*1024*1024)){bytes-=past.front().bytes();past.erase(past.begin());}
}
void Artwork::cancel() { if (pending) { restore(*pending); pending.reset(); } }
void Artwork::undo() {
    commit(); if (past.empty()) return;
    State now = capture(); now.label = past.back().label; future.push_back(std::move(now));
    restore(past.back()); past.pop_back();
}
void Artwork::redo() {
    commit(); if (future.empty()) return;
    State now = capture(); now.label = future.back().label; past.push_back(std::move(now));
    restore(future.back()); future.pop_back();
}
const char *Artwork::undo_label() const { return past.empty() ? "Undo" : past.back().label.c_str(); }
void Artwork::paint(int index, int x, int y, unsigned char value, int radius,const PixelSelection *selection) {
    IMG *im = sprite(index); if (!im || !palette(im->palnum) || value >= palette(im->palnum)->numc) return;
    if(x < -32 || y < -32 || x > (int)im->w+32 || y > (int)im->h+32)return;
    if(selection&&!selection->fits(im->w,im->h))return;
    begin(value ? "Paint pixels" : "Erase pixels");
    int stride = (im->w + 3) & ~3; auto p = (unsigned char *)im->data_p;
    radius = std::clamp(radius, 1, 32);
    for (int yy = std::max(0, y - radius + 1); yy <= std::min((int)im->h - 1, y + radius - 1); ++yy)
        for (int xx = std::max(0, x - radius + 1); xx <= std::min((int)im->w - 1, x + radius - 1); ++xx)
            if ((xx-x)*(xx-x)+(yy-y)*(yy-y) < radius*radius&&(!selection||!selection->valid||selection->contains(xx,yy))) p[yy*stride+xx] = value;
    ++serial;
}
void Artwork::fill(int index, int x, int y, unsigned char value,const PixelSelection *selection) {
    IMG *im = sprite(index); if (!im || !palette(im->palnum)||x < 0 || y < 0 || x >= im->w || y >= im->h || value >= palette(im->palnum)->numc) return;
    if(selection&&(!selection->fits(im->w,im->h)||(selection->valid&&!selection->contains(x,y))))return;
    int stride = (im->w + 3) & ~3; auto p = (unsigned char *)im->data_p; unsigned char old = p[y*stride+x];
    if (old == value) return;
    begin("Fill connected pixels");
    std::vector<std::pair<int,int>> todo{{x,y}}; p[y*stride+x] = value;
    while (!todo.empty()) {
        auto pt = todo.back(); todo.pop_back();
        const int dx[] = {-1,1,0,0}, dy[] = {0,0,-1,1};
        for (int k=0;k<4;++k) { int nx=pt.first+dx[k], ny=pt.second+dy[k];
            if(nx>=0 && ny>=0 && nx<im->w && ny<im->h && p[ny*stride+nx]==old&&(!selection||!selection->valid||selection->contains(nx,ny))) {
                p[ny*stride+nx]=value; todo.emplace_back(nx,ny);
            }
        }
    }
    commit();
}
void Artwork::anchor(int index, int x, int y) {
    IMG *im = sprite(index); if (!im) return; begin("Move animation anchor");
    im->anix = (unsigned short)std::clamp(x,-32768,32767); im->aniy = (unsigned short)std::clamp(y,-32768,32767); ++serial;
}
void Artwork::color(int pi, int slot, unsigned short value) {
    PAL *p = palette(pi); if (!p || slot <= 0 || slot >= p->numc) return;
    begin("Edit shared palette color"); auto bytes=(unsigned char *)p->data_p;
    value &= 0x7fff; bytes[slot*2]=(unsigned char)value; bytes[slot*2+1]=(unsigned char)(value>>8); ++serial;
}
Clipboard Artwork::copy(int index, int x, int y, int w, int h) const {
    Clipboard c; IMG *im=sprite(index); if(!im) return c;
    x=std::clamp(x,0,(int)im->w); y=std::clamp(y,0,(int)im->h);
    c.w=std::clamp(w,0,(int)im->w-x); c.h=std::clamp(h,0,(int)im->h-y);
    c.pixels.resize((size_t)c.w*c.h); int stride=(im->w+3)&~3;
    for(int yy=0;yy<c.h;++yy) memcpy(c.pixels.data()+yy*c.w,(unsigned char *)im->data_p+(y+yy)*stride+x,c.w);
    PAL *pal=palette(im->palnum); for(int i=0;i<pal->numc;++i) c.colors.push_back(palette_word_at((unsigned char *)pal->data_p,i));
    return c;
}
void Artwork::paste(int index,const Clipboard &c,int x,int y) {
    IMG *im=sprite(index); if(!im||c.w<1||c.h<1||c.w>8192||c.h>8192||(size_t)c.w*c.h>4*1024*1024||c.pixels.size()!=(size_t)c.w*c.h||c.colors.empty()||c.colors.size()>256||(!c.mask.empty()&&c.mask.size()!=c.pixels.size()))return;
    PAL *pal=palette(im->palnum);if(!pal||!pal->data_p||pal->numc<1||pal->numc>256)return;unsigned char map[256]={};
    for(size_t i=0;i<c.pixels.size();++i)if((c.mask.empty()||c.mask[i])&&c.pixels[i]>=c.colors.size())return;
    bool same=c.colors.size()==pal->numc;
    for(size_t i=0;same && i<c.colors.size();++i) same=c.colors[i]==palette_word_at((unsigned char *)pal->data_p,(int)i);
    for(size_t i=1;i<c.colors.size();++i) map[i]=same?(unsigned char)i:nearest_palette_index_for_word(c.colors[i],pal);
    begin("Paste pixels"); int stride=(im->w+3)&~3;
    for(int yy=0;yy<c.h;++yy) for(int xx=0;xx<c.w;++xx) {
        long long dx=(long long)x+xx,dy=(long long)y+yy;size_t at=(size_t)yy*c.w+xx;auto v=c.pixels[at];
        if((c.mask.empty()?v!=0:c.mask[at]!=0)&&dx>=0&&dy>=0&&dx<im->w&&dy<im->h)((unsigned char *)im->data_p)[(size_t)dy*stride+(size_t)dx]=map[v];
    }
    commit();
}
Clipboard Artwork::copy_selection(int index,const PixelSelection &s)const{
    auto im=sprite(index);if(!im||!s.valid||!s.fits(im->w,im->h))return {};
    auto c=copy(index,s.x,s.y,s.w,s.h);c.mask.resize(c.pixels.size());
    for(int y=0;y<s.h;++y)for(int x=0;x<s.w;++x)c.mask[(size_t)y*s.w+x]=s.contains(s.x+x,s.y+y)?1:0;
    return c;
}
bool Artwork::clear_selection(int index,const PixelSelection &s,std::string &error){
    auto im=sprite(index);if(!im||!s.valid||!s.fits(im->w,im->h)){error="Select pixels on this sprite first.";return false;}
    begin("Clear selected pixels");int stride=(im->w+3)&~3;auto p=(unsigned char *)im->data_p;
    for(int y=s.y;y<s.y+s.h;++y)for(int x=s.x;x<s.x+s.w;++x)if(s.contains(x,y))p[y*stride+x]=0;
    commit();error.clear();return true;
}
bool Artwork::move_selection(int index,const PixelSelection &s,int dx,int dy,std::string &error){
    auto im=sprite(index);if(!im||!s.valid||!s.fits(im->w,im->h)){error="Select pixels on this sprite first.";return false;}
    long long x=(long long)s.x+dx,y=(long long)s.y+dy;
    if(x<0||y<0||x+s.w>im->w||y+s.h>im->h){error="The selection would leave the sprite. Increase the canvas size first.";return false;}
    if(!dx&&!dy){error.clear();return true;}
    auto c=copy_selection(index,s);begin("Move selected pixels");int stride=(im->w+3)&~3;auto p=(unsigned char *)im->data_p;
    for(int yy=0;yy<c.h;++yy)for(int xx=0;xx<c.w;++xx)if(c.mask[(size_t)yy*c.w+xx])p[(s.y+yy)*stride+s.x+xx]=0;
    for(int yy=0;yy<c.h;++yy)for(int xx=0;xx<c.w;++xx)if(c.mask[(size_t)yy*c.w+xx])p[(size_t)(y+yy)*stride+(size_t)x+xx]=c.pixels[(size_t)yy*c.w+xx];
    commit();error.clear();return true;
}
void Artwork::clear(int index,int x,int y,int w,int h) {
    IMG *im=sprite(index); if(!im) return; begin("Clear selected pixels"); int stride=(im->w+3)&~3;
    for(int yy=std::max(0,y);yy<std::min((int)im->h,y+h);++yy)
        for(int xx=std::max(0,x);xx<std::min((int)im->w,x+w);++xx) ((unsigned char *)im->data_p)[yy*stride+xx]=0;
    commit();
}
L2Report Artwork::check(int ppp) { LayerCompositeScope layers(data);CodecScope scope(data); return VerifyLoad2Packing(ppp); }

std::unique_ptr<Artwork> Artwork::demo() {
    auto d=std::make_unique<Artwork>(); CodecScope scope(d->data);
    PAL *p=AllocPal(); strcpy(p->n_s,"STUDIOPAL"); p->numc=16;p->bitspix=4;p->data_p=calloc(16,2);
    const unsigned colors[]={0x000000,0x151c29,0x303d50,0x66768c,0xb3becb,0xf1e9d2,0x511c29,0x8c293b,0xcc4050,0xf7746a,0x6b4e39,0xb88c60,0xecc29a,0x183c46,0x357583,0x70bec5};
    for(int i=0;i<16;++i) rgb8_to_pal_word((colors[i]>>16)&255,(colors[i]>>8)&255,colors[i]&255,(unsigned char *)p->data_p+i*2);
    for(int frame=0;frame<8;++frame) {
        IMG *im=AllocImg(); snprintf(im->n_s,16,"SENTINEL%d",frame+1); im->w=48;im->h=64;im->anix=24;im->aniy=60; im->opals=0xffff;im->file_frm=0xffff;
        im->data_p=calloc(48,64); auto pix=(unsigned char *)im->data_p;
        auto rect=[&](int x,int y,int w,int h,int c){for(int yy=std::max(0,y);yy<std::min(64,y+h);++yy)for(int xx=std::max(0,x);xx<std::min(48,x+w);++xx)pix[yy*48+xx]=(unsigned char)c;};
        int bob=(frame/2)%2, step=frame%4-1;
        rect(16,8+bob,16,15,1); rect(18,9+bob,12,12,7);rect(19,11+bob,10,3,9);rect(18,16+bob,12,3,5);
        rect(12,24+bob,24,23,1);rect(15,24+bob,18,20,7);rect(17,25+bob,4,17,8);rect(26,25+bob,5,17,6);
        rect(8,25+bob,7,18,2);rect(9,27+bob,4,11,3);rect(33,25+bob,7,18,2);rect(35,27+bob,3,11,3);
        rect(8,41+bob,7,5,11);rect(33,41+bob,7,5,11);rect(14,43+bob,20,4,4);
        rect(15-step,47,8,12,2);rect(26+step,47,8,12,2);rect(13-step,57,10,4,1);rect(26+step,57,11,4,1);
        im->baseline_w=48;im->baseline_h=64;im->baseline_p=malloc(48*64);memcpy(im->baseline_p,im->data_p,48*64);
    }
    d->assign_initial_ids();return d;
}
void Artwork::assign_initial_ids(){sprite_ids.clear();for(int i=0;i<count();++i)sprite_ids.push_back(next_sprite_id++);selected_sprite_id=sprite_ids.empty()?0:sprite_ids.front();}
uint64_t Artwork::sprite_id(int index) const{return index>=0&&index<(int)sprite_ids.size()?sprite_ids[index]:0;}
int Artwork::sprite_index(uint64_t id) const{if(!id)return -1;auto found=std::find(sprite_ids.begin(),sprite_ids.end(),id);return found==sprite_ids.end()?-1:(int)(found-sprite_ids.begin());}
}
