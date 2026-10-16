#include "studio_fatality.h"
#include "studio_file_io.h"
#include "mk2_fatality.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
namespace studio {
namespace {
namespace fs=std::filesystem;
std::string lower(std::string s){for(char &c:s)c=(char)std::tolower((unsigned char)c);return s;}
std::string lf(const std::string &s){std::string out;out.reserve(s.size());for(size_t i=0;i<s.size();++i){if(s[i]=='\r'&&i+1<s.size()&&s[i+1]=='\n')continue;out+=s[i];}return out;}
bool read(const std::string &path,std::string &out,std::string &error){
    std::ifstream f(fs::u8path(path),std::ios::binary|std::ios::ate);auto size=f?f.tellg():std::streampos(-1);
    if(size<0||size>4*1024*1024){error="Cannot read ASM source (4 MiB limit).";return false;}
    std::string bytes((size_t)size,'\0');f.seekg(0);if(size&&!f.read(bytes.data(),size)){error="Cannot finish reading ASM source.";return false;}out=std::move(bytes);return true;
}
bool same_path(const fs::path &a,const fs::path &b){
#ifdef _WIN32
    return lower(a.lexically_normal().u8string())==lower(b.lexically_normal().u8string());
#else
    return a.lexically_normal()==b.lexically_normal();
#endif
}
bool label(const std::string &line){
    size_t begin=line.find_first_not_of(" \t");
    if(begin==line.npos||!(std::isalpha((unsigned char)line[begin])||line[begin]=='_'))return false;
    size_t end=line.find_first_of(" \t:;",begin);
    // Column-zero labels and indented colon labels both define symbols.
    if(begin&& (end==line.npos||line[end]!=':'))return false;
    auto name=line.substr(begin,end==line.npos?line.npos:end-begin);
    return std::all_of(name.begin(),name.end(),[](unsigned char c){return std::isalnum(c)||c=='_';});
}
}
bool FatalitySource::parse(const std::string &text,Parsed &out,std::string &error){
    error.clear();if(text.size()>4*1024*1024||text.find('\0')!=text.npos){error="Choose bounded ASM text without NUL bytes.";return false;}
    Parsed p;mk2fatal::SourceFile file;file.rel_path="MOVES.ASM";
    for(size_t begin=0;begin<text.size();){
        size_t end=text.find('\n',begin);bool terminated=end!=text.npos;if(!terminated)end=text.size();
        size_t content=end;if(content>begin&&text[content-1]=='\r')--content;
        if(content-begin>2048||p.lines.size()>=100000){error="ASM source exceeds its line limits.";return false;}
        p.lines.push_back({begin,content,terminated?end+1:end});file.lines.push_back(text.substr(begin,content-begin));begin=terminated?end+1:end;
    }
    if(!file.lines.empty()&&file.lines[0].compare(0,3,"\xef\xbb\xbf")==0)file.lines[0].erase(0,3);
    // Scan commands/combos and animation data independently, without filename
    // assumptions. Both views refer to the same bounded byte ranges in this file.
    mk2fatal::Document doc;doc.files.push_back(file);file.rel_path="CHARACTER.ASM";doc.files.push_back(std::move(file));
    if(!mk2fatal::reparse(&doc,&error))return false;
    for(auto &r:doc.commands){FatalityBlock b;b.kind=FatalityKind::Command;b.label=r.label;b.first=r.start_line;b.last=r.end_line;b.combo=r.combo_label;
        b.summary="Routine: "+r.routine+" / "+r.transfer;
        if(!r.trigger.empty())b.summary+=" / trigger: "+r.trigger;if(r.finish_him_only)b.summary+=" / Finish him check";if(!r.range_note.empty())b.summary+=" / "+r.range_note;p.blocks.push_back(std::move(b));}
    for(auto &r:doc.combos){FatalityBlock b;b.kind=FatalityKind::Combo;b.label=r.label;b.first=r.start_line;b.last=r.end_line;b.summary="Time token: "+r.time_token+" / "+std::to_string(r.words.size())+" word operands";p.blocks.push_back(std::move(b));}
    for(auto &r:doc.animations)if(r.long_count||r.word_count){FatalityBlock b;b.kind=FatalityKind::Animation;b.label=r.label;b.first=r.start_line;b.last=r.end_line;b.body_ending=r.body_ending;b.summary=std::to_string(r.long_count)+" long + "+std::to_string(r.word_count)+" word operands / "+std::to_string(r.adjust_count)+" XY adjustments";p.blocks.push_back(std::move(b));}
    if(p.blocks.size()>4096){error="ASM source exceeds 4096 inspected blocks.";return false;}
    std::stable_sort(p.blocks.begin(),p.blocks.end(),[](const FatalityBlock &a,const FatalityBlock &b){return a.kind!=b.kind?a.kind<b.kind:a.first<b.first;});
    for(auto &b:p.blocks)if(b.first<1||b.last<b.first||b.last>(int)p.lines.size()){error="A source block has invalid bounds.";return false;}
    out=std::move(p);return true;
}
std::unique_ptr<FatalitySource> FatalitySource::open(const std::string &path,std::string &error){
    try{std::string bytes;if(!read(path,bytes,error))return {};auto result=std::make_unique<FatalitySource>();if(!parse(bytes,result->parsed,error))return {};
        if(result->parsed.blocks.empty()){error="No fatality command, scom_* combo or animation data blocks found.";return {};}
        result->source_path=fs::absolute(fs::u8path(path)).lexically_normal().u8string();result->text=bytes;result->baseline=std::move(bytes);return result;
    }catch(const std::exception &e){error=e.what();return {};}
}
std::string FatalitySource::header(int block)const{
    if(block<0||block>=(int)parsed.blocks.size())return {};auto &l=parsed.lines[parsed.blocks[block].first-1];return text.substr(l.begin,l.end-l.begin);
}
std::string FatalitySource::body(int block)const{
    if(block<0||block>=(int)parsed.blocks.size())return {};auto &b=parsed.blocks[block];size_t begin=parsed.lines[b.first-1].next,end=parsed.lines[b.last-1].next;return lf(text.substr(begin,end-begin));
}
bool FatalitySource::review(int block,const std::string &input,FatalityPatch &out,std::string &error)const{
    out={};error.clear();try{
        if(block<0||block>=(int)parsed.blocks.size()){error="Choose a source block.";return false;}
        if(input.size()>256*1024||input.find('\0')!=input.npos){error="A block body must fit 256 KiB without NUL bytes.";return false;}
        FatalityPatch p;p.owner=this;p.generation=serial;p.block=block;p.body=lf(input);p.before=text;p.after=text;
        if(p.body!=body(block)){
            size_t start=0;while(start<p.body.size()){auto end=p.body.find('\n',start);if(end==p.body.npos)end=p.body.size();auto line=p.body.substr(start,end-start);
                if(label(line)){error="Keep the existing label fixed. New labels cannot be introduced inside a block; indent instructions and data directives.";return false;}
                if(line.find('\r')!=line.npos){error="Use normal line endings in the block body.";return false;}start=end==p.body.size()?end:end+1;
            }
            auto &b=parsed.blocks[block];auto &first=parsed.lines[b.first-1];size_t begin=first.next,end=parsed.lines[b.last-1].next;
            std::string ending=first.next>=2&&text[first.next-2]=='\r'?"\r\n":"\n";
            std::string replacement;if(first.next==first.end&&!p.body.empty())replacement+=ending;
            for(char c:p.body){if(c=='\n')replacement+=ending;else replacement+=c;}
            if(end<text.size()&&!replacement.empty()&&replacement.back()!='\n')replacement+=ending;
            p.after.replace(begin,end-begin,replacement);Parsed checked;if(!parse(p.after,checked,error))return false;
        }
        p.changed=p.before!=p.after;out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory for the block review. Source unchanged.";return false;}
}
void FatalitySource::trim_history(){size_t size=0;for(auto &s:past)size+=s.size();while(past.size()>1&&(past.size()>64||size>64*1024*1024)){size-=past.front().size();past.erase(past.begin());}}
bool FatalitySource::apply(const FatalityPatch &p,std::string &error){
    error.clear();try{
        if(p.owner!=this||p.generation!=serial||p.before!=text){error="The source changed. Rebuild the block review.";return false;}
        FatalityPatch checked;if(!review(p.block,p.body,checked,error))return false;
        if(p.after!=checked.after||p.changed!=checked.changed){error="The block patch differs from its review.";return false;}if(!p.changed)return true;
        Parsed next;if(!parse(p.after,next,error))return false;std::string bytes=p.after;past.reserve(past.size()+1);
        past.push_back(std::move(text));text=std::move(bytes);parsed=std::move(next);future.clear();++serial;trim_history();return true;
    }catch(const std::bad_alloc &){error="Not enough memory to apply the block. Source unchanged.";return false;}
}
bool FatalitySource::history(bool redo,std::string &error){
    error.clear();auto &from=redo?future:past;auto &to=redo?past:future;if(from.empty())return true;
    try{Parsed next;if(!parse(from.back(),next,error))return false;to.reserve(to.size()+1);to.push_back(std::move(text));text=std::move(from.back());from.pop_back();parsed=std::move(next);++serial;trim_history();return true;}
    catch(const std::bad_alloc &){error="Not enough memory for source history. Source unchanged.";return false;}
}
bool FatalitySource::undo(std::string &error){return history(false,error);}bool FatalitySource::redo(std::string &error){return history(true,error);}
bool FatalitySource::review_save(const std::string &path,FatalityWriteReview &out,std::string &error)const{
    out={};error.clear();try{
        auto dest=fs::absolute(fs::u8path(path)).lexically_normal();if(lower(dest.extension().u8string())!=".asm"){error="Choose an .ASM source filename.";return false;}
        std::error_code ec;bool original=same_path(dest,fs::u8path(source_path));
        if(!original&&fs::equivalent(dest,fs::u8path(source_path),ec)){error="Save As cannot replace an alias of the loaded source.";return false;}
        FatalityWriteReview r;r.owner=this;r.generation=serial;r.path=dest.u8string();r.bytes=text;r.existed=fs::exists(dest,ec);if(ec){error="Cannot inspect the output path.";return false;}
        if(r.existed&&!read(r.path,r.previous,error))return false;
        if(original&&(!r.existed||r.previous!=baseline)){error="Source changed on disk. Reopen it or Save As a new file.";return false;}
        out=std::move(r);return true;
    }catch(const std::exception &e){error=e.what();return false;}
}
bool FatalitySource::save(const FatalityWriteReview &r,std::string &error){
    error.clear();try{
        if(r.owner!=this||r.generation!=serial||r.bytes!=text){error="Source changed after save review.";return false;}
        FatalityWriteReview now;if(!review_save(r.path,now,error))return false;
        if(now.existed!=r.existed||now.previous!=r.previous){error="Destination changed after save review.";return false;}
        std::string saved=text,path=r.path;std::vector<unsigned char> bytes(text.begin(),text.end());
        if(!atomic_write_file(path,bytes,error))return false;baseline.swap(saved);source_path.swap(path);++serial;return true;
    }catch(const std::exception &e){error=e.what();return false;}
}
}
