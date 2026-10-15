#include "studio_bindings.h"
#include "studio_file_io.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
namespace studio {
namespace {
namespace fs=std::filesystem;
std::string lower(std::string s){for(char &c:s)if(c>='A'&&c<='Z')c+=32;return s;}
std::string trim(const std::string &s){auto b=s.find_first_not_of(" \t\r"),e=s.find_last_not_of(" \t\r");return b==s.npos?"":s.substr(b,e-b+1);}
bool label(const std::string &s){if(s.empty()||s.size()>128)return false;for(size_t i=0;i<s.size();++i){auto c=s[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||(i&&c>='0'&&c<='9')))return false;}return true;}
bool read(const std::string &path,std::string &bytes,bool &exists,std::string &error){std::error_code ec;exists=fs::exists(fs::u8path(path),ec);if(ec){error="Cannot inspect the association sidecar.";return false;}if(!exists){bytes.clear();return true;}
    std::ifstream in(fs::u8path(path),std::ios::binary|std::ios::ate);auto n=in?in.tellg():std::streampos(-1);if(n<0||n>256*1024){error="Cannot read association sidecar (256 KiB limit).";return false;}bytes.resize((size_t)n);in.seekg(0);if(n&&!in.read(bytes.data(),n)){error="Could not finish reading association sidecar.";return false;}return true;}
bool alias(const fs::path &a,const fs::path &b){std::error_code ec;return lower(a.lexically_normal().u8string())==lower(b.lexically_normal().u8string())||fs::equivalent(a,b,ec);}
}
bool StrikeBindings::parse(const std::string &bytes,std::string &error){error.clear();if(bytes.size()>256*1024||bytes.find('\0')!=bytes.npos){error="Oversized or binary association sidecar.";return false;}
    std::vector<Row> next;std::vector<std::string> warnings;size_t offset=0;int line=0;
    while(offset<bytes.size()){if(++line>4096){error="Association sidecar exceeds 4096 lines.";return false;}auto nl=bytes.find('\n',offset),end=nl==bytes.npos?bytes.size():nl;auto raw=bytes.substr(offset,end-offset);size_t bom=line==1&&raw.compare(0,3,"\xef\xbb\xbf")==0?3:0;
        if(raw.size()>2048){error="Association sidecar exceeds 2048 bytes per line.";return false;}auto content=trim(raw.substr(bom));
        if(!content.empty()&&content[0]!='#'&&content[0]!=';'){
            auto eq=raw.find('=',bom);if(eq==raw.npos){warnings.push_back("Line "+std::to_string(line)+": unrecognized text preserved.");}
            else{auto anim=trim(raw.substr(bom,eq-bom)),strike=trim(raw.substr(eq+1));bool valid=label(anim)&&(label(strike)||strike=="-");
                if(!valid)warnings.push_back("Line "+std::to_string(line)+": unusual assignment preserved; edit it outside Studio.");
                if(label(anim)){auto b=raw.find_first_not_of(" \t\r",eq+1),e=raw.find_last_not_of(" \t\r");next.push_back({anim,offset+bom,nl==bytes.npos?end:nl+1,b==raw.npos?end:offset+b,e==raw.npos?end:offset+e+1,valid});}
            }
        }
        if(nl==bytes.npos)break;offset=nl+1;
    }
    auto normalized=bytes;if(normalized.compare(0,3,"\xef\xbb\xbf")==0)normalized.erase(0,3);auto parsed=strike_bind::ParseSidecar(normalized);
    if(parsed.size()>4096){error="Too many association records.";return false;}
    rows=std::move(next);pairs=std::move(parsed);notices=std::move(warnings);return true;
}
std::unique_ptr<StrikeBindings> StrikeBindings::open_for(const std::string &source,std::string &error){
    error.clear();try{if(source.empty()||source.size()>32768||source.find('\0')!=source.npos){error="Choose a valid strike ASM source path.";return {};}auto p=fs::absolute(fs::u8path(source)).lexically_normal();if(lower(p.extension().u8string())!=".asm"){error="Associations require a strike ASM source path.";return {};}
        auto out=std::make_unique<StrikeBindings>();out->source_path=p.u8string();out->sidecar_path=strike_bind::SidecarPathFor(out->source_path);
        if(alias(fs::u8path(out->sidecar_path),p)){error="The association sidecar aliases its ASM source.";return {};}
        std::string bytes;if(!read(out->sidecar_path,bytes,out->existed,error)||!out->parse(bytes,error))return {};out->text=out->baseline=std::move(bytes);return out;
    }catch(const std::exception &e){error=e.what();return {};}
}
std::optional<std::string> StrikeBindings::lookup(const std::string &anim) const {for(auto &p:pairs)if(lower(p.anim)==lower(anim))return p.strike;return {};}
void StrikeBindings::trim_history(){size_t bytes=0;for(auto &s:past)bytes+=s.size();while(!past.empty()&&(past.size()>100||bytes>16*1024*1024)){bytes-=past.front().size();past.erase(past.begin());}}
bool StrikeBindings::set(const std::string &animation,const std::optional<std::string> &strike,std::string &error){error.clear();if(!label(animation)||(strike&&!label(*strike)&&*strike!="-")){error="Use assembly labels, or '-' for no association.";return false;}
    auto name=lower(animation);for(auto &r:rows)if(lower(r.animation)==name&&!r.editable){error="This animation has an unusual assignment. Reload after correcting it outside Studio.";return false;}
    std::string next=text;bool found=false;for(auto it=rows.rbegin();it!=rows.rend();++it)if(lower(it->animation)==name){found=true;if(strike)next.replace(it->begin,it->end-it->begin,*strike);else next.erase(it->first,it->last-it->first);}
    if(!found&&strike){std::string ending="\n";auto nl=next.find('\n');if(nl!=next.npos&&nl&&next[nl-1]=='\r')ending="\r\n";
        if(next.empty())next="# imgtool strike bindings"+ending+"# animation_label = strike_label; '-' disables the suggestion"+ending;
        if(!next.empty()&&next.back()!='\n')next+=ending;next+=animation+" = "+*strike+ending;
    }
    if(next==text)return true;StrikeBindings checked;if(!checked.parse(next,error))return false;
    past.push_back(text);future.clear();text=std::move(next);rows=std::move(checked.rows);pairs=std::move(checked.pairs);notices=std::move(checked.notices);++serial;trim_history();return true;
}
void StrikeBindings::undo(){if(past.empty())return;future.push_back(text);text=std::move(past.back());past.pop_back();std::string error;parse(text,error);++serial;}
void StrikeBindings::redo(){if(future.empty())return;past.push_back(text);text=std::move(future.back());future.pop_back();std::string error;parse(text,error);++serial;trim_history();}
void StrikeBindings::discard(){if(text==baseline)return;text=baseline;past.clear();future.clear();std::string error;parse(text,error);++serial;}
bool StrikeBindings::review_save(BindingsWriteReview &out,std::string &error) const {
    error.clear();try{auto dest=fs::u8path(sidecar_path);std::error_code ec;if(alias(dest,fs::u8path(source_path))||fs::is_symlink(dest,ec)){error="Association output cannot alias ASM or replace a symbolic link.";return false;}
        BindingsWriteReview r;r.owner=this;r.generation=serial;r.path=sidecar_path;r.bytes=text;if(!read(r.path,r.previous,r.existed,error))return false;
        if(r.existed!=existed||r.previous!=baseline){error="The association file changed on disk. Reload it before saving.";return false;}out=std::move(r);return true;
    }catch(const std::exception &e){error=e.what();return false;}
}
bool StrikeBindings::save(const BindingsWriteReview &r,std::string &error){if(r.owner!=this||r.generation!=serial||r.path!=sidecar_path||r.bytes!=text){error="Associations changed after review.";return false;}
    BindingsWriteReview current;if(!review_save(current,error))return false;if(current.existed!=r.existed||current.previous!=r.previous){error="The association destination changed after review.";return false;}
    if(!atomic_write_file(r.path,{text.begin(),text.end()},error))return false;baseline=text;existed=true;return true;
}
}
