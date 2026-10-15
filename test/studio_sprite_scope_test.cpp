#include "studio_sprite_scope.h"
#include "studio_library_copy.h"
#include "studio_batch_delete.h"
#include "studio_sequences.h"
#include <chrono>
#include <filesystem>
#include <iostream>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " / "<<error<<"\n";++failures;}}while(0)
int main(){using namespace studio;std::string error;auto art=std::make_unique<Artwork>();CHECK(art->add_palette("P",{0,0x7fff},error)==0);
    for(auto name:{"HERO1","HERO1A","HERO1B","HERO1A_1","HERO1FLIP","HERO1C","HERO10","HERO","HERO_1","HERO-2A","hero1D","KEEP"})CHECK(art->add_sprite(name,4,3,0,error)>=0);
    for(int i=0;i<art->count();++i)memcpy(art->sprite(i)->src_filename,"LIBRARY",8);memcpy(art->sprite(5)->src_filename,"OTHER",6);
    auto id=[&](int i){return art->sprite_id(i);};SpriteScope s;auto gen=art->generation();CHECK(review_sprite_scope(*art,{id(0)},false,s,error)&&s.rows.size()==1&&!s.rows[0].parent);
    CHECK(review_sprite_scope(*art,{id(0)},true,s,error)&&s.rows.size()==3&&s.rows[1].sprite==id(1)&&s.rows[1].parent==id(0)&&s.rows[1].parent_name=="HERO1"&&s.rows[2].sprite==id(2));
    CHECK(review_sprite_scope(*art,{id(1)},true,s,error)&&s.rows.size()==2&&s.rows[1].sprite==id(3)); // Never expand a child to its siblings.
    CHECK(review_sprite_scope(*art,{id(0),id(1)},true,s,error)&&s.rows.size()==4&&!s.rows[1].parent&&s.rows[2].parent==id(0)&&s.rows[3].parent==id(1));
    CHECK(review_sprite_scope(*art,{id(7)},true,s,error)&&s.rows.size()==5&&s.rows[0].sprite==id(0)&&s.rows[0].parent==id(7)&&s.rows[1].sprite==id(6)&&s.rows[3].sprite==id(8)&&s.rows[4].sprite==id(9));
    CHECK(!review_sprite_scope(*art,{},true,s,error)&&s.rows.empty());CHECK(!review_sprite_scope(*art,{id(0),id(0)},true,s,error));CHECK(!review_sprite_scope(*art,{999999},true,s,error));CHECK(art->generation()==gen);
    // Duplicate visible parent names and overlapping numbered/split rules never guess.
    auto extra=art->add_sprite("DUPLICATE",4,3,0,error);CHECK(extra>=0);memcpy(art->sprite(extra)->n_s,art->sprite(0)->n_s,16);memcpy(art->sprite(extra)->src_filename,art->sprite(0)->src_filename,16);
    CHECK(review_sprite_scope(*art,{id(0)},true,s,error)&&s.rows.size()==1&&s.ambiguous==2);CHECK(review_sprite_scope(*art,{id(0),id(1)},true,s,error)&&s.rows.size()==3&&s.ambiguous==1);
    memcpy(art->sprite(extra)->n_s,"DUPLICATE",10);CHECK(review_sprite_scope(*art,{id(0)},true,s,error)&&s.rows.size()==3&&!s.ambiguous);
    auto overlap=Artwork::create();CHECK(overlap->rename_sprite(0,"BASE1",error));CHECK(overlap->add_sprite("BASE1_",4,3,0,error)==1&&overlap->add_sprite("BASE1_1",4,3,0,error)==2);CHECK(review_sprite_scope(*overlap,{overlap->sprite_id(0)},true,s,error)&&s.rows.size()==1&&s.ambiguous==1&&s.skipped[0]=="BASE1_1");CHECK(review_sprite_scope(*overlap,{overlap->sprite_id(0),overlap->sprite_id(2)},true,s,error)&&s.rows.size()==2&&!s.ambiguous);
    auto target=Artwork::create();LibraryCopyOptions options;options.include_children=true;LibraryCopyPlan copy;CHECK(plan_library_copy(*art,*target,{id(0)},options,copy,error)&&copy.rows.size()==3&&copy.scope.rows[1].parent==id(0));auto bad=copy;bad.scope.rows[1].parent=0;CHECK(target->apply_library_copy(*art,bad,error)==-1&&target->count()==1);
    options.excluded.insert(id(2));CHECK(plan_library_copy(*art,*target,{id(0)},options,copy,error)&&copy.rows.size()==3&&!copy.rows[2].enabled);CHECK(target->apply_library_copy(*art,copy,error)==2&&target->count()==3&&std::string(target->sprite(2)->n_s)=="HERO1A");target->undo();CHECK(target->count()==1);target->redo();CHECK(target->count()==3);target->undo();
    CHECK(plan_library_copy(*art,*target,{id(0)},options,copy,error));memcpy(art->sprite(2)->src_filename,"OTHER",6);CHECK(target->apply_library_copy(*art,copy,error)==-1);memset(art->sprite(2)->src_filename,0,16);memcpy(art->sprite(2)->src_filename,"LIBRARY",8);
    CHECK(art->embedded_add_record(false,"USE_CHILD",error)==0&&art->embedded_append(0,2,error));BatchDeletePlan deletion;CHECK(plan_batch_delete(*art,{id(0)},deletion,error,true)&&deletion.rows.size()==3&&!deletion.rows[2].references.empty()&&art->apply_batch_delete(deletion,error)==-1);
    auto invalid=deletion;invalid.scope.rows[1].parent_name="FORGED";CHECK(art->apply_batch_delete(invalid,error)==-1);deletion.rows[2].enabled=false;auto retained=id(2);CHECK(art->apply_batch_delete(deletion,error)==2&&art->sprite_index(retained)==0);std::vector<EmbeddedRecord> records;CHECK(read_embedded(*art,records,error)&&records[0].entries[0].values.index==0);art->undo();CHECK(art->sprite_index(retained)==2&&art->count()==13);art->redo();CHECK(art->sprite_index(retained)==0);art->undo();
    auto root=std::filesystem::temp_directory_path()/("studio-families-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(root);auto file=(root/"families.IMG").u8string();CHECK(art->save(file,error));auto opened=Artwork::open(file,error);CHECK(opened&&opened->disk_version()==0x634);// Source-library labels are runtime provenance; a reopened IMG has one library label.
    if(opened){CHECK(plan_batch_delete(*opened,{opened->sprite_id(0)},deletion,error,true)&&deletion.rows.size()==4);deletion.rows[2].enabled=false;CHECK(opened->apply_batch_delete(deletion,error)==3&&opened->save((root/"deleted.IMG").u8string(),error));auto result=Artwork::open((root/"deleted.IMG").u8string(),error);CHECK(result&&result->disk_version()==0x634&&std::string(result->sprite(0)->n_s)=="HERO1B");}
    std::filesystem::remove_all(root);CHECK(g_doc==nullptr);std::cout<<"Sprite family scope: "<<failures<<" failures\n";return failures?1:0;
}
