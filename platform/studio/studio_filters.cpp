#include "studio_filters.h"
#include "palette_math.h"
#include <algorithm>
#include <cmath>
namespace studio {
namespace {
bool dimensions(int w,int h){return w>0&&h>0&&w<=8192&&h<=8192&&(size_t)w*h<=4*1024*1024;}
bool brush_valid(const FilterBrush &b){return (b.kind==FilterKind::Blur||b.kind==FilterKind::Smudge)&&b.radius>=1&&b.radius<=32&&b.strength>=0&&b.strength<=100&&(b.shape==BrushShape::Round||b.shape==BrushShape::Square);}
bool snapshot_valid(const FilterStroke &s){
    auto &c=s.original;return dimensions(c.w,c.h)&&c.pixels.size()==(size_t)c.w*c.h&&s.pixels.size()==c.pixels.size()&&s.visited.size()==c.pixels.size()&&!c.colors.empty()&&c.colors.size()<=256&&s.selection.fits(c.w,c.h)&&brush_valid(s.brush)&&std::none_of(c.pixels.begin(),c.pixels.end(),[&](unsigned char v){return v>=c.colors.size();})&&std::none_of(s.pixels.begin(),s.pixels.end(),[&](unsigned char v){return v>=c.colors.size();});
}
bool selected(const FilterStroke &s,int x,int y){return x>=0&&y>=0&&x<s.original.w&&y<s.original.h&&(!s.selection.valid||s.selection.contains(x,y));}
struct Rgb {int r,g,b;};
Rgb rgb(unsigned short w){return {(w>>10)&31,(w>>5)&31,w&31};}
unsigned char match(FilterStroke &s,const std::array<Rgb,256> &colors,int here,double r,double g,double b){
    int rr=std::clamp((int)std::lround(r),0,31),gg=std::clamp((int)std::lround(g),0,31),bb=std::clamp((int)std::lround(b),0,31);
    auto distance=[&](int slot){auto &c=colors[slot];double dr=r-c.r,dg=g-c.g,db=b-c.b;return dr*dr+dg*dg+db*db;};
    if(std::abs(r-rr)<1e-9&&std::abs(g-gg)<1e-9&&std::abs(b-bb)<1e-9){unsigned short target=(unsigned short)((rr<<10)|(gg<<5)|bb);auto slot=s.nearest[target];
        if(slot==0||slot>=s.original.colors.size()){PAL pal{};pal.numc=(unsigned short)s.original.colors.size();pal.data_p=s.original.colors.data();slot=nearest_palette_index_for_word(target,&pal);s.nearest[target]=slot;}
        return distance(here)<=distance(slot)+1e-12?(unsigned char)here:slot;
    }
    // Preserve fractional averages through matching, avoiding rounding bias and
    // repeated-pass oscillation at ties in a sparse palette.
    int best=here;double best_distance=distance(here);for(int slot=1;slot<(int)s.original.colors.size();++slot){double d=distance(slot);if(d<best_distance-1e-12){best=slot;best_distance=d;}}return (unsigned char)best;
}
int stamp(FilterStroke &s,int cx,int cy,int px,int py,const std::array<Rgb,256> &colors){
    auto &c=s.original;auto &b=s.brush;struct Write{size_t at;unsigned char value;};std::vector<Write> writes;writes.reserve((size_t)(2*b.radius-1)*(2*b.radius-1));
    for(int y=std::max(0,cy-b.radius+1);y<=std::min(c.h-1,cy+b.radius-1);++y)for(int x=std::max(0,cx-b.radius+1);x<=std::min(c.w-1,cx+b.radius-1);++x){
        int dx=x-cx,dy=y-cy;if((b.shape==BrushShape::Round&&dx*dx+dy*dy>=b.radius*b.radius)||!selected(s,x,y))continue;size_t at=(size_t)y*c.w+x;int here=s.pixels[at];if(!here)continue;
        double r=0,g=0,blue=0;int pair=-1;Rgb current=colors[here];
        if(b.kind==FilterKind::Blur){if(s.visited[at])continue;s.visited[at]=1;here=c.pixels[at];current=colors[here];int n=0;
            for(int yy=std::max(0,y-1);yy<=std::min(c.h-1,y+1);++yy)for(int xx=std::max(0,x-1);xx<=std::min(c.w-1,x+1);++xx){if(!selected(s,xx,yy))continue;int index=c.pixels[(size_t)yy*c.w+xx];if(!index)continue;r+=colors[index].r;g+=colors[index].g;blue+=colors[index].b;++n;}
            if(!n)continue;r/=n;g/=n;blue/=n;
        }else{int sx=x-(cx-px),sy=y-(cy-py);if(!selected(s,sx,sy))continue;int from=s.pixels[(size_t)sy*c.w+sx];if(!from)continue;pair=here*256+from;auto cached=s.pairs[pair];if(cached>=2&&cached<=s.original.colors.size()){auto value=(unsigned char)(cached-1);if(value!=here)writes.push_back({at,value});continue;}r=colors[from].r;g=colors[from].g;blue=colors[from].b;}
        double amount=b.strength/100.;auto value=match(s,colors,here,current.r+(r-current.r)*amount,current.g+(g-current.g)*amount,current.b+(blue-current.b)*amount);if(pair>=0)s.pairs[pair]=(unsigned short)value+1;if(value!=s.pixels[at])writes.push_back({at,value});
    }
    // All reads finish before the first write: overlapping pixels cannot self-feed.
    for(auto &write:writes)s.pixels[write.at]=write.value;return (int)writes.size();
}
}
bool begin_filter_stroke(const Artwork &art,int index,int x,int y,const FilterBrush &brush,const PixelSelection &selection,FilterStroke &out,std::string &error){
    out={};error.clear();auto im=art.sprite(index);auto pal=im?art.palette(im->palnum):nullptr;
    if(!im||!im->data_p||!dimensions(im->w,im->h)||!pal||!pal->data_p||pal->numc<1||pal->numc>256||!brush_valid(brush)||!selection.fits(im->w,im->h)||x<0||y<0||x>=im->w||y>=im->h){error="Start a filter stroke inside a valid sprite.";return false;}
    FilterStroke s;s.owner=&art;s.generation=art.generation();s.sprite=art.sprite_id(index);s.original=art.copy(index,0,0,im->w,im->h);s.pixels=s.original.pixels;s.visited.resize(s.pixels.size());s.brush=brush;s.selection=selection;s.last_x=x;s.last_y=y;
    if(!snapshot_valid(s)){error="The sprite contains indices outside its palette.";return false;}out=std::move(s);return true;
}
int filter_stroke_pixels(FilterStroke &s,int x,int y,std::string &error){
    error.clear();if(!snapshot_valid(s)){error="The filter brush, selection or source snapshot is invalid.";return -1;}
    if(s.brush.strength==0||s.original.colors.size()<2||(s.started&&x==s.last_x&&y==s.last_y)){s.last_x=x;s.last_y=y;s.started=true;return 0;}
    if(s.brush.kind==FilterKind::Smudge&&(s.pair_strength!=s.brush.strength||s.pairs.size()!=65536)){s.pairs.assign(65536,0);s.pair_strength=s.brush.strength;}
    std::vector<std::pair<int,int>> path;if(!make_brush_path(s.original.w,s.original.h,s.brush.radius,s.last_x,s.last_y,x,y,path,error))return -1;
    std::array<Rgb,256> colors{};for(size_t i=0;i<s.original.colors.size();++i)colors[i]=rgb(s.original.colors[i]);int changed=0;int px=path.empty()?0:path.front().first,py=path.empty()?0:path.front().second;
    for(auto center:path){bool first=center.first==px&&center.second==py;if(s.brush.kind==FilterKind::Blur||!first)changed+=stamp(s,center.first,center.second,px,py,colors);px=center.first;py=center.second;}
    s.last_x=x;s.last_y=y;s.started=true;return changed;
}
int Artwork::filter_stroke(FilterStroke &s,int x,int y,std::string &error){
    error.clear();if(s.owner!=this||s.generation!=generation()){error="The filter source changed. Start a new stroke.";return -1;}int index=sprite_index(s.sprite);auto im=sprite(index);
    auto pal=im?palette(im->palnum):nullptr;
    if(!im||!im->data_p||!pal||!pal->data_p||pal->numc<1||pal->numc>256||im->w!=s.original.w||im->h!=s.original.h){error="The filter source sprite is unavailable.";return -1;}auto live=copy(index,0,0,im->w,im->h);
    if(live.pixels!=s.pixels||live.colors!=s.original.colors){error="The filter source pixels or palette no longer match.";return -1;}int changed=filter_stroke_pixels(s,x,y,error);if(changed<=0)return changed;
    begin(s.brush.kind==FilterKind::Blur?"Blur pixels":"Smudge pixels");int stride=(im->w+3)&~3;for(int yy=0;yy<im->h;++yy)memcpy((unsigned char *)im->data_p+(size_t)yy*stride,s.pixels.data()+(size_t)yy*im->w,im->w);++serial;s.generation=generation();return changed;
}
}
