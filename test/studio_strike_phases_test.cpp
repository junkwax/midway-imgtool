#include "studio_strike_phases.h"
#include "studio_strikes.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static std::string read(const std::filesystem::path &p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
int main(int argc,char **argv){using namespace studio;namespace fs=std::filesystem;std::string error;
    auto record=[](const std::string &name){return name+"\n\t.word 50,-10,30,20,r_test,4\n\t.long 0\n\t.word sound\n";};
    std::string prefix="PHASE_MAX .set 2\nstrike_tables\n\t.long demo_strikes\n\t.if wunit\n\t.long external_strikes\n\t.endif\n\t.if stkphase\nstk_phase_tab\n\t.long demo_phases\n\t.if wunit\n\t.long demo_phases\n\t.endif\ndemo_phases\n\t.long 0,move_phases\n\t.long -1\nmove_phases\n\t.word 4,2\n\t.word 0,0\n\t.endif\ndemo_strikes\n\t.long stk_base\nhelper_slot\n\t.long stk_other\n\t.if stkphase\n\t.long stk_early\n\t.endif\n";
    auto text=prefix+record("stk_base")+record("stk_other")+record("stk_early");PhaseCatalog c;CHECK(read_strike_phases(text,c,error)&&c.limit_known&&c.scan_limit==2&&c.lists.size()==1&&c.lists[0].entries.size()==2);
    PhaseOptions on;on.enabled=true;PhaseOptions off;auto chars=phase_characters(c,on,error);CHECK(chars==std::vector<std::string>{"demo_strikes"});
    auto p=pick_strike_phase(c,0,0,6,on);CHECK(p.valid&&p.active&&p.phased&&p.slot==2&&p.record=="stk_early"&&p.base_record=="stk_base");CHECK(pick_strike_phase(c,0,0,4,on).record=="stk_early");p=pick_strike_phase(c,0,0,3,on);CHECK(p.valid&&p.phased&&p.record=="stk_base"&&p.row==1);CHECK(pick_strike_phase(c,0,0,1,on).record=="stk_base");
    p=pick_strike_phase(c,0,0,0,on);CHECK(p.valid&&!p.active&&!p.phased&&p.record=="stk_base");p=pick_strike_phase(c,0,0,6,off);CHECK(p.valid&&p.active&&!p.phased&&p.record=="stk_base");
    CHECK(phase_slot_record(c,0,1,on,error)=="stk_other");CHECK(phase_slot_record(c,0,2,off,error).empty());CHECK(!pick_strike_phase(c,0,0,-1,on).valid);CHECK(!pick_strike_phase(c,99,0,6,on).valid);CHECK(!pick_strike_phase(c,0,999,6,on).valid);
    int character=-1,slot=-1;CHECK(first_strike_phase(c,character,slot)&&character==0&&slot==0);
    p=pick_strike_phase(c,0,1,6,on);CHECK(p.valid&&!p.phased&&p.record=="stk_other");auto wolf=on;wolf.wolf=true;CHECK(phase_characters(c,wolf,error).size()==2);CHECK(!pick_strike_phase(c,1,0,6,wolf).valid);
    auto changed=text;changed.replace(changed.find("4,2"),3,"8,2");changed.replace(changed.find("0,0"),3,"7,0");CHECK(read_strike_phases(changed,c,error));p=pick_strike_phase(c,0,0,1,on);CHECK(p.valid&&p.fallback&&p.record=="stk_base");
    changed=text;changed.replace(changed.find("4,2"),3,"4,99");CHECK(read_strike_phases(changed,c,error));CHECK(!pick_strike_phase(c,0,0,6,on).valid);
    changed=text;changed.replace(changed.find("4,2"),3,"4,variable");CHECK(read_strike_phases(changed,c,error));CHECK(!pick_strike_phase(c,0,0,6,on).valid);
    changed=text;changed.replace(changed.find("4,2"),3,"4,fadeh");CHECK(read_strike_phases(changed,c,error));CHECK(!pick_strike_phase(c,0,0,6,on).valid);
    changed=text;changed.replace(changed.find(".word 0,0"),9,".word 0");CHECK(read_strike_phases(changed,c,error));CHECK(!c.lists[0].error.empty()&&!pick_strike_phase(c,0,0,1,on).valid);
    changed=text;changed.replace(changed.find(".long -1"),8,".long 0ffffh");CHECK(read_strike_phases(changed,c,error));CHECK(!pick_strike_phase(c,0,1,6,on).valid);
    changed=text;changed.replace(changed.find(".long -1"),8,".word -1");CHECK(read_strike_phases(changed,c,error));CHECK(!pick_strike_phase(c,0,0,6,on).valid);
    changed=text;changed.replace(changed.find(".set 2"),6,".set unknown");CHECK(read_strike_phases(changed,c,error)&&!c.limit_known&&!pick_strike_phase(c,0,0,6,on).valid);
    changed=text;changed.replace(changed.find(".long 0,move_phases"),19,".long 0,0");CHECK(read_strike_phases(changed,c,error));p=pick_strike_phase(c,0,0,6,on);CHECK(p.valid&&!p.phased&&p.record=="stk_base");
    changed=text;changed.replace(changed.find(".if stkphase"),12,".if custom_flag");CHECK(read_strike_phases(changed,c,error));CHECK(!pick_strike_phase(c,0,0,6,on).valid);
    CHECK(!read_strike_phases(text+"\n.else\n",c,error));CHECK(!read_strike_phases(text+"\n.if stkphase\n",c,error));CHECK(!read_strike_phases(text+"\n.if stkphase\n.elseif wunit\n.endif\n",c,error));CHECK(!read_strike_phases(text+"\nstrike_tables\n",c,error));CHECK(!read_strike_phases(std::string(4*1024*1024+1,'x'),c,error));CHECK(!read_strike_phases(std::string("a\0b",3),c,error));
    CHECK(read_strike_phases(text+"\x1a\r\n",c,error));CHECK(!read_strike_phases(text+"\x1a" "unreadable tail",c,error));
    std::string oversized_table="stk_phase_tab\n";for(int i=0;i<257;++i)oversized_table+="\t.long 0\n";CHECK(!read_strike_phases(oversized_table,c,error));
    changed=text;std::string compact_rows=".word 4,2\n\t.word 0,0";changed.replace(changed.find(compact_rows),compact_rows.size(),".word 4,2,0,0");CHECK(read_strike_phases(changed,c,error));p=pick_strike_phase(c,0,0,3,on);CHECK(p.valid&&p.position==c.lists[0].entries[1].position&&p.position!=c.lists[0].entries[0].position);
    changed=text;auto pos=changed.find("move_phases\n",changed.find(".long -1"));changed.insert(pos+12,"move_alias\n");CHECK(read_strike_phases(changed,c,error));CHECK(pick_strike_phase(c,0,0,6,on).record=="stk_early");
    // Editing preserves exact non-operand bytes and shares source history / save guards.
    auto temp=fs::temp_directory_path()/("studio-phase-edit-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(temp);auto file=temp/fs::u8path(u8"phase \u65e5.ASM");
    auto write=[&](const std::string &bytes){std::ofstream output(file,std::ios::binary);output<<bytes;};
    auto editable_record=[](const std::string &name){return name+"\n\t.word 50\n\t.word -10\n\t.word 30\n\t.word 20\n\t.word r_test\n\t.word 4\n\t.long 0\n\t.word sound\n";};
    auto edit_text=prefix+editable_record("stk_base")+editable_record("stk_other")+editable_record("stk_early");edit_text.replace(edit_text.find(".word 4,2\n\t.word 0,0"),20,".WORD 004H , 002H , 0,0 ; keep the two pairs on one line");
    std::string exact="\xef\xbb\xbf";for(char ch:edit_text){if(ch=='\n')exact+='\r';exact+=ch;}exact+="\x1a";write(exact);auto doc=StrikeSource::open(file.u8string(),error);CHECK(doc);
    if(doc){PhasePatch patch;std::vector<std::array<std::string,2>> values={{{"004H","002H"}},{{"0","0"}}};auto serial=doc->generation();
        CHECK(review_phase_edit(*doc,"move_phases",values,on,patch,error)&&!patch.changed&&patch.consumers.size()==1);CHECK(doc->apply_phase(patch,error)&&doc->generation()==serial&&!doc->can_undo());
        CHECK(!review_phase_edit(*doc,"move_phases",values,off,patch,error));CHECK(!review_phase_edit(*doc,"move_phases",values,wolf,patch,error));
        values[0][0]="005H";CHECK(review_phase_edit(*doc,"move_phases",values,on,patch,error));auto expected=exact;expected.replace(expected.find("004H"),4,"005H");CHECK(patch.after==expected&&patch.changed&&!doc->dirty());
        auto foreign=StrikeSource::open(file.u8string(),error);CHECK(!foreign->apply_phase(patch,error));auto bad=patch;bad.after+='x';CHECK(!doc->apply_phase(bad,error));bad=patch;bad.consumers.clear();CHECK(!doc->apply_phase(bad,error));
        CHECK(doc->apply_phase(patch,error)&&doc->bytes()==expected&&doc->dirty());CHECK(!doc->apply_phase(patch,error));doc->undo();CHECK(doc->bytes()==exact&&!doc->dirty());doc->redo();CHECK(doc->bytes()==expected);
        std::array<std::string,8> geometry;for(int i=0;i<8;++i)geometry[i]=doc->records().back().fields[i].raw;geometry[0]="77";StrikePatch shape;CHECK(doc->review((int)doc->records().size()-1,geometry,shape,error)&&doc->apply(shape,error));doc->undo();CHECK(doc->bytes()==expected);doc->undo();CHECK(doc->bytes()==exact);doc->redo();CHECK(doc->bytes()==expected);doc->redo();CHECK(doc->records().back().fields[0].value==77);doc->undo();
        StrikeWriteReview saved;auto copy=temp/"reviewed.ASM";CHECK(doc->review_save(copy.u8string(),saved,error)&&doc->save(saved,error));CHECK(read(copy)==expected&&!doc->dirty());auto reopened=StrikeSource::open(copy.u8string(),error);CHECK(reopened&&reopened->bytes()==expected);CHECK(read(file)==exact);
        for(auto value:{"-1","32768","65535","65536","symbol","fadeh","0"}){auto invalid=values;invalid[0][0]=value;CHECK(!review_phase_edit(*doc,"move_phases",invalid,on,patch,error));}
        for(auto value:{"-1","256","65536","symbol","1 ; inject"}){auto invalid=values;invalid[0][1]=value;CHECK(!review_phase_edit(*doc,"move_phases",invalid,on,patch,error));}
        auto invalid=values;invalid[1][0]="1";CHECK(!review_phase_edit(*doc,"move_phases",invalid,on,patch,error));invalid.pop_back();CHECK(!review_phase_edit(*doc,"move_phases",invalid,on,patch,error));
        CHECK(!review_phase_edit(*doc,"demo_strikes",values,on,patch,error));
        auto alias_text=exact;alias_text.insert(alias_text.find("move_phases\r\n",alias_text.find(".long -1"))+13,"phase_alias\r\n");write(alias_text);auto alias=StrikeSource::open(file.u8string(),error);CHECK(alias&&review_phase_edit(*alias,"phase_alias",values,on,patch,error)&&alias->apply_phase(patch,error));
        // A shared list must validate all active consumers, including aliases.
        auto shared=exact;shared.replace(shared.find("external_strikes"),16,"second_strikes");shared.erase(shared.find('\x1a'));shared+="\r\nsecond_strikes\r\n\t.long stk_base,stk_other,stk_early\r\n";write(shared);auto shared_doc=StrikeSource::open(file.u8string(),error);CHECK(shared_doc&&review_phase_edit(*shared_doc,"move_phases",values,wolf,patch,error)&&patch.consumers.size()==2);
        shared.replace(shared.find(".long stk_base,stk_other,stk_early"),std::string(".long stk_base,stk_other,stk_early").size(),".long stk_base,stk_other\r\n\t.align 2");write(shared);shared_doc=StrikeSource::open(file.u8string(),error);CHECK(shared_doc&&!review_phase_edit(*shared_doc,"move_phases",values,wolf,patch,error));
        auto conditional=exact;conditional.insert(conditional.find(".WORD 004H"),".if wunit\r\n\t");conditional.insert(conditional.find("; keep"),"\r\n\t.endif\r\n\t");write(conditional);auto irregular=StrikeSource::open(file.u8string(),error);CHECK(irregular&&!review_phase_edit(*irregular,"move_phases",values,on,patch,error));
    }fs::remove_all(temp);
    if(argc>1&&fs::exists(fs::u8path(argv[1]))){auto original=read(fs::u8path(argv[1]));auto source=StrikeSource::open(argv[1],error);bool parsed=source&&read_strike_phases(original,c,error);CHECK(parsed);if(!parsed)std::cerr<<error<<"\n";if(parsed){CHECK(c.scan_limit==8&&c.lists.size()==1&&c.lists[0].label=="lk_lokick_phases");CHECK(phase_characters(c,on,error).size()==20&&phase_characters(c,wolf,error).size()==25);
            p=pick_strike_phase(c,1,1,6,on);CHECK(p.valid&&p.record=="stk_lklokick_early"&&p.slot==23);CHECK(pick_strike_phase(c,1,1,3,on).record=="stk_lklokick");CHECK(pick_strike_phase(c,1,1,6,off).record=="stk_lklokick");CHECK(phase_slot_record(c,1,23,off,error)=="stk_lkdubknee");CHECK(phase_slot_record(c,1,24,on,error)=="stk_lkdubknee");CHECK(phase_slot_record(c,1,25,on,error)=="stk_lkknee_link");CHECK(!pick_strike_phase(c,22,1,6,wolf).valid);
            CHECK(first_strike_phase(c,character,slot)&&character==1&&slot==1);PhasePatch real_patch;std::vector<std::array<std::string,2>> real_values={{{"5","017h"}},{{"0","001h"}}};CHECK(review_phase_edit(*source,"lk_lokick_phases",real_values,on,real_patch,error)&&real_patch.changed&&real_patch.consumers.size()==1);CHECK(!review_phase_edit(*source,"lk_lokick_phases",real_values,wolf,real_patch,error));CHECK(!source->dirty()&&!source->can_undo()&&source->bytes()==original&&read(fs::u8path(argv[1]))==original);
        }
    }
    std::cout<<"Strike phases: "<<failures<<" failures\n";return failures?1:0;
}
