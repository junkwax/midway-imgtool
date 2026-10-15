#include "studio_anchor_link.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <climits>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
int main(){using namespace studio;namespace fs=std::filesystem;std::string error;auto ref=Artwork::demo(),dst=Artwork::demo();
    auto root=fs::temp_directory_path()/("studio-link-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto ref_path=root/"ref.IMG",dst_path=root/"target.IMG";CHECK(ref->save(ref_path.u8string(),error)&&dst->save(dst_path.u8string(),error));
    auto ref_id=ref->sprite_id(0),dst_id=dst->sprite_id(1);auto baseline_ref=ref->copy(0,0,0,48,64).pixels,baseline_dst=dst->copy(1,0,0,48,64).pixels;
    auto ref_gen=ref->generation();int ref_x=(short)ref->sprite(0)->anix,ref_y=(short)ref->sprite(0)->aniy;
    AnchorLinkOptions o;o.reference_x=12;o.reference_y=19;o.target_x=15;o.target_y=23;o.offset_x=7;o.offset_y=-2;
    for(int convention=0;convention<2;++convention)for(int rflip=0;rflip<2;++rflip)for(int tflip=0;tflip<2;++tflip){o.reference_flip=rflip!=0;o.target_flip=tflip!=0;o.convention=(MirrorConvention)convention;AnchorLinkPlan plan;
        int old_x=(short)dst->sprite(1)->anix,old_y=(short)dst->sprite(1)->aniy;CHECK(plan_anchor_link(*ref,ref_id,*dst,dst_id,o,plan,error));CHECK(!dst->dirty()&&ref->generation()==ref_gen);
        auto r=place_sprite(*ref->sprite(0),0,0,o.reference_flip,o.convention);int rx=r.x+(rflip?ref->sprite(0)->w-1-o.reference_x:o.reference_x),ry=r.y+o.reference_y;
        CHECK(plan.changed&&apply_anchor_link(*dst,*ref,plan,error));auto t=place_sprite(*dst->sprite(1),o.offset_x,o.offset_y,o.target_flip,o.convention);
        CHECK(t.x+(tflip?dst->sprite(1)->w-1-o.target_x:o.target_x)==rx&&t.y+o.target_y==ry);
        CHECK(dst->copy(1,0,0,48,64).pixels==baseline_dst&&ref->copy(0,0,0,48,64).pixels==baseline_ref);
        CHECK(dst->dirty()&&!ref->dirty()&&ref->generation()==ref_gen&&(short)ref->sprite(0)->anix==ref_x&&(short)ref->sprite(0)->aniy==ref_y);
        dst->undo();CHECK(!dst->dirty()&&(short)dst->sprite(1)->anix==old_x&&(short)dst->sprite(1)->aniy==old_y);dst->redo();CHECK(dst->dirty());
        auto out=root/"linked.IMG";CHECK(dst->save(out.u8string(),error));auto read=Artwork::open(out.u8string(),error);CHECK(read&&read->disk_version()==dst->disk_version()&&(short)read->sprite(1)->anix==plan.x&&(short)read->sprite(1)->aniy==plan.y);
        dst->undo();CHECK(dst->save(dst_path.u8string(),error));
    }
    AnchorLinkPlan plan;o={};o.reference_x=12;o.reference_y=19;o.target_x=15;o.target_y=23;
    CHECK(plan_anchor_link(*ref,ref_id,*dst,dst_id,o,plan,error));auto before=dst->generation();auto tampered=plan;tampered.x++;CHECK(!apply_anchor_link(*dst,*ref,tampered,error)&&dst->generation()==before);
    auto foreign=Artwork::demo();CHECK(!apply_anchor_link(*foreign,*ref,plan,error));
    ref->anchor(0,42,52);ref->commit();CHECK(!apply_anchor_link(*dst,*ref,plan,error)&&dst->generation()==before);ref->undo();CHECK(!apply_anchor_link(*dst,*ref,plan,error));
    CHECK(plan_anchor_link(*ref,ref_id,*dst,dst_id,o,plan,error));dst->move_sprite(1,4,error);CHECK(!apply_anchor_link(*dst,*ref,plan,error));
    CHECK(plan_anchor_link(*ref,ref_id,*dst,dst_id,o,plan,error)&&apply_anchor_link(*dst,*ref,plan,error));CHECK(dst->sprite_index(dst_id)==4);
    dst->undo();dst->undo();CHECK(dst->sprite_index(dst_id)==1);
    CHECK(!plan_anchor_link(*ref,ref_id,*ref,ref_id,o,plan,error));CHECK(!plan_anchor_link(*ref,0,*dst,dst_id,o,plan,error));
    o.reference_x=48;CHECK(!plan_anchor_link(*ref,ref_id,*dst,dst_id,o,plan,error));o.reference_x=0;o.target_y=-1;CHECK(!plan_anchor_link(*ref,ref_id,*dst,dst_id,o,plan,error));
    o.target_y=0;o.offset_x=4097;CHECK(!plan_anchor_link(*ref,ref_id,*dst,dst_id,o,plan,error));o.offset_x=0;
    ref->anchor(0,SHRT_MAX,0);ref->commit();o.target_x=47;CHECK(!plan_anchor_link(*ref,ref_id,*dst,dst_id,o,plan,error));ref->undo();
    o.reference_x=0;o.reference_y=0;o.target_x=0;o.target_y=0;dst->anchor(1,ref_x,ref_y);dst->commit();CHECK(plan_anchor_link(*ref,ref_id,*dst,dst_id,o,plan,error)&&!plan.changed);before=dst->generation();CHECK(!apply_anchor_link(*dst,*ref,plan,error)&&dst->generation()==before);
    // Within-library linking also has one undo, and never propagates to other frames.
    o.target_x=2;auto untouched=(short)ref->sprite(2)->anix;CHECK(plan_anchor_link(*ref,ref_id,*ref,ref->sprite_id(1),o,plan,error));CHECK(apply_anchor_link(*ref,*ref,plan,error));CHECK((short)ref->sprite(2)->anix==untouched);ref->undo();CHECK(!ref->dirty());
    fs::remove_all(root);std::cout<<"Anchor link: "<<failures<<" failures\n";return failures?1:0;
}
