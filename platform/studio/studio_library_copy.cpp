#include "studio_library_copy.h"
#include "studio_sprite_clipboard.h"
#include "studio_batch_delete.h"
#include "studio_palette.h"
#include <algorithm>
#include <map>
namespace studio {
namespace {
std::string key(std::string s){for(char &c:s)if(c>='A'&&c<='Z')c+=32;return s;}
bool valid_name(const std::string &s){return !s.empty()&&s.size()<=15&&std::all_of(s.begin(),s.end(),[](unsigned char c){return c>=33&&c<=126;});}
bool valid_palette(const PAL *p){return p&&p->data_p&&p->numc>=1&&p->numc<=256;}
bool dimensions(int w,int h){return w>=1&&h>=1&&w<=8192&&h<=8192&&(size_t)w*h<=4*1024*1024;}
size_t size(int w,int h){return (((size_t)w+3)&~size_t(3))*h;}
std::array<unsigned char,sizeof(PAL)> palette_fields(const PAL &p){PAL fields=p;fields.nxt_p=fields.data_p=fields.temp=nullptr;std::array<unsigned char,sizeof(PAL)> bytes;memcpy(bytes.data(),&fields,sizeof fields);return bytes;}
bool equal_row(const LibraryCopyRow &a,const LibraryCopyRow &b){return a.sprite==b.sprite&&a.enabled==b.enabled&&a.w==b.w&&a.h==b.h&&a.source_palette==b.source_palette&&a.target_palette==b.target_palette&&a.name==b.name&&a.copied_name==b.copied_name&&a.error==b.error&&a.fields==b.fields&&a.pixels==b.pixels&&a.baseline==b.baseline&&a.points==b.points&&a.alternate==b.alternate&&a.mapped_alternate==b.mapped_alternate;}
void *copy_bytes(const std::vector<unsigned char> &b){if(b.empty())return nullptr;void *p=malloc(b.size());if(!p)throw std::bad_alloc();memcpy(p,b.data(),b.size());return p;}
}
bool plan_library_copy(const Artwork &source,const Artwork &target,const std::vector<uint64_t> &ids,const LibraryCopyOptions &o,LibraryCopyPlan &out,std::string &error){
    out={};error.clear();if(&source==&target||ids.empty()||ids.size()>2000||o.prefix.size()>15||std::any_of(o.prefix.begin(),o.prefix.end(),[](unsigned char c){return c<33||c>126;})){error="Choose different source and destination libraries, sprites and an optional ASCII prefix of up to 15 characters.";return false;}
    SpriteScope scope;if(!review_sprite_scope(source,ids,o.include_children,scope,error))return false;
    std::set<uint64_t> seen;for(auto &row:scope.rows)seen.insert(row.sprite);
    LibraryCopyPlan p;p.scope=std::move(scope);p.source=&source;p.target=&target;p.source_generation=source.generation();p.target_generation=target.generation();p.options=o;size_t snapshots=0;std::set<int> referenced,needed;std::map<std::string,int> names;
    for(int i=0;i<target.count();++i)++names[key(target.sprite(i)->n_s)];
    try{for(int i=0;i<source.count();++i){auto id=source.sprite_id(i);if(!seen.count(id))continue;auto im=source.sprite(i);LibraryCopyRow r;r.sprite=id;r.enabled=!o.excluded.count(id);r.w=im->w;r.h=im->h;r.name=im->n_s;r.copied_name=o.prefix+r.name;r.source_palette=im->palnum;memcpy(r.fields.data(),im,sizeof(IMG));
        if(!dimensions(r.w,r.h)||!im->data_p){error="A source sprite has invalid dimensions or pixels.";return false;}if(im->baseline_p&&!dimensions(im->baseline_w,im->baseline_h)){error="A source baseline has invalid dimensions.";return false;}
        snapshots+=size(r.w,r.h)+(im->baseline_p?size(im->baseline_w,im->baseline_h):0);if(snapshots>256*1024*1024){error="The copy preview exceeds its 256 MiB snapshot limit.";return false;}r.pixels.assign((const unsigned char *)im->data_p,(const unsigned char *)im->data_p+size(r.w,r.h));if(im->baseline_p)r.baseline.assign((const unsigned char *)im->baseline_p,(const unsigned char *)im->baseline_p+size(im->baseline_w,im->baseline_h));if(im->pttbl_p)r.points.assign((const unsigned char *)im->pttbl_p,(const unsigned char *)im->pttbl_p+40);if(im->opaltbl_p)r.alternate.assign((const unsigned char *)im->opaltbl_p,(const unsigned char *)im->opaltbl_p+16);
        auto issue=[&](const std::string &s){if(r.error.empty())r.error=s;};if(im->layer_p)issue("Flatten the source overlay layer before copying.");if(!valid_name(r.copied_name))issue("The copied name must fit 1 to 15 ASCII characters without spaces.");if(r.enabled){++names[key(r.copied_name)];p.added_pixels+=r.pixels.size();}
        auto add_palette=[&](int index){if(!valid_palette(source.palette(index))){issue("A required source palette is unavailable.");return;}referenced.insert(index);if(r.enabled)needed.insert(index);};add_palette(r.source_palette);
        auto pal=source.palette(r.source_palette);if(valid_palette(pal)){int stride=(r.w+3)&~3;for(int y=0;y<r.h&&r.error.empty();++y)for(int x=0;x<r.w;++x)if(r.pixels[y*stride+x]>=pal->numc){issue("Source pixels contain unresolved primary palette indices.");break;}}
        for(auto value:r.alternate){if(value>=128)break;if(value>=NUMDEFPAL)add_palette(value-NUMDEFPAL);}
        if(!r.alternate.empty()&&target.disk_version()<0x61d)issue("The destination's original IMG layout cannot store alternate palettes.");p.rows.push_back(std::move(r));
    }
    int next=target.palette_count();std::map<int,int> mapping;
    for(auto index:referenced){LibraryCopyPalette pal;pal.source=index;pal.fields=palette_fields(*source.palette(index));pal.words=copy_palette(source,index).words;if(needed.count(index)){if(o.reuse)for(int i=0;i<target.palette_count();++i)if(valid_palette(target.palette(i))&&pal.fields==palette_fields(*target.palette(i))&&pal.words==copy_palette(target,i).words){pal.target=i;pal.reused=true;break;}
            if(pal.target<0){pal.target=next++;++p.added_palettes;}mapping[index]=pal.target;}p.palettes.push_back(std::move(pal));}
    for(auto &r:p.rows){if(r.enabled&&names[key(r.copied_name)]>1&&r.error.empty())r.error="The copied name collides with another sprite. Change the prefix or exclude this row.";if(mapping.count(r.source_palette))r.target_palette=mapping[r.source_palette];r.mapped_alternate=r.alternate;
        if(r.enabled)for(auto &value:r.mapped_alternate){if(value>=128)break;if(value<NUMDEFPAL)continue;auto found=mapping.find(value-NUMDEFPAL);if(found==mapping.end()||found->second+NUMDEFPAL>=128){if(r.error.empty())r.error="The destination palette index cannot fit this alternate table. Exclude the sprite or free low palette slots.";break;}value=(unsigned char)(found->second+NUMDEFPAL);}}
    }catch(const std::bad_alloc &){error="Not enough memory for the copy review.";return false;}out=std::move(p);return true;
}
bool same_library_copy_plan(const LibraryCopyPlan &a,const LibraryCopyPlan &b){
    if(a.source!=b.source||a.target!=b.target||a.source_generation!=b.source_generation||a.target_generation!=b.target_generation||!(a.scope==b.scope)||a.options.prefix!=b.options.prefix||a.options.reuse!=b.options.reuse||a.options.include_children!=b.options.include_children||a.options.excluded!=b.options.excluded||a.added_palettes!=b.added_palettes||a.added_pixels!=b.added_pixels||a.rows.size()!=b.rows.size()||a.palettes.size()!=b.palettes.size())return false;
    for(size_t i=0;i<a.rows.size();++i)if(!equal_row(a.rows[i],b.rows[i]))return false;
    for(size_t i=0;i<a.palettes.size();++i){auto &x=a.palettes[i];auto &y=b.palettes[i];if(x.source!=y.source||x.target!=y.target||x.reused!=y.reused||x.fields!=y.fields||x.words!=y.words)return false;}
    return true;
}
std::vector<unsigned char> render_library_copy(const LibraryCopyPlan &p,int index){if(index<0||index>=(int)p.rows.size())return {};auto &r=p.rows[index];if(!dimensions(r.w,r.h)||r.pixels.size()!=size(r.w,r.h))return {};auto pal=std::find_if(p.palettes.begin(),p.palettes.end(),[&](const LibraryCopyPalette &v){return v.source==r.source_palette;});std::vector<unsigned char> rgba((size_t)r.w*r.h*4);int stride=(r.w+3)&~3;for(int y=0;y<r.h;++y)for(int x=0;x<r.w;++x){int c=r.pixels[y*stride+x];auto out=rgba.data()+((size_t)y*r.w+x)*4;if(pal==p.palettes.end()||c>=(int)pal->words.size()){out[0]=out[2]=255;out[1]=0;}else{auto word=pal->words[c];unsigned char bytes[]={(unsigned char)word,(unsigned char)(word>>8)};pal_word_to_rgb8(bytes,out,out+1,out+2);}out[3]=c?255:0;}return rgba;}
int Artwork::validate_library_copy(const Artwork &source,const LibraryCopyPlan &p,std::string &error) const{
    error.clear();if(pending||source.pending){error="Finish edits in both libraries before copying sprites.";return -1;}if(p.source!=&source||p.target!=this||p.source_generation!=source.generation()||p.target_generation!=generation()){error="A library changed. Cancel and reopen the copy review.";return -1;}
    LibraryCopyPlan checked;if(!plan_library_copy(source,*this,p.scope.requested,p.options,checked,error))return -1;
    if(!(p.scope==checked.scope)||p.added_pixels!=checked.added_pixels||p.added_palettes!=checked.added_palettes||p.rows.size()!=checked.rows.size()||p.palettes.size()!=checked.palettes.size()){error="The copy review no longer matches its source or destination.";return -1;}
    for(size_t i=0;i<p.palettes.size();++i){auto &a=p.palettes[i];auto &b=checked.palettes[i];if(a.source!=b.source||a.target!=b.target||a.reused!=b.reused||a.fields!=b.fields||a.words!=b.words){error="A copied palette no longer matches the review.";return -1;}}
    int added=0;for(size_t i=0;i<p.rows.size();++i){auto &r=p.rows[i];if(!equal_row(r,checked.rows[i])){error="A copied sprite no longer matches the review.";return -1;}if(r.enabled){if(!r.error.empty()){error=r.name+": "+r.error;return -1;}++added;}}
    if(!added)return 0;if(count()+added>2000||palette_count()+p.added_palettes>4093){error="The copy exceeds the library's sprite or palette limit.";return -1;}size_t total=p.added_pixels;for(int i=0;i<count();++i){auto im=sprite(i);total+=size(im->w,im->h);}if(total>128*1024*1024){error="The destination would exceed 128 MiB of decoded pixels.";return -1;}
    return added;
}
namespace {
// Detached buffers are shared by normal copy and clipboard capture. No history
// snapshot is needed for clipboard ownership, and no source pointers are kept.
std::unique_ptr<Artwork> stage_copy(const LibraryCopyPlan &p,int first_palette,std::string &error){
    try{auto staged=std::make_unique<Artwork>();IMG **images=(IMG **)&staged->data.img_p;for(auto &r:p.rows)if(r.enabled){auto im=(IMG *)calloc(1,sizeof(IMG));if(!im)throw std::bad_alloc();memcpy(im,r.fields.data(),sizeof(IMG));im->nxt_p=im->temp=im->data_p=im->baseline_p=im->pttbl_p=im->opaltbl_p=im->layer_p=nullptr;*images=im;images=(IMG **)&im->nxt_p;++staged->data.imgcnt;memset(im->n_s,0,16);memcpy(im->n_s,r.copied_name.data(),r.copied_name.size());im->palnum=(unsigned short)r.target_palette;im->data_p=copy_bytes(r.pixels);im->baseline_p=copy_bytes(r.baseline);im->pttbl_p=copy_bytes(r.points);im->opaltbl_p=copy_bytes(r.mapped_alternate);}
        PAL **palettes=(PAL **)&staged->data.pal_p;for(auto &r:p.palettes)if(r.target>=first_palette&&!r.reused){auto pal=(PAL *)calloc(1,sizeof(PAL));if(!pal)throw std::bad_alloc();memcpy(pal,r.fields.data(),sizeof(PAL));*palettes=pal;palettes=(PAL **)&pal->nxt_p;++staged->data.palcnt;pal->data_p=malloc(r.words.size()*2);if(!pal->data_p)throw std::bad_alloc();memcpy(pal->data_p,r.words.data(),r.words.size()*2);}
        return staged;
    }catch(const std::bad_alloc &){error="Not enough memory to stage the complete copy. No sprites were added.";return {};}
}
}
std::shared_ptr<const SpriteClipboard> SpriteClipboard::capture(const Artwork &source,const LibraryCopyPlan &p,const std::string &label,std::string &error){
    error.clear();if(!p.target||p.target->count()||p.target->palette_count()||p.target->data.seqcnt||p.target->data.scrcnt||!p.options.prefix.empty()){error="Clipboard capture requires an empty staging library and unchanged sprite names.";return {};}
    int count=p.target->validate_library_copy(source,p,error);if(count<=0){if(!count)error="Choose at least one sprite for the clipboard.";return {};}
    try{
        auto result=std::make_shared<SpriteClipboard>();result->storage=stage_copy(p,0,error);if(!result->storage)return {};
        result->storage->assign_initial_ids();result->scope.include_children=p.scope.include_children;result->scope.ambiguous=p.scope.ambiguous;result->scope.skipped=p.scope.skipped;result->source_label=label;
        for(size_t i=0;i<p.rows.size();++i)if(p.rows[i].enabled){result->scope.rows.push_back(p.scope.rows[i]);if(!p.scope.rows[i].parent)result->scope.requested.push_back(p.rows[i].sprite);}
        return result;
    }catch(const std::bad_alloc &){error="Not enough memory to capture the sprite clipboard. The previous clipboard was retained.";return {};}
}
int Artwork::apply_library_copy(const Artwork &source,const LibraryCopyPlan &p,std::string &error){
    int added=validate_library_copy(source,p,error);if(added<=0)return added;auto staged=stage_copy(p,palette_count(),error);if(!staged)return -1;
    try{sprite_ids.reserve(sprite_ids.size()+added);begin("Copy sprites from library");}catch(const std::bad_alloc &){error="Not enough memory to start the complete copy. No sprites were added.";return -1;}
    append_staged_library(*staged,added,true);commit();return added;
}
int Artwork::apply_library_move(Artwork &source,const LibraryCopyPlan &copy,const BatchDeletePlan &removal,std::string &error){
    try{
        int added=validate_library_copy(source,copy,error);if(added<0)return -1;
        int removed=source.validate_batch_delete(removal,error);if(removed<0)return -1;
        std::set<uint64_t> copied,deleted_ids;for(auto &row:copy.rows)if(row.enabled)copied.insert(row.sprite);for(auto &row:removal.rows)if(row.enabled)deleted_ids.insert(row.sprite);
        if(copied!=deleted_ids||added!=removed){error="The source removal must match the reviewed destination sprites exactly.";return -1;}if(!added)return 0;
        auto staged=stage_copy(copy,palette_count(),error);if(!staged)return -1;
        std::vector<int> mapping(source.count(),-1);std::vector<IMG *> kept,deleted;std::vector<uint64_t> retained;
        int first=std::max(0,source.sprite_index(source.selected_sprite_id));for(int i=0;i<source.count();++i){auto im=source.sprite(i);if(deleted_ids.count(source.sprite_id(i))){deleted.push_back(im);continue;}mapping[i]=(int)kept.size();kept.push_back(im);retained.push_back(source.sprite_id(i));}
        std::vector<unsigned char> blob;if(!source.remap_sequence_indices(mapping,blob,error))return -1;
        uint64_t selection=source.selected_sprite_id;if(deleted_ids.count(selection)){int next=std::min(first,(int)kept.size()-1);for(int old=first;old<(int)mapping.size();++old)if(mapping[old]>=0){next=mapping[old];break;}selection=retained[next];}
        auto reserved=legacy_reserved_alternates;
        if(disk_version()<0x61d&&source.disk_version()<0x61d){uint64_t next=next_sprite_id;for(auto &row:copy.rows)if(row.enabled){auto found=source.legacy_reserved_alternates.find(row.sprite);if(found!=source.legacy_reserved_alternates.end())reserved[next]=found->second;++next;}}
        State source_before=source.capture(),target_before=capture();source_before.label="Move sprites to library";target_before.label="Move sprites from library";
        sprite_ids.reserve(sprite_ids.size()+added);past.reserve(past.size()+1);source.past.reserve(source.past.size()+1);
        // All storage and both Undo entries exist before either library changes.
        // Publishing the pair below cannot allocate or fail halfway through.
        append_staged_library(*staged,added,true);legacy_reserved_alternates.swap(reserved);
        source.data.img_p=kept.front();for(size_t i=0;i<kept.size();++i)kept[i]->nxt_p=i+1<kept.size()?kept[i+1]:nullptr;
        for(auto im:deleted)FreeImg(im);source.data.imgcnt=(unsigned short)kept.size();source.sprite_ids.swap(retained);source.selected_sprite_id=selection;
        if(!blob.empty())memcpy(source.data.scrseqmem_p,blob.data(),blob.size());++source.serial;++source.structure_serial;
        source.past.push_back(std::move(source_before));source.future.clear();source.revision=source.next_revision++;source.trim_history();
        past.push_back(std::move(target_before));future.clear();revision=next_revision++;trim_history();return added;
    }catch(const std::bad_alloc &){error="Not enough memory to prepare the move. Neither library was changed.";return -1;}
}
void Artwork::append_staged_library(Artwork &staged,int added,bool select_first){
    // Callers reserve stable-ID capacity before publishing any detached buffers.
    int first=count();IMG **images=(IMG **)&data.img_p;while(*images)images=(IMG **)&(*images)->nxt_p;
    *images=(IMG *)staged.data.img_p;data.imgcnt+=staged.data.imgcnt;staged.data.img_p=nullptr;staged.data.imgcnt=0;
    PAL **palettes=(PAL **)&data.pal_p;while(*palettes)palettes=(PAL **)&(*palettes)->nxt_p;
    *palettes=(PAL *)staged.data.pal_p;data.palcnt+=staged.data.palcnt;staged.data.pal_p=nullptr;staged.data.palcnt=0;
    for(int i=0;i<added;++i)sprite_ids.push_back(next_sprite_id++);
    if(select_first)selected_sprite_id=sprite_ids[first];++serial;++structure_serial;
}
bool Artwork::install_animation_with_artwork(const Artwork &source,const LibraryCopyPlan &p,const std::vector<unsigned char> &blob,unsigned sequences,unsigned scripts,std::string &error){
    int added=0;if(!p.rows.empty()){added=validate_library_copy(source,p,error);if(added<=0){if(!added)error="The animation artwork review is empty.";return false;}}
    if(blob.size()>32*1024*1024){error="Embedded animation data exceeds 32 MiB.";return false;}
    std::unique_ptr<Artwork> staged;if(added){staged=stage_copy(p,palette_count(),error);if(!staged)return false;}
    std::unique_ptr<void,decltype(&free)> replacement(blob.empty()?nullptr:malloc(blob.size()),&free);
    if(!blob.empty()&&!replacement){error="Could not allocate animation data. No artwork was added.";return false;}
    if(replacement)memcpy(replacement.get(),blob.data(),blob.size());
    try{
        State before=capture();before.label=added?"Copy animations and artwork":"Copy embedded animations";
        sprite_ids.reserve(sprite_ids.size()+added);past.reserve(past.size()+1);
        // No allocations after this point: artwork, records and history publish
        // together. Existing sprite/palette pointers and selection stay intact.
        if(added)append_staged_library(*staged,added,false);else ++serial;
        free(data.scrseqmem_p);data.scrseqmem_p=replacement.release();data.scrseqbytes=(unsigned)blob.size();data.seqcnt=sequences;data.scrcnt=scripts;
        past.push_back(std::move(before));future.clear();revision=next_revision++;trim_history();return true;
    }catch(const std::bad_alloc &){error="Not enough memory to prepare animation history. Nothing was copied.";return false;}
}

std::unique_ptr<Artwork> Artwork::split_target() const{
    auto target=std::make_unique<Artwork>();target->source_version=source_version;target->source_header=source_header;return target;
}
std::unique_ptr<Artwork> Artwork::prepare_library_split(const LibraryCopyPlan &p,std::string &error) const{
    error.clear();if(!p.target||p.target->count()||p.target->palette_count()||p.target->data.seqcnt||p.target->data.scrcnt||p.target->disk_version()!=disk_version()||!p.options.prefix.empty()){error="Split requires an empty target with the source IMG layout and unchanged sprite names.";return {};}
    int added=p.target->validate_library_copy(*this,p,error);if(added<=0){if(!added)error="Choose at least one sprite for the new library.";return {};}
    try{
        auto result=stage_copy(p,0,error);if(!result)return {};
        result->source_version=source_version;result->source_header=source_header;result->assign_initial_ids();
        int destination=0;for(auto &row:p.rows)if(row.enabled){auto found=legacy_reserved_alternates.find(row.sprite);if(found!=legacy_reserved_alternates.end())result->legacy_reserved_alternates[result->sprite_id(destination)]=found->second;++destination;}
        // The new tab must participate in the normal unsaved-changes guard.
        result->revision=result->next_revision++;return result;
    }catch(const std::bad_alloc &){error="Not enough memory to prepare the new library. The source was not changed.";return {};}
}
int Artwork::remove_split_source(const LibraryCopyPlan &copy,const BatchDeletePlan &removal,std::string &error){
    if(!copy.target||copy.target->count()||copy.target->palette_count()||copy.target->data.seqcnt||copy.target->data.scrcnt||copy.target->disk_version()!=disk_version()||!copy.options.prefix.empty()){error="The split target no longer matches the source layout.";return -1;}
    int added=copy.target->validate_library_copy(*this,copy,error);if(added<=0)return added;
    std::set<uint64_t> copied,removed;for(auto &row:copy.rows)if(row.enabled)copied.insert(row.sprite);for(auto &row:removal.rows)if(row.enabled)removed.insert(row.sprite);
    if(copied!=removed){error="The source removal must match the reviewed split sprites exactly.";return -1;}
    return apply_batch_delete(removal,error);
}
}
