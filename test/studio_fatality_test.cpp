#include "studio_fatality.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cstdlib>
#include <new>
static long fail_allocation=-1;
void *operator new(std::size_t n){if(fail_allocation==0){fail_allocation=-1;throw std::bad_alloc();}if(fail_allocation>0)--fail_allocation;if(void *p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void *p)noexcept{std::free(p);}void operator delete(void *p,std::size_t)noexcept{std::free(p);}
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " / "<<error<<"\n";++failures;}}while(0)
static std::string read(const std::filesystem::path &path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const std::filesystem::path &path,const std::string &bytes){std::ofstream f(path,std::ios::binary);f.write(bytes.data(),bytes.size());}
int main(int argc,char **argv){
    using namespace studio;namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-fatality-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto path=root/fs::u8path(u8"fatality \u65e5.ASM");
    std::string original="\xef\xbb\xbf; preserve BOM and CRLF\r\nfatal_demo\r\n\tmovi r_fatal,a7 ; routine\r\n\tmovi scom_demo,a11\r\n\tmovi l_hp,a0\r\n\tcmpi 3,winner_status\r\n\tjmp fatality_xfer\r\n; untouched command tail\r\nscom_demo\r\n\t.word 20,sw_down,sw_up\r\n\t.word sw_hp\r\na_torso_ripped\r\n\t.long SENTINEL1\r\n\t.long 0\r\ntail_label\r\n\t.long 123";
    write(path,original);auto source=FatalitySource::open(path.u8string(),error);CHECK(source);if(!source)return 1;
    CHECK(source->blocks().size()==3&&source->blocks()[0].kind==FatalityKind::Command&&source->blocks()[0].label=="fatal_demo"&&source->blocks()[0].combo=="scom_demo"&&source->blocks()[0].summary.find("r_fatal")!=std::string::npos&&source->blocks()[1].kind==FatalityKind::Combo&&source->blocks()[1].summary.find("20")!=std::string::npos&&source->blocks()[2].kind==FatalityKind::Animation&&source->blocks()[2].body_ending);
    CHECK(source->header(0)=="fatal_demo"&&source->body(0).find('\r')==std::string::npos&&source->header(-1).empty()&&source->body(100).empty());
    auto generation=source->generation();FatalityPatch patch;auto body=source->body(0);
    CHECK(source->review(0,body,patch,error)&&!patch.changed&&source->apply(patch,error)&&source->generation()==generation&&!source->can_undo()&&!source->dirty()&&source->bytes()==original);
    CHECK(!source->review(-1,"",patch,error)&&!source->review(0,"new_label\n\t.long 0\n",patch,error)&&!source->review(0,"\tnew_label:\n\t.long 0\n",patch,error)&&!source->review(0,std::string(2050,' '),patch,error)&&!source->review(0,std::string("\t.word 0\0BAD",12),patch,error)&&!source->review(0,std::string(256*1024+1,' '),patch,error));
    auto changed=body;changed.replace(changed.find("r_fatal"),7,"r_other");CHECK(source->review(0,changed,patch,error)&&patch.changed&&patch.after.find("r_other,a7 ; routine\r\n")!=std::string::npos&&patch.after.substr(patch.after.find("scom_demo\r\n"))==original.substr(original.find("scom_demo\r\n")));
    auto tampered=patch;tampered.after+="X";CHECK(!source->apply(tampered,error)&&source->bytes()==original&&!source->dirty());
    auto second=FatalitySource::open(path.u8string(),error);CHECK(second&&!second->apply(patch,error));
    bool applied=false;int injected=0;for(long i=0;i<2000;++i){fail_allocation=i;applied=source->apply(patch,error);fail_allocation=-1;if(applied)break;++injected;CHECK(source->bytes()==original&&source->generation()==generation&&!source->can_undo()&&!source->dirty());}
    CHECK(applied&&injected>20&&source->dirty()&&source->can_undo()&&read(path)==original&&source->blocks()[0].summary.find("r_other")!=std::string::npos);
    auto edited=source->bytes();CHECK(!source->apply(patch,error)&&source->bytes()==edited);
    fail_allocation=0;bool undone=source->undo(error);fail_allocation=-1;CHECK(!undone&&source->bytes()==edited&&source->can_undo());
    CHECK(source->undo(error)&&source->bytes()==original&&!source->dirty()&&source->can_redo());
    fail_allocation=0;bool redone=source->redo(error);fail_allocation=-1;CHECK(!redone&&source->bytes()==original&&source->can_redo());
    CHECK(source->redo(error)&&source->bytes()==edited&&source->dirty());
    FatalityWriteReview save;CHECK(!source->review_save((root/"wrong.IMG").u8string(),save,error));
    CHECK(source->review_save(path.u8string(),save,error));auto bad_save=save;bad_save.bytes+="X";CHECK(!source->save(bad_save,error)&&read(path)==original);
    write(path,"external edit");CHECK(!source->save(save,error)&&source->dirty()&&read(path)=="external edit");write(path,original);
    auto copy=root/"copy.ASM";CHECK(source->review_save(copy.u8string(),save,error)&&!save.existed);write(copy,"created after review");CHECK(!source->save(save,error)&&read(copy)=="created after review");
    CHECK(source->review_save(copy.u8string(),save,error)&&save.previous=="created after review");write(copy,"changed destination");CHECK(!source->save(save,error)&&read(copy)=="changed destination");
    CHECK(source->review_save((root/"missing"/"fail.ASM").u8string(),save,error)&&!source->save(save,error)&&source->dirty()&&read(path)==original);
    CHECK(source->review_save(copy.u8string(),save,error));bool saved=false;injected=0;for(long i=0;i<2000;++i){fail_allocation=i;saved=source->save(save,error);fail_allocation=-1;if(saved)break;++injected;CHECK(source->dirty()&&source->bytes()==edited&&read(path)==original&&read(copy)=="changed destination");}
    CHECK(saved&&injected>5&&!source->dirty()&&read(copy)==edited&&read(path)==original&&source->path()==copy.u8string());
    CHECK(source->undo(error)&&source->dirty()&&source->bytes()==original&&source->redo(error)&&!source->dirty());
    auto reopened=FatalitySource::open(copy.u8string(),error);CHECK(reopened&&reopened->blocks().size()==3&&reopened->bytes()==edited);
    // A new Save review is required after any history or save transition.
    CHECK(source->review_save(copy.u8string(),save,error)&&source->undo(error)&&!source->save(save,error)&&read(copy)==edited);CHECK(source->redo(error));
    std::error_code ec;auto alias=root/"alias.ASM";fs::create_hard_link(copy,alias,ec);if(!ec)CHECK(!source->review_save(alias.u8string(),save,error));
    // Blank bodies may remove a command from the catalogue. Source Undo restores it.
    CHECK(source->review(0,"",patch,error)&&source->apply(patch,error)&&source->blocks().size()==2&&source->undo(error)&&source->blocks().size()==3);
    // No-op mixed endings and missing final newline are byte-exact; edited
    // bodies take their label's ending while the following source stays exact.
    auto mixed=root/"mixed.ASM";std::string mix=";x\nscom_test\r\n\t.word 20\n\t.word sw_up\r\ntail\n\t.long 3";write(mixed,mix);auto m=FatalitySource::open(mixed.u8string(),error);CHECK(m&&m->review(0,m->body(0),patch,error)&&!patch.changed&&patch.after==mix);
    CHECK(m->review(0,"\t.word 30",patch,error)&&m->apply(patch,error)&&m->bytes()==";x\nscom_test\r\n\t.word 30\r\ntail\n\t.long 3");
    auto eof=root/"eof.ASM";write(eof,"scom_tail");auto e=FatalitySource::open(eof.u8string(),error);CHECK(e&&e->review(0,"\t.word 10",patch,error)&&e->apply(patch,error)&&e->bytes()=="scom_tail\n\t.word 10");
    auto inline_file=root/"inline.ASM";write(inline_file,"a_head .long 1\n");auto in=FatalitySource::open(inline_file.u8string(),error);CHECK(in&&in->header(0)=="a_head .long 1"&&in->review(0,"\t.long 0\n",patch,error)&&in->apply(patch,error)&&in->bytes()=="a_head .long 1\n\t.long 0\n");
    // Reading/cataloguing real source must never rewrite it.
    for(int i=1;i<argc;++i)if(fs::exists(fs::u8path(argv[i]))){auto before=read(fs::u8path(argv[i]));auto real=FatalitySource::open(argv[i],error);CHECK(real&&!real->blocks().empty()&&real->bytes()==before&&!real->dirty());if(real)CHECK(real->review(0,real->body(0),patch,error)&&!patch.changed);CHECK(read(fs::u8path(argv[i]))==before);}
    write(root/"bad.ASM",std::string("bad\0data",8));CHECK(!FatalitySource::open((root/"bad.ASM").u8string(),error));
    CHECK(read(path)==original);fs::remove_all(root);std::cout<<"Studio fatality: "<<failures<<" failures\n";return failures?1:0;
}
