#include "studio_batch_delete.h"
#include "studio_sequences.h"
#include "studio_palette.h"
#include <algorithm>
#include <map>
#include <set>
namespace studio {
namespace {
bool equal_row(const BatchDeleteRow &a,const BatchDeleteRow &b){if(a.sprite!=b.sprite||a.index!=b.index||a.w!=b.w||a.h!=b.h||a.name!=b.name||a.fields!=b.fields||a.pixels!=b.pixels||a.colors!=b.colors||a.references.size()!=b.references.size())return false;for(size_t i=0;i<a.references.size();++i)if(a.references[i].record!=b.references[i].record||a.references[i].entries!=b.references[i].entries||a.references[i].name!=b.references[i].name)return false;return true;}
}
bool plan_batch_delete(const Artwork &art,const std::vector<uint64_t> &ids,BatchDeletePlan &out,std::string &error,bool include_children){
    out={};error.clear();SpriteScope scope;if(!review_sprite_scope(art,ids,include_children,scope,error))return false;
    std::set<uint64_t> seen;for(auto &row:scope.rows)seen.insert(row.sprite);
    std::vector<EmbeddedRecord> records;if(!read_embedded(art,records,error))return false;
    std::map<int,std::vector<BatchDeleteReference>> refs;for(int r=0;r<(int)records.size();++r){auto &record=records[r];if(record.script)continue;std::map<int,int> counts;for(auto &entry:record.entries){int index=entry.values.index;if(index<0)continue;if(index>=art.count()){error="An embedded sequence has an unresolved sprite index. Repair it before deleting sprites.";return false;}++counts[index];}for(auto &entry:counts)refs[entry.first].push_back({record.name,r,entry.second});}
    BatchDeletePlan p;p.scope=std::move(scope);p.owner=&art;p.generation=art.generation();p.sequences=art.data.seqcnt;p.scripts=art.data.scrcnt;if(art.data.scrseqbytes)p.embedded.assign((const unsigned char *)art.data.scrseqmem_p,(const unsigned char *)art.data.scrseqmem_p+art.data.scrseqbytes);size_t total=0;
    try{for(int i=0;i<art.count();++i){auto id=art.sprite_id(i);if(!seen.count(id))continue;auto im=art.sprite(i);BatchDeleteRow row;row.sprite=id;row.index=i;row.w=im->w;row.h=im->h;row.name=im->n_s;memcpy(row.fields.data(),im,sizeof(IMG));if(!im->data_p||row.w<1||row.h<1||row.w>8192||row.h>8192||(size_t)row.w*row.h>4*1024*1024){error="A sprite has invalid dimensions or pixel data.";return false;}size_t size=(((size_t)row.w+3)&~size_t(3))*row.h;total+=size;if(total>128*1024*1024){error="The deletion preview exceeds its 128 MiB pixel limit.";return false;}row.pixels.assign((const unsigned char *)im->data_p,(const unsigned char *)im->data_p+size);auto pal=art.palette(im->palnum);if(pal&&pal->data_p&&pal->numc>=1&&pal->numc<=256)row.colors=copy_palette(art,im->palnum).words;row.references=refs[i];p.rows.push_back(std::move(row));}}
    catch(const std::bad_alloc &){error="Not enough memory for the deletion review.";return false;}out=std::move(p);return true;
}
std::vector<std::string> batch_delete_scene_uses(const BatchDeletePlan &p,uint64_t document,const SceneState &scene,const std::string &name){
    std::vector<std::string> uses(p.rows.size());std::map<uint64_t,size_t> rows;for(size_t i=0;i<p.rows.size();++i)rows[p.rows[i].sprite]=i;
    for(auto &lane:scene.lanes)for(auto &frame:lane.frames)for(auto &ref:frame.pieces){if(ref.asset<0||ref.asset>=(int)scene.assets.size()||scene.assets[ref.asset].runtime_doc!=document)continue;auto found=rows.find(ref.runtime_id);if(found!=rows.end()&&uses[found->second].empty())uses[found->second]=name+" / "+lane.name+" / "+frame.name;}return uses;
}
int Artwork::apply_batch_delete(const BatchDeletePlan &p,std::string &error){
    error.clear();if(pending){error="Finish the current edit before deleting sprites.";return -1;}if(p.owner!=this||p.generation!=generation()){error="The library changed. Cancel and reopen the deletion review.";return -1;}
    BatchDeletePlan checked;if(!plan_batch_delete(*this,p.scope.requested,checked,error,p.scope.include_children))return -1;
    if(!(p.scope==checked.scope)||p.sequences!=checked.sequences||p.scripts!=checked.scripts||p.embedded!=checked.embedded||p.rows.size()!=checked.rows.size()){error="Embedded data or the deletion scope changed after review.";return -1;}
    std::set<int> remove;for(size_t i=0;i<p.rows.size();++i){auto &r=p.rows[i];if(!equal_row(r,checked.rows[i])){error="The deletion review no longer matches its source.";return -1;}if(!r.enabled)continue;if(!r.references.empty()){error=r.name+" is used by embedded sequence '"+r.references[0].name+"'. Remove the reference or exclude this sprite.";return -1;}remove.insert(r.index);}
    if(remove.empty())return 0;if(remove.size()>=(size_t)count()){error="Keep at least one sprite in an IMG library.";return -1;}
    std::vector<int> mapping(count(),-1);std::vector<IMG *> kept,deleted;std::vector<uint64_t> retained;int first=std::max(0,sprite_index(selected_sprite_id));for(int i=0;i<count();++i){auto im=sprite(i);if(remove.count(i)){deleted.push_back(im);continue;}mapping[i]=(int)kept.size();kept.push_back(im);retained.push_back(sprite_ids[i]);}
    std::vector<unsigned char> blob;if(!remap_sequence_indices(mapping,blob,error))return -1;
    begin("Batch delete sprites");data.img_p=kept.front();for(size_t i=0;i<kept.size();++i)kept[i]->nxt_p=i+1<kept.size()?kept[i+1]:nullptr;for(auto im:deleted)FreeImg(im);data.imgcnt=(unsigned short)kept.size();sprite_ids=std::move(retained);
    if(sprite_index(selected_sprite_id)<0){int next=std::min(first,count()-1);for(int old=first;old<(int)mapping.size();++old)if(mapping[old]>=0){next=mapping[old];break;}selected_sprite_id=sprite_ids[next];}
    if(!blob.empty())memcpy(data.scrseqmem_p,blob.data(),blob.size());++serial;++structure_serial;commit();return (int)deleted.size();
}
}
