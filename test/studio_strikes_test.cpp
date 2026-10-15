#include "studio_strikes.h"
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
static std::string read(const std::filesystem::path &p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
static void write(const std::filesystem::path &p,const std::string &s){std::ofstream out(p,std::ios::binary);out.write(s.data(),s.size());}
static std::array<std::string,8> operands(const studio::StrikeRow &r){std::array<std::string,8> out;for(int i=0;i<8;++i)out[i]=r.fields[i].raw;return out;}
int main(int argc,char **argv){namespace fs=std::filesystem;using namespace studio;std::string error;
    auto root=fs::temp_directory_path()/("studio-strikes-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);auto source=root/fs::u8path(u8"source \u65e5.ASM");
    std::string body="; original comments\r\nlongname_strikes\r\n\t.long stk_test\r\n\t.long stk_alias\r\nstk_test:\r\nstk_alias\r\n\t.WORD 059H   ; x, keep indentation\r\n\t.word -10\r\n\t.word 040h\r\n\t.word 020h\r\n\t.word r_custom\r\n\t.word 02008h\r\n\t.long 0ffffffffh\r\n\t.word sf_squeeze ; last line has no newline";
    std::string bytes="\xef\xbb\xbf"+body;write(source,bytes);auto doc=StrikeSource::open(source.u8string(),error);CHECK(doc);
    if(!doc)return 1;CHECK(doc->records().size()==2&&doc->characters().size()==1&&doc->characters()[0].name=="longname"&&doc->characters()[0].moves.size()==2);
    CHECK(doc->records()[0].editable&&doc->records()[0].aliases.size()==2&&doc->bytes()==bytes&&!doc->dirty());CHECK(doc->records()[0].fields[6].numeric&&doc->records()[0].fields[6].value==4294967295ll);
    StrikeRect rect;CHECK(strike_rect(doc->records()[0].fields,200,100,false,rect)&&rect.x==225&&rect.y==90&&rect.w==64&&rect.h==32);CHECK(strike_rect(doc->records()[0].fields,200,100,true,rect)&&rect.x==111&&rect.y==90);
    auto fields=doc->records()[0].fields;fields[0].numeric=false;CHECK(!strike_rect(fields,200,100,false,rect));CHECK(!strike_rect(doc->records()[0].fields,2147483647,0,false,rect));
    auto values=operands(doc->records()[0]);StrikePatch patch;auto serial=doc->generation();CHECK(doc->review(0,values,patch,error)&&!patch.changed&&doc->apply(patch,error)&&doc->generation()==serial&&!doc->can_undo());
    values[0]=strike_literal(values[0],0,101);values[4]="r_other";values[7]="sf_squeeze+sf_other";CHECK(doc->review(0,values,patch,error)&&patch.changed&&!doc->dirty());
    auto expected=bytes;expected.replace(expected.find("059H"),4,"00065H");expected.replace(expected.find("r_custom"),8,"r_other");expected.replace(expected.find("sf_squeeze"),10,"sf_squeeze+sf_other");CHECK(patch.after==expected);CHECK(doc->apply(patch,error)&&doc->bytes()==expected&&doc->dirty());
    CHECK(doc->records()[1].fields[0].value==101&&doc->records()[1].fields[4].raw=="r_other");doc->undo();CHECK(doc->bytes()==bytes&&!doc->dirty());doc->redo();CHECK(doc->bytes()==expected&&doc->dirty());
    StrikeWriteReview output;CHECK(doc->review_save(source.u8string(),output,error)&&doc->save(output,error));CHECK(read(source)==expected&&!doc->dirty());doc->undo();CHECK(doc->dirty());doc->redo();CHECK(!doc->dirty());
    values=operands(doc->records()[0]);values[5]="65535";values[6]="4294967295";CHECK(doc->review(0,values,patch,error));auto tampered=patch;tampered.after+="bad";CHECK(!doc->apply(tampered,error));auto foreign=StrikeSource::open(source.u8string(),error);CHECK(!foreign->apply(patch,error));
    CHECK(doc->apply(patch,error));CHECK(!doc->apply(patch,error));doc->undo();CHECK(!doc->dirty());
    for(auto bad:{"32768","65535","-1"}){values=operands(doc->records()[0]);values[2]=bad;CHECK(!doc->review(0,values,patch,error));}
    for(auto bad:{"4294967296","999999999999999999999"}){values=operands(doc->records()[0]);values[6]=bad;CHECK(!doc->review(0,values,patch,error));}
    for(auto bad:{"sf_ok\n.word 1","sf_ok; comment","sf_a,sf_b","999999999999999999999"}){values=operands(doc->records()[0]);values[7]=bad;CHECK(!doc->review(0,values,patch,error));}
    auto newfile=root/"copy.ASM";CHECK(doc->review_save(newfile.u8string(),output,error));write(newfile,"external");CHECK(!doc->save(output,error)&&read(newfile)=="external");CHECK(doc->review_save(newfile.u8string(),output,error));CHECK(doc->save(output,error)&&read(newfile)==expected);
    CHECK(doc->review_save(newfile.u8string(),output,error));write(newfile,expected+"external");CHECK(!doc->save(output,error));CHECK(!doc->review_save(newfile.u8string(),output,error));CHECK(!doc->review_save((root/"no.IMG").u8string(),output,error));
    auto alias=root/"alias.ASM";std::error_code ec;fs::create_hard_link(newfile,alias,ec);if(!ec)CHECK(!doc->review_save(alias.u8string(),output,error));
#ifdef _WIN32
    auto readonly=root/"readonly.ASM";write(readonly,"keep original");CHECK(doc->review_save(readonly.u8string(),output,error));
    CHECK(SetFileAttributesW(readonly.c_str(),FILE_ATTRIBUTE_READONLY)!=0);CHECK(!doc->save(output,error)&&read(readonly)=="keep original");CHECK(SetFileAttributesW(readonly.c_str(),FILE_ATTRIBUTE_NORMAL)!=0);
#endif
    auto unsafe=root/"unsafe.ASM";write(unsafe,"stk_bad\n\t.word 1,2\n\t.word 3\n");auto partial=StrikeSource::open(unsafe.u8string(),error);CHECK(partial&&!partial->records()[0].editable&&!partial->warnings().empty());
    write(unsafe,"stk_x\nmov a0,a1\n"+body.substr(body.find("\t.WORD")));auto crossed=StrikeSource::open(unsafe.u8string(),error);CHECK(crossed&&!crossed->records()[0].editable);
    write(unsafe,bytes+"\r\nstk_test\r\n");CHECK(!StrikeSource::open(unsafe.u8string(),error));write(unsafe,std::string("a\0b",3));CHECK(!StrikeSource::open(unsafe.u8string(),error));write(unsafe,std::string(2050,'a'));CHECK(!StrikeSource::open(unsafe.u8string(),error));
    // Read-only real source; edits and saves occur only on temporary copies.
    if(argc>1&&fs::exists(fs::u8path(argv[1]))){auto original=read(fs::u8path(argv[1]));auto real=StrikeSource::open(argv[1],error);CHECK(real);if(real){int editable=0,chosen=-1;for(int i=0;i<(int)real->records().size();++i){if(real->records()[i].editable)++editable;if(real->records()[i].label=="stk_jchikick")chosen=i;}
        // The additive text API has exactly the classic file parser's results.
        mk2::Document classic,memory;CHECK(mk2::load(&classic,argv[1],&error));memory.source_path="owned snapshot";CHECK(mk2::parse_text(&memory,original,&error));
        CHECK(memory.source_path=="owned snapshot"&&memory.raw_lines==classic.raw_lines&&memory.records.size()==classic.records.size()&&memory.char_tables.size()==classic.char_tables.size());
        if(memory.records.size()==classic.records.size())for(size_t r=0;r<classic.records.size();++r){CHECK(memory.records[r].label==classic.records[r].label);for(int f=0;f<8;++f)CHECK(memory.records[r].fields[f].raw==classic.records[r].fields[f].raw&&memory.records[r].fields[f].line==classic.records[r].fields[f].line&&memory.records[r].fields[f].value==classic.records[r].fields[f].value);}
        auto classic_a=root/"classic-a.ASM",classic_b=root/"classic-b.ASM";classic.source_path=classic_a.u8string();CHECK(mk2::save(&classic,&error));CHECK(mk2::load(&classic,classic.source_path.c_str(),&error));classic.source_path=classic_b.u8string();CHECK(mk2::save(&classic,&error)&&read(classic_a)==read(classic_b));
        CHECK(editable>100&&chosen>=0);if(chosen>=0){values=operands(real->records()[chosen]);values[0]="93";CHECK(real->review(chosen,values,patch,error)&&real->apply(patch,error));auto copy=root/"real-copy.ASM";CHECK(real->review_save(copy.u8string(),output,error)&&real->save(output,error));auto reopened=StrikeSource::open(copy.u8string(),error);CHECK(reopened&&reopened->bytes()==real->bytes());real->undo();CHECK(real->bytes()==original);}
        CHECK(read(fs::u8path(argv[1]))==original);std::cout<<"Real source: "<<editable<<" editable / "<<real->records().size()<<" records\n";}}
    fs::remove_all(root);std::cout<<"Strike source: "<<failures<<" failures\n";return failures?1:0;
}
