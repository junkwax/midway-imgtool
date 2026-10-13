#include "studio_regions.h"
#include "studio_sequences.h"
#include "studio_animation.h"
#include "anipoint.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " ("<<error<<")\n";++failures;}}while(0)
static std::vector<char> bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv){using namespace studio;namespace fs=std::filesystem;std::string error;auto art=Artwork::demo();auto parent=art->sprite(0);
    parent->pttbl_p=calloc(1,40);((unsigned char *)parent->pttbl_p)[0]=42;parent->flags=0x80;parent->file_frm=17;parent->anix2=10;parent->aniy2=20;parent->aniz2=3;
    parent->opals=1;parent->opaltbl_p=calloc(1,16);((unsigned char *)parent->opaltbl_p)[0]=0;
    memcpy(parent->file_name_raw,parent->n_s,16);parent->file_name_raw[14]=0x76;
    CHECK(art->embedded_add_record(false,"TEST",error)==0&&art->embedded_append(0,0,error));parent=art->sprite(0);
    auto original=art->copy(0,0,0,48,64).pixels;auto id=art->sprite_id(0);auto root=fs::temp_directory_path()/("studio-regions-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);auto source=root/"source.IMG";CHECK(art->save(source.u8string(),error));auto saved=bytes(source);
    auto selected=combine_selection(rectangle_selection(48,64,16,8,16,15),rectangle_selection(48,64,20,12,2,2),SelectionOp::Subtract,48,64);
    std::vector<RegionRequest> requests={{"Head","SENTINEL1_HD",selected,true}};RegionPlan plan;CHECK(build_region_plan(*art,0,requests,true,false,plan,error));CHECK(plan.children.size()==1&&plan.children[0].x==16&&plan.children[0].y==8&&plan.children[0].anix==8&&plan.children[0].aniy==52);
    CHECK(!art->dirty());int first=art->extract_regions(plan,error);CHECK(first==8&&art->count()==9&&art->sprite_id(0)==id&&art->copy(0,0,0,48,64).pixels==original);auto child=art->sprite(first);parent=art->sprite(0);CHECK(child&&child->palnum==parent->palnum&&child->opals==parent->opals&&child->opaltbl_p&&child->opaltbl_p!=parent->opaltbl_p&&!child->pttbl_p&&!secondary_anipoint_in_use(child)&&child->file_frm==0xffff&&child->flags==0);
    CHECK(((unsigned char *)child->data_p)[4*((child->w+3)&~3)+4]==0);auto placement=place_sprite(*child,100,100,false,MirrorConvention_Ani2);auto parent_place=place_sprite(*art->sprite(0),100,100,false,MirrorConvention_Ani2);CHECK(placement.x==parent_place.x+16&&placement.y==parent_place.y+8);
    auto child_id=art->sprite_id(first);CHECK(art->selection_id()==child_id);art->undo();CHECK(art->count()==8&&!art->dirty());art->redo();CHECK(art->sprite_id(8)==child_id);CHECK(art->save((root/"child.IMG").u8string(),error));auto loaded=Artwork::open((root/"child.IMG").u8string(),error);CHECK(loaded&&loaded->count()==9&&loaded->sprite(8)->opaltbl_p&&!loaded->sprite(8)->pttbl_p);art->undo();CHECK(art->save(source.u8string(),error)&&bytes(source)==saved);
    // Frozen masks drive both copying and the source cut, including their holes.
    CHECK(build_region_plan(*art,0,requests,false,true,plan,error));first=art->extract_regions(plan,error);CHECK(first==8);auto cut=art->copy(0,0,0,48,64).pixels;for(int y=0;y<64;++y)for(int x=0;x<48;++x)CHECK(cut[y*48+x]==(selected.contains(x,y)?0:original[y*48+x]));art->undo();CHECK(art->copy(0,0,0,48,64).pixels==original);
    bool fallback=false;auto body=propose_body_regions(*art->sprite(0),fallback);CHECK(body.size()==5);CHECK(build_region_plan(*art,0,body,true,true,plan,error)&&plan.children.size()>=3);int body_count=(int)plan.children.size();first=art->extract_regions(plan,error);CHECK(first==8&&art->count()==8+body_count);
    // Reconstruct all child pixels at their rebased source positions.
    auto reconstructed=art->copy(0,0,0,48,64).pixels;for(size_t i=0;i<plan.children.size();++i){auto c=art->sprite(first+(int)i);const auto &region=plan.children[i];int stride=(c->w+3)&~3;for(int y=0;y<c->h;++y)for(int x=0;x<c->w;++x){auto value=((unsigned char *)c->data_p)[y*stride+x];if(value)reconstructed[(region.y+y)*48+region.x+x]=value;}}CHECK(reconstructed==original);std::vector<EmbeddedRecord> records;CHECK(read_embedded(*art,records,error)&&records[0].entries[0].values.index==0);art->undo();CHECK(art->count()==8&&art->save(source.u8string(),error)&&bytes(source)==saved);
    // Invalid/foreign/stale and overflow plans are rejected before mutation.
    auto generation=art->generation();requests[0].name="bad name";CHECK(!build_region_plan(*art,0,requests,true,false,plan,error)&&art->generation()==generation);requests[0].name="VALID";requests[0].selection=rectangle_selection(48,64,0,0,1,1);CHECK(!build_region_plan(*art,0,requests,true,false,plan,error));requests[0].selection=selected;CHECK(build_region_plan(*art,0,requests,true,false,plan,error));auto other=Artwork::demo();CHECK(other->extract_regions(plan,error)<0&&!other->dirty());art->color(0,1,0x7fff);art->commit();CHECK(art->extract_regions(plan,error)<0);art->undo();
    art->sprite(0)->anix=(unsigned short)-32768;CHECK(!build_region_plan(*art,0,requests,true,false,plan,error));art->sprite(0)->anix=24;
    requests.push_back(requests[0]);CHECK(build_region_plan(*art,0,requests,true,false,plan,error)&&plan.children[0].name!=plan.children[1].name&&plan.overlap_pixels==plan.opaque_pixels);
    auto tampered=plan;tampered.requests[0].selection.canvas_w=49;tampered.requests[0].selection.mask.resize(1);generation=art->generation();CHECK(art->extract_regions(tampered,error)<0&&art->generation()==generation&&art->count()==8);
    for(int n=1;n<argc;++n){auto game=Artwork::open(argv[n],error);CHECK(game);if(!game)continue;auto im=game->sprite(0);auto selection=opaque_selection(*im);if(!selection.valid)continue;int w=im->w,h=im->h;std::vector<RegionRequest> req={{"Fixture","EXTRACT",selection,true}};CHECK(build_region_plan(*game,0,req,true,true,plan,error));auto file=root/("fixture"+std::to_string(n)+".IMG");CHECK(game->save(file.u8string(),error));auto before=bytes(file);CHECK(build_region_plan(*game,0,req,true,true,plan,error));int count=game->count();first=game->extract_regions(plan,error);CHECK(first==count&&game->count()==count+1);CHECK(game->save((root/("cut"+std::to_string(n)+".IMG")).u8string(),error));game->undo();CHECK(game->count()==count&&game->save(file.u8string(),error)&&bytes(file)==before);}
    CHECK(g_doc==nullptr);fs::remove_all(root);return failures?1:0;
}
