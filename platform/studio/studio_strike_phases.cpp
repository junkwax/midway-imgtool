#include "studio_strike_phases.h"
#include "studio_strikes.h"
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <algorithm>
namespace studio {
namespace {
std::string trim(const std::string &s){auto a=s.find_first_not_of(" \t\r\n");return a==s.npos?"":s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);}
std::string lower(std::string s){for(auto &c:s)if(c>='A'&&c<='Z')c+=32;return s;}
bool symbol(const std::string &s){if(s.empty()||s.size()>128)return false;for(size_t i=0;i<s.size();++i){char c=s[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||(i&&c>='0'&&c<='9')))return false;}return true;}
struct Guard {std::string expression;bool inverse=false;std::shared_ptr<const Guard> parent;};
struct Token {std::string raw;int width=0,line=0;std::shared_ptr<const Guard> guard;size_t position=0,begin=0,end=0;};
struct Body {std::string label;size_t start=0,end=0;int line=0;std::shared_ptr<const Guard> guard;};
int expression(std::string s,const PhaseOptions &o){s=lower(trim(s));bool inverse=false;while(!s.empty()){if(s[0]=='!'){inverse=!inverse;s=trim(s.substr(1));}else if(s.size()>1&&s.front()=='('&&s.back()==')')s=trim(s.substr(1,s.size()-2));else break;}int v=s=="stkphase"?(int)o.enabled:s=="wunit"?(int)o.wolf:s=="1"?1:s=="0"?0:-1;return v<0?v:inverse?!v:v;}
int enabled(const std::shared_ptr<const Guard> &g,const PhaseOptions &o){if(!g)return 1;int parent=enabled(g->parent,o);if(parent==0)return 0;int v=expression(g->expression,o);if(v==0&&g->inverse)v=1;else if(v==1&&g->inverse)v=0;if(v==0)return 0;return parent<0||v<0?-1:1;}
std::string conditions(std::shared_ptr<const Guard> g){std::string text;while(g){if(!text.empty())text=" AND "+text;text=(g->inverse?"NOT ":"")+g->expression+text;g=g->parent;}return text.empty()?"Unconditional":text;}
bool number(const Token &t,int64_t &out){size_t i=!t.raw.empty()&&(t.raw[0]=='-'||t.raw[0]=='+')?1:0;if(i>=t.raw.size()||t.raw[i]<'0'||t.raw[i]>'9')return false;if(!strike_numeric(t.raw,t.width==32?6:0,out))return false;if(t.width==32)out=(int32_t)out;else out=(int16_t)out;return true;}
bool take(const Token &t,const PhaseOptions &o,bool &include,std::string &error){int e=enabled(t.guard,o);if(e<0){error="Line "+std::to_string(t.line)+": cannot evaluate build condition ("+conditions(t.guard)+").";return false;}include=e!=0;return true;}
}
struct PhaseCatalog::Data {
    std::vector<Token> tokens;std::map<std::string,Body> bodies;
    std::vector<std::string> order;Token limit;
};
namespace {
const Body *body(const PhaseCatalog &c,const std::string &label){if(!c.data)return nullptr;auto it=c.data->bodies.find(lower(label));return it==c.data->bodies.end()?nullptr:&it->second;}
bool root(const PhaseCatalog &c,const std::string &label,const PhaseOptions &o,std::vector<Token> &out,std::string &error){auto b=body(c,label);if(!b){error="Table is not defined in this ASM: "+label;return false;}int active=enabled(b->guard,o);if(active!=1){error=active<0?"Table has an unresolved build condition: "+label:"Table is excluded by preview flags: "+label;return false;}
    for(size_t i=b->start;i<b->end;++i){auto &t=c.data->tokens[i];bool include;if(!take(t,o,include,error))return false;if(!include)continue;if(t.width!=32){error="Line "+std::to_string(t.line)+": expected a contiguous .long table.";return false;}out.push_back(t);if(out.size()>256){error="Table exceeds 256 entries.";return false;}}return true;}
std::string at_slot(const PhaseCatalog &c,int character,int slot,const PhaseOptions &o,std::string &table,std::string &error){auto chars=phase_characters(c,o,error);if(!error.empty())return {};if(character<0||character>=(int)chars.size()||slot<0||slot>255){error="Choose an available character and a strike slot from 0 to 255.";return {};}table=chars[character];auto b=body(c,table);if(!b){error="Strike table comes from another source/include: "+table;return {};}
    if(enabled(b->guard,o)!=1){error="Strike table is excluded or has an unresolved condition: "+table;return {};}
    int current=0;for(size_t i=b->start;i<c.data->tokens.size();++i){auto &t=c.data->tokens[i];bool include;if(!take(t,o,include,error))return {};if(!include)continue;
        int64_t n=0;bool null=number(t,n)&&n==0;auto key=lower(t.raw);
        if(t.width!=32||(!null&&key.compare(0,4,"stk_")!=0)||(!null&&!symbol(t.raw))){error="Strike slot is beyond the contiguous table, or an unsupported directive interrupts it.";return {};}
        if(current++==slot){if(null){error="Strike slot is null.";return {};}auto record=body(c,t.raw);if(!record){error="Strike record is defined outside this source: "+t.raw;return {};}if(enabled(record->guard,o)!=1){error="Strike record is excluded or has an unresolved condition: "+t.raw;return {};}return t.raw;}
    }error="Strike slot is outside this source table.";return {};
}
}
bool read_strike_phases(const std::string &text,PhaseCatalog &out,std::string &error){error.clear();if(text.size()>4*1024*1024||text.find('\0')!=text.npos){error="Phase inspection requires bounded ASM text.";return false;}
    auto d=std::make_shared<PhaseCatalog::Data>();std::shared_ptr<const Guard> guard;struct Frame {std::shared_ptr<const Guard> before;std::string expression;bool alternate=false;};std::vector<Frame> stack;
    auto dos_end=text.find('\x1a');if(dos_end!=text.npos)for(size_t i=dos_end+1;i<text.size();++i)if(text[i]!=' '&&text[i]!='\t'&&text[i]!='\r'&&text[i]!='\n'){error="Unexpected data after the DOS text EOF marker.";return false;}
    std::istringstream input(text.substr(0,dos_end));std::string raw;int line=0;size_t offset=0;
    while(std::getline(input,raw)){size_t line_start=offset;offset+=raw.size()+1;if(++line>100000||raw.size()>2048){error="ASM exceeds phase inspection line limits.";return false;}if(line==1&&raw.compare(0,3,"\xef\xbb\xbf")==0){raw.erase(0,3);line_start+=3;}raw.resize(std::min(raw.size(),raw.find(';')));auto work=trim(raw);if(work.empty()||work[0]=='*')continue;
        auto ws=work.find_first_of(" \t");auto first=work.substr(0,ws);std::string label;
        if((raw[0]!=' '&&raw[0]!='\t'&&first[0]!='.')||first.back()==':'){label=first;if(label.back()==':')label.pop_back();work=ws==work.npos?"":trim(work.substr(ws));}
        ws=work.find_first_of(" \t");auto directive=lower(work.substr(0,ws)),rest=ws==work.npos?"":trim(work.substr(ws));
        if(directive==".if"){if(stack.size()>=32||rest.size()>256||rest.empty()){error="Conditional nesting or expression exceeds phase limits.";return false;}stack.push_back({guard,rest,false});guard=std::make_shared<Guard>(Guard{rest,false,guard});continue;}
        if(directive==".elseif"||directive==".elif"){error="Unsupported conditional directive in phase inspection: "+directive;return false;}
        if(directive==".else"){if(stack.empty()||stack.back().alternate||!rest.empty()){error="Unmatched, repeated or malformed .else in ASM.";return false;}stack.back().alternate=true;guard=std::make_shared<Guard>(Guard{stack.back().expression,true,stack.back().before});continue;}
        if(directive==".endif"){if(stack.empty()||!rest.empty()){error="Unmatched or malformed .endif in ASM.";return false;}guard=stack.back().before;stack.pop_back();continue;}
        if(directive==".set"){if(lower(label)=="phase_max"){if(!d->limit.raw.empty()){error="Multiple PHASE_MAX declarations cannot be resolved.";return false;}d->limit={rest,32,line,guard};}continue;}
        if(!label.empty()){if(!symbol(label)||d->bodies.count(lower(label))||d->order.size()>=16384){error="Unsupported or duplicate ASM label: "+label;return false;}d->bodies[lower(label)]={label,d->tokens.size(),0,line,guard};d->order.push_back(lower(label));}
        if(work.empty())continue;
        if(directive==".long"||directive==".word"){size_t start=0,count=0;do{auto comma=rest.find(',',start);auto token=trim(rest.substr(start,comma==rest.npos?rest.npos:comma-start));if(token.empty()||token.size()>128||++count>256||d->tokens.size()>=200000){error="ASM operands exceed phase inspection limits.";return false;}size_t begin=line_start+raw.rfind(rest)+start;while(begin<text.size()&&(text[begin]==' '||text[begin]=='\t'))++begin;d->tokens.push_back({token,directive==".long"?32:16,line,guard,0,begin,begin+token.size()});if(comma==rest.npos)break;start=comma+1;}while(true);}
        else if(directive!=".ref"&&directive!=".global"&&directive!=".def"&&directive!=".bss"&&directive!=".emsg"&&directive!=".file"&&directive!=".width"&&directive!=".option"&&directive!=".mnolist"){
            if(d->tokens.size()>=200000){error="Too many ASM tokens.";return false;}d->tokens.push_back({work,0,line,guard});}
    }
    if(!stack.empty()){error="Unclosed ASM build condition.";return false;}
    for(size_t i=0;i<d->tokens.size();++i)d->tokens[i].position=i+1;
    // Consecutive aliases share a body; inline slot labels do not end table lookup.
    size_t end=d->tokens.size();for(auto it=d->order.rbegin();it!=d->order.rend();++it){auto &b=d->bodies[*it];if(b.start<end){b.end=end;end=b.start;}else b.end=end;}
    for(size_t i=0;i<d->order.size();++i){auto &b=d->bodies[d->order[i]];if(b.start==b.end){size_t j=i+1;while(j<d->order.size()&&d->bodies[d->order[j]].start==b.start)++j;b.end=j<d->order.size()?d->bodies[d->order[j]].start:d->tokens.size();}}
    PhaseCatalog c;c.data=d;int64_t max=0;c.limit_known=number(d->limit,max)&&max>=1&&max<=64;c.scan_limit=c.limit_known?(int)max:0;
    auto tab=body(c,"stk_phase_tab");std::set<std::string> directories,lists;size_t expanded=0;
    if(tab&&tab->end-tab->start>256){error="Phase character table exceeds 256 source entries.";return false;}
    if(tab)for(size_t i=tab->start;i<tab->end;++i)if(d->tokens[i].width==32&&symbol(d->tokens[i].raw))directories.insert(lower(d->tokens[i].raw));
    for(auto &name:directories){auto b=body(c,name);if(!b)continue;for(size_t i=b->start;i+1<b->end;i+=2){expanded+=2;if(expanded>65536){error="Expanded phase catalog exceeds 65536 tokens.";return false;}if(d->tokens[i].width!=32||d->tokens[i+1].width!=32)break;int64_t n;if(number(d->tokens[i],n)&&n==-1)break;if(symbol(d->tokens[i+1].raw))lists.insert(lower(d->tokens[i+1].raw));}}
    if(lists.size()>1024){error="Source declares more than 1024 phase lists.";return false;}
    for(auto &name:lists){auto b=body(c,name);PhaseList list;list.label=b?b->label:name;if(!b){list.error="Phase list is defined outside this source.";c.lists.push_back(std::move(list));continue;}list.line=b->line;list.conditions=conditions(b->guard);
        std::vector<Token> words;for(size_t i=b->start;i<b->end;++i){if(++expanded>65536){error="Expanded phase catalog exceeds 65536 tokens.";return false;}auto &t=d->tokens[i];if(t.width!=16||words.size()>=128){list.error="Phase list is not bounded contiguous .word pairs.";break;}words.push_back(t);}
        if(words.size()%2)list.error="Phase list ends with an incomplete word pair.";
        for(size_t i=0;i+1<words.size();i+=2){int64_t threshold=0,slot=0;bool numeric=number(words[i],threshold)&&number(words[i+1],slot);list.entries.push_back({words[i].raw,words[i+1].raw,(int)threshold,(int)slot,words[i].line,numeric,words[i].position});}
        if(list.entries.empty()&&list.error.empty())list.error="Phase list has no word pairs.";c.lists.push_back(std::move(list));
    }
    out=std::move(c);return true;
}
std::vector<std::string> phase_characters(const PhaseCatalog &c,const PhaseOptions &o,std::string &error){error.clear();std::vector<Token> tokens;if(!root(c,"strike_tables",o,tokens,error))return {};std::vector<std::string> chars;
    for(auto &t:tokens){if(!symbol(t.raw)){error="Character strike table uses an unsupported pointer: "+t.raw;return {};}chars.push_back(t.raw);}return chars;}
std::string phase_slot_record(const PhaseCatalog &c,int character,int slot,const PhaseOptions &o,std::string &error){error.clear();std::string table;return at_slot(c,character,slot,o,table,error);}
PhasePick pick_strike_phase(const PhaseCatalog &c,int character,int base_slot,int remaining,const PhaseOptions &o){PhasePick p;if(remaining<0||remaining>32767){p.error="Countdown must be between 0 and 32767.";return p;}
    p.base_record=at_slot(c,character,base_slot,o,p.character_table,p.error);if(!p.error.empty())return p;p.record=p.base_record;p.slot=base_slot;p.valid=true;p.active=remaining>0;
    if(!p.active){p.note="No active tick: striker exits when the countdown reaches zero.";return p;}if(!o.enabled){p.note="Phase preview disabled: use the base strike.";return p;}
    if(!c.limit_known||enabled(c.data->limit.guard,o)!=1){p.valid=false;p.error="PHASE_MAX is unavailable or not a supported literal (1–64).";return p;}
    std::vector<Token> directory;if(!root(c,"stk_phase_tab",o,directory,p.error)){p.valid=false;return p;}if(character<0||character>=(int)directory.size()){p.valid=false;p.error="Character has no phase-table slot.";return p;}
    int64_t n;if(number(directory[character],n)&&n==0){p.note="No phase directory: use the base strike.";return p;}p.directory=directory[character].raw;std::vector<Token> entries;if(!root(c,p.directory,o,entries,p.error)){p.valid=false;return p;}
    size_t cursor=0;for(int scan=0;scan<c.scan_limit;++scan){if(cursor>=entries.size()){p.valid=false;p.error="Phase directory lacks its -1 terminator.";return p;}if(!number(entries[cursor],n)){p.valid=false;p.error="Phase move index is symbolic or invalid.";return p;}if(n==-1){p.note="Move has no phase list: use the base strike.";return p;}
        if(n<0||n>32767||cursor+1>=entries.size()){p.valid=false;p.error="Invalid or incomplete phase-directory entry.";return p;}auto target=entries[cursor+1];cursor+=2;if(n!=base_slot)continue;
        int64_t null;if(number(target,null)&&null==0){p.note="Null phase list: use the base strike.";return p;}p.list=target.raw;auto list=body(c,p.list);if(!list||enabled(list->guard,o)!=1){p.valid=false;p.error="Phase list is unavailable or excluded: "+p.list;return p;}p.list=list->label;
        std::vector<Token> words;for(size_t i=list->start;i<list->end;++i){auto &t=c.data->tokens[i];bool include;if(!take(t,o,include,p.error)){p.valid=false;return p;}if(!include)continue;if(t.width!=16){p.valid=false;p.error="Phase list must be contiguous .word pairs.";return p;}words.push_back(t);}
        for(int row=0;row<c.scan_limit;++row){if(row*2+1>=(int)words.size()){p.valid=false;p.error="Phase scan reaches the end of source data without a match.";return p;}int64_t threshold,slot;if(!number(words[row*2],threshold)||!number(words[row*2+1],slot)){p.valid=false;p.error="Phase word pair is symbolic or invalid.";return p;}
            if(remaining<threshold)continue;if(slot<0||slot>255){p.valid=false;p.error="Selected phase strike slot is outside supported bounds.";return p;}p.slot=(int)slot;p.row=row;p.line=words[row*2].line;p.position=words[row*2].position;p.record=at_slot(c,character,p.slot,o,p.character_table,p.error);p.valid=p.error.empty();p.phased=p.valid;p.note="Selected by countdown >= threshold.";return p;}
        p.fallback=true;p.note="PHASE_MAX reached without a matching threshold: engine uses the base strike.";return p;
    }p.fallback=true;p.note="PHASE_MAX reached without the move: engine uses the base strike.";return p;
}
bool first_strike_phase(const PhaseCatalog &c,int &character,int &slot){PhaseOptions o;o.enabled=true;std::string error;std::vector<Token> chars;if(!root(c,"stk_phase_tab",o,chars,error))return false;
    for(int i=0;i<(int)chars.size();++i){if(!symbol(chars[i].raw))continue;std::vector<Token> dir;if(!root(c,chars[i].raw,o,dir,error)||dir.empty())continue;int64_t n;if(number(dir[0],n)&&n>=0&&n<=255){character=i;slot=(int)n;return true;}}return false;
}
bool review_phase_edit(const StrikeSource &source,const std::string &label,const std::vector<std::array<std::string,2>> &values,const PhaseOptions &o,PhasePatch &out,std::string &error){
    error.clear();PhaseCatalog c;if(!read_strike_phases(source.bytes(),c,error))return false;
    auto fail=[&](const std::string &message){error=message;return false;};
    if(!o.enabled)return fail("Enable the stkphase preview assumption before reviewing phase edits. Build settings are unchanged.");
    if(!c.limit_known||enabled(c.data->limit.guard,o)!=1)return fail("PHASE_MAX must resolve to a literal from 1 to 64.");
    auto b=body(c,label);if(!b||enabled(b->guard,o)!=1)return fail("Choose a phase list available for these preview flags.");
    auto declared=std::find_if(c.lists.begin(),c.lists.end(),[&](const PhaseList &list){auto target=body(c,list.label);return target&&target->start==b->start&&target->end==b->end;});
    if(declared==c.lists.end()||!declared->error.empty())return fail("Choose a declared, contiguous phase list.");
    size_t count=b->end-b->start;if(!count||count%2||values.size()!=count/2||values.size()>(size_t)c.scan_limit)return fail("Keep the existing row count within PHASE_MAX. Adding or removing rows is not supported.");
    PhasePatch p;p.owner=&source;p.generation=source.generation();p.options=o;p.label=b->label;p.before=source.bytes();p.after=p.before;
    int previous=32768;std::vector<int> slots;
    for(size_t row=0;row<values.size();++row){std::array<std::string,2> pair;
        for(int field=0;field<2;++field){auto &t=c.data->tokens[b->start+row*2+field];int64_t old;
            if(t.width!=16||t.guard!=b->guard||!number(t,old)||t.begin>=t.end||t.end>p.before.size()||p.before.substr(t.begin,t.end-t.begin)!=t.raw)return fail("Conditional, symbolic or irregular phase rows cannot be edited safely.");
            pair[field]=trim(values[row][field]);Token candidate;candidate.raw=pair[field];candidate.width=16;int64_t n;
            if(!number(candidate,n)||n<0||n>(field?255:32767))return fail("Use numeric thresholds from 0 to 32767 and strike slots from 0 to 255.");
            if(!field){if(n>=previous)return fail("Thresholds must decrease strictly from top to bottom.");previous=(int)n;}else slots.push_back((int)n);
        }p.operands.push_back(std::move(pair));
    }
    if(previous!=0)return fail("The last threshold must be zero so every active countdown has a matching row.");
    std::vector<Token> directories;if(!root(c,"stk_phase_tab",o,directories,error))return false;
    for(int character=0;character<(int)directories.size();++character){int64_t null;if(number(directories[character],null)&&null==0)continue;
        std::vector<Token> moves;if(!root(c,directories[character].raw,o,moves,error))return false;size_t cursor=0;bool terminated=false;
        for(int scan=0;scan<c.scan_limit;++scan){if(cursor>=moves.size())return fail("A phase directory is missing its bounded terminator.");int64_t move;
            if(!number(moves[cursor],move))return fail("A phase directory has an unresolved move index.");if(move==-1){terminated=true;break;}
            if(move<0||move>255||cursor+1>=moves.size())return fail("A phase directory entry is invalid or incomplete.");auto target=body(c,moves[cursor+1].raw);cursor+=2;
            if(!target||target->start!=b->start||target->end!=b->end)continue;
            if(enabled(target->guard,o)!=1)return fail("An affected phase-list alias is excluded or unresolved for these preview flags.");
            std::string table;auto base=at_slot(c,character,(int)move,o,table,error);if(base.empty())return false;
            for(auto slot:slots){auto record=at_slot(c,character,slot,o,table,error);if(record.empty())return false;
                auto editable=std::find_if(source.records().begin(),source.records().end(),[&](const StrikeRow &r){return r.editable&&(lower(r.label)==lower(record)||std::any_of(r.aliases.begin(),r.aliases.end(),[&](const std::string &alias){return lower(alias)==lower(record);}));});
                if(editable==source.records().end())return fail("An affected strike record is incomplete or unsupported: "+record);
            }p.consumers.push_back("["+std::to_string(character)+"] "+table+" / base slot "+std::to_string(move));
        }
        if(!terminated&&cursor<moves.size()){int64_t sentinel;if(number(moves[cursor],sentinel)&&sentinel==-1)terminated=true;}
        if(!terminated)return fail("A phase directory exceeds the supported bounded scan.");
    }
    if(p.consumers.empty())return fail("This phase list has no resolved consumer for the selected preview flags.");
    for(size_t row=values.size();row-->0;)for(int field=1;field>=0;--field){auto &t=c.data->tokens[b->start+row*2+field];p.after.replace(t.begin,t.end-t.begin,p.operands[row][field]);}
    PhaseCatalog candidate;if(!read_strike_phases(p.after,candidate,error))return false;
    p.changed=p.before!=p.after;out=std::move(p);return true;
}
}
