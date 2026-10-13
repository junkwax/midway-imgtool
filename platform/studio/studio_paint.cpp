#include "studio_paint.h"
#include "palette_math.h"
#include <algorithm>
#include <array>
#include <cmath>
namespace studio {
namespace {
bool dimensions(int w,int h,int stride){return w>0&&h>0&&w<=8192&&h<=8192&&(size_t)w*h<=4*1024*1024&&stride>=w&&stride<=((w+3)&~3);}
bool palette_valid(const PAL &p){return p.data_p&&p.numc>=1&&p.numc<=256;}
bool brush_valid(const PaintBrush &b,const PAL &p){return palette_valid(p)&&b.color>=0&&b.color<p.numc&&b.radius>=1&&b.radius<=32&&b.source>=-1&&b.source<p.numc&&b.tolerance>=0&&b.tolerance<=255&&(b.shape==BrushShape::Round||b.shape==BrushShape::Square);}
bool selected(const PixelSelection *s,int x,int y){return !s||!s->valid||s->contains(x,y);}
bool matches(PAL &p,int seed,int value,bool colors,int tolerance){return value<p.numc&&(colors?PaletteIndexWithinTolerance(&p,seed,value,tolerance):seed==value);}
// Clip before Bresenham so even INT_MIN/INT_MAX endpoints have bounded work.
bool clip_line(int w,int h,int radius,int &x0,int &y0,int &x1,int &y1){
    double ax=x0,ay=y0,dx=(double)x1-x0,dy=(double)y1-y0,lo=0,hi=1;
    auto edge=[&](double p,double q){if(p==0)return q>=0;double r=q/p;if(p<0){if(r>hi)return false;lo=std::max(lo,r);}else{if(r<lo)return false;hi=std::min(hi,r);}return true;};
    int margin=radius-1;if(!edge(-dx,ax+margin)||!edge(dx,w-1+margin-ax)||!edge(-dy,ay+margin)||!edge(dy,h-1+margin-ay))return false;
    x1=(int)std::lround(ax+hi*dx);y1=(int)std::lround(ay+hi*dy);x0=(int)std::lround(ax+lo*dx);y0=(int)std::lround(ay+lo*dy);return true;
}
// Shared clipping, brush footprint and interpolation for paint and clone tools.
template<class Pixel> int raster_brush(int w,int h,int x0,int y0,int x1,int y1,const PaintBrush &b,Pixel pixel){
    if(!clip_line(w,h,b.radius,x0,y0,x1,y1))return 0;
    int changed=0,dx=std::abs(x1-x0),sx=x0<x1?1:-1,dy=-std::abs(y1-y0),sy=y0<y1?1:-1,e=dx+dy;
    for(;;){
        for(int y=std::max(0,y0-b.radius+1);y<=std::min(h-1,y0+b.radius-1);++y)for(int x=std::max(0,x0-b.radius+1);x<=std::min(w-1,x0+b.radius-1);++x){
            int ox=x-x0,oy=y-y0;if(b.shape==BrushShape::Square||ox*ox+oy*oy<b.radius*b.radius)changed+=pixel(x,y);}
        if(x0==x1&&y0==y1)break;int twice=2*e;if(twice>=dy){e+=dy;x0+=sx;}if(twice<=dx){e+=dx;y0+=sy;}
    }return changed;
}
PAL review_palette(const Clipboard &c){PAL p{};p.numc=(unsigned short)c.colors.size();p.data_p=(void *)c.colors.data();return p;}
}
bool make_brush_path(int w,int h,int radius,int x0,int y0,int x1,int y1,std::vector<std::pair<int,int>> &out,std::string &error){
    out.clear();error.clear();if(!dimensions(w,h,w)||radius<1||radius>32){error="Invalid brush path dimensions or radius.";return false;}
    if(!clip_line(w,h,radius,x0,y0,x1,y1))return true;int dx=std::abs(x1-x0),sx=x0<x1?1:-1,dy=-std::abs(y1-y0),sy=y0<y1?1:-1,e=dx+dy;
    for(;;){out.emplace_back(x0,y0);if(x0==x1&&y0==y1)break;int twice=2*e;if(twice>=dy){e+=dy;x0+=sx;}if(twice<=dx){e+=dx;y0+=sy;}}return true;
}
int paint_line_pixels(unsigned char *pixels,int w,int h,int stride,const PAL &palette,int x0,int y0,int x1,int y1,const PaintBrush &b,const PixelSelection *selection,std::string &error){
    error.clear();if(!pixels||!dimensions(w,h,stride)||!brush_valid(b,palette)||(selection&&!selection->fits(w,h))){error="Invalid brush, palette, image or selection.";return -1;}
    if(!clip_line(w,h,b.radius,x0,y0,x1,y1))return 0;
    PAL pal=palette;std::array<bool,256> matched{};if(b.source>=0)for(int i=0;i<pal.numc;++i)matched[i]=matches(pal,b.source,i,b.match_colors,b.tolerance);
    return raster_brush(w,h,x0,y0,x1,y1,b,[&](int x,int y){auto &v=pixels[(size_t)y*stride+x];if(selected(selection,x,y)&&(b.source<0||matched[v])&&v!=b.color){v=(unsigned char)b.color;return 1;}return 0;});
}
int fill_matching_pixels(unsigned char *pixels,int w,int h,int stride,const PAL &palette,int x,int y,int color,bool colors,int tolerance,bool contiguous,const PixelSelection *selection,std::string &error){
    error.clear();if(!pixels||!dimensions(w,h,stride)||!palette_valid(palette)||color<0||color>=palette.numc||tolerance<0||tolerance>255||(selection&&!selection->fits(w,h))){error="Invalid fill, palette, image or selection.";return -1;}
    if(x<0||y<0||x>=w||y>=h||!selected(selection,x,y))return 0;int seed=pixels[(size_t)y*stride+x];if(seed>=palette.numc){error="The clicked pixel is outside its palette.";return -1;}
    PAL pal=palette;std::array<bool,256> matched{};for(int i=0;i<pal.numc;++i)matched[i]=matches(pal,seed,i,colors,tolerance);int changed=0;
    auto replace=[&](int xx,int yy){auto &v=pixels[(size_t)yy*stride+xx];if(v!=color){v=(unsigned char)color;++changed;}};
    if(!contiguous){for(int yy=0;yy<h;++yy)for(int xx=0;xx<w;++xx)if(selected(selection,xx,yy)&&matched[pixels[(size_t)yy*stride+xx]])replace(xx,yy);return changed;}
    std::vector<unsigned char> seen((size_t)w*h);std::vector<int> todo{y*w+x};seen[y*w+x]=1;
    while(!todo.empty()){int at=todo.back();todo.pop_back();int xx=at%w,yy=at/w;replace(xx,yy);const int dx[]={-1,1,0,0},dy[]={0,0,-1,1};
        for(int k=0;k<4;++k){int nx=xx+dx[k],ny=yy+dy[k];if(nx<0||ny<0||nx>=w||ny>=h)continue;size_t p=(size_t)ny*w+nx;if(seen[p])continue;seen[p]=1;if(selected(selection,nx,ny)&&matched[pixels[(size_t)ny*stride+nx]])todo.push_back((int)p);}}
    return changed;
}
int Artwork::paint_stroke(int index,int x0,int y0,int x1,int y1,const PaintBrush &b,const PixelSelection *selection,std::string &error){
    auto im=sprite(index);auto pal=im?palette(im->palnum):nullptr;
    if(!im||!im->data_p||!pal||!dimensions(im->w,im->h,(im->w+3)&~3)||!brush_valid(b,*pal)||(selection&&!selection->fits(im->w,im->h))){error="Invalid brush, palette, sprite or selection.";return -1;}
    int ax=x0,ay=y0,bx=x1,by=y1;if(!clip_line(im->w,im->h,b.radius,ax,ay,bx,by)){error.clear();return 0;}
    begin(b.source>=0?"Replace matching pixels":b.color?"Paint pixels":"Erase pixels");int changed=paint_line_pixels((unsigned char *)im->data_p,im->w,im->h,(im->w+3)&~3,*pal,x0,y0,x1,y1,b,selection,error);if(changed>0)++serial;return changed;
}
int Artwork::fill_matching(int index,int x,int y,int color,bool colors,int tolerance,bool contiguous,const PixelSelection *selection,std::string &error){
    auto im=sprite(index);auto pal=im?palette(im->palnum):nullptr;
    if(!im||!im->data_p||!pal||!dimensions(im->w,im->h,(im->w+3)&~3)||!palette_valid(*pal)||color<0||color>=pal->numc||tolerance<0||tolerance>255||(selection&&!selection->fits(im->w,im->h))){error="Invalid fill, palette, sprite or selection.";return -1;}
    if(x<0||y<0||x>=im->w||y>=im->h||!selected(selection,x,y)){error.clear();return 0;}
    if(((unsigned char *)im->data_p)[y*((im->w+3)&~3)+x]>=pal->numc){error="The clicked pixel is outside its palette.";return -1;}
    begin(color?"Fill matching pixels":"Erase matching pixels");int changed=fill_matching_pixels((unsigned char *)im->data_p,im->w,im->h,(im->w+3)&~3,*pal,x,y,color,colors,tolerance,contiguous,selection,error);if(changed>0)++serial;commit();return changed;
}
int clone_line_pixels(unsigned char *pixels,int w,int h,int stride,const Clipboard &source,int dx,int dy,int x0,int y0,int x1,int y1,const PaintBrush &brush,bool transparent,const PixelSelection *selection,std::string &error){
    error.clear();auto pal=review_palette(source);
    if(!pixels||!dimensions(w,h,stride)||source.w!=w||source.h!=h||source.colors.empty()||source.colors.size()>256||pixels==source.pixels.data()||source.pixels.size()!=(size_t)w*h||!brush_valid(brush,pal)||(selection&&!selection->fits(w,h))||std::any_of(source.pixels.begin(),source.pixels.end(),[&](unsigned char v){return v>=source.colors.size();})){
        error="Invalid clone source, brush, sprite or selection.";return -1;}
    return raster_brush(w,h,x0,y0,x1,y1,brush,[&](int x,int y){
        int64_t sx=(int64_t)x+dx,sy=(int64_t)y+dy;if(sx<0||sy<0||sx>=w||sy>=h||!selected(selection,x,y))return 0;
        auto value=source.pixels[(size_t)sy*w+(size_t)sx];auto &dest=pixels[(size_t)y*stride+x];if((!value&&!transparent)||dest==value)return 0;dest=value;return 1;});
}
bool begin_clone_stroke(const Artwork &art,int index,int dx,int dy,const PaintBrush &brush,bool transparent,const PixelSelection &selection,CloneStroke &out,std::string &error){
    out={};error.clear();auto im=art.sprite(index);auto pal=im?art.palette(im->palnum):nullptr;
    if(!im||!im->data_p||!pal||!dimensions(im->w,im->h,(im->w+3)&~3)||!brush_valid(brush,*pal)||!selection.fits(im->w,im->h)){error="Choose a valid clone source and brush.";return false;}
    CloneStroke result;result.owner=&art;result.generation=art.generation();result.sprite=art.sprite_id(index);result.original=art.copy(index,0,0,im->w,im->h);result.selection=selection;result.brush=brush;result.dx=dx;result.dy=dy;result.copy_transparent=transparent;
    if(std::any_of(result.original.pixels.begin(),result.original.pixels.end(),[&](unsigned char v){return v>=result.original.colors.size();})){error="The clone source contains pixels outside its palette.";return false;}
    out=std::move(result);return true;
}
int Artwork::clone_stroke(CloneStroke &s,int x0,int y0,int x1,int y1,std::string &error){
    error.clear();if(s.owner!=this||s.generation!=generation()){error="The clone source changed. Start a new stroke.";return -1;}
    auto im=sprite(sprite_index(s.sprite));auto pal=im?palette(im->palnum):nullptr;
    if(!im||!im->data_p||!pal||im->w!=s.original.w||im->h!=s.original.h||pal->numc!=s.original.colors.size()||!pal->data_p||memcmp(pal->data_p,s.original.colors.data(),s.original.colors.size()*2)){error="The clone source sprite or palette is unavailable.";return -1;}
    // Validate and calculate against owned pixels before opening history or writing.
    auto dest=copy(sprite_index(s.sprite),0,0,im->w,im->h).pixels;
    int changed=clone_line_pixels(dest.data(),im->w,im->h,im->w,s.original,s.dx,s.dy,x0,y0,x1,y1,s.brush,s.copy_transparent,&s.selection,error);
    if(changed<=0)return changed;begin("Clone pixels");int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)memcpy((unsigned char *)im->data_p+y*stride,dest.data()+(size_t)y*im->w,im->w);++serial;s.generation=generation();return changed;
}
bool begin_paint_line(const Artwork &art,int index,int x,int y,const PaintBrush &brush,const PixelSelection &selection,PaintLineReview &out,std::string &error){
    out={};error.clear();auto im=art.sprite(index);auto pal=im?art.palette(im->palnum):nullptr;
    if(!im||!pal||!im->data_p||!dimensions(im->w,im->h,(im->w+3)&~3)||!brush_valid(brush,*pal)||!selection.fits(im->w,im->h)||x<0||y<0||x>=im->w||y>=im->h){error="Start a line inside a valid sprite.";return false;}
    PaintLineReview line;line.owner=&art;line.generation=art.generation();line.sprite=art.sprite_id(index);line.original=art.copy(index,0,0,im->w,im->h);line.selection=selection;line.brush=brush;line.x0=line.x1=x;line.y0=line.y1=y;out=std::move(line);return true;
}
bool preview_paint_line(const PaintLineReview &line,std::vector<unsigned char> &pixels,int &changed,std::string &error){
    pixels.clear();changed=0;const auto &c=line.original;
    if(!dimensions(c.w,c.h,c.w)||c.pixels.size()!=(size_t)c.w*c.h||c.colors.empty()||c.colors.size()>256||std::any_of(c.pixels.begin(),c.pixels.end(),[&](unsigned char v){return v>=c.colors.size();})){error="The line source snapshot is invalid.";return false;}
    auto pal=review_palette(c);pixels=c.pixels;changed=paint_line_pixels(pixels.data(),c.w,c.h,c.w,pal,line.x0,line.y0,line.x1,line.y1,line.brush,&line.selection,error);if(changed<0){pixels.clear();changed=0;return false;}return true;
}
bool Artwork::apply_paint_line(const PaintLineReview &line,std::string &error){
    if(line.owner!=this||line.generation!=generation()){error="The source changed during the line preview. Draw it again.";return false;}
    int index=sprite_index(line.sprite);auto im=sprite(index);if(!im||im->w!=line.original.w||im->h!=line.original.h){error="The line's source sprite is unavailable.";return false;}
    auto live=copy(index,0,0,im->w,im->h);if(live.pixels!=line.original.pixels||live.colors!=line.original.colors){error="The line source snapshot no longer matches the sprite.";return false;}
    std::vector<unsigned char> pixels;int changed;if(!preview_paint_line(line,pixels,changed,error))return false;if(!changed){error.clear();return true;}
    begin("Draw straight line");int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)memcpy((unsigned char *)im->data_p+y*stride,pixels.data()+(size_t)y*im->w,im->w);++serial;commit();return true;
}
}
