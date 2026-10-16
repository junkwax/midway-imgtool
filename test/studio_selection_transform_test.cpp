#include "studio_selection_transform.h"
#include "studio_sequences.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <climits>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " ("<<error<<")\n";++failures;}}while(0)
static std::vector<char> bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv){using namespace studio;namespace fs=std::filesystem;std::string error;auto a=Artwork::demo();int index=a->add_sprite("TRANSFORM",5,7,0,error);CHECK(index>=0);auto im=a->sprite(index);auto p=(unsigned char *)im->data_p;
    for(int y=0;y<7;++y){for(int x=0;x<5;++x)p[y*8+x]=8;for(int x=5;x<8;++x)p[y*8+x]=0xaa;}const unsigned char source[]={0,9,10,11,12,13};for(int y=0;y<3;++y)for(int x=0;x<2;++x)p[(y+1)*8+x+1]=source[y*2+x];
    im->anix=17;im->aniy=(unsigned short)-4;im->anix2=3;im->aniy2=4;im->aniz2=5;im->pttbl_p=calloc(1,40);((unsigned char *)im->pttbl_p)[0]=42;im->flags=0x80;im->file_frm=23;
    memcpy(im->file_name_raw,im->n_s,16);im->file_name_raw[14]=0x76;CHECK(a->embedded_add_record(false,"TEST",error)==0&&a->embedded_append(0,index,error));
    auto root=fs::temp_directory_path()/("selection-transform-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);auto file=root/"original.IMG";CHECK(a->save(file.u8string(),error));auto original_file=bytes(file);auto original=a->copy(index,0,0,5,7).pixels;auto identity=a->sprite_id(index),gen=a->generation();
    auto rect=rectangle_selection(5,7,1,1,2,3);SelectionTransform s;SelectionTransformPreview preview;CHECK(begin_selection_transform(*a,index,rect,s,error)&&a->generation()==gen&&!a->dirty());
    const std::vector<std::vector<unsigned char>> expected={{0,9,10,11,12,13},{12,10,0,13,11,9},{13,12,11,10,9,0},{9,11,13,0,10,12}};
    // Every quarter-turn/flip combination maps indices exactly, including transparency.
    for(int rotation=0;rotation<4;++rotation)for(int flip=0;flip<4;++flip){s.rotation=rotation;s.flip_h=(flip&1)!=0;s.flip_v=(flip&2)!=0;s.copy=true;s.x=s.y=0;s.w=rotation%2?3:2;s.h=rotation%2?2:3;CHECK(preview_selection_transform(s,preview,error));for(int y=0;y<s.h;++y)for(int x=0;x<s.w;++x){int ex=s.flip_h?s.w-1-x:x,ey=s.flip_v?s.h-1-y:y;CHECK(preview.pixels[y*5+x]==expected[rotation][ey*s.w+ex]);}CHECK(preview.selection.count()==6&&a->copy(index,0,0,5,7).pixels==original&&!a->dirty());}
    auto masked=combine_selection(rect,rectangle_selection(5,7,1,2,1,1),SelectionOp::Subtract,5,7);CHECK(begin_selection_transform(*a,index,masked,s,error));s.rotation=1;s.w=3;s.h=2;s.x=1;s.y=4;
    CHECK(preview_selection_transform(s,preview,error)&&preview.selection.count()==5);CHECK(preview.pixels[4*5+1]==12&&preview.pixels[4*5+2]==8&&preview.pixels[4*5+3]==0&&preview.pixels[5*5+3]==9);CHECK(preview.pixels[2*5+1]==10&&preview.pixels[1*5+2]==0);
    auto expected_move=preview.pixels;CHECK(a->apply_selection_transform(s,error)&&a->copy(index,0,0,5,7).pixels==expected_move&&a->sprite_id(index)==identity);im=a->sprite(index);CHECK(im->anix==17&&(short)im->aniy==-4&&im->anix2==3&&im->aniy2==4&&im->aniz2==5&&im->flags==0x80&&im->file_frm==23&&((unsigned char *)im->pttbl_p)[0]==42);for(int y=0;y<7;++y)for(int x=5;x<8;++x)CHECK(((unsigned char *)im->data_p)[y*8+x]==0xaa);
    std::vector<EmbeddedRecord> records;CHECK(read_embedded(*a,records,error)&&records[0].entries[0].values.index==index);CHECK(a->save((root/"moved.IMG").u8string(),error));auto loaded=Artwork::open((root/"moved.IMG").u8string(),error);CHECK(loaded&&loaded->copy(index,0,0,5,7).pixels==expected_move);a->undo();CHECK(a->copy(index,0,0,5,7).pixels==original);a->redo();CHECK(a->copy(index,0,0,5,7).pixels==expected_move);a->undo();CHECK(a->save(file.u8string(),error)&&bytes(file)==original_file);
    CHECK(begin_selection_transform(*a,index,masked,s,error));s.copy=true;s.x=1;s.y=4;s.rotation=1;s.w=3;s.h=2;CHECK(preview_selection_transform(s,preview,error)&&preview.pixels[1*5+2]==9&&preview.pixels[2*5+1]==10);CHECK(a->apply_selection_transform(s,error));a->undo();CHECK(a->copy(index,0,0,5,7).pixels==original);
    // Size changes always sample the frozen source, not a previous scaled preview.
    CHECK(begin_selection_transform(*a,index,masked,s,error));s.copy=true;s.w=4;s.h=6;CHECK(preview_selection_transform(s,preview,error)&&preview.selection.count()==20&&preview.pixels[1*5+1]==0&&preview.pixels[1*5+2]==0&&!preview.selection.contains(1,3));s.w=2;s.h=3;CHECK(preview_selection_transform(s,preview,error)&&preview.pixels==original);auto label=std::string(a->undo_label());CHECK(a->apply_selection_transform(s,error)&&std::string(a->undo_label())==label);
    // Whole source stays read-only during a cancelled preview (no begin/commit required).
    auto cancel_gen=a->generation();CHECK(begin_selection_transform(*a,index,rect,s,error));s.flip_h=true;CHECK(preview_selection_transform(s,preview,error));s={};CHECK(a->generation()==cancel_gen&&a->copy(index,0,0,5,7).pixels==original);
    CHECK(begin_selection_transform(*a,index,rect,s,error));auto invalid=s;invalid.x=INT_MAX;gen=a->generation();CHECK(!preview_selection_transform(invalid,preview,error)&&!a->apply_selection_transform(invalid,error)&&a->generation()==gen);invalid=s;invalid.w=8192;invalid.h=8192;CHECK(!preview_selection_transform(invalid,preview,error));invalid=s;invalid.original.pop_back();CHECK(!preview_selection_transform(invalid,preview,error));invalid=s;invalid.clip.mask.pop_back();CHECK(!a->apply_selection_transform(invalid,error)&&a->generation()==gen);invalid=s;invalid.original[0]=9;CHECK(!a->apply_selection_transform(invalid,error));
    auto foreign=Artwork::demo();CHECK(!foreign->apply_selection_transform(s,error)&&!foreign->dirty());a->color(0,1,0x7fff);a->commit();CHECK(!a->apply_selection_transform(s,error));a->undo();
    auto corners=combine_selection(rectangle_selection(5,7,1,1,1,1),rectangle_selection(5,7,3,3,1,1),SelectionOp::Add,5,7);CHECK(begin_selection_transform(*a,index,corners,s,error));s.w=s.h=1;CHECK(!preview_selection_transform(s,preview,error));
    // Clipboard transforms freeze mapped destination indices and keep holes separate
    // from explicitly selected transparency. They never cut pixels from the target.
    auto clip=a->copy_selection(index,rect);gen=a->generation();
    CHECK(begin_clipboard_transform(*a,index,clip,s,error)&&s.clipboard&&s.copy&&a->generation()==gen);
    s.x=1;s.y=4;s.flip_h=true;CHECK(preview_selection_transform(s,preview,error));
    CHECK(preview.pixels[4*5+1]==9&&preview.pixels[4*5+2]==0&&preview.pixels[1*5+2]==9);
    auto pasted=preview.pixels;CHECK(a->apply_selection_transform(s,error)&&a->copy(index,0,0,5,7).pixels==pasted);a->undo();CHECK(a->copy(index,0,0,5,7).pixels==original);a->redo();CHECK(a->copy(index,0,0,5,7).pixels==pasted);a->undo();
    clip.mask.clear();CHECK(begin_clipboard_transform(*a,index,clip,s,error));s.x=1;s.y=4;CHECK(preview_selection_transform(s,preview,error)&&preview.pixels[4*5+1]==8&&preview.pixels[4*5+2]==9&&!preview.selection.contains(1,4));
    auto target_colors=a->copy(index,0,0,5,7).colors;Clipboard foreign_clip;foreign_clip.w=2;foreign_clip.h=1;foreign_clip.colors={0,target_colors[10],target_colors[9]};foreign_clip.pixels={1,2};CHECK(begin_clipboard_transform(*a,index,foreign_clip,s,error)&&s.clip.colors==target_colors&&s.clip.pixels==std::vector<unsigned char>({10,9}));
    auto preview_gen=a->generation();CHECK(preview_selection_transform(s,preview,error)&&preview.pixels[0]==10&&preview.pixels[1]==9&&a->generation()==preview_gen);
    gen=a->generation();invalid=s;invalid.copy=false;CHECK(!a->apply_selection_transform(invalid,error)&&a->generation()==gen);invalid=s;invalid.original[0]=9;CHECK(!a->apply_selection_transform(invalid,error)&&a->generation()==gen);invalid=s;invalid.clip.pixels[0]=255;CHECK(!preview_selection_transform(invalid,preview,error));
    CHECK(!foreign->apply_selection_transform(s,error));auto current=a->sprite(index);((unsigned char *)current->data_p)[0]=9;CHECK(!a->apply_selection_transform(s,error));((unsigned char *)current->data_p)[0]=original[0];
    foreign_clip.mask={1};CHECK(!begin_clipboard_transform(*a,index,foreign_clip,s,error));foreign_clip.mask.clear();foreign_clip.pixels[0]=255;CHECK(!begin_clipboard_transform(*a,index,foreign_clip,s,error));foreign_clip.pixels[0]=1;foreign_clip.w=6;CHECK(!begin_clipboard_transform(*a,index,foreign_clip,s,error));
    CHECK(a->copy(index,0,0,5,7).pixels==original&&a->save(file.u8string(),error)&&bytes(file)==original_file);
    for(int n=1;n<argc;++n){auto game=Artwork::open(argv[n],error);CHECK(game);if(!game)continue;auto sprite=game->sprite(0);int w=sprite->w,h=sprite->h;auto selection=opaque_selection(*sprite);if(!selection.valid)continue;auto out=root/("game"+std::to_string(n)+".IMG");CHECK(game->save(out.u8string(),error));auto before=bytes(out);CHECK(begin_selection_transform(*game,0,selection,s,error));s.flip_h=true;CHECK(preview_selection_transform(s,preview,error));auto result=preview.pixels;CHECK(game->apply_selection_transform(s,error));CHECK(game->save((root/("edit"+std::to_string(n)+".IMG")).u8string(),error));game->undo();CHECK(game->save(out.u8string(),error)&&bytes(out)==before);}
    CHECK(g_doc==nullptr);fs::remove_all(root);return failures?1:0;
}
