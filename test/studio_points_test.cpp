#include "studio_points.h"
#include <chrono>
#include <filesystem>
#include <iostream>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static void check_roundtrip(studio::Artwork &art,const std::filesystem::path &out){
    using namespace studio;std::string error;PointTableEdit original,edit;auto id=art.sprite_id(0);auto format=art.disk_version();CHECK(read_point_table(art,id,original,error));
    auto before=*art.sprite(0);auto pixels=art.copy(0,0,0,before.w,before.h).pixels;edit=original;edit.enabled=true;for(int i=0;i<20;++i)edit.points[i]=i%2?-i*71:i*63;
    CHECK(apply_point_table(art,edit,error));CHECK(art.dirty()&&art.sprite(0)->pttbl_p);auto fields=*art.sprite(0);fields.pttbl_p=before.pttbl_p;CHECK(!memcmp(&fields,&before,sizeof fields));CHECK(art.copy(0,0,0,before.w,before.h).pixels==pixels);
    CHECK(art.save(out.u8string(),error));auto opened=Artwork::open(out.u8string(),error);CHECK(opened&&opened->disk_version()==format);
    if(opened){PointTableEdit stored;CHECK(read_point_table(*opened,opened->sprite_id(0),stored,error)&&stored.existed&&stored.points==edit.points);}
    art.undo();CHECK(read_point_table(art,id,edit,error)&&edit.existed==original.existed&&edit.original==original.original);art.redo();CHECK(read_point_table(art,id,edit,error)&&edit.enabled);
    edit.enabled=false;CHECK(apply_point_table(art,edit,error)&&!art.sprite(0)->pttbl_p);art.undo();CHECK(art.sprite(0)->pttbl_p!=nullptr);art.redo();CHECK(!art.sprite(0)->pttbl_p);
    CHECK(art.save(out.u8string(),error));opened=Artwork::open(out.u8string(),error);CHECK(opened&&opened->disk_version()==format&&!opened->sprite(0)->pttbl_p);
}
int main(int argc,char **argv){using namespace studio;namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-points-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto art=Artwork::demo();check_roundtrip(*art,root/"points.IMG");
    art=Artwork::demo();PointTableEdit edit;auto id=art->sprite_id(0);CHECK(read_point_table(*art,id,edit,error)&&!edit.enabled);auto generation=art->generation();
    CHECK(apply_point_table(*art,edit,error)&&art->generation()==generation&&!art->dirty()&&!art->can_undo());edit.enabled=true;edit.points[0]=-32768;edit.points[1]=32767;
    CHECK(apply_point_table(*art,edit,error));CHECK(!apply_point_table(*art,edit,error));CHECK(read_point_table(*art,id,edit,error)&&edit.points[0]==-32768&&edit.points[1]==32767);
    art->commit();generation=art->generation();CHECK(apply_point_table(*art,edit,error)&&art->generation()==generation);edit.points[0]=32768;CHECK(!apply_point_table(*art,edit,error)&&art->generation()==generation);edit.points[0]=-32769;CHECK(!apply_point_table(*art,edit,error));
    auto foreign=Artwork::demo();CHECK(!apply_point_table(*foreign,edit,error));CHECK(!read_point_table(*art,0,edit,error));
    CHECK(read_point_table(*art,id,edit,error));edit.original[0]^=1;CHECK(!apply_point_table(*art,edit,error));CHECK(read_point_table(*art,id,edit,error));art->paint(0,0,0,8);art->commit();CHECK(!apply_point_table(*art,edit,error));art->undo();CHECK(!apply_point_table(*art,edit,error));
    CHECK(read_point_table(*art,id,edit,error));CHECK(art->move_sprite(0,3,error));CHECK(!apply_point_table(*art,edit,error));CHECK(read_point_table(*art,id,edit,error));edit.points[5]=123;CHECK(apply_point_table(*art,edit,error));CHECK(art->sprite_index(id)==3);
    art->undo();CHECK(read_point_table(*art,id,edit,error)&&edit.points[5]==0);art->undo();CHECK(art->sprite_index(id)==0);
    if(argc>1&&fs::exists(fs::u8path(argv[1]))){auto old=Artwork::open(argv[1],error);CHECK(old);if(old){CHECK(old->disk_version()<0x634);check_roundtrip(*old,root/"old-points.IMG");}}
    fs::remove_all(root);std::cout<<"Point table: "<<failures<<" failures\n";return failures?1:0;
}
