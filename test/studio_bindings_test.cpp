#include "studio_bindings.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static void write(const std::filesystem::path &p,const std::string &s){std::ofstream out(p,std::ios::binary);out.write(s.data(),s.size());}
static std::string read(const std::filesystem::path &p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
int main(){using namespace studio;namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-bindings-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto source=root/fs::u8path(u8"strike 日.ASM"),sidecar=root/fs::u8path(u8"strike 日.imgtool");write(source,"; untouched ASM\r\n");
    auto doc=StrikeBindings::open_for(source.u8string(),error);CHECK(doc&&!doc->dirty()&&doc->bindings().empty());if(!doc)return 1;
    auto generation=doc->generation();CHECK(doc->set("a_demo",{},error)&&doc->generation()==generation&&!doc->can_undo());
    CHECK(doc->set("a_demo",std::string("stk_demo"),error));CHECK(doc->lookup("A_DEMO")==std::optional<std::string>("stk_demo")&&doc->dirty());auto first=doc->bytes();
    CHECK(doc->set("A_DEMO",std::string("-"),error));auto classic=strike_bind::ParseSidecar(doc->bytes());CHECK(classic.size()==1&&classic[0].strike=="-");
    doc->undo();CHECK(doc->bytes()==first);doc->undo();CHECK(!doc->dirty()&&doc->bytes().empty());doc->redo();CHECK(doc->bytes()==first);doc->redo();CHECK(doc->dirty());
    BindingsWriteReview review;CHECK(doc->review_save(review,error)&&!review.existed&&doc->save(review,error));CHECK(read(sidecar)==doc->bytes()&&!doc->dirty()&&read(source)=="; untouched ASM\r\n");
    auto loaded=StrikeBindings::open_for(source.u8string(),error);CHECK(loaded&&loaded->lookup("a_demo")==std::optional<std::string>("-"));
    CHECK(doc->set("a_demo",{},error)&&!doc->lookup("a_demo"));doc->discard();CHECK(doc->lookup("a_demo")==std::optional<std::string>("-")&&!doc->dirty()&&!doc->can_undo());
    std::string original="\xef\xbb\xbf" "# hand annotations\r\n\r\n A_DEMO \t= stk_old \t\r\n; comment stays in place\nother = stk_other\r\na_demo=stk_last\nunknown text preserved";
    write(sidecar,original);doc=StrikeBindings::open_for(source.u8string(),error);CHECK(doc&&doc->warnings().size()==1);CHECK(doc->lookup("a_demo")==std::optional<std::string>("stk_last"));
    CHECK(doc->set("a_demo",std::string("stk_new"),error));auto expected=original;expected.replace(expected.find("stk_old"),7,"stk_new");expected.replace(expected.find("stk_last"),8,"stk_new");CHECK(doc->bytes()==expected);
    CHECK(doc->review_save(review,error));auto foreign=StrikeBindings::open_for(source.u8string(),error);CHECK(!foreign->save(review,error));auto tampered=review;tampered.path=source.u8string();CHECK(!doc->save(tampered,error));tampered=review;tampered.bytes+="tampered";CHECK(!doc->save(tampered,error));tampered=review;tampered.previous+="tampered";CHECK(!doc->save(tampered,error));
    CHECK(doc->set("other",std::string("stk_changed"),error));CHECK(!doc->save(review,error));doc->undo();CHECK(!doc->save(review,error));CHECK(doc->review_save(review,error));
    write(sidecar,original+"external");CHECK(!doc->save(review,error)&&read(sidecar)==original+"external"&&doc->dirty());CHECK(!doc->review_save(review,error));write(sidecar,original);CHECK(doc->review_save(review,error)&&doc->save(review,error)&&read(sidecar)==expected);
    CHECK(doc->set("A_DEMO",{},error));CHECK(!doc->lookup("a_demo")&&doc->lookup("other")==std::optional<std::string>("stk_other"));CHECK(doc->bytes().find("hand annotations")!=std::string::npos&&doc->bytes().find("comment stays in place")!=std::string::npos&&doc->bytes().find("unknown text preserved")!=std::string::npos);
    doc->undo();CHECK(doc->bytes()==expected&&!doc->dirty());CHECK(doc->set("a_extra",std::string("stk_extra"),error));CHECK(doc->bytes()==expected+"\r\na_extra = stk_extra\r\n");
    for(auto &bad:{"", "a bad", "a_test\n", "a;test", "9label", "a=label"})CHECK(!doc->set(bad,std::string("stk_demo"),error));CHECK(!doc->set("a_valid",std::string("stk_demo ; comment"),error));
    write(sidecar,"a_bad = stk_demo ; inline comment\r\na_good = stk_good\r\n");loaded=StrikeBindings::open_for(source.u8string(),error);CHECK(loaded&&loaded->warnings().size()==1);CHECK(!loaded->set("a_bad",{},error));CHECK(!loaded->set("a_bad",std::string("stk_new"),error));CHECK(loaded->set("a_good",std::string("stk_other"),error));CHECK(loaded->bytes().find("a_bad = stk_demo ; inline comment\r\n")==0);
    write(sidecar,std::string("a\0b",3));CHECK(!StrikeBindings::open_for(source.u8string(),error));write(sidecar,std::string(2049,'x'));CHECK(!StrikeBindings::open_for(source.u8string(),error));write(sidecar,std::string(256*1024+1,'x'));CHECK(!StrikeBindings::open_for(source.u8string(),error));CHECK(!StrikeBindings::open_for((root/"no.IMG").u8string(),error));
    auto newsource=root/"new.ASM",newsidecar=root/"new.imgtool";write(newsource,"unchanged");loaded=StrikeBindings::open_for(newsource.u8string(),error);CHECK(loaded&&loaded->set("a_demo",std::string("stk_demo"),error)&&loaded->review_save(review,error));write(newsidecar,"external");CHECK(!loaded->save(review,error)&&read(newsidecar)=="external");fs::remove(newsidecar);CHECK(loaded->review_save(review,error)&&loaded->save(review,error));
#ifdef _WIN32
    CHECK(loaded->set("a_demo",std::string("-"),error)&&loaded->review_save(review,error));auto saved=read(newsidecar);CHECK(SetFileAttributesW(newsidecar.c_str(),FILE_ATTRIBUTE_READONLY)!=0);CHECK(!loaded->save(review,error)&&read(newsidecar)==saved);CHECK(SetFileAttributesW(newsidecar.c_str(),FILE_ATTRIBUTE_NORMAL)!=0);
#endif
    fs::remove(newsidecar);std::error_code ec;fs::create_hard_link(newsource,newsidecar,ec);if(!ec)CHECK(!StrikeBindings::open_for(newsource.u8string(),error));
    fs::remove_all(root);std::cout<<"Strike associations: "<<failures<<" failures\n";return failures?1:0;
}
