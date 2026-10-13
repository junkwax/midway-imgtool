#include "studio_restore.h"
#include <algorithm>
#include <regex>
#include <unordered_map>
#include <unordered_set>
namespace studio {
namespace {
size_t bytes(int w,int h){return (((size_t)w+3)&~size_t(3))*h;}
bool dimensions(int w,int h){return w>0&&h>0&&w<=8192&&h<=8192&&(size_t)w*h<=4*1024*1024;}
std::string lower(std::string s){for(char &c:s)if(c>='A'&&c<='Z')c=(char)(c-'A'+'a');return s;}
bool valid_indices(const std::vector<unsigned char> &pixels,int w,int h,size_t colors){for(int y=0;y<h;++y)for(int x=0;x<w;++x)if(pixels[y*((w+3)&~3)+x]>=colors)return false;return true;}
bool pattern_valid(const std::string &p,std::string &error){
    if(p.empty()||p.size()>128){error="Use a name pattern of 1 to 128 characters with one parent-name capture.";return false;}bool escaped=false,in_class=false,group=false;
    for(char c:p){if(escaped){escaped=false;group=false;continue;}if(c=='\\'){escaped=true;group=false;continue;}if(c=='['){in_class=true;group=false;continue;}if(c==']'){in_class=false;group=false;continue;}if(!in_class&&(c=='{'||(group&&(c=='*'||c=='+'||c=='?')))){error="IMG-name patterns support one capture, character classes and simple wildcards. Counted repeats and repeated groups are not supported.";return false;}group=!in_class&&c==')';}return true;
}
bool capture(const Artwork &art,uint64_t id,RestoreSnapshot &s,bool baseline,std::string &error){
    auto im=art.sprite(art.sprite_index(id));auto pal=im?art.palette(im->palnum):nullptr;if(!im||!im->data_p||!dimensions(im->w,im->h)||!pal||!pal->data_p||pal->numc<1||pal->numc>256){error="A restoration sprite or palette is unavailable.";return false;}
    s.id=id;s.name.assign(im->n_s,strnlen(im->n_s,16));s.w=im->w;s.h=im->h;s.anix=(short)im->anix;s.aniy=(short)im->aniy;s.palette=im->palnum;memcpy(s.fields.data(),im,sizeof(IMG));s.pixels.assign((unsigned char *)im->data_p,(unsigned char *)im->data_p+bytes(im->w,im->h));s.colors.resize(pal->numc);memcpy(s.colors.data(),pal->data_p,pal->numc*2);
    if(!valid_indices(s.pixels,s.w,s.h,s.colors.size())){error=s.name+": pixel indices exceed the palette.";return false;}
    if(baseline&&im->baseline_p&&im->baseline_w==im->w&&im->baseline_h==im->h){s.baseline.assign((unsigned char *)im->baseline_p,(unsigned char *)im->baseline_p+bytes(im->w,im->h));if(!valid_indices(s.baseline,s.w,s.h,s.colors.size()))s.baseline.clear();}return true;
}
bool snapshot_same(const RestoreSnapshot &a,const RestoreSnapshot &b){return a.id==b.id&&a.name==b.name&&a.w==b.w&&a.h==b.h&&a.anix==b.anix&&a.aniy==b.aniy&&a.palette==b.palette&&a.fields==b.fields&&a.pixels==b.pixels&&a.baseline==b.baseline&&a.colors==b.colors;}
bool row_same(const RestoreRow &a,const RestoreRow &b){return a.child==b.child&&a.parent==b.parent&&a.child_name==b.child_name&&a.parent_name==b.parent_name&&a.status==b.status&&a.dx==b.dx&&a.dy==b.dy&&a.covered==b.covered&&a.total==b.total&&a.changed==b.changed&&a.added==b.added&&a.erased==b.erased&&a.pixels==b.pixels;}
}
const RestoreSnapshot *restore_source(const RestorePlan &plan,uint64_t id){for(auto &s:plan.sources)if(s.id==id)return &s;return nullptr;}
bool plan_restore(const Artwork &art,const std::vector<uint64_t> &ids,const RestoreOptions &o,RestorePlan &out,std::string &error){
    out={};error.clear();if(ids.empty()||ids.size()>2000||(o.mode!=RestoreMode::Reconstruct&&o.mode!=RestoreMode::Diff&&o.mode!=RestoreMode::Replace&&o.mode!=RestoreMode::FillTransparent)||(o.matching!=RestoreMatch::Names&&o.matching!=RestoreMatch::SelectedParent)){error="Choose child sprites and valid restoration settings.";return false;}
    RestorePlan plan;plan.owner=&art;plan.generation=art.generation();plan.options=o;size_t memory=0;std::unordered_map<uint64_t,size_t> captured;
    try{std::regex re;if(o.matching==RestoreMatch::Names){if(!pattern_valid(o.pattern,error))return false;re=std::regex(o.pattern,std::regex::ECMAScript|std::regex::icase);if(re.mark_count()!=1){error="The name pattern needs exactly one capture for the parent name.";return false;}}
        else if(art.sprite_index(o.parent)<0){error="Choose an available parent sprite.";return false;}
        std::unordered_map<std::string,std::vector<uint64_t>> names;for(int i=0;i<art.count();++i){auto im=art.sprite(i);names[lower(std::string(im->n_s,strnlen(im->n_s,16)))].push_back(art.sprite_id(i));}
        auto add_source=[&](uint64_t id){if(captured.count(id))return true;RestoreSnapshot s;if(!capture(art,id,s,o.mode==RestoreMode::Diff,error))return false;memory+=s.pixels.size()+s.baseline.size();if(memory>256*1024*1024){error="The restoration preview exceeds its 256 MiB pixel limit. Reduce the scope.";return false;}captured[id]=plan.sources.size();plan.sources.push_back(std::move(s));return true;};
        std::unordered_set<uint64_t> seen;for(auto id:ids){if(!seen.insert(id).second){error="A child occurs twice in the restoration scope.";return false;}if(!add_source(id))return false;RestoreRow r;r.child=id;r.child_name=plan.sources[captured.at(id)].name;r.parent=o.parent;
            if(o.matching==RestoreMatch::Names){std::smatch match;if(!std::regex_match(r.child_name,match,re)){r.status=RestoreStatus::NoPatternMatch;r.parent=0;}else{r.parent_name=match[1].str();auto found=names.find(lower(r.parent_name));if(found==names.end()){r.status=RestoreStatus::MissingParent;r.parent=0;}else if(found->second.size()!=1){r.status=RestoreStatus::AmbiguousParent;r.parent=0;}else r.parent=found->second[0];}}
            if(r.parent){if(!add_source(r.parent))return false;r.parent_name=plan.sources[captured.at(r.parent)].name;}
            // Reacquire references after adding a parent: source-vector growth
            // may move snapshots. Every read stays frozen for the whole batch.
            auto &child=plan.sources[captured.at(id)];r.total=child.w*child.h;r.pixels=child.pixels;memory+=r.pixels.size();if(memory>256*1024*1024){error="The restoration preview exceeds its 256 MiB pixel limit. Reduce the scope.";return false;}
            if(r.status==RestoreStatus::Ready){auto &parent=plan.sources[captured.at(r.parent)];if(r.parent==id)r.status=RestoreStatus::Self;else if(child.palette!=parent.palette)r.status=RestoreStatus::PaletteMismatch;else if(o.mode==RestoreMode::Diff&&parent.baseline.empty())r.status=RestoreStatus::MissingBaseline;
                r.dx=parent.anix-child.anix;r.dy=parent.aniy-child.aniy;int x0=std::max(0,-r.dx),y0=std::max(0,-r.dy),x1=std::min(child.w,parent.w-r.dx),y1=std::min(child.h,parent.h-r.dy);if(x1>x0&&y1>y0)r.covered=(x1-x0)*(y1-y0);if(!r.covered&&r.status==RestoreStatus::Ready)r.status=RestoreStatus::NoOverlap;
                if(r.status==RestoreStatus::Ready){int stride=(child.w+3)&~3,pstride=(parent.w+3)&~3;if(o.mode==RestoreMode::Replace)for(int y=0;y<child.h;++y)memset(r.pixels.data()+y*stride,0,child.w);
                    for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x){size_t at=(size_t)y*stride+x,from=(size_t)(y+r.dy)*pstride+(size_t)(x+r.dx);auto source=parent.pixels[from],before=child.pixels[at];bool copy=o.mode==RestoreMode::Replace||(o.mode==RestoreMode::Reconstruct&&source&&before&&source!=before)||(o.mode==RestoreMode::Diff&&source!=parent.baseline[from])||(o.mode==RestoreMode::FillTransparent&&!before&&source);if(copy)r.pixels[at]=source;}
                    for(int y=0;y<child.h;++y)for(int x=0;x<child.w;++x){size_t at=(size_t)y*stride+x;auto a=child.pixels[at],b=r.pixels[at];r.changed+=a!=b;r.added+=!a&&b;r.erased+=a&&!b;}r.enabled=r.covered==r.total;
                }
            }plan.rows.push_back(std::move(r));
        }
    }catch(const std::regex_error &){error="Invalid or overly complex name pattern.";return false;}catch(const std::bad_alloc &){error="Not enough memory for the restoration preview.";return false;}out=std::move(plan);return true;
}
int Artwork::apply_restore(const RestorePlan &plan,std::string &error){
    error.clear();if(pending){error="Finish the current edit before restoring a batch.";return -1;}if(plan.owner!=this||plan.generation!=generation()){error="The restoration source changed. Rebuild its preview.";return -1;}
    std::vector<uint64_t> ids;for(auto &r:plan.rows)ids.push_back(r.child);RestorePlan checked;if(!plan_restore(*this,ids,plan.options,checked,error))return -1;
    if(plan.sources.size()!=checked.sources.size()){error="The reviewed restoration sources no longer match.";return -1;}for(size_t i=0;i<plan.sources.size();++i)if(!snapshot_same(plan.sources[i],checked.sources[i])){error="A restoration sprite, palette or load baseline changed.";return -1;}
    int changed=0;for(size_t i=0;i<plan.rows.size();++i){auto &r=plan.rows[i];if(!row_same(r,checked.rows[i])||(r.enabled&&r.status!=RestoreStatus::Ready)){error="The reviewed restoration result is invalid. Rebuild its preview.";return -1;}changed+=r.enabled&&r.changed>0;}if(!changed)return 0;
    using Buffer=std::unique_ptr<unsigned char,decltype(&free)>;std::vector<Buffer> staged;
    try{staged.reserve(plan.rows.size());for(auto &r:plan.rows){Buffer pixels(nullptr,&free);if(r.enabled&&r.changed){pixels.reset((unsigned char *)malloc(r.pixels.size()));if(!pixels)throw std::bad_alloc();memcpy(pixels.get(),r.pixels.data(),r.pixels.size());}staged.push_back(std::move(pixels));}}
    catch(const std::bad_alloc &){error="Not enough memory to stage the complete restoration. No sprites changed.";return -1;}
    const char *labels[]={"Reconstruct child sprites","Propagate parent pixel edits","Replace child sprites from parents","Fill child transparency from parents"};begin(labels[(int)plan.options.mode]);
    for(size_t i=0;i<plan.rows.size();++i){auto &r=plan.rows[i];if(!r.enabled||!r.changed)continue;auto im=sprite(sprite_index(r.child));free(im->data_p);im->data_p=staged[i].release();}++serial;commit();return changed;
}
}
