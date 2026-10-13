#include "studio_repair.h"
#include "palette_math.h"
#include <algorithm>
#include <array>
#include <cmath>
namespace studio {
namespace {
bool valid(const Clipboard &c,const PixelSelection &mask){return c.w>0&&c.h>0&&c.w<=8192&&c.h<=8192&&(size_t)c.w*c.h<=4*1024*1024&&c.pixels.size()==(size_t)c.w*c.h&&!c.colors.empty()&&c.colors.size()<=256&&mask.fits(c.w,c.h)&&std::none_of(c.pixels.begin(),c.pixels.end(),[&](unsigned char v){return v>=c.colors.size();});}
bool brush_valid(const RepairBrush &b){return (b.kind==RepairKind::Heal||b.kind==RepairKind::Defringe)&&b.radius>=1&&b.radius<=32&&b.passes>=1&&b.passes<=64&&b.edge_width>=1&&b.edge_width<=4&&(b.shape==BrushShape::Round||b.shape==BrushShape::Square);}
bool selected(const Clipboard &c,const PixelSelection &s,int x,int y){return x>=0&&y>=0&&x<c.w&&y<c.h&&(!s.valid||s.contains(x,y));}
bool snapshot_valid(const RepairStroke &s){auto &c=s.original;if(!valid(c,s.selection)||!brush_valid(s.brush)||s.coverage.size()!=c.pixels.size()||s.pixels.size()!=c.pixels.size())return false;for(size_t at=0;at<c.pixels.size();++at){if(s.pixels[at]>=c.colors.size()||(!c.pixels[at])!=(!s.pixels[at])||s.coverage[at]>1||(s.coverage[at]&&(!c.pixels[at]||!selected(c,s.selection,(int)(at%c.w),(int)(at/c.w)))))return false;}return true;}
struct Rgb {double r=0,g=0,b=0;};
std::array<Rgb,256> colors(const Clipboard &c){std::array<Rgb,256> result{};for(size_t i=0;i<c.colors.size();++i){auto w=c.colors[i];result[i]={(double)((w>>10)&31),(double)((w>>5)&31),(double)(w&31)};}return result;}
double distance(Rgb a,Rgb b){double r=a.r-b.r,g=a.g-b.g,blue=a.b-b.b;return r*r+g*g+blue*blue;}
unsigned char nearest(const Clipboard &c,const std::array<Rgb,256> &rgb,int here,Rgb target){int best=here;double d=distance(rgb[here],target);for(int i=1;i<(int)c.colors.size();++i){double next=distance(rgb[i],target);if(next<d-1e-12){d=next;best=i;}}return (unsigned char)best;}
template<class Visit> void neighbors(const Clipboard &c,size_t at,Visit visit){int x=(int)(at%c.w),y=(int)(at/c.w);for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){int nx=x+dx,ny=y+dy;if((dx||dy)&&nx>=0&&ny>=0&&nx<c.w&&ny<c.h)visit((size_t)ny*c.w+nx);}}
// Distance from transparency (including the canvas border), eight-connected.
// Classification uses actual geometry; selection boundaries are not sprite edges.
std::vector<unsigned char> edge_band(const Clipboard &c,int width){
    std::vector<unsigned char> depth(c.pixels.size());std::vector<size_t> frontier;
    for(size_t at=0;at<c.pixels.size();++at)if(c.pixels[at]){int x=(int)(at%c.w),y=(int)(at/c.w);bool edge=x==0||y==0||x==c.w-1||y==c.h-1;neighbors(c,at,[&](size_t n){if(!c.pixels[n])edge=true;});if(edge){depth[at]=1;frontier.push_back(at);}}
    for(int layer=2;layer<=width&&!frontier.empty();++layer){std::vector<size_t> next;for(auto at:frontier)neighbors(c,at,[&](size_t n){if(c.pixels[n]&&!depth[n]){depth[n]=(unsigned char)layer;next.push_back(n);}});frontier.swap(next);}return depth;
}
// Synchronous waves diffuse from safe opaque pixels, in O(region * 8) work.
// Working indices are quantized per wave, matching what can be stored in an IMG.
void diffuse(const Clipboard &c,const PixelSelection &scope,const std::vector<unsigned char> &region,int passes,const std::array<Rgb,256> &rgb,std::vector<unsigned char> &filled){
    filled=c.pixels;std::vector<size_t> frontier;std::vector<unsigned char> queued(c.pixels.size());
    for(size_t at=0;at<region.size();++at)if(region[at])filled[at]=0;
    for(size_t at=0;at<region.size();++at)if(region[at]){bool ready=false;neighbors(c,at,[&](size_t n){if(filled[n]&&selected(c,scope,(int)(n%c.w),(int)(n/c.w)))ready=true;});if(ready){frontier.push_back(at);queued[at]=1;}}
    for(int pass=0;pass<passes&&!frontier.empty();++pass){std::vector<std::pair<size_t,unsigned char>> writes;writes.reserve(frontier.size());
        for(auto at:frontier){Rgb sum;int count=0;neighbors(c,at,[&](size_t n){if(!filled[n]||!selected(c,scope,(int)(n%c.w),(int)(n/c.w)))return;auto value=rgb[filled[n]];sum.r+=value.r;sum.g+=value.g;sum.b+=value.b;++count;});if(count){sum.r/=count;sum.g/=count;sum.b/=count;writes.emplace_back(at,nearest(c,rgb,c.pixels[at],sum));}}
        for(auto write:writes)filled[write.first]=write.second;
        std::vector<size_t> next;for(auto write:writes)neighbors(c,write.first,[&](size_t n){if(region[n]&&!queued[n]){queued[n]=1;next.push_back(n);}});frontier.swap(next);
    }
}
int difference(const std::vector<unsigned char> &a,const std::vector<unsigned char> &b){int changed=0;for(size_t i=0;i<a.size();++i)changed+=a[i]!=b[i];return changed;}
}
bool begin_repair_stroke(const Artwork &art,int index,int x,int y,const RepairBrush &brush,const PixelSelection &selection,RepairStroke &out,std::string &error){
    out={};error.clear();auto im=art.sprite(index);if(!im||!im->data_p||im->w>8192||im->h>8192||(size_t)im->w*im->h>4*1024*1024||!brush_valid(brush)||x<0||y<0||x>=im->w||y>=im->h){error="Start a repair inside a valid sprite.";return false;}auto source=art.copy(index,0,0,im->w,im->h);if(!valid(source,selection)){error="The repair source, palette or selection is invalid.";return false;}
    RepairStroke s;s.owner=&art;s.generation=art.generation();s.sprite=art.sprite_id(index);s.original=std::move(source);s.pixels=s.original.pixels;s.coverage.resize(s.pixels.size());s.selection=selection;s.brush=brush;s.last_x=x;s.last_y=y;out=std::move(s);return true;
}
int repair_stroke_pixels(RepairStroke &s,int x,int y,bool all_edges,std::string &error){
    error.clear();auto &c=s.original;if(!snapshot_valid(s)||(all_edges&&s.brush.kind!=RepairKind::Defringe)){error="The repair source, brush or coverage is invalid.";return -1;}
    bool added=false;if(all_edges){for(size_t at=0;at<c.pixels.size();++at)if(c.pixels[at]&&selected(c,s.selection,(int)(at%c.w),(int)(at/c.w))&&!s.coverage[at]){s.coverage[at]=1;added=true;}}
    else{std::vector<std::pair<int,int>> path;if(!make_brush_path(c.w,c.h,s.brush.radius,s.last_x,s.last_y,x,y,path,error))return -1;for(auto center:path){for(int yy=std::max(0,center.second-s.brush.radius+1);yy<=std::min(c.h-1,center.second+s.brush.radius-1);++yy)for(int xx=std::max(0,center.first-s.brush.radius+1);xx<=std::min(c.w-1,center.first+s.brush.radius-1);++xx){int dx=xx-center.first,dy=yy-center.second;if((s.brush.shape==BrushShape::Round&&dx*dx+dy*dy>=s.brush.radius*s.brush.radius)||!selected(c,s.selection,xx,yy))continue;size_t at=(size_t)yy*c.w+xx;if(c.pixels[at]&&!s.coverage[at]){s.coverage[at]=1;added=true;}}}}
    s.last_x=x;s.last_y=y;s.started=true;if(!added||c.colors.size()<2)return 0;auto rgb=colors(c);auto region=s.brush.kind==RepairKind::Defringe?edge_band(c,s.brush.edge_width):s.coverage;
    for(size_t at=0;at<region.size();++at)if(!selected(c,s.selection,(int)(at%c.w),(int)(at/c.w)))region[at]=0;
    std::vector<unsigned char> filled;diffuse(c,s.selection,region,s.brush.kind==RepairKind::Defringe?s.brush.edge_width:s.brush.passes,rgb,filled);auto result=c.pixels;s.unresolved=0;
    for(size_t at=0;at<region.size();++at)if(region[at]&&s.coverage[at]){if(filled[at])result[at]=filled[at];else ++s.unresolved;}
    int changed=difference(s.pixels,result);s.pixels=std::move(result);return changed;
}
int Artwork::repair_stroke(RepairStroke &s,int x,int y,bool all_edges,std::string &error){
    error.clear();if(s.owner!=this||s.generation!=generation()){error="The repair source changed. Start a new stroke.";return -1;}int index=sprite_index(s.sprite);auto im=sprite(index);if(!im||!im->data_p||im->w!=s.original.w||im->h!=s.original.h){error="The repair sprite is unavailable.";return -1;}auto live=copy(index,0,0,im->w,im->h);if(live.pixels!=s.pixels||live.colors!=s.original.colors){error="The repair pixels or palette no longer match.";return -1;}int changed=repair_stroke_pixels(s,x,y,all_edges,error);if(changed<=0)return changed;
    begin(s.brush.kind==RepairKind::Heal?"Heal pixels":"Defringe edges");int stride=(im->w+3)&~3;for(int yy=0;yy<im->h;++yy)memcpy((unsigned char *)im->data_p+(size_t)yy*stride,s.pixels.data()+(size_t)yy*im->w,im->w);++serial;s.generation=generation();return changed;
}
int smart_erase_pixels(const Clipboard &c,const PixelSelection &scope,int x,int y,bool match_colors,int tolerance,bool contiguous,bool cleanup,std::vector<unsigned char> &out,std::string &error){
    error.clear();out.clear();if(!valid(c,scope)||tolerance<0||tolerance>255){error="The eraser source, selection or tolerance is invalid.";return -1;}out=c.pixels;if(!selected(c,scope,x,y)||!out[(size_t)y*c.w+x])return 0;PAL pal{};pal.numc=(unsigned short)c.colors.size();pal.data_p=(void *)c.colors.data();int seed=out[(size_t)y*c.w+x];int erased=fill_matching_pixels(out.data(),c.w,c.h,c.w,pal,x,y,0,match_colors,tolerance,contiguous,&scope,error);if(erased<=0||!cleanup)return erased;
    auto rgb=colors(c);std::vector<unsigned char> kill(out.size());for(size_t at=0;at<out.size();++at)kill[at]=c.pixels[at]&&!out[at];
    for(size_t at=0;at<out.size();++at)if(out[at]&&selected(c,scope,(int)(at%c.w),(int)(at/c.w))){bool touches=false;neighbors(c,at,[&](size_t n){if(kill[n])touches=true;});if(!touches)continue;
        Rgb sum;std::array<unsigned char,8> candidates{};int count=0;neighbors(c,at,[&](size_t n){int slot=c.pixels[n];if(!slot||kill[n]||!selected(c,scope,(int)(n%c.w),(int)(n/c.w))||(match_colors?PaletteIndexWithinTolerance(&pal,seed,slot,tolerance):slot==seed))return;auto color=rgb[slot];sum.r+=color.r;sum.g+=color.g;sum.b+=color.b;candidates[count++]=(unsigned char)slot;});
        if(!count)continue;sum.r/=count;sum.g/=count;sum.b/=count;int best=candidates[0];double d=distance(rgb[best],sum);for(int i=1;i<count;++i){int slot=candidates[i];double next=distance(rgb[slot],sum);if(next<d-1e-12){best=slot;d=next;}}if(distance(rgb[out[at]],sum)<=d+1e-12&&std::find(candidates.begin(),candidates.begin()+count,out[at])!=candidates.begin()+count)best=out[at];out[at]=(unsigned char)best;
    }return difference(c.pixels,out);
}
int Artwork::smart_erase(int index,int x,int y,bool match_colors,int tolerance,bool contiguous,bool cleanup,const PixelSelection &scope,std::string &error){
    auto im=sprite(index);if(!im||!im->data_p||im->w>8192||im->h>8192||(size_t)im->w*im->h>4*1024*1024){error="The eraser sprite is unavailable.";return -1;}auto source=copy(index,0,0,im->w,im->h);std::vector<unsigned char> pixels;int changed=smart_erase_pixels(source,scope,x,y,match_colors,tolerance,contiguous,cleanup,pixels,error);if(changed<=0)return changed;begin(cleanup?"Smart erase and clean edges":"Smart erase pixels");int stride=(im->w+3)&~3;for(int yy=0;yy<im->h;++yy)memcpy((unsigned char *)im->data_p+(size_t)yy*stride,pixels.data()+(size_t)yy*im->w,im->w);++serial;commit();return changed;
}
}
