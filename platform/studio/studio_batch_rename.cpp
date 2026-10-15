#include "studio_batch_rename.h"
#include <algorithm>
#include <map>
#include <set>
#include <iomanip>
#include <sstream>
namespace studio {
namespace {
std::string key(std::string s){for(char &c:s)if(c>='A'&&c<='Z')c+=32;return s;}
std::string name_issue(const std::string &s){if(s.empty()||s.size()>15)return "Name must contain 1 to 15 characters; it will not be truncated.";for(unsigned char c:s)if(c<33||c>126)return "Use printable ASCII without spaces.";return {};}
}
bool plan_batch_rename(const Artwork &art,const std::vector<uint64_t> &ids,const BatchRenameOptions &o,BatchRenamePlan &out,std::string &error){
    out={};error.clear();if(ids.empty()||ids.size()>2000||o.text.size()>128||o.start<0||o.start>1000000000||o.padding<0||o.padding>9||(o.kind!=BatchRenameKind::Numbered&&o.kind!=BatchRenameKind::Prepend&&o.kind!=BatchRenameKind::TailNumbered)){error="Choose sprites and valid rename settings (start 0..1000000000, padding 0..9).";return false;}
    std::set<uint64_t> targets;for(auto id:ids)if(art.sprite_index(id)<0||!targets.insert(id).second){error="The rename scope contains a missing or repeated sprite.";return false;}
    BatchRenamePlan p;p.owner=&art;p.generation=art.generation();p.options=o;
    for(int i=0;i<art.count();++i){auto id=art.sprite_id(i);if(!targets.count(id))continue;auto im=art.sprite(i);BatchRenameRow row;row.sprite=id;memcpy(row.visible.data(),im->n_s,16);memcpy(row.raw.data(),im->file_name_raw,16);row.before.assign(im->n_s,strnlen(im->n_s,15));
        std::ostringstream number;number<<std::setfill('0')<<std::setw(o.padding)<<((int64_t)o.start+p.rows.size());
        row.after=o.kind==BatchRenameKind::Prepend?o.text+row.before:o.kind==BatchRenameKind::TailNumbered?row.before+o.text+number.str():o.text+number.str();row.changed=row.after!=row.before;p.rows.push_back(std::move(row));
    }out=std::move(p);return true;
}
int check_batch_rename(const Artwork &art,const BatchRenamePlan &p,std::vector<std::string> &issues,std::string &error){
    issues.clear();error.clear();if(p.owner!=&art||p.generation!=art.generation()){error="The library changed. Cancel and reopen the rename preview.";return -1;}
    std::vector<uint64_t> ids;for(auto &r:p.rows)ids.push_back(r.sprite);BatchRenamePlan checked;if(!plan_batch_rename(art,ids,p.options,checked,error))return -1;
    if(p.rows.size()!=checked.rows.size()){error="The rename scope no longer matches its preview.";return -1;}
    for(size_t i=0;i<p.rows.size();++i){auto &a=p.rows[i];auto &b=checked.rows[i];if(a.sprite!=b.sprite||a.before!=b.before||a.after!=b.after||a.changed!=b.changed||a.visible!=b.visible||a.raw!=b.raw){error="The rename preview no longer matches its source or settings.";return -1;}}
    issues.resize(p.rows.size());std::map<uint64_t,size_t> rows;for(size_t i=0;i<p.rows.size();++i)rows[p.rows[i].sprite]=i;
    std::map<std::string,int> final_names;for(int i=0;i<art.count();++i){auto found=rows.find(art.sprite_id(i));auto name=found!=rows.end()&&p.rows[found->second].enabled?p.rows[found->second].after:std::string(art.sprite(i)->n_s,strnlen(art.sprite(i)->n_s,15));++final_names[key(name)];}
    int changed=0;for(size_t i=0;i<p.rows.size();++i){auto &r=p.rows[i];if(!r.enabled||!r.changed)continue;++changed;issues[i]=name_issue(r.after);if(issues[i].empty()&&final_names[key(r.after)]>1)issues[i]="Name collides with another sprite (case-insensitive).";if(!issues[i].empty()&&error.empty())error=r.before+": "+issues[i];}
    return error.empty()?changed:-1;
}
int Artwork::apply_batch_rename(const BatchRenamePlan &p,std::string &error){
    if(pending){error="Finish the current edit before renaming sprites.";return -1;}std::vector<std::string> issues;int changed=check_batch_rename(*this,p,issues,error);if(changed<=0)return changed;
    begin("Batch rename sprites");for(auto &r:p.rows)if(r.enabled&&r.changed){auto im=sprite(sprite_index(r.sprite));memset(im->n_s,0,16);memcpy(im->n_s,r.after.data(),r.after.size());}
    ++serial;++structure_serial;commit();return changed;
}
}
