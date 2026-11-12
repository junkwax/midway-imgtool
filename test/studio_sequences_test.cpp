#include <algorithm>
#include "studio_sequences.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static void word(unsigned char *p,int v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static std::vector<unsigned char> blob(const studio::Artwork &a){auto p=(const unsigned char *)a.data.scrseqmem_p;return p?std::vector<unsigned char>(p,p+a.data.scrseqbytes):std::vector<unsigned char>{};}
static std::string bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv){
    using namespace studio;namespace fs=std::filesystem;std::string error;std::vector<EmbeddedRecord> records;
    auto root=fs::temp_directory_path()/("studio-sequences-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto art=Artwork::demo();std::vector<unsigned char> fixture(250);
    for(size_t i=0;i<fixture.size();++i)fixture[i]=(unsigned char)(i*7);
    memset(fixture.data(),0,16);memcpy(fixture.data(),"WALK",4);word(fixture.data()+16,0);word(fixture.data()+18,2); // Legacy-created sequences can have zero flags.
    word(fixture.data()+102,0);fixture[104]=0;word(fixture.data()+106,5);word(fixture.data()+108,-7);
    word(fixture.data()+120,1);fixture[122]=255;word(fixture.data()+124,-3);word(fixture.data()+126,9);
    auto script=fixture.data()+134;memset(script,0,16);memcpy(script,"TEST",4);word(script+16,64);word(script+18,1);word(script+102,0);script[104]=4;
    art->data.seqcnt=1;art->data.scrcnt=1;art->data.scrseqbytes=(unsigned)fixture.size();art->data.scrseqmem_p=malloc(fixture.size());memcpy(art->data.scrseqmem_p,fixture.data(),fixture.size());
    art->data.damcnt=1;art->data.damtblbytes=4;art->data.damtbl_p=calloc(1,4);art->data.file_bufscr[0]=1;
    CHECK(read_embedded(*art,records,error)&&records.size()==2);CHECK(!records[0].script&&records[1].script);
    CHECK(records[0].entries[0].values.index==1&&records[0].entries[1].values.index==0);CHECK(records[0].entries[0].values.ticks==255&&records[0].entries[1].values.ticks==0);
    CHECK(embedded_duration(records[0])==256&&embedded_frame_at(records[0],254)==0&&embedded_frame_at(records[0],255)==1&&embedded_frame_at(records[0],256)==-1);
    auto first=records[0].entries[0].raw,second=records[0].entries[1].raw;auto values=records[0].entries[0].values;
    CHECK(art->embedded_edit_entry(0,0,values,error));art->commit();CHECK(blob(*art)==fixture&&!art->dirty()&&!art->can_undo());
    auto source=root/"sequence.IMG";CHECK(art->save(source.u8string(),error));auto original_file=bytes(source);
    auto opened=Artwork::open(source.u8string(),error);CHECK(opened&&blob(*opened)==fixture&&opened->data.seqcnt==1&&opened->data.scrcnt==1);
    values.ticks=23;values.dx=-12;CHECK(art->embedded_edit_entry(0,0,values,error));art->commit();auto changed=blob(*art);
    for(size_t i=0;i<fixture.size();++i)if(i!=122&&i!=124&&i!=125)CHECK(changed[i]==fixture[i]);
    CHECK(art->dirty());art->undo();CHECK(blob(*art)==fixture&&!art->dirty());art->redo();CHECK(blob(*art)==changed);art->undo();
    CHECK(art->embedded_duplicate(0,0,error));CHECK(read_embedded(*art,records,error)&&records[0].entries.size()==3&&records[0].entries[1].raw==first);
    CHECK(art->embedded_move(0,0,2,error));CHECK(read_embedded(*art,records,error)&&records[0].entries[1].raw==second&&records[0].entries[2].raw==first);
    CHECK(art->embedded_delete(0,1,error));CHECK(read_embedded(*art,records,error)&&records[0].entries.size()==2);art->undo();art->undo();art->undo();CHECK(blob(*art)==fixture&&!art->dirty());
    CHECK(art->embedded_add_record(false,"NEW",error)==1);CHECK(art->data.seqcnt==2&&art->data.scrcnt==1);CHECK(read_embedded(*art,records,error)&&records[2].script&&records[2].entries[0].values.index==0);
    CHECK(std::equal(fixture.begin()+134,fixture.end(),blob(*art).begin()+232));
    CHECK(art->save((root/"new.IMG").u8string(),error));opened=Artwork::open((root/"new.IMG").u8string(),error);CHECK(opened&&opened->data.seqcnt==2&&opened->data.scrcnt==1&&read_embedded(*opened,records,error)&&records[1].entries.empty());CHECK(opened&&opened->data.damcnt==1&&opened->data.damtblbytes==4&&!memcmp(opened->data.damtbl_p,art->data.damtbl_p,4)&&opened->data.file_bufscr[0]==1);
    art->undo();CHECK(art->data.seqcnt==1&&art->data.scrcnt==1&&blob(*art)==fixture);art->redo();CHECK(art->data.seqcnt==2);art->undo();
    CHECK(art->embedded_add_record(true,"SCRIPT2",error)==2);CHECK(art->data.scrcnt==2);art->undo();CHECK(art->data.scrcnt==1&&blob(*art)==fixture);
    CHECK(art->embedded_rename(0,"SIXTEENCHARACTER",error));art->commit();changed=blob(*art);CHECK(std::equal(changed.begin()+16,changed.end(),fixture.begin()+16));art->undo();CHECK(blob(*art)==fixture);
    CHECK(art->embedded_append(0,2,error));CHECK(read_embedded(*art,records,error)&&records[0].entries.back().values.index==2&&records[0].entries.back().values.ticks==4);art->undo();
    CHECK(art->embedded_delete(1,0,error));CHECK(read_embedded(*art,records,error)&&records[1].entries.empty());art->undo();
    auto before=blob(*art);values.ticks=256;CHECK(!art->embedded_edit_entry(0,0,values,error));values.ticks=1;values.dx=32768;CHECK(!art->embedded_edit_entry(0,0,values,error));values.dx=0;values.index=art->count();CHECK(!art->embedded_edit_entry(0,0,values,error));
    CHECK(!art->embedded_append(1,1,error));CHECK(!art->embedded_rename(0,std::string(17,'X'),error));CHECK(blob(*art)==before);
    // Remap only sequence targets, independent of their flags. Script indices refer to sequences.
    CHECK(art->move_sprite(0,2,error));CHECK(read_embedded(*art,records,error)&&records[0].entries[0].values.index==0&&records[0].entries[1].values.index==2&&records[1].entries[0].values.index==0);art->undo();CHECK(blob(*art)==fixture);
    CHECK(!art->delete_sprite(0,error)&&blob(*art)==fixture);
    CHECK(art->save(source.u8string(),error));auto snapshot=embedded_scene(*art,10,0,error);CHECK(snapshot&&snapshot->state.lanes[0].frames[0].ticks==255&&snapshot->state.lanes[0].frames[1].ticks==1);
    CHECK(snapshot&&snapshot->state.lanes[0].frames[0].x==3&&snapshot->state.lanes[0].frames[0].y==-9);CHECK(!embedded_scene(*art,10,1,error));
    CHECK(blob(*art)==fixture);if(snapshot){auto file=root/"snapshot.ISTUDIO";CHECK(snapshot->save(file.u8string(),[&](uint64_t id){return id==10?art.get():nullptr;},error));CHECK(bytes(file).find("version=4\n")!=std::string::npos);auto p=SceneProject::read(file.u8string(),error);CHECK(p&&p->state.lanes[0].frames[0].ticks==255);}
    for(auto convention:{MirrorConvention_Ani2,MirrorConvention_Ganiof})for(bool flip:{false,true}){auto p=embedded_scene(*art,10,0,error,flip,convention);CHECK(p!=nullptr);if(p){auto pose=evaluate_scene(p->state,0);CHECK(pose.size()==1);if(!pose.empty()){auto im=art->sprite(1);auto placement=place_sprite(*im,pose[0].x,pose[0].y,flip,convention);CHECK(placement.x==p->state.origin_x-anipoint_effective((short)im->anix-3,im->w,flip,convention));CHECK(placement.y==p->state.origin_y-((short)im->aniy+9));}}}
    art->data.scrseqbytes--;CHECK(!read_embedded(*art,records,error)&&!art->embedded_append(0,0,error));art->data.scrseqbytes++;
    art->data.scrseqmem_p=realloc(art->data.scrseqmem_p,fixture.size()+1);((unsigned char *)art->data.scrseqmem_p)[fixture.size()]=42;art->data.scrseqbytes++;CHECK(!read_embedded(*art,records,error)&&!art->embedded_duplicate(0,0,error));art->data.scrseqbytes--;
    word((unsigned char *)art->data.scrseqmem_p+18,4097);CHECK(!read_embedded(*art,records,error));word((unsigned char *)art->data.scrseqmem_p+18,2);
    values={1,255,4097,0};CHECK(art->embedded_edit_entry(0,0,values,error));art->commit();CHECK(!embedded_scene(*art,10,0,error));art->undo();
    CHECK(art->save(source.u8string(),error));CHECK(bytes(source)==original_file);
    // Optional real libraries are read-only; only temporary copies are edited.
    for(int i=1;i<argc;++i){auto real=Artwork::open(argv[i],error);CHECK(real!=nullptr);if(!real)continue;CHECK(read_embedded(*real,records,error));std::cout<<fs::u8path(argv[i]).filename().u8string()<<": "<<real->data.seqcnt<<" sequences, "<<real->data.scrcnt<<" scripts\n";
        auto copy=root/("real-"+std::to_string(i)+".IMG");CHECK(real->save(copy.u8string(),error));auto baseline=bytes(copy);auto raw=blob(*real);if(!records.empty()&&!records[0].entries.empty()){auto v=records[0].entries[0].values;v.ticks=(v.ticks+1)%256;CHECK(real->embedded_edit_entry(0,0,v,error));real->commit();CHECK(real->save(copy.u8string(),error));auto reopened=Artwork::open(copy.u8string(),error);CHECK(reopened&&read_embedded(*reopened,records,error)&&records[0].entries[0].values.ticks==v.ticks);real->undo();CHECK(blob(*real)==raw);CHECK(real->save(copy.u8string(),error)&&bytes(copy)==baseline);}}
    CHECK(g_doc==nullptr);fs::remove_all(root);return failures?1:0;
}
