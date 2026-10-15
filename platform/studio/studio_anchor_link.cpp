#include "studio_anchor_link.h"
#include <climits>
namespace studio {
bool plan_anchor_link(const Artwork &ref,uint64_t ref_id,const Artwork &dst,uint64_t dst_id,const AnchorLinkOptions &o,AnchorLinkPlan &out,std::string &error){
    error.clear();auto r=ref.sprite(ref.sprite_index(ref_id)),t=dst.sprite(dst.sprite_index(dst_id));
    if(!r||!t){error="A chosen sprite is missing. Choose it again.";return false;}
    if(&ref==&dst&&ref_id==dst_id){error="Choose different reference and target sprites.";return false;}
    if(o.reference_x<0||o.reference_y<0||o.reference_x>=r->w||o.reference_y>=r->h||o.target_x<0||o.target_y<0||o.target_x>=t->w||o.target_y>=t->h){error="Choose one feature inside each sprite.";return false;}
    if(o.offset_x<-4096||o.offset_x>4096||o.offset_y<-4096||o.offset_y>4096||(o.convention!=MirrorConvention_Ani2&&o.convention!=MirrorConvention_Ganiof)){error="Invalid staging offset or mirror convention.";return false;}
    // Match feature pixel centers. Common world origin cancels from the difference.
    auto rp=place_sprite(*r,0,0,o.reference_flip,o.convention),tp=place_sprite(*t,o.offset_x,o.offset_y,o.target_flip,o.convention);
    int rx=rp.x+(o.reference_flip?r->w-1-o.reference_x:o.reference_x),ry=rp.y+o.reference_y;
    int tx=tp.x+(o.target_flip?t->w-1-o.target_x:o.target_x),ty=tp.y+o.target_y;
    int x=(short)t->anix+(o.target_flip?-1:1)*(tx-rx),y=(short)t->aniy+(ty-ry);
    if(x<SHRT_MIN||x>SHRT_MAX||y<SHRT_MIN||y>SHRT_MAX){error="Alignment would exceed the IMG's signed 16-bit anchor range.";return false;}
    AnchorLinkPlan p;p.reference=&ref;p.target=&dst;p.reference_generation=ref.generation();p.target_generation=dst.generation();p.reference_sprite=ref_id;p.target_sprite=dst_id;p.options=o;
    p.old_x=(short)t->anix;p.old_y=(short)t->aniy;p.x=x;p.y=y;p.changed=x!=p.old_x||y!=p.old_y;out=p;return true;
}
bool apply_anchor_link(Artwork &dst,const Artwork &ref,const AnchorLinkPlan &p,std::string &error){
    if(p.target!=&dst||p.reference!=&ref||p.reference_generation!=ref.generation()||p.target_generation!=dst.generation()){
        error="The reference or target changed. Pick the features again.";return false;}
    AnchorLinkPlan current;if(!plan_anchor_link(ref,p.reference_sprite,dst,p.target_sprite,p.options,current,error))return false;
    if(current.old_x!=p.old_x||current.old_y!=p.old_y||current.x!=p.x||current.y!=p.y||current.changed!=p.changed){error="The anchor preview no longer matches its sources.";return false;}
    if(!current.changed){error="The chosen features are already aligned.";return false;}
    dst.commit();dst.begin("Link animation anchor");dst.anchor(dst.sprite_index(p.target_sprite),p.x,p.y);dst.commit();return true;
}
}
