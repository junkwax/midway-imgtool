#include "studio_strikes.h"
#include "studio_strike_phases.h"
#include "studio_file_io.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <set>
#include <map>
#include <algorithm>
#include <iomanip>
namespace studio {
namespace {
namespace fs=std::filesystem;
std::string trim(const std::string &s){auto b=s.find_first_not_of(" \t\r"),e=s.find_last_not_of(" \t\r");return b==std::string::npos?"":s.substr(b,e-b+1);}
std::string lower(std::string s){for(char &c:s)if(c>='A'&&c<='Z')c+=32;return s;}
bool read(const std::string &path,std::string &bytes,std::string &error){
    std::ifstream in(fs::u8path(path),std::ios::binary|std::ios::ate);auto n=in?in.tellg():std::streampos(-1);
    if(n<0||n>4*1024*1024){error="Cannot read strike source (4 MiB limit).";return false;}bytes.resize((size_t)n);in.seekg(0);
    if(n&&!in.read(bytes.data(),n)){error="Could not finish reading the source file.";return false;}return true;
}
bool operand_ok(const std::string &s){if(s.empty()||s.size()>128)return false;for(unsigned char c:s)if(c<32||c>=127||c==';'||c==','||c==':'||c=='"'||c=='\'')return false;return true;}
bool same_path(const fs::path &a,const fs::path &b){return lower(a.lexically_normal().u8string())==lower(b.lexically_normal().u8string());}
}
bool strike_numeric(const std::string &raw,int field,int64_t &value){
    auto s=trim(raw);if(s.empty())return false;bool negative=false;if(s[0]=='-'||s[0]=='+'){negative=s[0]=='-';s.erase(0,1);}if(s.empty())return false;
    int base=10;if(s.back()=='h'||s.back()=='H'){base=16;s.pop_back();}else if(s.size()>2&&s[0]=='0'&&(s[1]=='x'||s[1]=='X')){base=16;s.erase(0,2);}if(s.empty())return false;
    for(char c:s)if(!((c>='0'&&c<='9')||(base==16&&((c>='a'&&c<='f')||(c>='A'&&c<='F')))))return false;
    try{size_t used=0;auto n=std::stoll(s,&used,base);if(negative)n=-n;int64_t lo=field==mk2::F_SCORE?-2147483648ll:-32768,hi=field==mk2::F_SCORE?4294967295ll:65535;
        if(used!=s.size()||n<lo||n>hi)return false;value=n;return true;}catch(...){return false;}
}
std::string strike_literal(const std::string &raw,int field,int64_t value){
    auto s=trim(raw);bool suffix=!s.empty()&&(s.back()=='h'||s.back()=='H'),prefix=s.size()>1&&s[0]=='0'&&(s[1]=='x'||s[1]=='X');
    if(!suffix&&!prefix)return std::to_string(value);uint64_t bits=(uint64_t)value&(field==mk2::F_SCORE?0xffffffffull:0xffffull);std::ostringstream out;
    if((suffix&&s.back()=='H')||(prefix&&s[1]=='X'))out<<std::uppercase;out<<(prefix?s.substr(0,2):"0")<<std::hex<<std::setfill('0')<<std::setw(field==mk2::F_SCORE?8:4)<<bits;if(suffix)out<<s.back();return out.str();
}
bool strike_rect(const std::array<StrikeOperand,8> &f,int x,int y,bool flip,StrikeRect &out){
    if(x<-4096||x>4096||y<-4096||y>4096)return false;
    for(int i=0;i<4;++i)if(!f[i].numeric)return false;int hx=(short)f[0].value,hy=(short)f[1].value,w=(short)f[2].value,h=(short)f[3].value;if(w<0||h<0)return false;
    // JOY.ASM strike_check: normal = origin + x_offset - width;
    // flipped = origin - x_offset; Y always adds its signed offset.
    out={x+(flip?-hx:hx-w),y+hy,w,h};return true;
}
bool StrikeSource::parse(const std::string &bytes,std::string &error){
    error.clear();if(bytes.size()>4*1024*1024||bytes.find('\0')!=std::string::npos){error="Strike source must be bounded assembly text.";return false;}
    std::vector<std::string> lines;std::vector<size_t> offsets;size_t start=0;
    while(start<bytes.size()){size_t end=bytes.find('\n',start);if(end==std::string::npos)end=bytes.size();auto line=bytes.substr(start,end-start);if(line.size()>2048||lines.size()>=100000){error="Strike source exceeds the line limits.";return false;}
        if(!line.empty()&&line.back()=='\r')line.pop_back();offsets.push_back(start);lines.push_back(line);start=end==bytes.size()?end:end+1;}
    std::string canonical;for(size_t i=0;i<lines.size();++i){auto line=lines[i];if(i==0&&line.size()>=3&&(unsigned char)line[0]==0xef&&(unsigned char)line[1]==0xbb&&(unsigned char)line[2]==0xbf)line.erase(0,3);
        auto code=trim(line.substr(0,line.find(';')));if(!code.empty()&&code.back()==':')code.pop_back();
        if(code.rfind("stk_",0)==0&&code.find_first_of(" \t") == std::string::npos)canonical+=code;else canonical+=line;canonical+='\n';}
    mk2::Document parsed;if(!mk2::parse_text(&parsed,canonical,&error))return false;
    if(parsed.records.size()>4096){error="The source exceeds the 4096-strike-record limit.";return false;}
    std::vector<StrikeRow> next;std::vector<std::string> warnings;std::set<std::string> labels;
    for(auto &r:parsed.records){StrikeRow row;row.label=r.label;row.line=r.label_line;row.editable=true;
        if(!labels.insert(lower(row.label)).second){error="Duplicate strike label: "+row.label;return false;}
        for(int i=0;i<8;++i){int li=r.fields[i].line-1;if(li<0||li>=(int)lines.size()){row.editable=false;row.error="Incomplete eight-field strike record.";continue;}
            auto &line=lines[li];size_t first=line.find_first_not_of(" \t"),space=first==std::string::npos?std::string::npos:line.find_first_of(" \t",first);
            auto directive=space==std::string::npos?"":lower(line.substr(first,space-first));if(directive!=(i==mk2::F_SCORE?".long":".word")){row.editable=false;row.error="Record directives do not match the eight-field strike layout.";continue;}
            size_t b=line.find_first_not_of(" \t",space),e=line.find(';',space);if(e==std::string::npos)e=line.size();while(e>b&&(line[e-1]==' '||line[e-1]=='\t'))--e;
            if(b==std::string::npos||e<=b){row.editable=false;row.error="Missing strike operand.";continue;}
            row.begin[i]=offsets[li]+b;row.end[i]=offsets[li]+e;auto &f=row.fields[i];f.raw=line.substr(b,e-b);f.numeric=strike_numeric(f.raw,i,f.value);
            if(!operand_ok(f.raw)){row.editable=false;row.error="This record uses unsupported multi-value or unusual operands.";}
        }
        if(row.editable){int last=r.fields[7].line;for(int li=row.line;li<last;++li){auto code=trim(lines[li].substr(0,lines[li].find(';')));
            if(code.empty()||code[0]=='*'||code.rfind("stk_",0)==0||lower(code).rfind(".word",0)==0||lower(code).rfind(".long",0)==0)continue;
            row.editable=false;row.error="This record crosses code, conditional directives or an unrelated label.";break;}}
        if(!row.editable)warnings.push_back(row.label+": "+row.error);next.push_back(std::move(row));
    }
    if(next.empty()){error="No stk_* strike records were found in this source.";return false;}
    std::map<std::array<size_t,8>,std::vector<std::string>> aliases;for(auto &r:next)if(r.editable)aliases[r.begin].push_back(r.label);
    for(auto &r:next)if(r.editable)r.aliases=aliases[r.begin];
    // Extended fighter names are valid too; the classic UI parser only lists two-letter names.
    std::vector<mk2::CharTable> chars;mk2::CharTable table;bool active=false;
    static const std::regex table_label(R"(^([A-Za-z][A-Za-z0-9_]*)_strikes:?$)"),move_line(R"(^\.(?:long|LONG)\s+(stk_[A-Za-z0-9_]+)$)");
    for(size_t i=0;i<lines.size();++i){auto code=trim(lines[i].substr(0,lines[i].find(';')));std::smatch match;
        if(std::regex_match(code,match,table_label)){if(active)chars.push_back(table);table={};table.name=match[1];table.line=(int)i+1;active=true;}
        else if(active&&std::regex_match(code,match,move_line))table.moves.push_back(match[1]);
        else if(active&&!code.empty()&&code[0]!='*'){chars.push_back(table);table={};active=false;}}
    if(active)chars.push_back(table);rows=std::move(next);tables=std::move(chars);notices=std::move(warnings);return true;
}
std::unique_ptr<StrikeSource> StrikeSource::open(const std::string &path,std::string &error){
    std::string bytes;if(!read(path,bytes,error))return {};auto source=std::make_unique<StrikeSource>();if(!source->parse(bytes,error))return {};
    std::error_code ec;source->source_path=fs::absolute(fs::u8path(path),ec).lexically_normal().u8string();if(ec){error=ec.message();return {};}
    source->text=source->baseline=std::move(bytes);return source;
}
bool StrikeSource::review(int index,const std::array<std::string,8> &values,StrikePatch &out,std::string &error) const {
    error.clear();if(index<0||index>=(int)rows.size()||!rows[index].editable){error="Choose a complete, editable strike record.";return false;}auto &r=rows[index];StrikePatch p;p.owner=this;p.generation=serial;p.record=index;p.before=text;p.after=text;p.aliases=r.aliases;
    for(int i=0;i<8;++i){p.operands[i]=trim(values[i]);if(p.operands[i]==r.fields[i].raw)continue;
        int64_t n=0;bool numeric=strike_numeric(p.operands[i],i,n);if(!operand_ok(p.operands[i])||((i<4||i==mk2::F_DAMAGE||i==mk2::F_SCORE)&&!numeric)){error="Invalid or out-of-range operand for "+std::string(mk2::kFieldNames[i])+".";return false;}
        if((i==mk2::F_X_SIZE||i==mk2::F_Y_SIZE)&&((short)n)<0){error="Strike box sizes must be 0–32767.";return false;}
        // Numeric-looking malformed/overflowing routine/sound values must not masquerade as symbols.
        if(!numeric&&(p.operands[i][0]=='-'||p.operands[i][0]=='+'||(p.operands[i][0]>='0'&&p.operands[i][0]<='9'))){error="Invalid numeric strike operand.";return false;}
    }
    for(int i=7;i>=0;--i)if(p.operands[i]!=r.fields[i].raw)p.after.replace(r.begin[i],r.end[i]-r.begin[i],p.operands[i]);
    p.changed=p.before!=p.after;out=std::move(p);return true;
}
bool StrikeSource::apply(const StrikePatch &p,std::string &error){
    if(p.owner!=this||p.generation!=serial||p.before!=text){error="The strike source changed. Rebuild the preview.";return false;}StrikePatch rebuilt;
    if(!review(p.record,p.operands,rebuilt,error))return false;if(rebuilt.after!=p.after||rebuilt.aliases!=p.aliases||rebuilt.changed!=p.changed){error="The source patch no longer matches its preview.";return false;}
    return commit_bytes(p.after,error);
}
bool StrikeSource::apply_phase(const PhasePatch &p,std::string &error){
    if(p.owner!=this||p.generation!=serial||p.before!=text){error="The strike source changed. Rebuild the phase preview.";return false;}PhasePatch rebuilt;
    if(!review_phase_edit(*this,p.label,p.operands,p.options,rebuilt,error))return false;
    if(rebuilt.after!=p.after||rebuilt.consumers!=p.consumers||rebuilt.changed!=p.changed){error="The phase patch no longer matches its preview.";return false;}
    return commit_bytes(p.after,error);
}
bool StrikeSource::commit_bytes(const std::string &bytes,std::string &error){
    if(bytes==text)return true;StrikeSource checked;if(!checked.parse(bytes,error))return false;
    past.push_back(text);future.clear();size_t size=0;for(auto &s:past)size+=s.size();while(past.size()>1&&(past.size()>100||size>64*1024*1024)){size-=past.front().size();past.erase(past.begin());}
    text=bytes;rows=std::move(checked.rows);tables=std::move(checked.tables);notices=std::move(checked.notices);++serial;return true;
}
void StrikeSource::undo(){if(past.empty())return;std::string error;if(!parse(past.back(),error))return;future.push_back(text);text=std::move(past.back());past.pop_back();++serial;}
void StrikeSource::redo(){if(future.empty())return;std::string error;if(!parse(future.back(),error))return;past.push_back(text);text=std::move(future.back());future.pop_back();++serial;}
bool StrikeSource::review_save(const std::string &path,StrikeWriteReview &out,std::string &error) const {
    error.clear();std::error_code ec;auto dest=fs::absolute(fs::u8path(path),ec).lexically_normal();if(ec||lower(dest.extension().u8string())!=".asm"){error="Choose an .ASM source filename.";return false;}
    auto source=fs::u8path(source_path);bool original=same_path(dest,source);
    if(!original&&fs::equivalent(dest,source,ec)){error="Save As cannot replace an alias of the loaded source.";return false;}
    StrikeWriteReview review;review.owner=this;review.generation=serial;review.path=dest.u8string();review.bytes=text;review.existed=fs::exists(dest,ec);if(ec){error="Cannot inspect the output path.";return false;}
    if(review.existed&&!read(review.path,review.previous,error))return false;if(original&&(!review.existed||review.previous!=baseline)){error="The source changed on disk. Reopen it or Save As a new file.";return false;}out=std::move(review);return true;
}
bool StrikeSource::save(const StrikeWriteReview &r,std::string &error){
    if(r.owner!=this||r.generation!=serial||r.bytes!=text){error="The source changed after save review.";return false;}StrikeWriteReview current;if(!review_save(r.path,current,error))return false;
    if(current.existed!=r.existed||current.previous!=r.previous){error="The destination changed after save review.";return false;}
    std::vector<unsigned char> bytes(text.begin(),text.end());if(!atomic_write_file(r.path,bytes,error))return false;source_path=r.path;baseline=text;return true;
}
}
