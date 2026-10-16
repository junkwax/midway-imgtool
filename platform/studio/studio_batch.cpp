#include "studio_batch.h"
#include "canvas_ops.h"
#include <algorithm>
#include <cmath>
#include <set>
namespace studio {
namespace {
size_t bytes(int w,int h){return (((size_t)w+3)&~size_t(3))*h;}
bool dimensions(int w,int h){return w>=1&&h>=1&&w<=8192&&h<=8192&&(size_t)w*h<=4*1024*1024;}
bool options_valid(const BatchGeometryOptions &o){return (o.kind==BatchGeometryKind::Trim||o.kind==BatchGeometryKind::Resize||o.kind==BatchGeometryKind::Canvas||o.kind==BatchGeometryKind::Transform)&&o.percent>=1&&o.percent<=800&&o.placement>=0&&o.placement<=8&&(o.transform==Transform::FlipHorizontal||o.transform==Transform::FlipVertical||o.transform==Transform::RotateClockwise||o.transform==Transform::RotateCounterclockwise||o.transform==Transform::Rotate180)&&((o.kind!=BatchGeometryKind::Canvas&&(o.kind!=BatchGeometryKind::Resize||o.relative))||dimensions(o.w,o.h));}
bool make_row(const Artwork &art,uint64_t id,const BatchGeometryOptions &o,BatchGeometryRow &row,std::string &error){
    int index=art.sprite_index(id);auto im=art.sprite(index);auto pal=im?art.palette(im->palnum):nullptr;
    if(!im||!im->data_p||!dimensions(im->w,im->h)||!pal||!pal->data_p||pal->numc<1||pal->numc>256){error="A batch sprite or palette is invalid.";return false;}
    if(im->layer_p){error="Flatten or remove overlays before batch geometry.";return false;}
    row.sprite=id;row.name=im->n_s;row.before_w=im->w;row.before_h=im->h;memcpy(row.source_fields.data(),im,sizeof(IMG));auto src=(unsigned char *)im->data_p;row.before.assign(src,src+bytes(im->w,im->h));row.colors.resize(pal->numc);memcpy(row.colors.data(),pal->data_p,pal->numc*2);
    for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x)if(src[y*((im->w+3)&~3)+x]>=pal->numc){error=row.name+": a pixel index is outside its palette.";return false;}
    if(im->pttbl_p)row.before_points.assign((unsigned char *)im->pttbl_p,(unsigned char *)im->pttbl_p+40);
    auto scratch=std::make_unique<Artwork>();int pi=scratch->add_palette("BATCH",row.colors,error);if(pi<0)return false;int scratch_index=scratch->add_sprite("BATCH",im->w,im->h,pi,error);if(scratch_index<0)return false;auto edit=scratch->sprite(scratch_index);memcpy(edit->data_p,im->data_p,row.before.size());edit->anix=im->anix;edit->aniy=im->aniy;edit->anix2=im->anix2;edit->aniy2=im->aniy2;edit->aniz2=im->aniz2;edit->flags=im->flags;
    if(im->pttbl_p){edit->pttbl_p=malloc(40);if(!edit->pttbl_p){error="Not enough memory for batch points.";return false;}memcpy(edit->pttbl_p,im->pttbl_p,40);}
    bool ok=true;switch(o.kind){
        case BatchGeometryKind::Trim:{int l,t,r,b;row.empty=!CanvasIndexedContentBounds(src,im->w,im->h,(im->w+3)&~3,&l,&t,&r,&b);if(!row.empty)ok=scratch->trim_sprite(scratch_index,error);break;}
        case BatchGeometryKind::Resize:{int w=o.relative?std::max(1,(int)std::lround(im->w*o.percent/100.)):o.w,h=o.relative?std::max(1,(int)std::lround(im->h*o.percent/100.)):o.h;if(!dimensions(w,h)){error=row.name+": resized dimensions exceed the sprite limit.";return false;}ok=scratch->resize_sprite(scratch_index,w,h,o.quality,o.anchors,error);break;}
        case BatchGeometryKind::Canvas:{int dx,dy;CanvasAnchorOffset(im->w,im->h,o.w,o.h,o.placement,&dx,&dy);for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x)if(src[y*((im->w+3)&~3)+x]&&(x+dx<0||y+dy<0||x+dx>=o.w||y+dy>=o.h))++row.cropped_pixels;ok=scratch->canvas_size(scratch_index,o.w,o.h,o.placement,error);break;}
        case BatchGeometryKind::Transform:ok=scratch->transform_sprite(scratch_index,o.transform,o.anchors,error);break;
    }if(!ok){error=row.name+": "+error;return false;}edit=scratch->sprite(scratch_index);row.w=edit->w;row.h=edit->h;row.anix=edit->anix;row.aniy=edit->aniy;row.anix2=edit->anix2;row.aniy2=edit->aniy2;row.pixels.assign((unsigned char *)edit->data_p,(unsigned char *)edit->data_p+bytes(edit->w,edit->h));if(edit->pttbl_p)row.points.assign((unsigned char *)edit->pttbl_p,(unsigned char *)edit->pttbl_p+40);
    row.changed=row.before_w!=row.w||row.before_h!=row.h||im->anix!=row.anix||im->aniy!=row.aniy||im->anix2!=row.anix2||im->aniy2!=row.aniy2||row.before_points!=row.points;
    if(!row.changed)for(int y=0;y<row.h;++y)if(memcmp(row.before.data()+y*((row.w+3)&~3),row.pixels.data()+y*((row.w+3)&~3),row.w)){row.changed=true;break;}
    // No-op geometry must not silently normalize unused row padding.
    if(!row.changed)row.pixels=row.before;return true;
}
bool equal_result(const BatchGeometryRow &a,const BatchGeometryRow &b){return a.sprite==b.sprite&&a.before_w==b.before_w&&a.before_h==b.before_h&&a.w==b.w&&a.h==b.h&&a.cropped_pixels==b.cropped_pixels&&a.changed==b.changed&&a.empty==b.empty&&a.anix==b.anix&&a.aniy==b.aniy&&a.anix2==b.anix2&&a.aniy2==b.aniy2&&a.source_fields==b.source_fields&&a.before==b.before&&a.pixels==b.pixels&&a.before_points==b.before_points&&a.points==b.points&&a.colors==b.colors;}
}
bool plan_batch_geometry(const Artwork &art,const std::vector<uint64_t> &ids,const BatchGeometryOptions &o,BatchGeometryPlan &out,std::string &error){
    out={};error.clear();if(ids.empty()||ids.size()>2000||!options_valid(o)){error="Choose sprites and valid batch geometry settings.";return false;}
    BatchGeometryPlan plan;plan.owner=&art;plan.generation=art.generation();plan.options=o;std::set<uint64_t> seen;size_t total=0;
    try{for(auto id:ids){if(!seen.insert(id).second){error="A sprite occurs twice in the batch.";return false;}BatchGeometryRow row;if(!make_row(art,id,o,row,error))return false;total+=row.before.size()+row.pixels.size();if(total>256*1024*1024){error="The batch preview exceeds its 256 MiB pixel limit.";return false;}plan.rows.push_back(std::move(row));}}
    catch(const std::bad_alloc &){error="Not enough memory to build the batch preview.";return false;}out=std::move(plan);return true;
}
int Artwork::apply_batch_geometry(const BatchGeometryPlan &plan,std::string &error){
    error.clear();if(pending){error="Finish the current edit before applying a batch.";return -1;}if(plan.owner!=this||plan.generation!=generation()){error="The source changed. Reopen the batch preview.";return -1;}
    std::vector<uint64_t> ids;for(auto &row:plan.rows)ids.push_back(row.sprite);BatchGeometryPlan checked;if(!plan_batch_geometry(*this,ids,plan.options,checked,error))return -1;
    size_t total=0;for(int i=0;i<count();++i){auto im=sprite(i);total+=bytes(im->w,im->h);}int changed=0;
    for(size_t i=0;i<plan.rows.size();++i){auto &row=plan.rows[i];if(!equal_result(row,checked.rows[i])){error="The reviewed batch no longer matches its source or settings.";return -1;}if(row.enabled&&row.changed){total-=bytes(row.before_w,row.before_h);total+=bytes(row.w,row.h);++changed;}}
    if(total>128*1024*1024){error="This batch would exceed the 128 MiB decoded IMG limit. Reduce the scale or exclude sprites.";return -1;}if(!changed)return 0;
    using Buffer=std::unique_ptr<unsigned char,decltype(&free)>;std::vector<Buffer> staged;
    try{staged.reserve(plan.rows.size());for(auto &row:plan.rows){Buffer pixels(nullptr,&free);if(row.enabled&&row.changed){pixels.reset((unsigned char *)malloc(row.pixels.size()));if(!pixels)throw std::bad_alloc();memcpy(pixels.get(),row.pixels.data(),row.pixels.size());}staged.push_back(std::move(pixels));}}
    catch(const std::bad_alloc &){error="Not enough memory to stage the complete batch. No sprites were changed.";return -1;}
    const char *labels[]={"Batch trim bounds","Batch resize artwork","Batch canvas size","Batch rotate / flip"};begin(labels[(int)plan.options.kind]);
    for(size_t i=0;i<plan.rows.size();++i){auto &row=plan.rows[i];if(!row.enabled||!row.changed)continue;auto im=sprite(sprite_index(row.sprite));free(im->data_p);im->data_p=staged[i].release();im->w=(unsigned short)row.w;im->h=(unsigned short)row.h;im->anix=row.anix;im->aniy=row.aniy;im->anix2=row.anix2;im->aniy2=row.aniy2;if(im->pttbl_p)memcpy(im->pttbl_p,row.points.data(),40);}
    ++serial;++structure_serial;commit();return changed;
}
}
