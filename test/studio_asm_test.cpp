#include "studio_asm.h"
#include "strike_bind.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
int main(int argc,char **argv){using namespace studio;namespace fs=std::filesystem;std::string error;
    std::string text="\xef\xbb\xbf" "; source\r\nchar_anitab\r\n\t.long a_demo ; Demo move\r\na_demo:\r\na_alias\r\n\t.long GROUP\r\n\t.long ani_adjustxy\r\n\t.word -020h,8\r\n\t.long ani_flip\r\n\t.long ani_adjustx\r\n\t.word 4\r\n\t.long ani_nosleep,SINGLE\r\n\t.long 0\r\n\t.long ani_flip_v,GROUP\r\n\t.long ani_jump,a_tail\r\na_tail\r\n\t.long SINGLE\r\n\t.long ani_jump,a_demo\r\nGROUP\r\n\t.long PIECE1,PIECE2,0\r\n";
    auto s=AsmSource::parse(text,error);CHECK(s&&s->animations.size()==3);
    if(s){auto &a=s->animations[0];CHECK(a.name=="Demo move"&&a.frames.size()==4&&!a.partial);CHECK(a.frames[0].pieces==std::vector<std::string>({"PIECE1","PIECE2"}));
        CHECK(a.frames[1].dx==-36&&a.frames[1].dy==8&&a.frames[1].flip&&a.frames[1].no_sleep);CHECK(a.frames[2].segment==1&&a.frames[2].flip_y);CHECK(a.frames[3].name=="SINGLE");CHECK(s->animations[1].frames.size()==4);
        IMG im{};im.w=40;im.h=50;im.anix=5;im.aniy=15;auto p=place_asm_piece(im,a.frames[2],200,100,false,MirrorConvention_Ani2);
        CHECK(p.x==129&&p.y==73);p=place_asm_piece(im,a.frames[2],200,100,true,MirrorConvention_Ani2);CHECK(p.x==231&&p.y==73);
    }
    s=AsmSource::parse("a_demo\n\t.long ONE,ani_ochar_jump\n\t.word 2\n\t.long a_other,TWO\na_other\n\t.long TWO,0\n",error);CHECK(s&&s->animations[0].partial&&s->animations[0].frames.size()==1);
    s=AsmSource::parse("a_demo\n\t.long ONE,ani_calla,routine,TWO,ani_sound,sfx,0\n",error);CHECK(s&&s->animations[0].partial&&s->animations[0].frames.size()==2);
    s=AsmSource::parse("a_demo\n\t.long ONE,ani_adjustxy\n\t.word variable,2\n\t.long TWO,0\n",error);CHECK(s&&s->animations[0].partial&&s->animations[0].frames.size()==1);
    s=AsmSource::parse("a_demo\n\t.long ONE+32,TWO,0\n",error);CHECK(s&&s->animations[0].partial&&s->animations[0].frames.empty());
    s=AsmSource::parse("a_demo\n\t.long ONE,ani_jump,external\n",error);CHECK(s&&s->animations[0].partial&&s->animations[0].frames.size()==1);
    s=AsmSource::parse("a_demo\n\t.long ONE,0\na_demo_local_anipts\n\t.word 0ffffh,3\n",error);CHECK(s&&s->animations.size()==1&&s->animations[0].frames[0].dx==-1);
    s=AsmSource::parse("a_demo\n\t.long ONE,0\na_demo_local_anipts\n\t.word 1\n",error);CHECK(s&&s->animations[0].partial&&s->animations[0].frames[0].dx==0);
    s=AsmSource::parse("a_demo\n\t.long BAD,0\nBAD\n\t.long ONE,TWO\n",error);CHECK(s&&s->animations[0].partial&&s->animations[0].frames.empty());
    s=AsmSource::parse("a_demo\n\t.long ONE\n\t.if SOMETHING\n\t.long TWO,0\n",error);CHECK(s&&s->animations[0].partial&&s->animations[0].frames.size()==1);
    CHECK(!AsmSource::parse("a_demo\n\t.long ONE\na_demo\n\t.long TWO\n",error));CHECK(!AsmSource::parse(std::string(4*1024*1024+1,'x'),error));CHECK(!AsmSource::parse(std::string("a_demo\0",7),error));CHECK(!AsmSource::parse("a_demo\n\t.long ONE,,TWO\n",error));CHECK(!AsmSource::parse(std::string(2049,'x'),error));
    s=AsmSource::parse("a_demo\n\t.long ONE,ani_nosleep\n",error);CHECK(s&&s->animations[0].partial&&s->animations[0].frames.size()==1);
    s=AsmSource::parse("a_demo\n\t.long ONE,ani_nosleep,ani_flip,TWO\n",error);CHECK(s&&s->animations[0].partial&&s->animations[0].frames.size()==1);
    auto art=Artwork::demo(),other=Artwork::demo();AsmSourceFrame f;f.pieces={art->sprite(0)->n_s,"missing"};std::vector<AsmLibrary> libs={{11,art.get()},{12,other.get()}};
    auto resolved=resolve_asm_frame(f,libs,0);CHECK(resolved[0].ambiguous&&!resolved[0].sprite&&!resolved[1].sprite&&!resolved[1].ambiguous);
    resolved=resolve_asm_frame(f,libs,11);auto id=art->sprite_id(0);CHECK(resolved[0].doc==11&&resolved[0].sprite==id);CHECK(art->move_sprite(0,3,error));CHECK(art->sprite_index(resolved[0].sprite)==3);
    CHECK(art->rename_sprite(3,"RENAMED",error));resolved=resolve_asm_frame(f,libs,11);CHECK(resolved[0].doc==12);resolved=resolve_asm_frame(f,{{11,art.get()}},11);CHECK(!resolved[0].sprite);
    CHECK(strike_bind::MatchAnimForStrike("stk_jchikick",{"a_jcstance","a_jchikick"})==1);CHECK(strike_bind::MatchAnimForStrike("stk_jczap1",{"a_jczap","a_jczap2"})==-1);
    auto root=fs::temp_directory_path()/("studio-asm-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto path=root/fs::u8path("动画.ASM");{std::ofstream out(path,std::ios::binary);out<<text;}s=AsmSource::open(path.u8string(),error);CHECK(s&&!s->path.empty());CHECK(!AsmSource::open((root/"absent.ASM").u8string(),error));
    if(argc>2&&fs::exists(fs::u8path(argv[1]))&&fs::exists(fs::u8path(argv[2]))){s=AsmSource::open(argv[1],error);auto cage=Artwork::open(argv[2],error);CHECK(s&&cage);
        if(s&&cage){int stance=-1,throw_index=-1;for(int i=0;i<(int)s->animations.size();++i){if(s->animations[i].label=="a_jcstance")stance=i;if(s->animations[i].label=="a_jc_fb_hh")throw_index=i;}
            CHECK(stance>=0&&throw_index>=0);if(stance>=0){auto &a=s->animations[stance];CHECK(a.frames.size()==7&&!a.partial&&a.frames[0].pieces.size()==4);auto pieces=resolve_asm_frame(a.frames[0],{{1,cage.get()}},1);int matched=0;for(auto &p:pieces)matched+=p.sprite!=0;CHECK(matched>=3);}
            if(throw_index>=0){auto &a=s->animations[throw_index];CHECK(a.frames.size()==5);CHECK(a.frames.back().segment==2);}
            CHECK(cage->save((root/"unchanged.IMG").u8string(),error));auto bytes=[](const fs::path &p){std::ifstream in(p,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};CHECK(bytes(root/"unchanged.IMG")==bytes(fs::u8path(argv[2])));
        }
    }
    fs::remove_all(root);std::cout<<"ASM preview: "<<failures<<" failures\n";return failures?1:0;
}
