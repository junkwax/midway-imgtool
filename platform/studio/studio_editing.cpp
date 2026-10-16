#include "studio_document.h"
#include "studio_layers.h"
#include "img_io.h"
#include "img_util.h"
#include "anipoint.h"
#include "canvas_ops.h"
#include "sprite_resize_ops.h"
#include "palette_math.h"
#include "stb_image_write.h"
#include "stb_image.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <chrono>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace studio {
namespace {
struct Scope {Document *old;explicit Scope(Document &d):old(g_doc){g_doc=&d;}~Scope(){g_doc=old;}};
void *clone_bytes(const void *p,size_t n){if(!p||!n)return nullptr;void *copy=malloc(n);if(!copy)throw std::bad_alloc();memcpy(copy,p,n);return copy;}
size_t pixel_bytes(int w,int h){return (((size_t)w+3)&~size_t(3))*h;}
bool dimensions(int w,int h,std::string &error){if(w<1||h<1||w>8192||h>8192||(size_t)w*h>4*1024*1024){error="Use dimensions from 1 to 8192, with at most 4 million pixels.";return false;}return true;}
bool fits_document(const Artwork &art,int w,int h,int replace,std::string &error){
    size_t bytes=pixel_bytes(w,h);for(int i=0;i<art.count();++i)if(i!=replace){auto im=art.sprite(i);bytes+=pixel_bytes(im->w,im->h);}
    if(bytes>128*1024*1024){error="This edit would exceed the 128 MiB decoded IMG limit.";return false;}return true;
}
bool name_valid(const std::string &name,std::string &error){
    if(name.empty()||name.size()>15){error="Sprite names must contain 1 to 15 characters.";return false;}
    for(unsigned char c:name)if(c<33||c>126){error="Use printable ASCII characters without spaces for an IMG name.";return false;}return true;
}
unsigned short u16(const unsigned char *p){return (unsigned short)(p[0]|p[1]<<8);}
void set16(unsigned char *p,int value){p[0]=(unsigned char)value;p[1]=(unsigned char)((unsigned)value>>8);}
IMG *copy_sprite(Document &d,const IMG &source){
    Scope scope(d);IMG *im=AllocImg();if(!im)throw std::bad_alloc();*im=source;
    im->nxt_p=im->temp=im->data_p=im->baseline_p=im->pttbl_p=im->opaltbl_p=im->layer_p=nullptr;
    im->data_p=clone_bytes(source.data_p,pixel_bytes(source.w,source.h));
    im->baseline_p=clone_bytes(source.baseline_p,pixel_bytes(source.baseline_w,source.baseline_h));
    im->pttbl_p=clone_bytes(source.pttbl_p,40);im->opaltbl_p=clone_bytes(source.opaltbl_p,16);im->layer_p=clone_bytes(source.layer_p,checked_layer_bytes(source));return im;
}
void replace_pixels(IMG &im,void *pixels,int w,int h){free(im.data_p);im.data_p=pixels;im.w=(unsigned short)w;im.h=(unsigned short)h;}
void offset_anchors(IMG &im,int dx,int dy){
    im.anix=signed_to_img_word((short)im.anix+dx);im.aniy=signed_to_img_word((short)im.aniy+dy);
    if(secondary_anipoint_in_use(&im)){im.anix2=signed_to_img_word((short)im.anix2+dx);im.aniy2=signed_to_img_word((short)im.aniy2+dy);}
    if(im.pttbl_p)for(int i=0;i<10;++i){auto p=(unsigned char *)im.pttbl_p+i*4;set16(p,signed_to_img_word((short)u16(p)+dx));set16(p+2,signed_to_img_word((short)u16(p+2)+dy));}
}
}
std::unique_ptr<Artwork> Artwork::create(){
    auto art=std::make_unique<Artwork>();std::string error;
    {Scope scope(art->data);PAL *p=AllocPal();if(!p)throw std::bad_alloc();strcpy(p->n_s,"DEFAULT");p->numc=16;p->bitspix=4;p->data_p=calloc(16,2);if(!p->data_p)throw std::bad_alloc();
        for(int i=1;i<16;++i){int c=(i-1)*31/14;set16((unsigned char *)p->data_p+i*2,(c<<10)|(c<<5)|c);}}
    art->add_sprite("SPRITE1",64,64,0,error);art->past.clear();return art;
}
std::string Artwork::unique_sprite_name(const std::string &stem) const{
    auto available=[&](const std::string &n){for(int i=0;i<count();++i)if(ascii_iequals(n,sprite(i)->n_s))return false;return true;};
    std::string base=stem.substr(0,15);if(available(base))return base;
    // A numeric _2 suffix means a grid-chop child to the existing name parser.
    // Copies must remain independent frames rather than replacing their parent.
    for(int n=1;;++n){std::string suffix="_COPY"+(n==1?std::string():std::to_string(n)),candidate=base.substr(0,15-suffix.size())+suffix;if(available(candidate))return candidate;}
}
bool Artwork::rename_sprite(int index,const std::string &name,std::string &error){
    error.clear();IMG *im=sprite(index);if(!im||!name_valid(name,error))return false;
    for(int i=0;i<count();++i)if(i!=index&&ascii_iequals(sprite(i)->n_s,name)){error="A sprite with that name already exists.";return false;}
    begin("Rename sprite");memset(im->n_s,0,16);memcpy(im->n_s,name.data(),name.size());++serial;++structure_serial;commit();return true;
}
int Artwork::add_sprite(const std::string &name,int w,int h,int pal,std::string &error){
    error.clear();if(!dimensions(w,h,error)||!name_valid(name,error)||!palette(pal))return -1;
    if(!fits_document(*this,w,h,-1,error))return -1;
    if(count()>=2000){error="This IMG has reached its 2000-sprite limit.";return -1;}
    for(int i=0;i<count();++i)if(ascii_iequals(sprite(i)->n_s,name)){error="A sprite with that name already exists.";return -1;}
    void *pixels=calloc(1,pixel_bytes(w,h));if(!pixels){error="Not enough memory for this sprite.";return -1;}
    begin("New sprite");Scope scope(data);IMG *im=AllocImg();if(!im){free(pixels);cancel();error="Not enough memory.";return -1;}
    memcpy(im->n_s,name.data(),name.size());im->w=(unsigned short)w;im->h=(unsigned short)h;im->palnum=(unsigned short)pal;
    im->anix=(unsigned short)(w/2);im->aniy=(unsigned short)(h-1);im->opals=im->file_pttblnum=im->file_frm=0xffff;im->data_p=pixels;
    sprite_ids.push_back(next_sprite_id++);if(!selected_sprite_id)selected_sprite_id=sprite_ids.back();++serial;++structure_serial;commit();return count()-1;
}
int Artwork::duplicate_sprite(int index,std::string &error){
    error.clear();auto im=sprite(index);if(!im)return -1;
    if(!fits_document(*this,im->w,im->h,-1,error))return -1;
    if(count()>=2000){error="This IMG has reached its 2000-sprite limit.";return -1;}
    std::string name=unique_sprite_name(im->n_s);begin("Duplicate sprite");
    IMG *copy=copy_sprite(data,*im);memset(copy->n_s,0,16);memcpy(copy->n_s,name.data(),name.size());memset(copy->file_name_raw,0,16);
    sprite_ids.push_back(next_sprite_id++);++serial;++structure_serial;commit();return count()-1;
}
bool Artwork::remap_sequence_indices(const std::vector<int> &mapping,std::vector<unsigned char> &blob,std::string &error) const{
    if(!data.scrseqbytes)return true;
    if(!data.scrseqmem_p){error="Embedded animation data is unavailable.";return false;}
    blob.assign((unsigned char *)data.scrseqmem_p,(unsigned char *)data.scrseqmem_p+data.scrseqbytes);
    size_t pos=0;for(unsigned s=0;s<data.seqcnt+data.scrcnt;++s){
        if(pos+98>blob.size()){error="Embedded animation data is truncated.";return false;}
        unsigned entries=u16(blob.data()+pos+18);
        if(entries>(blob.size()-pos-98)/18){error="Embedded animation entries are truncated.";return false;}
        if(s<data.seqcnt)for(unsigned e=0;e<entries;++e){auto p=blob.data()+pos+98+e*18+4;int target=(short)u16(p);
            if(target<0)continue;
            if(target>=(int)mapping.size()){error="An embedded animation has an unresolved sprite index. Fix it before changing sprite order.";return false;}
            if(mapping[target]<0){error="This sprite is used by embedded animation '"+std::string((char *)blob.data()+pos,strnlen((char *)blob.data()+pos,16))+"'. Remove that reference before deleting it.";return false;}
            set16(p,mapping[target]);
        }pos+=98+entries*18;
    }return true;
}
bool Artwork::delete_sprite(int index,std::string &error){
    error.clear();auto im=sprite(index);if(!im)return false;if(count()==1){error="Keep at least one sprite in an IMG library.";return false;}
    std::vector<int> mapping(count());for(int i=0;i<count();++i)mapping[i]=i==index?-1:i-(i>index);
    std::vector<unsigned char> blob;if(!remap_sequence_indices(mapping,blob,error))return false;
    begin("Delete sprite");IMG **link=(IMG **)&data.img_p;for(int i=0;i<index;++i)link=(IMG **)&(*link)->nxt_p;
    *link=(IMG *)im->nxt_p;FreeImg(im);--data.imgcnt;sprite_ids.erase(sprite_ids.begin()+index);
    if(sprite_index(selected_sprite_id)<0)selected_sprite_id=sprite_ids[std::min(index,count()-1)];
    if(!blob.empty())memcpy(data.scrseqmem_p,blob.data(),blob.size());++serial;++structure_serial;commit();return true;
}
bool Artwork::move_sprite(int index,int destination,std::string &error){
    error.clear();if(!sprite(index)||destination<0||destination>=count())return false;if(index==destination)return true;
    std::vector<IMG *> images;for(IMG *im=(IMG *)data.img_p;im;im=(IMG *)im->nxt_p)images.push_back(im);
    std::vector<int> order(count());std::iota(order.begin(),order.end(),0);int moving=order[index];order.erase(order.begin()+index);order.insert(order.begin()+destination,moving);
    std::vector<int> mapping(count());for(int i=0;i<count();++i)mapping[order[i]]=i;
    std::vector<unsigned char> blob;if(!remap_sequence_indices(mapping,blob,error))return false;
    begin("Reorder sprites");auto ids=sprite_ids;data.img_p=images[order[0]];
    for(int i=0;i<count();++i){images[order[i]]->nxt_p=i+1<count()?images[order[i+1]]:nullptr;sprite_ids[i]=ids[order[i]];}
    if(!blob.empty())memcpy(data.scrseqmem_p,blob.data(),blob.size());++serial;++structure_serial;commit();return true;
}
bool Artwork::resize_sprite(int index,int w,int h,bool quality,bool scale_anchors,std::string &error){
    error.clear();auto im=sprite(index);if(im&&im->layer_p){error="Flatten or remove the overlay before changing geometry or palette assignment.";return false;}if(!im||!dimensions(w,h,error))return false;if(w==im->w&&h==im->h)return true;
    if(!fits_document(*this,w,h,index,error))return false;
    unsigned stride;unsigned char *pixels=quality?ResizeSpritePixelsQuality(im,palette(im->palnum),nullptr,w,h,false,&stride):ResizeSpritePixelsNearest(im,w,h,&stride);
    if(!pixels){error="Not enough memory to resize.";return false;}
    begin("Resize sprite");if(scale_anchors){
        auto scale=[&](unsigned short x,int before,int after){return signed_to_img_word((int)std::round((short)x*(double)after/before));};
        im->anix=scale(im->anix,im->w,w);im->aniy=scale(im->aniy,im->h,h);
        if(secondary_anipoint_in_use(im)){im->anix2=scale(im->anix2,im->w,w);im->aniy2=scale(im->aniy2,im->h,h);}
        if(im->pttbl_p)for(int p=0;p<10;++p){auto point=(unsigned char *)im->pttbl_p+p*4;set16(point,scale(u16(point),im->w,w));set16(point+2,scale(u16(point+2),im->h,h));}
    }
    replace_pixels(*im,pixels,w,h);++serial;++structure_serial;commit();return true;
}
bool Artwork::canvas_size(int index,int w,int h,int placement,std::string &error){
    error.clear();auto im=sprite(index);if(im&&im->layer_p){error="Flatten or remove the overlay before changing geometry or palette assignment.";return false;}if(!im||!dimensions(w,h,error)||!fits_document(*this,w,h,index,error))return false;int dx,dy;CanvasAnchorOffset(im->w,im->h,w,h,placement,&dx,&dy);
    auto pixels=(unsigned char *)calloc(1,pixel_bytes(w,h));if(!pixels){error="Not enough memory for canvas size.";return false;}
    CanvasBlitIndexedOffset((unsigned char *)im->data_p,im->w,im->h,(im->w+3)&~3,pixels,w,h,(w+3)&~3,dx,dy);
    begin("Change canvas size");offset_anchors(*im,dx,dy);replace_pixels(*im,pixels,w,h);++serial;++structure_serial;commit();return true;
}
bool Artwork::trim_sprite(int index,std::string &error){
    error.clear();auto im=sprite(index);if(im&&im->layer_p){error="Flatten or remove the overlay before changing geometry or palette assignment.";return false;}if(!im)return false;int left,top,right,bottom;
    if(!CanvasIndexedContentBounds((unsigned char *)im->data_p,im->w,im->h,(im->w+3)&~3,&left,&top,&right,&bottom)){error="This sprite is entirely transparent.";return false;}
    int w=right-left+1,h=bottom-top+1;if(!left&&!top&&w==im->w&&h==im->h)return true;
    auto pixels=(unsigned char *)calloc(1,pixel_bytes(w,h));if(!pixels){error="Not enough memory to trim.";return false;}
    CanvasBlitIndexedOffset((unsigned char *)im->data_p,im->w,im->h,(im->w+3)&~3,pixels,w,h,(w+3)&~3,-left,-top);
    begin("Trim sprite bounds");offset_anchors(*im,-left,-top);replace_pixels(*im,pixels,w,h);++serial;++structure_serial;commit();return true;
}
bool Artwork::transform_sprite(int index,Transform op,bool transform_anchors,std::string &error){
    error.clear();auto im=sprite(index);if(im&&im->layer_p){error="Flatten or remove the overlay before changing geometry or palette assignment.";return false;}if(!im)return false;int oldw=im->w,oldh=im->h;
    bool quarter=op==Transform::RotateClockwise||op==Transform::RotateCounterclockwise;int w=quarter?oldh:oldw,h=quarter?oldw:oldh;
    auto pixels=(unsigned char *)calloc(1,pixel_bytes(w,h));if(!pixels){error="Not enough memory to transform.";return false;}
    auto transform=[&](int x,int y,int &dx,int &dy){dx=x;dy=y;switch(op){
        case Transform::FlipHorizontal:dx=oldw-1-x;break;case Transform::FlipVertical:dy=oldh-1-y;break;
        case Transform::RotateClockwise:dx=oldh-1-y;dy=x;break;case Transform::RotateCounterclockwise:dx=y;dy=oldw-1-x;break;
        case Transform::Rotate180:dx=oldw-1-x;dy=oldh-1-y;break;}};
    for(int y=0;y<oldh;++y)for(int x=0;x<oldw;++x){int dx,dy;transform(x,y,dx,dy);pixels[dy*((w+3)&~3)+dx]=((unsigned char *)im->data_p)[y*((oldw+3)&~3)+x];}
    begin("Transform sprite");if(transform_anchors){
        // IMG anchors use edge coordinates, matching classic's w-x / h-y math.
        auto point=[&](unsigned short &x,unsigned short &y){int dx,dy;transform((short)x,(short)y,dx,dy);
            if(op==Transform::FlipHorizontal||op==Transform::RotateClockwise||op==Transform::Rotate180)++dx;
            if(op==Transform::FlipVertical||op==Transform::RotateCounterclockwise||op==Transform::Rotate180)++dy;
            x=signed_to_img_word(dx);y=signed_to_img_word(dy);};
        point(im->anix,im->aniy);if(secondary_anipoint_in_use(im))point(im->anix2,im->aniy2);
        if(im->pttbl_p)for(int p=0;p<10;++p){auto ptr=(unsigned char *)im->pttbl_p+p*4;unsigned short x=u16(ptr),y=u16(ptr+2);point(x,y);set16(ptr,x);set16(ptr+2,y);}
    }replace_pixels(*im,pixels,w,h);++serial;++structure_serial;commit();return true;
}
bool Artwork::assign_palette(int index,int target,bool match,std::string &error){
    error.clear();auto im=sprite(index);if(im&&im->layer_p){error="Flatten or remove the overlay before changing geometry or palette assignment.";return false;}auto pal=palette(target);if(!im||!pal)return false;if(target==im->palnum)return true;
    auto source=palette(im->palnum);unsigned char map[256]={};
    if(match&&pal->numc<2){error="Color matching needs at least one opaque color in the destination palette.";return false;}
    if(match)for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x)if(((unsigned char *)im->data_p)[y*((im->w+3)&~3)+x]>=source->numc){error="This sprite contains unresolved palette indices. Correct them before matching colors.";return false;}
    if(match){for(int i=1;i<source->numc;++i)map[i]=nearest_palette_index_for_word(palette_word_at((unsigned char *)source->data_p,i),pal);}
    else {for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x)if(((unsigned char *)im->data_p)[y*((im->w+3)&~3)+x]>=pal->numc){error="The target palette has too few colors for these pixel indices. Choose Match colors.";return false;}}
    begin("Assign sprite palette");if(match)for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){auto &ci=((unsigned char *)im->data_p)[y*((im->w+3)&~3)+x];ci=map[ci];}
    im->palnum=(unsigned short)target;++serial;commit();return true;
}
int Artwork::duplicate_palette(int index,std::string &error,int assign_sprite,const std::string &name){
    error.clear();auto source=palette(index);if(!source)return -1;if(palette_count()>=4093){error="The palette limit has been reached.";return -1;}
    if(name.size()>9||std::any_of(name.begin(),name.end(),[](unsigned char c){return c<33||c>126;})){error="Use up to 9 printable ASCII characters without spaces for the palette name.";return -1;}
    if(assign_sprite>=0&&!sprite(assign_sprite)){error="Choose a sprite for this palette.";return -1;}
    void *colors=clone_bytes(source->data_p,source->numc*2);begin(assign_sprite>=0?"Make sprite palette private":"Duplicate palette");Scope scope(data);auto pal=AllocPal();if(!pal){free(colors);cancel();error="Not enough memory.";return -1;}
    *pal=*source;pal->nxt_p=pal->temp=nullptr;pal->data_p=colors;memset(pal->file_name_raw,0,10);
    memset(pal->n_s,0,10);if(name.empty())snprintf(pal->n_s,10,"PAL%u",data.palcnt);else memcpy(pal->n_s,name.data(),name.size());if(assign_sprite>=0)sprite(assign_sprite)->palnum=(unsigned short)(data.palcnt-1);++serial;++structure_serial;commit();return palette_count()-1;
}
int Artwork::import_png(const std::string &path,int match,std::string &error){
    error.clear();if(count()>=2000){error="This IMG has reached its sprite limit.";return -1;}
    std::ifstream file(std::filesystem::u8path(path),std::ios::binary|std::ios::ate);auto length=file?file.tellg():std::streampos(-1);
    if(length<=0||length>64*1024*1024){error="Could not read the PNG (limit: 64 MiB).";return -1;}
    std::vector<unsigned char> bytes((size_t)length);file.seekg(0);if(!file.read((char *)bytes.data(),bytes.size())){error="Could not finish reading PNG.";return -1;}
    if(bytes.size()<8||memcmp(bytes.data(),"\x89PNG\r\n\x1a\n",8)){error="Choose a PNG image.";return -1;}
    auto imported=std::make_unique<Artwork>();
    {Scope scope(imported->data);if(!ImportPngFromMemory(bytes.data(),(int)bytes.size(),"IMPORTED")){error="PNG decoding failed or its dimensions exceed 4 million pixels.";return -1;}}
    auto im=imported->sprite(0);auto pal=imported->palette(0);if(!im||!im->data_p||!pal||!pal->data_p){error="Not enough memory to import PNG.";return -1;}
    if(!fits_document(*this,im->w,im->h,-1,error))return -1;
    if(match>=0&&!palette(match)){error="Choose a destination palette.";return -1;}
    if(match>=0&&palette(match)->numc<2){error="PNG color matching needs at least one opaque palette color.";return -1;}
    if(match<0&&palette_count()>=4093){error="The palette limit has been reached.";return -1;}
    std::string stem=std::filesystem::u8path(path).stem().u8string();for(char &c:stem)if(!std::isalnum((unsigned char)c)&&c!='_')c='_';
    if(stem.empty())stem="IMPORTED";std::string name=unique_sprite_name(stem);
    std::unique_ptr<unsigned char,decltype(&stbi_image_free)> rgba(nullptr,stbi_image_free);
    if(match>=0){int w,h,channels;rgba.reset(stbi_load_from_memory(bytes.data(),(int)bytes.size(),&w,&h,&channels,4));if(!rgba){error="Could not decode PNG for palette matching.";return -1;}}
    begin("Import PNG sprite");IMG *copy=copy_sprite(data,*im);memset(copy->n_s,0,16);memcpy(copy->n_s,name.data(),name.size());copy->anix=copy->w/2;copy->aniy=copy->h-1;
    if(match<0){Scope scope(data);auto added=AllocPal();if(!added)throw std::bad_alloc();*added=*pal;added->nxt_p=added->temp=nullptr;added->data_p=clone_bytes(pal->data_p,pal->numc*2);
        snprintf(added->n_s,10,"PNG%u",data.palcnt);memset(added->file_name_raw,0,10);copy->palnum=(unsigned short)(data.palcnt-1);
    }else{auto target=palette(match);std::vector<int> map(32768,-1);
        for(int y=0;y<copy->h;++y)for(int x=0;x<copy->w;++x){auto p=rgba.get()+((size_t)y*copy->w+x)*4;auto &ci=((unsigned char *)copy->data_p)[y*((copy->w+3)&~3)+x];
            if(p[3]<128){ci=0;continue;}int word=((p[0]>>3)<<10)|((p[1]>>3)<<5)|(p[2]>>3);
            if(map[word]<0)map[word]=nearest_palette_index_for_word((unsigned short)word,target);ci=(unsigned char)map[word];
        }copy->palnum=(unsigned short)match;
    }
    sprite_ids.push_back(next_sprite_id++);++serial;++structure_serial;commit();return count()-1;
}
bool Artwork::export_png(int index,const std::string &path,std::string &error) const{
    error.clear();auto im=sprite(index);if(!im){error="Choose a sprite to export.";return false;}auto pal=palette(im->palnum);
    checked_layer_bytes(*im);std::vector<unsigned char> rgba((size_t)im->w*im->h*4);
    for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){int ci=visible_pixel(*im,x,y);auto p=rgba.data()+((size_t)y*im->w+x)*4;
        if(ci<pal->numc)pal_word_to_rgb8((unsigned char *)pal->data_p+ci*2,p,p+1,p+2);else{p[0]=255;p[2]=255;}p[3]=ci?255:0;}
    std::vector<unsigned char> png;auto write=[](void *context,void *bytes,int n){auto &out=*(std::vector<unsigned char> *)context;out.insert(out.end(),(unsigned char *)bytes,(unsigned char *)bytes+n);};
    if(!stbi_write_png_to_func(write,&png,im->w,im->h,4,rgba.data(),im->w*4)){error="PNG encoding failed.";return false;}
    std::error_code ec;auto dest=std::filesystem::absolute(std::filesystem::u8path(path),ec);
    std::string extension=dest.extension().u8string();for(char &c:extension)c=(char)std::tolower((unsigned char)c);
    if(ec||extension!=".png"){error="Choose a .png filename.";return false;}
    if(!file_path.empty()&&std::filesystem::equivalent(dest,std::filesystem::u8path(file_path),ec)){error="PNG export cannot replace the source IMG.";return false;}
    auto temp=dest;temp+=".studio-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".tmp";
    std::ofstream output(temp,std::ios::binary);if(!output){error="Cannot write PNG in this folder.";return false;}
    output.write((char *)png.data(),png.size());output.close();bool ok=!output.fail();
#ifdef _WIN32
    if(ok)ok=MoveFileExW(temp.c_str(),dest.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    if(ok){std::filesystem::rename(temp,dest,ec);ok=!ec;}
#endif
    if(!ok){std::filesystem::remove(temp,ec);error="Could not finish writing PNG. The destination was left intact.";}return ok;
}
}
