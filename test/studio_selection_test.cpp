#include "studio_document.h"
#include "studio_selection.h"
#include "studio_sequences.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <climits>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static std::vector<char> bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static unsigned char pixel(const studio::Artwork &a,int i,int x,int y){auto im=a.sprite(i);return ((unsigned char *)im->data_p)[y*((im->w+3)&~3)+x];}
int main(int argc,char **argv){using namespace studio;namespace fs=std::filesystem;std::string error;
    // Five-wide rows have padding. Two distinct opaque slots share black.
    unsigned short words[]={0,0,0,0x7fff,0x7bde};PAL pal{};pal.numc=5;pal.data_p=words;
    unsigned char pixels[]={1,1,0,2,3,255,255,255,1,0,0,2,3,255,255,255,0,2,0,0,3,255,255,255};IMG im{};im.w=5;im.h=3;im.data_p=pixels;
    auto connected=wand_selection(im,pal,0,0,0,true),global=wand_selection(im,pal,0,0,0,false);
    CHECK(connected.count()==3&&global.count()==6&&global.contains(3,0)&&!global.contains(2,0));
    auto transparent=wand_selection(im,pal,2,0,255,false);CHECK(transparent.count()==6&&!transparent.contains(0,0)&&!transparent.contains(4,0));
    auto near=wand_selection(im,pal,4,0,255,false);CHECK(near.count()==9&&!near.contains(2,0));
    pixels[4]=4;CHECK(wand_selection(im,pal,4,1,0,true).count()==2);CHECK(wand_selection(im,pal,4,1,16,true).count()==3);
    CHECK(!wand_selection(im,pal,-1,0,0,true).valid);pixels[4]=255;CHECK(!wand_selection(im,pal,4,0,0,true).valid);pixels[4]=3;
    auto rect=rectangle_selection(5,3,-1,-1,4,3);CHECK(rect.count()==6&&rect.x==0&&rect.y==0&&rect.w==3&&rect.h==2);
    CHECK(!rectangle_selection(5,3,INT_MAX,0,INT_MAX,1).valid);
    auto polygon=lasso_selection(5,3,{{0,0},{3,0},{3,2},{0,2}});CHECK(polygon.count()==6&&polygon.contains(2,1)&&!polygon.contains(3,1));
    CHECK(lasso_selection(5,3,{{0,0},{4,0},{0,3}}).count()==6);CHECK(!lasso_selection(5,3,{{-1,0},{2,0},{0,2}}).valid);
    auto hole=rectangle_selection(5,3,1,0,1,1),shape=combine_selection(rect,hole,SelectionOp::Subtract,5,3);
    CHECK(shape.count()==5&&!shape.contains(1,0)&&shape.contains(0,0));CHECK(combine_selection(shape,hole,SelectionOp::Add,5,3).count()==6);
    CHECK(!combine_selection(shape,hole,SelectionOp::Intersect,5,3).valid);CHECK(invert_selection(shape,5,3).count()==10);
    CHECK(opaque_selection(im).count()==9);CHECK(translate_selection(shape,1,1,5,3).count()==5);
    CHECK(!shape.fits(6,3)&&!combine_selection(shape,{},SelectionOp::Add,6,3).valid);
    auto a=Artwork::demo();int i=a->add_sprite("MASK",5,3,0,error);CHECK(i>=0);a->paint(i,0,0,8,8);a->commit();
    auto before=a->copy(i,0,0,5,3).pixels;a->paint(i,1,0,9,8,&shape);a->commit();
    for(int y=0;y<3;++y)for(int x=0;x<5;++x)CHECK(pixel(*a,i,x,y)==(shape.contains(x,y)?9:8));a->undo();CHECK(a->copy(i,0,0,5,3).pixels==before);
    a->fill(i,1,0,9,&shape);CHECK(a->copy(i,0,0,5,3).pixels==before);a->fill(i,0,0,9,&shape);CHECK(pixel(*a,i,0,0)==9&&pixel(*a,i,1,0)==8&&pixel(*a,i,2,1)==9&&pixel(*a,i,4,2)==8);a->undo();
    auto islands=combine_selection(rectangle_selection(5,3,0,0,1,1),rectangle_selection(5,3,4,0,1,1),SelectionOp::Add,5,3);a->fill(i,0,0,9,&islands);CHECK(pixel(*a,i,0,0)==9&&pixel(*a,i,4,0)==8);a->undo();
    a->paint(i,0,0,0);a->commit();auto clip=a->copy_selection(i,shape);CHECK(clip.mask.size()==6&&clip.mask[0]&&clip.pixels[0]==0&&!clip.mask[1]);
    // Covered transparency erases; the unselected hole retains destination art.
    a->paste(i,clip,2,1);CHECK(pixel(*a,i,2,1)==0&&pixel(*a,i,3,1)==8);a->undo();
    auto unmasked=a->copy(i,0,0,3,2);auto label=std::string(a->undo_label());a->paste(i,unmasked,2,1);CHECK(pixel(*a,i,2,1)==8&&label==a->undo_label());
    auto invalid=clip;invalid.mask.pop_back();auto snapshot=a->copy(i,0,0,5,3).pixels;a->paste(i,invalid,0,0);CHECK(a->copy(i,0,0,5,3).pixels==snapshot);
    invalid=clip;invalid.pixels[0]=255;a->paste(i,invalid,0,0);CHECK(a->copy(i,0,0,5,3).pixels==snapshot);a->paste(i,clip,INT_MAX,INT_MIN);CHECK(a->copy(i,0,0,5,3).pixels==snapshot);
    CHECK(a->clear_selection(i,shape,error));CHECK(pixel(*a,i,1,0)==8&&pixel(*a,i,2,1)==0&&pixel(*a,i,4,2)==8);a->undo();CHECK(a->copy(i,0,0,5,3).pixels==snapshot);
    auto stable=a->sprite_id(i);a->sprite(i)->anix=17;a->sprite(i)->aniy=(unsigned short)-4;
    CHECK(a->move_selection(i,shape,1,1,error));CHECK(pixel(*a,i,1,1)==0&&pixel(*a,i,2,1)==0&&pixel(*a,i,3,2)==8&&pixel(*a,i,1,0)==8);CHECK(a->sprite_id(i)==stable&&(short)a->sprite(i)->anix==17&&(short)a->sprite(i)->aniy==-4);
    a->undo();CHECK(a->copy(i,0,0,5,3).pixels==snapshot);a->redo();a->undo();CHECK(!a->move_selection(i,shape,-1,0,error)&&a->copy(i,0,0,5,3).pixels==snapshot);CHECK(a->move_selection(i,shape,0,0,error));
    // Geometry mismatch cannot silently become unrestricted painting.
    auto stale=shape;stale.canvas_w=6;a->paint(i,0,0,9,8,&stale);a->commit();a->fill(i,0,0,9,&stale);CHECK(!a->clear_selection(i,stale,error)&&!a->move_selection(i,stale,1,0,error));CHECK(a->copy(i,0,0,5,3).pixels==snapshot&&a->copy_selection(i,stale).pixels.empty());
    auto root=fs::temp_directory_path()/("studio-selection-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    CHECK(a->save((root/"source.IMG").u8string(),error));auto original=bytes(root/"source.IMG");CHECK(a->clear_selection(i,shape,error));CHECK(a->save((root/"cleared.IMG").u8string(),error));auto opened=Artwork::open((root/"cleared.IMG").u8string(),error);CHECK(opened&&pixel(*opened,i,1,0)==8&&pixel(*opened,i,2,1)==0);a->undo();CHECK(a->save((root/"restored.IMG").u8string(),error)&&bytes(root/"restored.IMG")==original);
    // Mapping a masked clipboard into a reordered palette preserves colors/holes.
    auto b=Artwork::demo();auto colors=clip.colors;std::swap(colors[8],colors[9]);CHECK(b->add_palette("REORDER",colors,error)==1);int bi=b->add_sprite("DEST",5,3,1,error);b->paint(bi,0,0,7,8);b->commit();b->paste(bi,clip,0,0);CHECK(pixel(*b,bi,0,0)==0&&pixel(*b,bi,1,0)==7&&pixel(*b,bi,2,0)==9);b->undo();CHECK(pixel(*b,bi,0,0)==7);
    for(int n=1;n<argc;++n){auto game=Artwork::open(argv[n],error);CHECK(game);if(!game)continue;auto sprite=game->sprite(0);int w=sprite->w,h=sprite->h;auto selected=opaque_selection(*sprite);if(!selected.valid)selected=rectangle_selection(w,h,0,0,1,1);auto file=root/("game-"+std::to_string(n)+".IMG");CHECK(game->save(file.u8string(),error));auto saved=bytes(file);auto id=game->sprite_id(0);auto baseline=game->copy(0,0,0,w,h).pixels;CHECK(game->clear_selection(0,selected,error));game->undo();CHECK(game->sprite_id(0)==id&&game->copy(0,0,0,w,h).pixels==baseline);CHECK(game->save(file.u8string(),error)&&bytes(file)==saved);}
    CHECK(g_doc==nullptr);fs::remove_all(root);return failures?1:0;
}
