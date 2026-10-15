#include "studio_asm.h"
#include "studio_strikes.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <cctype>
namespace studio {
namespace {
std::string trim(const std::string &s){auto a=s.find_first_not_of(" \t\r\n");if(a==s.npos)return {};return s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);}
std::string key(std::string s){for(auto &c:s)c=(char)std::toupper((unsigned char)c);return s;}
bool symbol(const std::string &s){if(s.empty()||!(std::isalpha((unsigned char)s[0])||s[0]=='_'))return false;for(auto c:s)if(!std::isalnum((unsigned char)c)&&c!='_')return false;return true;}
struct Token {std::string text;int line=0,width=0;};
struct Body {std::vector<Token> tokens;};
bool zero(const std::string &s){int64_t n;return s=="ani_end"||(strike_numeric(s,6,n)&&n==0);}
std::string at(const Token &t,const std::string &s){return "Line "+std::to_string(t.line)+": "+s;}
}
std::unique_ptr<AsmSource> AsmSource::open(const std::string &path,std::string &error){
    try{auto p=std::filesystem::u8path(path);std::ifstream in(p,std::ios::binary|std::ios::ate);if(!in){error="Cannot open animation ASM.";return {};}
        auto size=in.tellg();if(size<0||size>4*1024*1024){error="Animation source exceeds 4 MiB.";return {};}
        std::string bytes((size_t)size,'\0');in.seekg(0);if(!bytes.empty()&&!in.read(&bytes[0],bytes.size())){error="Cannot read animation ASM.";return {};}
        auto result=parse(bytes,error);if(result)result->path=std::filesystem::absolute(p).u8string();return result;
    }catch(const std::exception &e){error=e.what();return {};}
}
std::unique_ptr<AsmSource> AsmSource::parse(const std::string &text,std::string &error){
    error.clear();if(text.size()>4*1024*1024||text.find('\0')!=text.npos){error="Animation source is oversized or contains NUL bytes.";return {};}
    std::unordered_map<std::string,std::shared_ptr<Body>> bodies;std::vector<std::string> order,tables;
    std::unordered_map<std::string,std::string> comments;std::shared_ptr<Body> body;bool has_data=false;int line=0;size_t tokens=0;
    std::istringstream input(text);std::string raw;
    while(std::getline(input,raw)){
        if(++line>100000||raw.size()>2048){error="Animation source exceeds line limits.";return {};}
        if(line==1&&raw.compare(0,3,"\xef\xbb\xbf")==0)raw.erase(0,3);
        std::string comment;auto sc=raw.find(';');if(sc!=raw.npos){comment=trim(raw.substr(sc+1));raw.resize(sc);}auto work=trim(raw);
        if(work.empty()||work[0]=='*')continue;
        std::string label;bool labeled=raw[0]!=' '&&raw[0]!='\t';auto ws=work.find_first_of(" \t");auto first=work.substr(0,ws);
        if((labeled&&first[0]!='.')||first.back()==':'){
            label=first;if(label.back()==':')label.pop_back();if(!symbol(label)){body.reset();has_data=true;continue;}
            if(bodies.count(label)){error="Duplicate ASM label: "+label;return {};}
            if(!body||has_data){body=std::make_shared<Body>();has_data=false;}bodies[label]=body;order.push_back(label);
            if(key(label).find("ANITAB")!=std::string::npos)tables.push_back(label);
            work=ws==work.npos?"":trim(work.substr(ws));
        }
        if(work.empty())continue;ws=work.find_first_of(" \t");auto directive=key(work.substr(0,ws));
        auto rest=ws==work.npos?"":trim(work.substr(ws));
        if(!body)continue;
        if(directive!=".LONG"&&directive!=".WORD"){
            // Never turn an instruction/conditional into image data or join aliases across it.
            body->tokens.push_back({"!"+work,line,0});has_data=true;continue;
        }
        has_data=true;std::vector<std::string> operands;size_t start=0;
        do{auto comma=rest.find(',',start);auto op=trim(rest.substr(start,comma==rest.npos?rest.npos:comma-start));if(op.empty()){error=at({"",line,0},"Empty ASM operand.");return {};}
            operands.push_back(op);if(comma==rest.npos)break;start=comma+1;}while(true);
        for(auto &op:operands){if(++tokens>200000){error="Too many animation operands.";return {};}body->tokens.push_back({op,line,directive==".LONG"?32:16});}
        if(operands.size()==1&&!comment.empty())comments[operands[0]]=comment;
    }
    std::vector<std::string> labels;std::unordered_set<std::string> seen;
    auto add=[&](const std::string &s){if(symbol(s)&&bodies.count(s)&&s.find("_anipts")==s.npos&&seen.insert(s).second)labels.push_back(s);};
    for(auto &table:tables)for(auto &t:bodies[table]->tokens)if(t.width==32)add(t.text);
    for(auto &s:order)if(s.compare(0,2,"a_")==0)add(s);
    if(labels.size()>4096){error="Too many animation labels.";return {};}
    auto result=std::make_unique<AsmSource>();size_t total_frames=0,total_pieces=0;
    for(auto &label:labels){AsmSourceAnimation a;a.label=label;a.name=comments.count(label)?comments[label]:label;
        auto current=bodies[label];std::unordered_set<const Body*> visited;int dx=0,dy=0,segment=0;bool flip=false,flip_y=false,no_sleep=false;
        bool stop=false;while(current&&!stop){if(!visited.insert(current.get()).second){a.notices.push_back("Jump repeats an earlier source block; traversal stops here.");break;}
            for(size_t i=0;i<current->tokens.size()&&!stop;++i){auto t=current->tokens[i];auto op=t.text;
                auto issue=[&](const std::string &s){if(a.notices.size()<128)a.notices.push_back(at(t,s));a.partial=true;};
                if(no_sleep&&(t.width!=32||zero(op)||op.compare(0,4,"ani_")==0)){issue("No-sleep command requires a frame operand.");stop=true;break;}
                if(zero(op)&&t.width==32){++segment;continue;}
                if(t.width!=32){issue("Unexpected word operand; traversal stops.");stop=true;break;}
                if(op=="ani_jump"){
                    if(i+1>=current->tokens.size()||current->tokens[i+1].width!=32){issue("Missing long jump target.");stop=true;break;}
                    auto target=current->tokens[++i].text;auto found=bodies.find(target);if(found==bodies.end()){issue("Jump target is outside this source: "+target);stop=true;break;}
                    a.notices.push_back(at(t,"Jump to "+target));current=found->second;goto next_body;
                }
                if(op=="ani_flip"){flip=!flip;continue;}if(op=="ani_flip_v"){flip_y=!flip_y;continue;}
                if(op=="ani_nosleep"){no_sleep=true;continue;}
                if(op=="ani_adjustx"||op=="ani_adjustxy"){
                    int count=op=="ani_adjustx"?1:2;int value[2]={};bool ok=i+count<current->tokens.size();
                    for(int j=0;j<count&&ok;++j){auto &arg=current->tokens[i+1+j];int64_t n;ok=arg.width==16&&strike_numeric(arg.text,0,n);if(ok)value[j]=(int)(int16_t)n;}
                    if(!ok){issue("Invalid or symbolic adjustment; traversal stops.");stop=true;break;}
                    int64_t nx=(int64_t)dx+(flip?-value[0]:value[0]),ny=(int64_t)dy+value[1];
                    if(nx<-32768||nx>32767||ny<-32768||ny>32767){issue("Cumulative adjustment exceeds signed coordinates.");stop=true;break;}
                    dx=(int)nx;dy=(int)ny;i+=count;continue;
                }
                if(op=="ani_sound"||op=="ani_calla"){
                    if(i+1>=current->tokens.size()||current->tokens[i+1].width!=32){issue("Missing long command operand.");stop=true;break;}
                    auto arg=current->tokens[++i].text;
                    if(op=="ani_calla")issue("Runtime call not executed: "+arg);else a.notices.push_back(at(t,"Sound: "+arg));continue;
                }
                if(t.width!=32||!symbol(op)||op.compare(0,4,"ani_")==0||op.compare(0,2,"a_")==0||op[0]=='_'){
                    issue("Unsupported command or operand: "+op+". Traversal stops.");stop=true;break;
                }
                AsmSourceFrame f;f.name=op;f.line=t.line;f.dx=dx;f.dy=dy;f.segment=segment;f.flip=flip;f.flip_y=flip_y;f.no_sleep=no_sleep;no_sleep=false;
                auto found=bodies.find(op);bool valid=true;
                if(found==bodies.end())f.pieces.push_back(op);
                else{bool terminated=false;for(auto &piece:found->second->tokens){if(zero(piece.text)){terminated=true;break;}if(piece.width!=32||!symbol(piece.text)||piece.text.compare(0,4,"ani_")==0){valid=false;break;}f.pieces.push_back(piece.text);if(f.pieces.size()>256){valid=false;break;}}
                    if(!terminated)valid=false;}
                if(!valid){issue("Invalid multipart frame: "+op);stop=true;break;}
                if(++total_frames>32768||(total_pieces+=f.pieces.size())>262144){error="Expanded animation exceeds frame/piece limits.";return {};}
                a.frames.push_back(std::move(f));
            }
            if(no_sleep){a.partial=true;a.notices.push_back("No-sleep command has no frame operand.");}
            break;
            next_body: continue;
        }
        auto local=bodies.find(label+"_local_anipts");if(local!=bodies.end()){
            auto &ts=local->second->tokens;bool ok=ts.size()==a.frames.size()*2;std::vector<int> values;
            for(auto &t:ts){int64_t n;bool numeric=t.width==16&&strike_numeric(t.text,0,n);ok&=numeric;if(numeric)values.push_back((int)(int16_t)n);}
            if(ok)for(size_t i=0;i<a.frames.size();++i){a.frames[i].dx=values[i*2];a.frames[i].dy=values[i*2+1];}
            else{a.partial=true;a.notices.push_back("Local anchor table has invalid values or a different frame count; not applied.");}
        }
        result->animations.push_back(std::move(a));
    }
    if(result->animations.empty()){error="No defined animation labels or anitab entries found.";return {};}
    return result;
}
std::vector<AsmPiece> resolve_asm_frame(const AsmSourceFrame &f,const std::vector<AsmLibrary> &libs,uint64_t preferred){
    std::vector<AsmPiece> out;for(auto &s:f.pieces){AsmPiece p;p.symbol=s;int count=0;bool preferred_found=false;
        for(auto &lib:libs)if(lib.art)for(int i=0;i<lib.art->count();++i)if(key(lib.art->sprite(i)->n_s)==key(s)){
            bool favored=lib.uid==preferred;if(favored&&!preferred_found){count=0;preferred_found=true;}if(preferred_found&&!favored)continue;
            ++count;p.doc=lib.uid;p.sprite=lib.art->sprite_id(i);
        }
        if(count!=1){p.doc=p.sprite=0;p.ambiguous=count>1;}out.push_back(std::move(p));}return out;
}
SpritePlacement place_asm_piece(const IMG &im,const AsmSourceFrame &f,int x,int y,bool facing,MirrorConvention convention){
    auto p=place_sprite(im,x+(facing?-f.dx:f.dx),y+f.dy,facing!=f.flip,convention);
    if(f.flip_y)p.y=y+f.dy-anipoint_effective((short)im.aniy,im.h,true,convention);return p;
}
}
