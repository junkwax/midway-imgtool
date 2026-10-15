#include "studio_document.h"
#include "studio_img_codec.h"
#include "studio_sequences.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <type_traits>

static_assert(std::is_same_v<decltype(&studio::Artwork::save),
              bool (studio::Artwork::*)(const std::string &,std::string &)>);

namespace fs=std::filesystem;
using Bytes=std::vector<unsigned char>;
static int failures=0;
#define CHECK(x) do {if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
template<class T> T at(const Bytes &b,size_t p) {T v{};memcpy(&v,b.data()+p,sizeof v);return v;}
template<class T> void put(Bytes &b,size_t p,T v) {memcpy(b.data()+p,&v,sizeof v);}
static Bytes read(const fs::path &p) {std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const fs::path &p,const Bytes &b) {std::ofstream f(p,std::ios::binary);f.write((const char *)b.data(),b.size());}
// Independent hand-built near fixture with nonzero pointer placeholders, name
// tails, points/alternates, two entries in reverse sequence order and a script.
static Bytes fixture(unsigned version) {
    LIB_HDR h{};h.temp=0xabcd;h.version=(unsigned short)version;h.imgcnt=2;h.palcnt=4;
    h.seqcnt=h.scrcnt=1;h.bufscr[0]=0;h.bufscr[1]=0xff;h.spare2=0x7351;
    Bytes b(sizeof h,0);unsigned short colors[4]={0,0x7c00,0x03e0,0x001f};
    b.insert(b.end(),(unsigned char *)colors,(unsigned char *)(colors+4));
    unsigned pixoffset=(unsigned)b.size();const unsigned char pixels[]={0,1,2,3,3,2,1,0};b.insert(b.end(),pixels,pixels+8);
    h.oset=(unsigned)b.size();
    for(int i=0;i<2;++i) {
        IMAGE_disk d{};strcpy(d.n_s,i?"SECOND":"FIRST");d.n_s[12]=0x76;d.flags=0x10;
        d.w=3;d.h=1;d.anix=(unsigned short)-9;d.aniy=4;d.palnum=3;d.oset=pixoffset+i*4;d.data=0x23991234;d.lib=17;
        d.anix2=5;d.aniy2=6;d.aniz2=42+i;d.frm=(unsigned short)-7; // raw near FRM / Z.
        d.opals=i?0xffff:0;d.pttblnum=(version>=0x61d&&!i)?0:0xffff; // raw near POINT / ALT.
        b.insert(b.end(),(unsigned char *)&d,(unsigned char *)&d+sizeof d);
    }
    PALETTE_disk pal{};strcpy(pal.n_s,"TESTPAL");pal.n_s[9]=0;pal.flags=4;pal.bitspix=2;pal.numc=4;pal.oset=sizeof h;pal.data=0x7190;pal.lib=3;pal.spare=0x6471;
    b.insert(b.end(),(unsigned char *)&pal,(unsigned char *)&pal+sizeof pal);
    for(int script=0;script<2;++script) {
        Bytes r(58,0);memcpy(r.data(),script?"SCRIPT":"SEQUENCE",script?6:8);r[15]=0x61;
        put<unsigned short>(r,16,script?0:0x40);put<unsigned short>(r,18,script?1:2);
        for(int i=0;i<16;++i)put<unsigned short>(r,20+i*2,0x9170+i*16);
        put<unsigned short>(r,52,11);put<unsigned short>(r,54,(unsigned short)-12);put<unsigned short>(r,56,0x4571);
        b.insert(b.end(),r.begin(),r.end());
        for(int i=0;i<(script?1:2);++i) {
            Bytes e(16,0);put<unsigned short>(e,0,0x7342);put<unsigned short>(e,2,script?0:1-i);e[4]=i+3;e[5]=0xa6;
            put<unsigned short>(e,6,(unsigned short)-20);put<unsigned short>(e,8,23);put<unsigned short>(e,10,0x8654);e[15]=0x61;
            b.insert(b.end(),e.begin(),e.end());
        }
    }
    b.insert(b.end(),40,0x35);if(version>=0x61d)b.insert(b.end(),16,0xff);
    put(b,0,h);return b;
}
static void compare(studio::Artwork &a,studio::Artwork &b) {
    CHECK(a.count()==b.count()&&a.palette_count()==b.palette_count());if(a.count()!=b.count())return;
    for(int i=0;i<a.count();++i) {
        auto x=a.sprite(i),y=b.sprite(i);CHECK(x->w==y->w&&x->h==y->h);
        CHECK(a.copy(i,0,0,x->w,x->h).pixels==b.copy(i,0,0,y->w,y->h).pixels);
        CHECK(x->anix==y->anix&&x->aniy==y->aniy&&x->anix2==y->anix2&&x->aniy2==y->aniy2&&x->aniz2==y->aniz2&&x->file_frm==y->file_frm);
        CHECK(!memcmp(x->file_name_raw,y->file_name_raw,16));CHECK(bool(x->pttbl_p)==bool(y->pttbl_p));CHECK(bool(x->opaltbl_p)==bool(y->opaltbl_p));
        if(x->pttbl_p&&y->pttbl_p)CHECK(!memcmp(x->pttbl_p,y->pttbl_p,40));
        if(x->opaltbl_p&&y->opaltbl_p)CHECK(!memcmp(x->opaltbl_p,y->opaltbl_p,16));
    }
    CHECK(a.data.scrseqbytes==b.data.scrseqbytes);
    if(a.data.scrseqbytes==b.data.scrseqbytes&&a.data.scrseqbytes)CHECK(!memcmp(a.data.scrseqmem_p,b.data.scrseqmem_p,a.data.scrseqbytes));
    for(int i=0;i<a.palette_count()&&i<b.palette_count();++i){auto x=a.palette(i),y=b.palette(i);CHECK(x->numc==y->numc);if(x->numc==y->numc)CHECK(!memcmp(x->data_p,y->data_p,x->numc*2));}
}
int main(int argc,char **argv) {
    auto parent=fs::temp_directory_path();auto dir=parent/("studio-legacy-img-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directory(dir);
    std::string error;LIB_HDR h{};Document sentinel{};g_doc=&sentinel;
    for(unsigned v:{0x60au,0x61cu,0x61du,0x626u,0x62eu,0x631u}) {
        auto original=fixture(v);Bytes modern;CHECK(studio::inspect_img(original,h));CHECK(studio::normalize_img(original,modern,error));
        CHECK(at<LIB_HDR>(modern,0).version==0x634);auto back=modern;CHECK(studio::project_img_layout(back,v,error));CHECK(back==original);
        auto file=dir/fs::u8path(u8"older-é-日.img");write(file,original);
        auto a=studio::Artwork::open(file.u8string(),error);CHECK(a&&error.empty());if(!a)continue;
        CHECK(g_doc==&sentinel&&!a->dirty()&&a->disk_version()==v&&a->data.fileversion==0x634);
        auto im=a->sprite(0);CHECK(im->file_frm==42&&(short)im->aniz2==-7&&im->file_pttblnum==0&&im->pttbl_p);
        CHECK(bool(im->opaltbl_p)==(v>=0x61d));std::vector<studio::EmbeddedRecord> records;CHECK(studio::read_embedded(*a,records,error));
        if(records.size()==2) {CHECK(records[0].entries[0].values.index==0&&records[0].entries[1].values.index==1);CHECK(records[0].entries[0].values.ticks==4);CHECK(records[1].entries[0].values.dx==-20);}
        CHECK(a->save(file.u8string(),error));CHECK(read(file)==original); // This fixture is already in writer order.
        a->paint(0,0,0,2);a->commit();CHECK(a->dirty());CHECK(a->move_sprite(0,1,error));
        CHECK(studio::read_embedded(*a,records,error));if(!records.empty())CHECK(records[0].entries[0].values.index==1);
        CHECK(a->save((dir/"edited.img").u8string(),error));auto edited=studio::Artwork::open((dir/"edited.img").u8string(),error);CHECK(edited);if(edited)compare(*a,*edited);
        a->undo();a->undo();CHECK(a->save((dir/"undo.img").u8string(),error));CHECK(read(dir/"undo.img")==original);
        CHECK(a->embedded_edit_entry(0,0,{1,9,-31,42},error));a->commit();CHECK(a->save((dir/"animation.img").u8string(),error));
        auto animation=studio::Artwork::open((dir/"animation.img").u8string(),error);CHECK(animation);if(animation)compare(*a,*animation);a->undo();
        // Nonrepresentable far metadata blocks an original-layout save without
        // altering its destination. Save As must also refuse it; there is no
        // format conversion path in the application's save API.
        ((unsigned char *)a->data.scrseqmem_p)[22]=1;
        auto was_dirty=a->dirty();auto previous_path=a->path();auto generation=a->generation();
        CHECK(!a->save(file.u8string(),error));CHECK(error.find("original IMG layout")!=std::string::npos);CHECK(read(file)==original);
        CHECK(a->disk_version()==v&&a->dirty()==was_dirty&&a->generation()==generation&&a->path()==previous_path);
        auto copy=dir/"copy.img";write(copy,original);CHECK(!a->save(copy.u8string(),error));CHECK(read(copy)==original);
        CHECK(!a->save((dir/"new-output.img").u8string(),error));CHECK(!fs::exists(dir/"new-output.img"));
        ((unsigned char *)a->data.scrseqmem_p)[22]=0;CHECK(a->save(copy.u8string(),error));
        auto reopened=studio::Artwork::open(copy.u8string(),error);CHECK(reopened&&reopened->disk_version()==v);if(reopened)compare(*a,*reopened);
        // No alternate/point overreads, no partially loaded live document.
        auto truncated=original;truncated.pop_back();write(dir/"bad.img",truncated);CHECK(!studio::Artwork::open((dir/"bad.img").u8string(),error));CHECK(g_doc==&sentinel);
    }
    auto b=fixture(0x62e);for(size_t n=0;n<b.size();++n){Bytes cut(b.begin(),b.begin()+n);CHECK(!studio::inspect_img(cut,h));}
    for(unsigned v:{0x500u,0x609u,0x632u,0x633u,0x655u}){auto bad=b;put<unsigned short>(bad,8,(unsigned short)v);CHECK(!studio::inspect_img(bad,h));}
    auto bad=b;bad.push_back(0);CHECK(!studio::inspect_img(bad,h));bad=b;put<unsigned short>(bad,at<LIB_HDR>(bad,0).oset+26,2);CHECK(!studio::inspect_img(bad,h));
    bad=b;put<unsigned int>(bad,at<LIB_HDR>(bad,0).oset+28,0xffffffff);CHECK(!studio::inspect_img(bad,h));
    // Preserve dormant words and empty-pool sentinels through the owned adapter.
    auto reserved=fixture(0x60a);auto rh=at<LIB_HDR>(reserved,0);size_t records_end=rh.oset+2*sizeof(IMAGE_disk)+sizeof(PALETTE_disk);
    reserved.erase(reserved.begin()+records_end,reserved.begin()+records_end+164);
    rh.seqcnt=rh.scrcnt=0xffff;rh.damcnt=0x7351;put(reserved,0,rh);put<unsigned short>(reserved,rh.oset+48,0x7135);
    write(dir/"reserved.img",reserved);auto opaque=studio::Artwork::open((dir/"reserved.img").u8string(),error);CHECK(opaque);
    if(opaque){CHECK(!opaque->data.seqcnt&&!opaque->data.scrcnt&&!opaque->data.damcnt);CHECK(opaque->save((dir/"reserved.img").u8string(),error));CHECK(read(dir/"reserved.img")==reserved);}
    // Modern regression, compressed rows with scaling, pristine baselines.
    auto compressed=studio::Artwork::demo();for(int i=0;i<compressed->count();++i)memcpy(compressed->sprite(i)->file_name_raw,compressed->sprite(i)->n_s,16);compressed->sprite(0)->flags=0x580;CHECK(compressed->save((dir/"cmp.img").u8string(),error));
    auto cmp=studio::Artwork::open((dir/"cmp.img").u8string(),error);CHECK(cmp);if(cmp)compare(*compressed,*cmp);
    // Every supported far-format version is immutable too. A mistaken change
    // to the public codec state cannot upgrade/downgrade the saved library.
    auto far_source=read(dir/"cmp.img");
    for(unsigned v=0x634;v<=0x654;++v){
        auto source=far_source;put<unsigned short>(source,8,(unsigned short)v);auto file=dir/"far.img";write(file,source);
        auto art=studio::Artwork::open(file.u8string(),error);CHECK(art);if(!art)continue;
        art->data.fileversion=v==0x654?0x634:0x654;
        auto codec_version=art->data.fileversion;art->paint(0,0,0,2);art->commit();CHECK(art->save(file.u8string(),error));
        CHECK(at<LIB_HDR>(read(file),0).version==v&&art->disk_version()==v&&art->data.fileversion==codec_version);
        CHECK(art->save((dir/"far-copy.img").u8string(),error));CHECK(at<LIB_HDR>(read(dir/"far-copy.img"),0).version==v);
    }
    // Optional real game fixtures are read-only. All edited/saved files are here.
    int fixtures=0;
    for(int arg=1;arg<argc;++arg) {
        fs::path input=fs::u8path(argv[arg]);std::vector<fs::path> paths;
        if(fs::is_directory(input)) {for(auto &e:fs::directory_iterator(input))if(e.path().extension()==".IMG")paths.push_back(e.path());}
        else paths.push_back(input);
        for(auto &p:paths) {
            auto src=read(p);if(src.size()<sizeof(LIB_HDR))continue;auto head=at<LIB_HDR>(src,0);if(head.version<0x60a||head.version>=0x632)continue;
            auto art=studio::Artwork::open(p.u8string(),error);CHECK(art);if(!art){std::cerr<<p<<": "<<error<<"\n";continue;}
            Bytes normalized;CHECK(studio::normalize_img(src,normalized,error));auto reverted=normalized;CHECK(studio::project_img_layout(reverted,head.version,error));CHECK(reverted==src);
            CHECK(art->save((dir/"real.img").u8string(),error));auto round=studio::Artwork::open((dir/"real.img").u8string(),error);CHECK(round);if(!round)continue;CHECK(round->disk_version()==head.version);compare(*art,*round);
            auto saved=read(dir/"real.img");CHECK(round->save((dir/"real.img").u8string(),error));CHECK(read(dir/"real.img")==saved);
            art->anchor(0,13,21);art->commit();CHECK(art->save((dir/"edit-real.img").u8string(),error));auto edit=studio::Artwork::open((dir/"edit-real.img").u8string(),error);CHECK(edit);if(edit)compare(*art,*edit);
            CHECK(read(p)==src);++fixtures;
        }
    }
    CHECK(g_doc==&sentinel);g_doc=nullptr;
    CHECK(fs::weakly_canonical(dir).parent_path()==fs::weakly_canonical(parent));if(fs::weakly_canonical(dir).parent_path()==fs::weakly_canonical(parent))fs::remove_all(dir);
    std::cout<<"Studio legacy IMG: "<<failures<<" failure(s), "<<fixtures<<" read-only fixtures\n";return failures?1:0;
}
