#include "studio_legacy_media.h"
#include "studio_layers.h"
#include "studio_palette.h"
#include "studio_file_io.h"
#include "palette_math.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <set>
namespace studio {
namespace {
using Bytes=std::vector<unsigned char>;
unsigned le16(const unsigned char *p){return p[0]|p[1]<<8;}
unsigned be16(const unsigned char *p){return p[0]<<8|p[1];}
unsigned be32(const unsigned char *p){return (unsigned)p[0]<<24|(unsigned)p[1]<<16|p[2]<<8|p[3];}
void word(Bytes &b,unsigned n,bool big){b.push_back((unsigned char)(big?n>>8:n));b.push_back((unsigned char)(big?n:n>>8));}
void dword(Bytes &b,unsigned n){for(int s=24;s>=0;s-=8)b.push_back((unsigned char)(n>>s));}
void chunk(Bytes &b,const char *tag,const Bytes &data){b.insert(b.end(),tag,tag+4);dword(b,(unsigned)data.size());b.insert(b.end(),data.begin(),data.end());if(data.size()&1)b.push_back(0);}
bool dimensions(int w,int h){return w>0&&h>0&&w<=8192&&h<=8192&&(size_t)w*h<=4*1024*1024;}
bool valid(const IndexedMedia &m){return dimensions(m.w,m.h)&&!m.words.empty()&&m.words.size()<=256&&m.pixels.size()==(size_t)m.w*m.h&&std::all_of(m.pixels.begin(),m.pixels.end(),[&](unsigned char c){return c<m.words.size();});}
bool row(const Bytes &body,size_t &pos,size_t n,bool compressed,Bytes &out){
    out.clear();if(!compressed){if(pos>body.size()||n>body.size()-pos)return false;out.insert(out.end(),body.begin()+pos,body.begin()+pos+n);pos+=n;return true;}
    while(out.size()<n){if(pos>=body.size())return false;int c=(signed char)body[pos++];if(c==-128)continue;
        size_t count=c>=0?(size_t)c+1:(size_t)(1-c);if(count>n-out.size())return false;
        if(c>=0){if(count>body.size()-pos)return false;out.insert(out.end(),body.begin()+pos,body.begin()+pos+count);pos+=count;}
        else{if(pos>=body.size())return false;out.insert(out.end(),count,body[pos++]);}}
    return true;
}
bool transparency(IndexedMedia &m,const Bytes &mask,std::string &error){
    if(mask.empty())return true;
    std::array<bool,256> used{};bool opaque_zero=false;
    for(size_t i=0;i<m.pixels.size();++i)if(mask[i]){if(m.pixels[i]>=m.words.size()){error="An opaque pixel references a missing palette color.";return false;}used[m.pixels[i]]=true;opaque_zero|=m.pixels[i]==0;}
    int replacement=0;
    if(opaque_zero){for(int i=1;i<(int)m.words.size();++i)if(m.words[i]==m.words[0]){replacement=i;break;}
        if(!replacement)for(int i=1;i<256;++i)if(!used[i]){replacement=i;break;}
        if(!replacement){error="This image needs transparency plus 256 distinct opaque palette entries. It cannot fit an IMG palette.";return false;}
        if(m.words.size()<=(size_t)replacement)m.words.resize(replacement+1);m.words[replacement]=m.words[0];
        m.note+=" Opaque color zero moved to an available slot to preserve its color.";
    }
    for(size_t i=0;i<m.pixels.size();++i)if(!mask[i])m.pixels[i]=0;else if(!m.pixels[i])m.pixels[i]=(unsigned char)replacement;
    return true;
}
bool lbm(const Bytes &b,IndexedMedia &m,std::string &error){
    if(b.size()<12||memcmp(b.data(),"FORM",4)){error="Choose an IFF PBM or ILBM image.";return false;}
    size_t end=8+(size_t)be32(b.data()+4);bool planar=!memcmp(b.data()+8,"ILBM",4),packed=!memcmp(b.data()+8,"PBM ",4);
    if(end!=b.size()||end<12||(!planar&&!packed)){error="The IFF FORM is incomplete or unsupported (PBM / ILBM only).";return false;}
    Bytes header,cmap,body,destination;bool have_body=false,grab=false;unsigned mode=0;int ax=0,ay=0;
    for(size_t p=12;p<end;){if(end-p<8){error="Truncated IFF chunk header.";return false;}size_t n=be32(b.data()+p+4),start=p+8;
        if(n>end-start||(n&1)>end-start-n){error="Truncated IFF chunk or padding.";return false;}
        auto copy=[&](Bytes &out){out.assign(b.begin()+start,b.begin()+start+n);};
        if(!memcmp(b.data()+p,"BMHD",4)){if(!header.empty()||have_body||n!=20){error="Invalid or duplicate bitmap header.";return false;}copy(header);}
        else if(!memcmp(b.data()+p,"CMAP",4)){if(!cmap.empty()||have_body||!n||n%3||n>768){error="Invalid or duplicate palette chunk.";return false;}copy(cmap);}
        else if(!memcmp(b.data()+p,"BODY",4)){if(have_body||header.empty()||cmap.empty()){error="The image needs one BODY after its bitmap header and palette.";return false;}copy(body);have_body=true;}
        else if(!memcmp(b.data()+p,"GRAB",4)){if(n!=4||grab){error="Invalid hotspot chunk.";return false;}ax=(short)be16(b.data()+start);ay=(short)be16(b.data()+start+2);grab=true;}
        else if(!memcmp(b.data()+p,"DEST",4)){if(n!=8||have_body){error="DEST must contain eight bytes and precede BODY.";return false;}copy(destination);}
        else if(!memcmp(b.data()+p,"CAMG",4)){if(n!=4||have_body){error="CAMG must contain four bytes and precede BODY.";return false;}mode=be32(b.data()+start);}
        p=start+n+(n&1);
    }
    if(header.empty()||!have_body||cmap.empty()){error="The IFF image is missing a required chunk.";return false;}
    m.w=be16(header.data());m.h=be16(header.data()+2);int planes=header[8],masking=header[9],compression=header[10];
    bool constant=planar&&!destination.empty()&&planes==0;
    if(!dimensions(m.w,m.h)||(planes<1&&!constant)||planes>8||(packed&&planes!=8)||masking>2||(packed&&masking==1)||compression>1){error="Unsupported bitmap dimensions, depth, mask or compression.";return false;}
    int depth=planes;std::array<unsigned char,256> dest_indices{};
    if(!destination.empty()){
        depth=destination[0];if(!planar||depth<1||depth>8){error="DEST import needs ILBM and a destination depth from 1 to 8.";return false;}
        unsigned bits=(1u<<depth)-1,pick=be16(destination.data()+2)&bits,on=be16(destination.data()+4)&bits,mask=be16(destination.data()+6)&bits;
        int count=0;for(int bit=0;bit<depth;++bit)count+=(pick>>bit)&1;
        if(count!=planes){error="DEST planePick must select exactly the number of source bitplanes.";return false;}
        if(mask!=bits){error="This DEST leaves background bitplanes unchanged. Import needs a complete destination; merge it with its background first.";return false;}
        // EA ILBM: successive source planes fill set planePick bits; other
        // destination planes use planeOnOff. Bits above depth are ignored.
        for(int index=0;index<(1<<planes);++index){unsigned value=on&~pick;int source=0;for(int bit=0;bit<depth;++bit)if(pick&(1u<<bit))value|=((index>>source++)&1u)<<bit;dest_indices[index]=(unsigned char)value;}
    }
    if(mode&0x800){error="HAM display modes are not supported by indexed import.";return false;}
    bool halfbrite=(mode&0x80)!=0;
    if(halfbrite&&(!planar||depth!=6||cmap.size()<32*3)){error="Extra Halfbrite import needs six destination planes and at least 32 base palette entries.";return false;}
    for(size_t p=0;p<cmap.size();p+=3)m.words.push_back((unsigned short)(((cmap[p]>>3)<<10)|((cmap[p+1]>>3)<<5)|(cmap[p+2]>>3)));
    // EHB plane 6 selects half intensity (Amiga Hardware Manual, node0098).
    // Mask each RGB555 channel after shifting so bits cannot spill between channels.
    if(halfbrite){m.words.resize(64);for(size_t i=0;i<32;++i){auto color=m.words[i];m.words[i+32]=(unsigned short)(((color>>1)&0x3c00)|((color>>1)&0x01e0)|((color>>1)&0x000f));}}
    m.anchor_x=grab?ax:m.w/2;m.anchor_y=grab?ay:m.h-1;m.pixels.resize((size_t)m.w*m.h);Bytes coverage;if(masking)coverage.resize(m.pixels.size(),255);
    size_t pos=0;Bytes scan;size_t stride=packed?((m.w+1)&~size_t(1)):((m.w+15)/16)*2;
    for(int y=0;y<m.h;++y)for(int plane=0;plane<(packed?1:planes+(masking==1));++plane){
        if(!row(body,pos,stride,compression==1,scan)){error="Truncated BODY or ByteRun1 packet crossing a scanline.";return false;}
        for(int x=0;x<m.w;++x){size_t i=(size_t)y*m.w+x;
            if(packed)m.pixels[i]=scan[x];else if(plane==planes)coverage[i]=(scan[x/8]&(128>>(x&7)))?255:0;
            else if(scan[x/8]&(128>>(x&7)))m.pixels[i]|=(unsigned char)(1<<plane);}}
    // ByteRun1 no-op bytes may legally finish a BODY; never accept other residue.
    if(compression==1)while(pos<body.size()&&body[pos]==128)++pos;
    if(pos!=body.size()){error="Unexpected bytes after the bitmap rows.";return false;}
    // transparentColor is a SOURCE bit pattern, before DEST scattering.
    if(masking==2){unsigned transparent=be16(header.data()+12);for(size_t i=0;i<m.pixels.size();++i)coverage[i]=m.pixels[i]==transparent?0:255;}
    if(!destination.empty())for(auto &index:m.pixels)index=dest_indices[index];
    m.note=halfbrite?"Extra Halfbrite ILBM: 32 base colors plus 32 derived half-bright colors; six-bit indices retained.":planar?"ILBM bitplanes decoded without quantizing palette order.":"PBM indexed pixels retain their source palette order.";
    if(!destination.empty()){if(halfbrite)m.note="Extra Halfbrite ILBM: 32 base colors plus 32 derived half-bright colors.";m.note+=" DEST remapped "+std::to_string(planes)+" source planes into "+std::to_string(depth)+" destination planes; palette order retained.";}
    if(halfbrite&&cmap.size()>32*3)m.note+=" Stored upper palette entries ignored in favor of derived colors.";
    if(!masking)m.note+=" Index zero is treated as IMG transparency.";
    if(!transparency(m,coverage,error))return false;
    return valid(m);
}
bool tga(const Bytes &b,IndexedMedia &m,std::string &error){
    if(b.size()<18||b[1]!=1||(b[2]!=1&&b[2]!=9)||b[16]!=8||(b[17]&192)){error="Use an 8-bit indexed TGA (raw or RLE); full-color TGA uses the sprite-sheet import.";return false;}
    unsigned first=le16(b.data()+3),count=le16(b.data()+5),depth=b[7];m.w=le16(b.data()+12);m.h=le16(b.data()+14);
    if(!dimensions(m.w,m.h)||!count||first+count>256||(depth!=15&&depth!=16&&depth!=24&&depth!=32)){error="Invalid TGA dimensions or palette.";return false;}
    size_t pos=18+b[0],entry=(depth+7)/8;if(pos>b.size()||count*entry>b.size()-pos){error="Truncated TGA palette.";return false;}
    m.words.resize(first+count);std::array<unsigned char,256> alpha{};alpha.fill(255);
    for(unsigned i=first;i<first+count;++i){const auto *p=b.data()+pos;
        m.words[i]=(unsigned short)(depth<=16?(le16(p)&0x7fff):((p[2]>>3)<<10)|((p[1]>>3)<<5)|(p[0]>>3));
        if(depth==32){if(p[3]!=0&&p[3]!=255){error="Partial palette alpha cannot be represented by indexed IMG pixels.";return false;}alpha[i]=p[3];}pos+=entry;}
    m.pixels.resize((size_t)m.w*m.h);size_t total=m.pixels.size(),done=0;Bytes coverage;if(depth==32)coverage.resize(total);
    while(done<total){unsigned n=1;bool repeat=false;if(b[2]==9){if(pos>=b.size()){error="Truncated TGA packet.";return false;}unsigned c=b[pos++];n=(c&127)+1;repeat=(c&128)!=0;}
        if(n>total-done||pos>=b.size()||(!repeat&&n>b.size()-pos)){error="Truncated or oversized TGA pixel packet.";return false;}
        for(unsigned i=0;i<n;++i){unsigned c=b[pos+(repeat?0:i)];if(c<first||c>=first+count){error="TGA pixel references a missing palette entry.";return false;}
            size_t file=done+i;int x=(int)(file%m.w),y=(int)(file/m.w);if(b[17]&16)x=m.w-1-x;if(!(b[17]&32))y=m.h-1-y;
            size_t at=(size_t)y*m.w+x;m.pixels[at]=(unsigned char)c;if(!coverage.empty())coverage[at]=alpha[c];}
        pos+=repeat?1:n;done+=n;
    }
    m.anchor_x=m.w/2;m.anchor_y=m.h-1;m.note="Indexed TGA retains palette positions and respects both image origins.";
    if(depth!=32)m.note+=" Index zero is treated as IMG transparency.";
    return transparency(m,coverage,error)&&valid(m);
}
std::string ext(std::string v){for(char &c:v)if(c>='A'&&c<='Z')c+=32;return v;}
}
bool decode_indexed_media(const Bytes &b,LegacyMediaKind kind,IndexedMedia &out,std::string &error){
    error.clear();IndexedMedia m;if(b.size()>64*1024*1024){error="Import file exceeds 64 MiB.";return false;}
    if(!(kind==LegacyMediaKind::Lbm?lbm(b,m,error):tga(b,m,error))){if(error.empty())error="Image pixels reference missing palette colors.";return false;}
    out=std::move(m);return true;
}
bool read_indexed_media(const std::string &path,IndexedMedia &out,std::string &error){
    std::ifstream f(std::filesystem::u8path(path),std::ios::binary|std::ios::ate);auto n=f?f.tellg():std::streampos(-1);
    if(n<=0||n>64*1024*1024){error="Could not read the image (limit: 64 MiB).";return false;}
    Bytes b((size_t)n);f.seekg(0);if(!f.read((char *)b.data(),b.size())){error="Could not finish reading the image.";return false;}
    auto extension=ext(std::filesystem::u8path(path).extension().u8string());if(extension!=".lbm"&&extension!=".iff"&&extension!=".tga"){error="Choose a .lbm, .iff or indexed .tga image.";return false;}
    if(!decode_indexed_media(b,extension==".tga"?LegacyMediaKind::Tga:LegacyMediaKind::Lbm,out,error))return false;out.path=path;return true;
}
bool build_indexed_import(const Artwork &art,const IndexedMedia &m,const std::string &name,int palette,int ax,int ay,ImportBatch &out,std::string &error){
    error.clear();if(!valid(m)||palette<-1||palette>=art.palette_count()||ax<-32768||ax>32767||ay<-32768||ay>32767||name.empty()||name.size()>15||std::any_of(name.begin(),name.end(),[](unsigned char c){return !((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_');})){error="Use a valid source, palette, signed anchor and 1–15 character sprite name (letters/digits/_).";return false;}
    auto upper=[](std::string s){for(char &c:s)if(c>='a'&&c<='z')c-=32;return s;};for(int i=0;i<art.count();++i)if(upper(art.sprite(i)->n_s)==upper(name)){error="A sprite already uses this name.";return false;}
    ImportBatch b;b.owner=&art;b.generation=art.generation();b.palette=palette;b.sequence=false;b.source_count=1;b.words=palette<0?m.words:copy_palette(art,palette).words;b.note=m.note;
    b.palette_name=name.substr(0,8)+"P";for(int k=0;;++k){auto suffix=k?std::to_string(k):"";std::string candidate=b.palette_name.substr(0,9-suffix.size())+suffix;bool exists=false;for(int i=0;i<art.palette_count();++i)exists|=upper(art.palette(i)->n_s)==upper(candidate);if(!exists){b.palette_name=candidate;break;}if(k>=4093){error="Palette name limit reached.";return false;}}
    ImportFrame frame;frame.name=name;frame.w=m.w;frame.h=m.h;frame.anix=ax;frame.aniy=ay;frame.ticks=4;size_t stride=(m.w+3)&~size_t(3);frame.pixels.resize(stride*m.h);
    std::array<unsigned char,256> map{};for(size_t i=1;i<m.words.size();++i){if(palette<0)map[i]=(unsigned char)i;else{if(b.words.size()<2){error="The matching palette needs an opaque color.";return false;}map[i]=nearest_palette_index_for_word(m.words[i],art.palette(palette));}}
    for(int y=0;y<m.h;++y)for(int x=0;x<m.w;++x)frame.pixels[y*stride+x]=map[m.pixels[(size_t)y*m.w+x]];
    size_t decoded=frame.pixels.size();for(int i=0;i<art.count();++i){auto im=art.sprite(i);decoded+=((im->w+3)&~size_t(3))*im->h;}
    if(art.count()>=2000||decoded>128*1024*1024||(palette<0&&art.palette_count()>=4093)){error="Import would exceed the library's sprite, palette or decoded-byte limits.";return false;}
    if(palette>=0)b.note+=" Colors matched to the chosen palette; indices can change.";
    b.frames.push_back(std::move(frame));out=std::move(b);return true;
}
bool snapshot_indexed_sprite(const Artwork &art,int index,IndexedMedia &out,std::string &error){
    auto im=art.sprite(index);if(!im||!im->data_p||!dimensions(im->w,im->h)){error="Choose a sprite with valid export dimensions.";return false;}IndexedMedia m;m.w=im->w;m.h=im->h;m.anchor_x=(short)im->anix;m.anchor_y=(short)im->aniy;
    m.words=copy_palette(art,im->palnum).words;m.pixels.resize((size_t)m.w*m.h);size_t stride=(m.w+3)&~size_t(3);
    checked_layer_bytes(*im);for(int y=0;y<m.h;++y)for(int x=0;x<m.w;++x)m.pixels[(size_t)y*m.w+x]=visible_pixel(*im,x,y);
    if(!valid(m)){error="Sprite dimensions or palette indices are invalid.";return false;}out=std::move(m);error.clear();return true;
}
bool encode_indexed_media(const IndexedMedia &m,LegacyMediaKind kind,Bytes &out,std::string &error){
    if(!valid(m)||m.anchor_x<-32768||m.anchor_x>32767||m.anchor_y<-32768||m.anchor_y>32767){error="Cannot export invalid indexed pixels, palette or anchors.";return false;}Bytes b;
    if(kind==LegacyMediaKind::Lbm){b.insert(b.end(),{'F','O','R','M',0,0,0,0,'P','B','M',' '});Bytes header;word(header,m.w,true);word(header,m.h,true);word(header,0,true);word(header,0,true);header.insert(header.end(),{8,2,0,0});word(header,0,true);header.insert(header.end(),{1,1});word(header,m.w,true);word(header,m.h,true);chunk(b,"BMHD",header);
        Bytes palette;for(auto c:m.words){unsigned char raw[]={(unsigned char)c,(unsigned char)(c>>8)},r,g,blue;pal_word_to_rgb8(raw,&r,&g,&blue);palette.insert(palette.end(),{r,g,blue});}chunk(b,"CMAP",palette);
        Bytes grab;word(grab,(unsigned short)m.anchor_x,true);word(grab,(unsigned short)m.anchor_y,true);chunk(b,"GRAB",grab);Bytes body;for(int y=0;y<m.h;++y){body.insert(body.end(),m.pixels.begin()+(size_t)y*m.w,m.pixels.begin()+(size_t)(y+1)*m.w);if(m.w&1)body.push_back(0);}chunk(b,"BODY",body);unsigned size=(unsigned)b.size()-8;for(int s=0;s<4;++s)b[4+s]=(unsigned char)(size>>(24-s*8));
    }else{b={0,1,1,0,0};word(b,(unsigned)m.words.size(),false);b.push_back(32);word(b,0,false);word(b,0,false);word(b,m.w,false);word(b,m.h,false);b.insert(b.end(),{8,40});
        for(size_t i=0;i<m.words.size();++i){unsigned c=m.words[i];unsigned char raw[]={(unsigned char)c,(unsigned char)(c>>8)},r,g,blue;pal_word_to_rgb8(raw,&r,&g,&blue);b.insert(b.end(),{blue,g,r,(unsigned char)(i?255:0)});}b.insert(b.end(),m.pixels.begin(),m.pixels.end());}
    out=std::move(b);error.clear();return true;
}
bool export_indexed_sprite(const Artwork &art,int index,const std::string &path,LegacyMediaKind kind,std::string &error){
    IndexedExportReview review;return review_indexed_export(art,index,path,kind,review,error)&&write_indexed_export(art,review,error);
}
bool review_indexed_export(const Artwork &art,int index,const std::string &path,LegacyMediaKind kind,IndexedExportReview &out,std::string &error){
    namespace fs=std::filesystem;auto extension=ext(fs::u8path(path).extension().u8string());
    if(extension!=(kind==LegacyMediaKind::Lbm?".lbm":".tga")){error="Choose the matching .lbm or .tga filename.";return false;}
    std::error_code ec;auto target=fs::absolute(fs::u8path(path),ec).lexically_normal();if(ec){error="Invalid destination path.";return false;}
    if(!art.path().empty()&&(target==fs::u8path(art.path()).lexically_normal()||fs::equivalent(target,fs::u8path(art.path()),ec))){error="Export cannot replace the source IMG library.";return false;}
    IndexedExportReview r;r.owner=&art;r.generation=art.generation();r.sprite=art.sprite_id(index);r.kind=kind;r.path=target.u8string();
    if(!snapshot_indexed_sprite(art,index,r.media,error)||!encode_indexed_media(r.media,kind,r.bytes,error))return false;
    ec.clear();r.existed=fs::exists(target,ec);if(ec){error="Could not inspect the export destination.";return false;}
    if(r.existed){std::ifstream f(target,std::ios::binary|std::ios::ate);auto n=f?f.tellg():std::streampos(-1);if(n<0||n>64*1024*1024){error="Cannot review this destination (limit: 64 MiB).";return false;}r.previous.resize((size_t)n);f.seekg(0);if(n&&!f.read((char *)r.previous.data(),r.previous.size())){error="Could not review the destination.";return false;}}
    out=std::move(r);error.clear();return true;
}
bool write_indexed_export(const Artwork &art,const IndexedExportReview &r,std::string &error){
    if(r.owner!=&art||r.generation!=art.generation()){error="The library changed. Rebuild the export review.";return false;}
    IndexedExportReview now;if(!review_indexed_export(art,art.sprite_index(r.sprite),r.path,r.kind,now,error))return false;
    if(now.bytes!=r.bytes||now.media.w!=r.media.w||now.media.h!=r.media.h||now.media.anchor_x!=r.media.anchor_x||now.media.anchor_y!=r.media.anchor_y||now.media.words!=r.media.words||now.media.pixels!=r.media.pixels){error="Sprite pixels, palette or export settings changed. Rebuild the review.";return false;}
    if(now.existed!=r.existed||now.previous!=r.previous){error="The destination changed after review. Rebuild before replacing it.";return false;}
    return atomic_write_file(r.path,r.bytes,error);
}
}
