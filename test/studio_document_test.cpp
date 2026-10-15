#include "studio_document.h"
#include "palette_math.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
static int failures=0;
#define CHECK(x) do { if(!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; ++failures; } } while(0)
static std::vector<char> bytes(const std::filesystem::path &p) {
    std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}};
}
int main(int argc, char **argv) {
    namespace fs=std::filesystem;
    auto folder=fs::temp_directory_path()/("imgtool-studio-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(folder);
    std::string error;
    auto a=studio::Artwork::demo(), b=studio::Artwork::demo();
    CHECK(g_doc==nullptr);
    auto original=a->copy(0,0,0,48,64);
    a->paint(0,0,0,8); a->commit(); CHECK(a->dirty()); CHECK(a->can_undo());
    auto structure=a->structure_generation();
    b->paint(0,1,1,9);b->commit();
    a->undo(); CHECK(!a->dirty());CHECK(a->copy(0,0,0,48,64).pixels==original.pixels);
    CHECK(a->structure_generation()==structure);
    CHECK(b->dirty());a->redo();CHECK(a->dirty());
    auto dest=(folder/"roundtrip.img").u8string();
    CHECK(a->save(dest,error)); CHECK(!a->dirty()); CHECK(error.empty());
    a->undo();CHECK(a->dirty());a->redo();CHECK(!a->dirty());
    a->undo();a->paint(0,2,2,6);a->commit();CHECK(!a->can_redo());CHECK(a->dirty());
    a->begin("cancelled edit");a->paint(0,4,4,8);a->cancel();CHECK(a->dirty());
    CHECK(a->save(dest,error));
    auto first=bytes(dest);
    auto loaded=studio::Artwork::open(dest,error);CHECK(loaded!=nullptr);
    if(loaded) { CHECK(loaded->count()==8); CHECK(loaded->copy(0,0,0,48,64).pixels==a->copy(0,0,0,48,64).pixels); }
    CHECK(a->save(dest,error));CHECK(bytes(dest)==first);
    CHECK(!a->save((folder/"missing"/"file.img").u8string(),error));CHECK(!error.empty());
    a->paint(0,4,4,8);a->commit();
    CHECK(!a->save(folder.u8string(),error)); CHECK(a->dirty()); CHECK(bytes(dest)==first);
    {std::ofstream out(dest,std::ios::binary|std::ios::app);out << 'X';}
    CHECK(!a->save(dest,error)); CHECK(error.find("changed on disk")!=std::string::npos);CHECK(a->dirty());

    // Preserve opaque metadata, compressed rows, near/far-independent raw names.
    auto meta=studio::Artwork::demo(); IMG *im=meta->sprite(0);
    im->flags=0x80;im->anix2=17;im->aniy2=23;im->aniz2=4;im->file_frm=42;
    memcpy(im->file_name_raw,im->n_s,16);im->file_name_raw[13]=0x76;
    im->pttbl_p=malloc(40);memset(im->pttbl_p,0x35,40);
    im->opals=0;im->opaltbl_p=malloc(16);memset(im->opaltbl_p,0x42,16);
    meta->data.seqcnt=1;meta->data.scrseqbytes=116;meta->data.scrseqmem_p=calloc(1,116);
    ((unsigned char *)meta->data.scrseqmem_p)[18]=1;
    meta->data.damcnt=1;meta->data.damtblbytes=4;meta->data.damtbl_p=malloc(4);memset(meta->data.damtbl_p,0x31,4);
    meta->data.file_bufscr[2]=73;meta->data.file_spare2=0xa621;
    std::string mp=(folder/"metadata.img").u8string();CHECK(meta->save(mp,error));
    auto m=studio::Artwork::open(mp,error);CHECK(m!=nullptr);
    if(m) {
        CHECK(m->sprite(0)->file_name_raw[13]==0x76);CHECK(m->sprite(0)->file_frm==42);
        CHECK(m->sprite(0)->anix2==17 && m->sprite(0)->aniy2==23 && m->sprite(0)->aniz2==4);
        CHECK(m->sprite(0)->pttbl_p && memcmp(m->sprite(0)->pttbl_p,im->pttbl_p,40)==0);
        CHECK(m->sprite(0)->opaltbl_p && memcmp(m->sprite(0)->opaltbl_p,im->opaltbl_p,16)==0);
        CHECK(m->data.scrseqbytes==116 && memcmp(m->data.scrseqmem_p,meta->data.scrseqmem_p,116)==0);
        CHECK(m->data.damtblbytes==4 && memcmp(m->data.damtbl_p,meta->data.damtbl_p,4)==0);
        CHECK(m->data.file_bufscr[2]==73 && m->data.file_spare2==0xa621);
        auto old=bytes(mp);CHECK(m->save(mp,error));CHECK(bytes(mp)==old);
    }
    auto clip=b->copy(0,1,1,1,1);CHECK(clip.pixels[0]==9);
    a->paste(0,clip,3,3);CHECK(a->copy(0,3,3,1,1).pixels[0]==9);a->undo();
    auto mapped=studio::Artwork::demo();
    mapped->color(0,2,clip.colors[9]);mapped->color(0,9,0x7fff);mapped->commit();
    mapped->paste(0,clip,0,0);CHECK(mapped->copy(0,0,0,1,1).pixels[0]==2);
    clip.pixels[0]=0;mapped->paste(0,clip,0,0);CHECK(mapped->copy(0,0,0,1,1).pixels[0]==2);
    mapped->undo();CHECK(mapped->copy(0,0,0,1,1).pixels[0]==0);
    a->anchor(0,-15,400);a->commit();CHECK((short)a->sprite(0)->anix==-15);a->undo();CHECK(a->sprite(0)->anix==24);
    auto oldword=palette_word_at((unsigned char *)a->palette(0)->data_p,3);
    a->color(0,3,0x7fff);a->commit();a->undo();CHECK(palette_word_at((unsigned char *)a->palette(0)->data_p,3)==oldword);
    a->fill(0,47,63,4);CHECK(a->copy(0,47,63,1,1).pixels[0]==4);a->undo();
    auto small=studio::Artwork::demo();small->sprite(0)->w=1;small->sprite(0)->h=1;
    CHECK(small->save((folder/"small.img").u8string(),error));
    auto sm=studio::Artwork::open((folder/"small.img").u8string(),error);CHECK(sm && sm->sprite(0)->w==1);
    auto broken=bytes(mp);broken.resize(30);
    {std::ofstream out(folder/"bad.img",std::ios::binary);out.write(broken.data(),broken.size());}
    CHECK(!studio::Artwork::open((folder/"bad.img").u8string(),error));CHECK(g_doc==nullptr);
    auto unicode=(folder/fs::u8path(u8"art-\u00e9-\u65e5.img")).u8string();
    CHECK(b->save(unicode,error));CHECK(studio::Artwork::open(unicode,error)!=nullptr);
    auto legacy=bytes(mp);legacy[8]=0x32;legacy[9]=6;
    {std::ofstream out(folder/"legacy.img",std::ios::binary);out.write(legacy.data(),legacy.size());}
    CHECK(!studio::Artwork::open((folder/"legacy.img").u8string(),error));CHECK(error.find("unsupported layout")!=std::string::npos);
    // Optional local fixtures are read only; all output stays in our temp dir.
    for(int arg=1;arg<argc;++arg){
        auto real=studio::Artwork::open(argv[arg],error);
        CHECK(real!=nullptr);if(!real){std::cerr<<error<<"\n";continue;}
        auto copy_path=(folder/"real.img").u8string();CHECK(real->save(copy_path,error));
        auto round=studio::Artwork::open(copy_path,error);CHECK(round!=nullptr);if(!round)continue;
        CHECK(real->count()==round->count());CHECK(real->palette_count()==round->palette_count());
        for(int i=0;i<real->count();++i){auto im=real->sprite(i), other=round->sprite(i);
            CHECK(real->copy(i,0,0,im->w,im->h).pixels==round->copy(i,0,0,other->w,other->h).pixels);
            CHECK(im->anix==other->anix&&im->aniy==other->aniy&&im->aniz2==other->aniz2&&im->file_frm==other->file_frm);
            CHECK(memcmp(im->file_name_raw,other->file_name_raw,16)==0);
        }
        auto saved=bytes(copy_path);CHECK(round->save(copy_path,error));CHECK(bytes(copy_path)==saved);
        std::cout<<"Roundtrip: "<<argv[arg]<<" ("<<real->count()<<" sprites)\n";
    }
    for(auto &f:fs::directory_iterator(folder))CHECK(f.path().extension()!=".tmp");
    fs::remove_all(folder);
    std::cout << "Studio document checks: " << failures << " failure(s)\n";
    return failures?1:0;
}
