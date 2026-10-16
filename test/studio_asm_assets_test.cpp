#include "studio_asm_assets.h"
#include "studio_reactions.h"
#include <iostream>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
int main(){using namespace studio;std::string error;auto art=Artwork::demo(),other=Artwork::demo();
    auto generation=art->generation();AsmSourceAnimation animation;
    AsmSourceFrame a;a.pieces={"sentinel1","SENTINEL2","SENTINEL1"};a.source="first.ASM";a.line=17;
    AsmSourceFrame b;b.pieces={"SENTINEL2","MISSING"};b.source="second.ASM";b.line=9;
    animation.frames={a,b};AsmAssetReview review;AsmArtworkChoices choices;
    std::vector<AsmLibrary> libs={{17,art.get()},{29,other.get()}};
    CHECK(review_asm_assets(animation,libs,0,choices,review,error));CHECK(review.rows.size()==3&&review.ambiguous==2&&review.missing==1);
    CHECK(review.rows[0].symbol=="sentinel1"&&review.rows[0].uses==2&&review.rows[0].source=="first.ASM"&&review.rows[0].line==17);
    CHECK(review.rows[1].uses==2&&review.rows[2].first_frame==1&&review.rows[2].line==9&&review.rows[0].candidates.size()==2);
    CHECK(review_asm_assets(animation,libs,17,choices,review,error));CHECK(review.resolved==2&&review.missing==1&&review.rows[0].resolved.doc==17);
    auto id=other->sprite_id(0);choices[asm_artwork_key("sentinel1")]={29,id};
    CHECK(review_asm_assets(animation,libs,17,choices,review,error));CHECK(review.rows[0].resolved.doc==29&&review.rows[0].resolved.chosen);
    auto frame=resolve_asm_frame(a,libs,17,choices);CHECK(frame[0].doc==29&&frame[2].sprite==id&&frame[1].doc==17);
    auto anchors=review_reaction_anchors(animation,{{17,art.get()},{29,other.get()}},17,false,choices);
    CHECK(anchors.rows[0].reference.doc==29&&anchors.rows[2].reference.sprite==id);
    CHECK(other->move_sprite(0,3,error));CHECK(review_asm_assets(animation,libs,17,choices,review,error));CHECK(review.rows[0].resolved.sprite==id&&other->sprite_index(id)==3);
    CHECK(other->rename_sprite(3,"RENAMED",error));CHECK(review_asm_assets(animation,libs,17,choices,review,error));
    CHECK(review.stale==1&&!review.rows[0].resolved.sprite&&!review.rows[0].resolved.ambiguous&&review.rows[0].candidates.size()==1);
    frame=resolve_asm_frame(a,libs,17,choices);CHECK(frame[0].stale&&!frame[0].sprite);other->undo();
    CHECK(review_asm_assets(animation,libs,17,choices,review,error));CHECK(review.rows[0].resolved.sprite==id&&!review.rows[0].resolved.stale);
    CHECK(review_asm_assets(animation,{{17,art.get()}},17,choices,review,error));CHECK(review.stale==1&&review.resolved==1);
    // A closed chosen source never silently falls back to a matching preferred sprite.
    choices.clear();CHECK(review_asm_assets(animation,{{17,art.get()}},17,choices,review,error));CHECK(review.resolved==2&&review.stale==0);
    choices["MISSING"]={17,art->sprite_id(0)};CHECK(review_asm_assets(animation,libs,17,choices,review,error));CHECK(review.rows[2].resolved.stale); // No arbitrary symbol substitution.
    choices.clear();auto before=review.rows.size();CHECK(!review_asm_assets(animation,{{17,art.get()},{17,other.get()}},0,choices,review,error));CHECK(!error.empty()&&review.rows.size()==before);
    CHECK(!review_asm_assets(animation,{{0,art.get()}},0,choices,review,error));CHECK(review.rows.size()==before);
    AsmSourceAnimation large;large.frames.resize(1);for(int i=0;i<32769;++i)large.frames[0].pieces.push_back("SYMBOL"+std::to_string(i));
    CHECK(!review_asm_assets(large,{},0,{},review,error)&&review.rows.size()==before);
    large.frames[0].pieces.assign(262145,"SENTINEL1");CHECK(!review_asm_assets(large,{},0,{},review,error));
    std::vector<AsmLibrary> many;for(int i=1;i<=65537;++i)many.push_back({(uint64_t)i,art.get()});
    CHECK(!review_asm_assets(animation,many,0,{},review,error));CHECK(review.rows.size()==before);
    CHECK(review_asm_assets({},libs,0,{},review,error)&&review.rows.empty()&&error.empty());
    CHECK(review_asm_assets(animation,{{17,nullptr}},17,{},review,error)&&review.missing==3);
    CHECK(art->generation()==generation&&!art->dirty());
    std::cout<<"ASM artwork: "<<failures<<" failures\n";return failures?1:0;
}
