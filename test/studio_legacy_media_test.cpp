#include "studio_legacy_media.h"
#include "studio_palette.h"
#include "studio_img_codec.h"
#include "stb_image.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
using Bytes=std::vector<unsigned char>;namespace fs=std::filesystem;
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static Bytes read(const fs::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const fs::path &p,const Bytes &b){std::ofstream f(p,std::ios::binary);f.write((const char *)b.data(),b.size());}
static void be16(Bytes &b,unsigned n){b.push_back(n>>8);b.push_back(n);}
static void be32(Bytes &b,unsigned n){for(int i=24;i>=0;i-=8)b.push_back(n>>i);}
static void chunk(Bytes &b,const char *tag,Bytes data){b.insert(b.end(),tag,tag+4);be32(b,(unsigned)data.size());b.insert(b.end(),data.begin(),data.end());if(data.size()&1)b.push_back(0);}
static Bytes iff(bool planar,int w,int h,int planes,int mask,int comp,Bytes body,int trans=0,bool special=false){
    Bytes b={'F','O','R','M',0,0,0,0};const char *type=planar?"ILBM":"PBM ";b.insert(b.end(),type,type+4);Bytes header;be16(header,w);be16(header,h);be16(header,0);be16(header,0);header.insert(header.end(),{(unsigned char)planes,(unsigned char)mask,(unsigned char)comp,0});be16(header,trans);header.insert(header.end(),{1,1});be16(header,w);be16(header,h);chunk(b,"BMHD",header);chunk(b,"JUNK",{1,2,3});chunk(b,"CMAP",{0,0,248,248,0,0,0,248,0,248,248,248});if(special)chunk(b,"CAMG",{0,0,8,0});chunk(b,"BODY",body);unsigned n=(unsigned)b.size()-8;for(int i=0;i<4;++i)b[4+i]=n>>(24-i*8);return b;
}
static Bytes tga(int first,int depth,int origin,bool rle,Bytes pixels){
    Bytes b={0,1,(unsigned char)(rle?9:1),(unsigned char)first,0,3,0,(unsigned char)depth,0,0,0,0,2,0,2,0,8,(unsigned char)origin};
    if(depth==24)b.insert(b.end(),{0,0,248,0,248,0,248,0,0});else if(depth==32)b.insert(b.end(),{0,0,248,255,0,248,0,255,248,0,0,255});else b.insert(b.end(),{0,124,224,3,31,0});
    if(rle)b.push_back(3);b.insert(b.end(),pixels.begin(),pixels.end());return b;
}
static studio::IndexedMedia sample(){studio::IndexedMedia m;m.w=3;m.h=2;m.words={0,0x7c00,0x3e0,0x1f};m.pixels={0,1,2,3,2,1};m.anchor_x=-7;m.anchor_y=11;return m;}
int main(int argc,char **argv){
    auto parent=fs::temp_directory_path();auto dir=parent/("studio-legacy-media-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directory(dir);std::string error;auto original=sample();
    for(auto kind:{studio::LegacyMediaKind::Lbm,studio::LegacyMediaKind::Tga}){
        Bytes encoded;CHECK(studio::encode_indexed_media(original,kind,encoded,error));studio::IndexedMedia round;CHECK(studio::decode_indexed_media(encoded,kind,round,error));CHECK(round.words==original.words&&round.pixels==original.pixels);
        if(kind==studio::LegacyMediaKind::Lbm)CHECK(round.anchor_x==-7&&round.anchor_y==11);
        else{int w,h,c;auto rgba=stbi_load_from_memory(encoded.data(),(int)encoded.size(),&w,&h,&c,4);CHECK(rgba&&w==3&&h==2);if(rgba){CHECK(rgba[3]==0&&rgba[7]==255&&rgba[4]==255);stbi_image_free(rgba);}}
        auto file=dir/fs::u8path(kind==studio::LegacyMediaKind::Lbm?u8"sprite-é-日.lbm":u8"sprite-é-日.tga");write(file,encoded);CHECK(studio::read_indexed_media(file.u8string(),round,error));
        for(size_t n=0;n<encoded.size();++n){Bytes cut(encoded.begin(),encoded.begin()+n);studio::IndexedMedia unchanged=original;CHECK(!studio::decode_indexed_media(cut,kind,unchanged,error));CHECK(unchanged.pixels==original.pixels);}
    }
    studio::IndexedMedia m;
    auto packed=iff(false,3,2,8,0,0,{0,1,2,0,3,2,1,0});CHECK(studio::decode_indexed_media(packed,studio::LegacyMediaKind::Lbm,m,error));CHECK(m.pixels==original.pixels);CHECK(m.words[1]==0x7c00&&m.words[2]==0x3e0);
    auto compressed=iff(false,4,2,8,0,1,{128,255,1,1,2,3,3,3,2,1,0});CHECK(studio::decode_indexed_media(compressed,studio::LegacyMediaKind::Lbm,m,error));CHECK(m.pixels==Bytes({1,1,2,3,3,2,1,0}));
    auto badrun=iff(false,4,1,8,0,1,{252,1});CHECK(!studio::decode_indexed_media(badrun,studio::LegacyMediaKind::Lbm,m,error));
    auto trans=iff(false,3,1,8,2,0,{0,1,2,0},2);CHECK(studio::decode_indexed_media(trans,studio::LegacyMediaKind::Lbm,m,error));CHECK(m.pixels[2]==0&&m.pixels[0]!=0&&m.words[m.pixels[0]]==0x1f);
    // 9 pixels, two bitplanes and a mask, including opaque palette index zero.
    auto planar=iff(true,9,1,2,1,0,{0x80,0x80,0x40,0,0x7f,0x80});CHECK(studio::decode_indexed_media(planar,studio::LegacyMediaKind::Lbm,m,error));CHECK(m.pixels[0]==0&&m.pixels[1]==2&&m.pixels[2]!=0&&m.words[m.pixels[2]]==0x1f&&m.pixels[8]==1);
    auto planar_rle=iff(true,9,1,2,1,1,{128,1,0x80,0x80,1,0x40,0,1,0x7f,0x80});studio::IndexedMedia compressed_planes;CHECK(studio::decode_indexed_media(planar_rle,studio::LegacyMediaKind::Lbm,compressed_planes,error));CHECK(m.pixels==compressed_planes.pixels&&m.words==compressed_planes.words);
    CHECK(!studio::decode_indexed_media(iff(true,9,1,2,0,0,{0x80,0x80,0x40,0},0,true),studio::LegacyMediaKind::Lbm,m,error));
    for(int depth:{15,16,24,32})for(int origin:{0,16,32,48})for(bool rle:{false,true}){
        auto b=tga(7,depth,origin,rle,{7,8,9,7});CHECK(studio::decode_indexed_media(b,studio::LegacyMediaKind::Tga,m,error));CHECK(m.words.size()==10&&m.words[7]==0x7c00&&m.words[8]==0x3e0&&m.words[9]==0x1f);
        Bytes expect={7,8,9,7};if(origin&16){std::swap(expect[0],expect[1]);std::swap(expect[2],expect[3]);}if(!(origin&32)){std::swap(expect[0],expect[2]);std::swap(expect[1],expect[3]);}CHECK(m.pixels==expect);
    }
    CHECK(!studio::decode_indexed_media(tga(7,24,32,false,{7,8,9,6}),studio::LegacyMediaKind::Tga,m,error));
    auto oversized=tga(0,24,32,true,{0,1,2,0});oversized[27]=255;CHECK(!studio::decode_indexed_media(oversized,studio::LegacyMediaKind::Tga,m,error));
    auto partial=tga(0,32,32,false,{0,1,2,0});partial[21]=128;CHECK(!studio::decode_indexed_media(partial,studio::LegacyMediaKind::Tga,m,error));
    auto repeated=tga(0,24,32,true,{});repeated.resize(27);repeated.insert(repeated.end(),{131,1});CHECK(studio::decode_indexed_media(repeated,studio::LegacyMediaKind::Tga,m,error));CHECK(m.pixels==Bytes({1,1,1,1}));
    auto full=tga(0,32,32,false,{});full.resize(18);full[5]=0;full[6]=1;full[12]=0;full[13]=1;full[14]=1;
    for(unsigned i=0;i<256;++i)full.insert(full.end(),{(unsigned char)((i&31)<<3),(unsigned char)(((i>>5)&31)<<3),0,255});for(int i=0;i<256;++i)full.push_back((unsigned char)i);
    CHECK(!studio::decode_indexed_media(full,studio::LegacyMediaKind::Tga,m,error));CHECK(error.find("256 distinct opaque")!=std::string::npos);
    auto art=studio::Artwork::demo();studio::ImportBatch batch;auto generation=art->generation();CHECK(studio::build_indexed_import(*art,original,"IMPORTED",-1,-7,11,batch,error));CHECK(art->generation()==generation);int count=art->count(),palettes=art->palette_count();auto target_version=art->disk_version();
    CHECK(art->import_batch(batch,error)==count);CHECK(art->count()==count+1&&art->palette_count()==palettes+1&&art->disk_version()==target_version);
    CHECK(art->sprite(count)->anix==(unsigned short)-7&&art->sprite(count)->aniy==11);CHECK(art->sprite(count)->anix2==0xffff&&art->sprite(count)->aniy2==0xffff&&art->sprite(count)->aniz2==0xffff);CHECK(studio::copy_palette(*art,palettes).words==original.words);CHECK(art->copy(count,0,0,3,2).pixels==original.pixels);
    art->undo();CHECK(art->count()==count&&art->palette_count()==palettes);art->redo();CHECK(art->count()==count+1);CHECK(art->save((dir/"round.img").u8string(),error));auto reopened=studio::Artwork::open((dir/"round.img").u8string(),error);CHECK(reopened&&reopened->disk_version()==target_version);
    CHECK(!studio::build_indexed_import(*art,original,"IMPORTED",-1,0,0,batch,error));CHECK(studio::build_indexed_import(*art,original,"MATCHED",0,0,0,batch,error));CHECK(batch.words==studio::copy_palette(*art,0).words);CHECK(batch.frames[0].pixels[0]==0);CHECK(art->import_batch(batch,error)>=0);CHECK(art->palette_count()==palettes+1);art->undo();
    CHECK(studio::build_indexed_import(*art,original,"STALE",-1,0,0,batch,error));art->anchor(0,1,2);art->commit();CHECK(art->import_batch(batch,error)<0);art->undo();
    auto destination=(dir/fs::u8path(u8"export-é-日.lbm")).u8string();studio::IndexedExportReview review;auto before=art->generation();auto dirty=art->dirty();CHECK(studio::review_indexed_export(*art,count,destination,studio::LegacyMediaKind::Lbm,review,error));CHECK(studio::write_indexed_export(*art,review,error));CHECK(art->generation()==before&&art->dirty()==dirty);auto saved=read(fs::u8path(destination));
    CHECK(!studio::write_indexed_export(*art,review,error));CHECK(error.find("destination changed")!=std::string::npos);CHECK(studio::review_indexed_export(*art,count,destination,studio::LegacyMediaKind::Lbm,review,error));CHECK(studio::write_indexed_export(*art,review,error));
    auto tampered=review;tampered.bytes[0]^=1;CHECK(!studio::write_indexed_export(*art,tampered,error));CHECK(read(fs::u8path(destination))==saved);
    tampered=review;tampered.media.pixels[0]^=1;CHECK(!studio::write_indexed_export(*art,tampered,error));CHECK(read(fs::u8path(destination))==saved);
    auto foreign=studio::Artwork::demo();CHECK(!studio::write_indexed_export(*foreign,review,error));art->anchor(0,3,4);art->commit();CHECK(!studio::write_indexed_export(*art,review,error));art->undo();
    CHECK(!studio::export_indexed_sprite(*art,count,(dir/"wrong.img").u8string(),studio::LegacyMediaKind::Lbm,error));CHECK(!studio::export_indexed_sprite(*art,count,(dir/"missing"/"out.tga").u8string(),studio::LegacyMediaKind::Tga,error));
    auto alias=dir/"source.lbm";std::error_code ec;fs::create_hard_link(dir/"round.img",alias,ec);if(!ec)CHECK(!studio::export_indexed_sprite(*art,count,alias.u8string(),studio::LegacyMediaKind::Lbm,error));
    int fixtures=0;for(int arg=1;arg<argc;++arg){auto real=studio::Artwork::open(argv[arg],error);CHECK(real);if(!real)continue;auto source=read(fs::u8path(argv[arg]));for(int i=0;i<std::min(real->count(),5);++i){studio::IndexedMedia image;CHECK(studio::snapshot_indexed_sprite(*real,i,image,error));for(auto kind:{studio::LegacyMediaKind::Lbm,studio::LegacyMediaKind::Tga}){Bytes bytes;CHECK(studio::encode_indexed_media(image,kind,bytes,error));studio::IndexedMedia round;CHECK(studio::decode_indexed_media(bytes,kind,round,error));CHECK(image.pixels==round.pixels);for(size_t c=0;c<image.words.size()&&c<round.words.size();++c)CHECK((image.words[c]&0x7fff)==round.words[c]);}studio::ImportBatch import;CHECK(studio::build_indexed_import(*real,image,"NEWIMPORT"+std::to_string(i),-1,image.anchor_x,image.anchor_y,import,error));CHECK(real->import_batch(import,error)>=0);CHECK(real->save((dir/"real.img").u8string(),error));auto round=studio::Artwork::open((dir/"real.img").u8string(),error);CHECK(round&&round->disk_version()==real->disk_version());real->undo();}CHECK(read(fs::u8path(argv[arg]))==source);++fixtures;}
    CHECK(fs::weakly_canonical(dir).parent_path()==fs::weakly_canonical(parent));if(fs::weakly_canonical(dir).parent_path()==fs::weakly_canonical(parent))fs::remove_all(dir);std::cout<<"Indexed legacy media: "<<failures<<" failure(s), "<<fixtures<<" read-only IMG fixtures\n";return failures?1:0;
}
