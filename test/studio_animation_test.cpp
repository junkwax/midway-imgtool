#include "studio_animation.h"
#include <cmath>
#include <iostream>
static int failures=0;
#define CHECK(x) do {if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
int main(int argc,char **argv){
    auto art=studio::Artwork::demo();
    const char *names[]={"WALK10","WALK2","WALK2A","WALK2B","WALK10A","WALK10B","WALK2FLIP","IDLE"};
    for(int i=0;i<8;++i)snprintf(art->sprite(i)->n_s,16,"%s",names[i]);
    auto sequences=studio::infer_preview_sequences(*art);
    int seq=-1,frame=-1;CHECK(studio::locate_preview_sprite(sequences,2,seq,frame));
    CHECK(sequences[seq].name=="WALK");CHECK(sequences[seq].frames.size()==2);CHECK(frame==0);
    auto &first=sequences[seq].frames[0];CHECK(first.sprite==1);CHECK(first.pieces==std::vector<int>({2,3}));
    CHECK(studio::preview_pieces(first,true)==std::vector<int>({2,3}));CHECK(studio::preview_pieces(first,false)==std::vector<int>({1}));
    CHECK(studio::locate_preview_sprite(sequences,6,seq,frame));CHECK(sequences[seq].name=="WALK2FLIP");
    // Orphan subframes with a real sibling reconstruct a frame; word suffixes don't.
    strcpy(art->sprite(0)->n_s,"TURN1A");strcpy(art->sprite(4)->n_s,"TURN1B");
    sequences=studio::infer_preview_sequences(*art);CHECK(studio::locate_preview_sprite(sequences,0,seq,frame));
    CHECK(sequences[seq].name=="TURN");CHECK(sequences[seq].frames[frame].pieces.size()==2);
    strcpy(art->sprite(7)->n_s,"WALK2");sequences=studio::infer_preview_sequences(*art);
    CHECK(studio::locate_preview_sprite(sequences,7,seq,frame));
    studio::PreviewPlayer p;p.playing=true;std::vector<int> holds{2,5,3};
    p.advance(2/studio::mk2_tick_hz,holds,studio::mk2_tick_hz);CHECK(p.frame==1);
    p.advance(4/studio::mk2_tick_hz,holds,studio::mk2_tick_hz);CHECK(p.frame==1);
    p.advance(1/studio::mk2_tick_hz,holds,studio::mk2_tick_hz);CHECK(p.frame==2);
    p.advance(10003/studio::mk2_tick_hz,holds,studio::mk2_tick_hz);CHECK(p.frame==0);CHECK(p.elapsed_ticks<1e-8);
    p.loop=false;p.seek(1,3);p.advance(20/studio::mk2_tick_hz,holds,studio::mk2_tick_hz);CHECK(p.frame==2&&!p.playing);
    p.step(1,3);CHECK(p.frame==0);p.step(-1,3);CHECK(p.frame==2);
    p.playing=true;p.advance(1,{},studio::mk2_tick_hz);CHECK(p.frame==2);
    IMG im{};im.w=63;im.h=134;im.anix=29;im.aniy=(unsigned short)-16;
    auto a=studio::place_sprite(im,200,210,false,MirrorConvention_Ani2);
    auto b=studio::place_sprite(im,200,210,true,MirrorConvention_Ani2);
    auto c=studio::place_sprite(im,200,210,true,MirrorConvention_Ganiof);
    CHECK(a.x==171&&a.y==226);CHECK(b.x==166&&c.x==167);
    // Shifting a composite preserves relative attachment; one undo restores all pieces.
    auto before=art->copy(2,0,0,48,64).pixels;
    studio::shift_frame_anchors(*art,{2,3,2},-11,7);art->commit();
    CHECK((short)art->sprite(2)->anix==13&&(short)art->sprite(3)->aniy==67);
    CHECK(art->copy(2,0,0,48,64).pixels==before);art->undo();
    CHECK(art->sprite(2)->anix==24&&art->sprite(3)->aniy==60&&!art->dirty());
    for(int arg=1;arg<argc;++arg){std::string error;auto real=studio::Artwork::open(argv[arg],error);CHECK(real!=nullptr);if(!real)continue;
        auto groups=studio::infer_preview_sequences(*real);
        for(int i=0;i<real->count();++i)CHECK(studio::locate_preview_sprite(groups,i,seq,frame));
        std::cout<<"Preview groups: "<<groups.size()<<" for "<<real->count()<<" sprites\n";
    }
    std::cout<<"Studio animation checks: "<<failures<<" failure(s)\n";return failures?1:0;
}
