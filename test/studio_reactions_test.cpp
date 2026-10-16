#include "studio_reactions.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>

static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " / "<<error<<"\n";++failures;}}while(0)
static std::vector<unsigned char> bytes(const std::string &path){std::ifstream f(std::filesystem::u8path(path),std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv){
    using namespace studio;std::string error;
    auto source=AsmSource::parse(
        "demo_anitab\n\t.long a_attack ; hi punch\n\t.long a_jchithi ; 10 - hit high\n"
        "\t.long a_jc_fb_lk ; flipped by liu kang\n\t.long a_back_breaker ; back breaker\n"
        "\t.long a_torso_ripped ; torso ripped\n\t.long a_jckdown ; knocked down\n"
        "a_attack\n\t.long SENTINEL1,0\na_jchithi\n\t.long GROUP\n\t.long ani_adjustxy\n\t.word 5,-3\n"
        "\t.long ani_flip,SENTINEL3\n\t.long ani_flip_v,SENTINEL2\n\t.long ani_calla,r_runtime\n\t.long 0\n"
        "a_jc_fb_lk\n\t.long SENTINEL1,0\na_back_breaker\n\t.long SENTINEL1,0\n"
        "a_torso_ripped\n\t.long SENTINEL1,0\na_jckdown\n\t.long SENTINEL1,0\n"
        "GROUP\n\t.long SENTINEL1,SENTINEL2,MISSING,0\n",error);
    CHECK(source&&source->animations.size()==6);if(!source)return 1;
    auto catalogue=classify_asm_reactions(*source);CHECK(catalogue.size()==6);
    auto all=filter_asm_reactions(catalogue,-1,false,"");CHECK(all.size()==4);
    CHECK(catalogue[all[0]].classification.kind==ReactionKind::Hit&&catalogue[all[1]].classification.kind==ReactionKind::Knockdown&&catalogue[all[2]].classification.kind==ReactionKind::FlippedBy&&catalogue[all[3]].classification.kind==ReactionKind::Death);
    auto thrower=filter_asm_reactions(catalogue,-1,false,"LIU KANG");CHECK(thrower.size()==1&&catalogue[thrower[0]].thrower=="Liu Kang"&&catalogue[thrower[0]].animation==2&&!catalogue[thrower[0]].classification.rule.empty());
    CHECK(filter_asm_reactions(catalogue,(int)ReactionKind::Hit,false,"hit high").size()==1);
    CHECK(filter_asm_reactions(catalogue,(int)ReactionKind::Death,false,"hit high").empty());
    CHECK(filter_asm_reactions(catalogue,-1,false,"attacker half").empty());
    CHECK(filter_asm_reactions(catalogue,-1,true,"attacker half").size()==1);
    CHECK(filter_asm_reactions(catalogue,0,true,"").size()==2&&filter_asm_reactions(catalogue,-1,true,"").size()==6);
    CHECK(filter_asm_reactions(catalogue,-2,true,"").empty()&&filter_asm_reactions(catalogue,7,true,"").empty());
    AsmSource replacement;replacement.animations.push_back({"a_newhit","new getting hit"});auto replaced=classify_asm_reactions(replacement);
    CHECK(replaced.size()==1&&replaced[0].label=="a_newhit"&&replaced[0].animation==0);
    auto art=Artwork::demo();for(int i=0;i<art->count();++i){art->anchor(i,10,60);art->commit();}
    auto generation=art->generation(),selection=art->selection_id();bool dirty=art->dirty();std::string undo=art->undo_label();
    auto &animation=source->animations[1];CHECK(animation.frames.size()==3&&animation.partial);
    auto review=review_reaction_anchors(animation,{{17,art.get()}},17,false);
    CHECK(!review.truncated&&review.rows.size()==5);
    auto &r=review.rows;
    CHECK(r[0].frame==0&&r[0].piece==0&&r[0].reference.doc==17&&r[0].reference.sprite==art->sprite_id(0)&&r[0].line>0&&r[0].w==48&&r[0].h==64&&r[0].anix==10&&r[0].aniy==60&&r[0].x==-10&&r[0].y==-60&&!r[0].flip);
    CHECK(r[1].frame==0&&r[1].piece==1&&r[1].reference.sprite==art->sprite_id(1));
    CHECK(!r[2].reference.sprite&&!r[2].reference.ambiguous&&r[2].reference.symbol=="MISSING");
    CHECK(r[3].frame==1&&r[3].x==-33&&r[3].y==-63&&r[3].flip&&!r[3].flip_y);
    CHECK(r[4].frame==2&&r[4].x==-33&&r[4].y==-7&&r[4].flip_y);
    auto facing=review_reaction_anchors(animation,{{17,art.get()}},17,true);
    CHECK(facing.rows[0].x==-38&&facing.rows[0].flip&&facing.rows[3].x==-15&&!facing.rows[3].flip&&facing.rows[4].x==-15&&facing.rows[4].flip_y);
    auto ganiof=review_reaction_anchors(animation,{{17,art.get(),MirrorConvention_Ganiof}},17,false);
    CHECK(ganiof.rows[3].x==-32&&ganiof.rows[4].y==-6&&ganiof.rows[4].convention==MirrorConvention_Ganiof);
    auto duplicate=Artwork::demo();
    auto ambiguous=review_reaction_anchors(animation,{{17,art.get()},{29,duplicate.get()}},0,false);
    CHECK(ambiguous.rows[0].reference.ambiguous&&!ambiguous.rows[0].reference.sprite&&!ambiguous.rows[2].reference.ambiguous);
    auto preferred=review_reaction_anchors(animation,{{17,art.get()},{29,duplicate.get()}},29,false);
    CHECK(preferred.rows[0].reference.doc==29&&preferred.rows[0].reference.sprite==duplicate->sprite_id(0));
    auto closed=review_reaction_anchors(animation,{{17,nullptr}},17,false);CHECK(!closed.rows[0].reference.sprite&&!closed.rows[0].reference.ambiguous);
    CHECK(art->generation()==generation&&art->selection_id()==selection&&art->dirty()==dirty&&std::string(art->undo_label())==undo);
    // Reviews own metadata and stable references, and refresh sees anchor edits.
    art->anchor(0,-12,-9);art->commit();auto changed=review_reaction_anchors(animation,{{17,art.get()}},17,false);
    CHECK(changed.rows[0].anix==-12&&changed.rows[0].aniy==-9&&changed.rows[0].x==12&&changed.rows[0].y==9&&review.rows[0].anix==10);
    art->undo();auto restored=review_reaction_anchors(animation,{{17,art.get()}},17,false);CHECK(restored.rows[0].anix==10);
    // Snapshot ID does not silently retarget when its source sprite is deleted.
    auto snapshot_id=restored.rows[0].reference.sprite;CHECK(art->delete_sprite(0,error));CHECK(art->sprite_index(snapshot_id)<0);
    auto deleted=review_reaction_anchors(animation,{{17,art.get()}},17,false);CHECK(!deleted.rows[0].reference.sprite&&restored.rows[0].reference.sprite==snapshot_id);
    art->undo();CHECK(art->sprite_index(snapshot_id)==0);
    AsmSourceAnimation large;AsmSourceFrame frame;frame.pieces.assign(256,"MISSING");large.frames.assign(129,frame);
    auto bounded=review_reaction_anchors(large,{},0,false);CHECK(bounded.truncated&&bounded.rows.size()==32768&&bounded.rows.back().frame==127);
    large.frames.resize(128);CHECK(!review_reaction_anchors(large,{},0,false).truncated);
    CHECK(review_reaction_anchors({}, {},0,false).rows.empty());
    if(argc>1&&std::filesystem::exists(std::filesystem::u8path(argv[1]))){
        auto original=bytes(argv[1]);auto real=AsmSource::open(argv[1],error);CHECK(real);
        if(real){auto rows=classify_asm_reactions(*real);CHECK(rows.size()==real->animations.size()&&filter_asm_reactions(rows,-1,false,"").size()>10&&filter_asm_reactions(rows,-1,true,"").size()==rows.size());
            auto high=filter_asm_reactions(rows,-1,false,"a_jchithi");CHECK(high.size()==1&&rows[high[0]].classification.kind==ReactionKind::Hit);
            CHECK(filter_asm_reactions(rows,-1,false,"a_back_breaker").empty());
        }CHECK(bytes(argv[1])==original);
    }
    CHECK(g_doc==nullptr);std::cout<<"Studio reactions: "<<failures<<" failures\n";return failures?1:0;
}
