#include "studio_region_match.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace studio {
RegionMatchSample sample_feature_region(const PaletteCompactionSprite &source,const PixelSelection &selection,int excluded){
    IMG im;memcpy(&im,source.fields.data(),sizeof im);RegionMatchSample s;if(!im.w||!im.h||im.w>8192||im.h>8192||(size_t)im.w*im.h>4u*1024u*1024u||source.pixels.size()!=(size_t)((im.w+3)&~3)*im.h||!selection.valid||!selection.fits(im.w,im.h)||excluded<0||excluded>255)return s;int stride=(im.w+3)&~3,left=im.w,top=im.h,right=0,bottom=0;long long sumx=0,sumy=0;
    for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x){int c=source.pixels[(size_t)y*stride+x];if(c&&c!=excluded&&selection.contains(x,y)){s.colors[c]=true;++s.area;sumx+=x;sumy+=y;left=std::min(left,x);top=std::min(top,y);right=std::max(right,x);bottom=std::max(bottom,y);}}
    if(!s.area)return s;s.w=right-left+1;s.h=bottom-top+1;s.cx=(double)sumx/s.area-(short)im.anix;s.cy=(double)sumy/s.area-(short)im.aniy;int step=std::max(1,(s.area+767)/768),ordinal=0;
    for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x)if(source.pixels[(size_t)y*stride+x]&&source.pixels[(size_t)y*stride+x]!=excluded&&selection.contains(x,y)){if(ordinal%step==0)s.seeds.emplace_back(x-(short)im.anix,y-(short)im.aniy);++ordinal;}return s;
}
std::vector<unsigned char> match_feature_region(const PaletteCompactionSprite &source,const RegionMatchSample &s,bool original,const PixelSelection &selection,uint64_t &work){
    IMG im;memcpy(&im,source.fields.data(),sizeof im);int w=im.w,h=im.h,stride=(w+3)&~3;if(!w||!h||w>8192||h>8192||(size_t)w*h>4u*1024u*1024u||source.pixels.size()!=(size_t)stride*h||s.area<1||s.w<1||s.h<1||s.seeds.size()>768||!selection.valid)return {};std::vector<unsigned char> region((size_t)w*h);auto matches=[&](int x,int y){int c=source.pixels[(size_t)y*stride+x];return c&&s.colors[c];};
    if(original){for(int y=0;y<h;++y)for(int x=0;x<w;++x)region[(size_t)y*w+x]=matches(x,y)&&selection.contains(x,y);return region;}
    std::vector<unsigned char> visited(region.size());std::vector<int> component;int radius=std::clamp(std::max(s.w,s.h)/2,6,18);double reach=std::max(28.,std::max(s.w,s.h)*2.5+12.);
    for(auto seed:s.seeds){int ex=(short)im.anix+seed.first,ey=(short)im.aniy+seed.second,best=-1,distance=0x7fffffff;for(int y=std::max(0,ey-radius);y<=std::min(h-1,ey+radius)&&distance!=0;++y)for(int x=std::max(0,ex-radius);x<=std::min(w-1,ex+radius)&&distance!=0;++x){if(++work>64u*1024u*1024u)throw std::runtime_error("Region matching exceeds its 64 Mi-step work budget. Narrow the source feature or split the library.");if(matches(x,y)){int d=(x-ex)*(x-ex)+(y-ey)*(y-ey);if(d<distance){distance=d;best=y*w+x;}}}if(best<0||visited[best])continue;
        component.clear();component.push_back(best);visited[best]=1;long long sumx=0,sumy=0;for(size_t at=0;at<component.size();++at){if(++work>64u*1024u*1024u)throw std::runtime_error("Region matching exceeds its 64 Mi-step work budget. Narrow the source feature or split the library.");int pos=component[at],x=pos%w,y=pos/w;sumx+=x;sumy+=y;const int dx[]={0,1,0,-1},dy[]={-1,0,1,0};for(int n=0;n<4;++n){int xx=x+dx[n],yy=y+dy[n];if(xx<0||yy<0||xx>=w||yy>=h)continue;int next=yy*w+xx;if(!visited[next]&&matches(xx,yy)){visited[next]=1;component.push_back(next);}}}
        double ratio=(double)component.size()/s.area,cx=(double)sumx/component.size()-(short)im.anix-s.cx,cy=(double)sumy/component.size()-(short)im.aniy-s.cy;if(ratio<.04||ratio>8.||std::sqrt(cx*cx+cy*cy)>reach)continue;for(int pos:component)region[pos]=1;
    }return region;
}
}
