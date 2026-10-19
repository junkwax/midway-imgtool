#include "studio_ramp_transfer.h"
#include "studio_palette.h"
#include "material_mask.h"
#include "palette_math.h"
#include <algorithm>
#include <set>
#include <cmath>
namespace studio {
namespace {
std::vector<TransferBlock> detect(const std::vector<unsigned short> &words,float split){
    auto params=MaterialSeedParamsDefault();params.tolerance_deg=split;PaletteRampBlock blocks[64];int n=DetectPaletteRampBlocks(words.data(),(int)words.size(),params,blocks,64);std::vector<TransferBlock> out;for(int i=0;i<n;++i)out.push_back({blocks[i].start,blocks[i].count});return out;
}
bool palette_ok(const PAL *p){return p&&p->data_p&&p->numc>=2&&p->numc<=256&&p->bitspix<=8;}
bool equal_row(const BatchPaletteRow &a,const BatchPaletteRow &b){return a.sprite==b.sprite&&a.changed==b.changed&&a.alternates==b.alternates&&a.packing_risk==b.packing_risk&&a.source==b.source&&a.w==b.w&&a.h==b.h&&a.index_changes==b.index_changes&&a.color_changes==b.color_changes&&a.name==b.name&&a.error==b.error&&a.fields==b.fields&&a.before==b.before&&a.after==b.after&&a.source_words==b.source_words;}
}
bool plan_ramp_transfer(const Artwork &art,const std::vector<uint64_t> &ids,const RampTransferOptions &o,RampTransferPlan &out,std::string &error){
    error.clear();auto src=art.palette(o.source),dst=art.palette(o.target);
    if(ids.empty()||ids.size()>2000||o.source==o.target||!palette_ok(src)||!palette_ok(dst)||o.ppp<0||o.ppp>8||(o.mode!=kPaletteTransferRelative&&o.mode!=kPaletteTransferLuma)||!std::isfinite(o.source_split)||!std::isfinite(o.target_split)||o.source_split<5||o.source_split>90||o.target_split<5||o.target_split>90){error="Choose sprites, two different palettes with opaque colors, split angles from 5 to 90, and valid shading/PPP settings.";return false;}
    try{
        std::set<uint64_t> seen;for(auto id:ids){auto index=art.sprite_index(id);if(index<0||!seen.insert(id).second||art.sprite(index)->palnum!=o.source){error="The transfer scope contains a missing/repeated sprite or a different source palette.";return false;}}
        RampTransferPlan p;p.options=o;p.preview.owner=&art;p.preview.generation=art.generation();p.preview.options={o.target,o.ppp,true};p.source_words=copy_palette(art,o.source).words;p.preview.target_words=copy_palette(art,o.target).words;memcpy(p.source_fields.data(),src,sizeof(PAL));memcpy(p.target_fields.data(),dst,sizeof(PAL));
        p.source_blocks=detect(p.source_words,o.source_split);p.target_blocks=detect(p.preview.target_words,o.target_split);p.pairs=o.pairs;
        if(p.pairs.empty()){p.pairs.resize(p.source_blocks.size(),-1);SuggestTransferPairs(p.source_words.data(),(int)p.source_words.size(),p.source_blocks.data(),(int)p.source_blocks.size(),p.preview.target_words.data(),(int)p.preview.target_words.size(),p.target_blocks.data(),(int)p.target_blocks.size(),p.pairs.data());}
        if(p.pairs.size()!=p.source_blocks.size()||std::any_of(p.pairs.begin(),p.pairs.end(),[&](int pair){return pair< -1||pair>=(int)p.target_blocks.size();})){error="Ramp pairings no longer match the detected blocks. Reset suggestions or reopen the review.";return false;}
        BuildPaletteTransferMap(p.source_words.data(),(int)p.source_words.size(),p.source_blocks.data(),(int)p.source_blocks.size(),p.preview.target_words.data(),(int)p.preview.target_words.size(),p.target_blocks.data(),(int)p.target_blocks.size(),p.pairs.data(),o.mode,p.mapping.data());
        if(p.mapping[0]!=0){error="The transfer map must preserve transparency.";return false;}for(size_t c=1;c<p.source_words.size();++c)if(!p.mapping[c]||p.mapping[c]>=p.preview.target_words.size()){error="The transfer map must keep every opaque slot in the destination palette.";return false;}
        size_t total=0;for(int i=0;i<art.count();++i){auto id=art.sprite_id(i);if(!seen.count(id))continue;auto im=art.sprite(i);BatchPaletteRow row;row.sprite=id;row.name=im->n_s;row.source=o.source;row.source_words=p.source_words;row.w=im->w;row.h=im->h;row.changed=true;memcpy(row.fields.data(),im,sizeof(IMG));std::vector<unsigned char> alternate;if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;alternate.assign(refs,refs+16);row.alternates=refs[0]<128;}
            if(!im->data_p||!row.w||!row.h||row.w>8192||row.h>8192||(size_t)row.w*row.h>4u*1024u*1024u){error="A sprite has invalid pixel data or dimensions.";return false;}int stride=(row.w+3)&~3;size_t size=(size_t)stride*row.h;total+=size*2;if(total>256u*1024u*1024u){error="The ramp preview exceeds its 256 MiB pixel budget. Use a smaller scope.";return false;}
            auto pixels=(const unsigned char *)im->data_p;row.before.assign(pixels,pixels+size);row.after=row.before;if(im->layer_p)row.error="Flatten or remove this sprite's overlay before transferring its palette.";
            for(int y=0;y<row.h;++y)for(int x=0;x<row.w;++x){size_t at=(size_t)y*stride+x;int c=pixels[at];if(c>=p.source_words.size()){row.error="Unresolved source color indices cannot be transferred. Exclude this sprite.";continue;}++p.usage[c];int mapped=p.mapping[c];row.after[at]=(unsigned char)mapped;row.index_changes+=c!=mapped;if(c)row.color_changes+=PaletteColorDistance5(p.source_words[c],p.preview.target_words[mapped])!=0;}
            if(row.error.empty()&&row.alternates&&row.index_changes)row.error="Remapping would change alternate-palette appearances. Exclude this sprite or resolve its alternate references.";
            if(!row.error.empty())row.after=row.before;row.packing_risk=o.ppp&&((p.source_words.size()>(size_t)(1<<o.ppp))!=(p.preview.target_words.size()>(size_t)(1<<o.ppp)));p.preview.rows.push_back(std::move(row));p.alternates.push_back(std::move(alternate));
        }
        std::array<bool,256> paired{};for(size_t b=0;b<p.source_blocks.size();++b){auto block=p.source_blocks[b];uint64_t used=0;for(int c=block.start;c<block.start+block.count;++c){used+=p.usage[c];if(p.pairs[b]>=0)paired[c]=true;}p.block_usage.push_back(used);}for(size_t c=1;c<p.source_words.size();++c)if(!paired[c])p.fallback_pixels+=p.usage[c];
        out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory for the ramp transfer review. The library is unchanged.";return false;}
}
int Artwork::apply_ramp_transfer(const RampTransferPlan &reviewed,bool allow_color_changes,std::string &error){
    error.clear();if(pending||reviewed.preview.owner!=this||reviewed.preview.generation!=generation()){error="The library changed or an edit is pending. Cancel and reopen the ramp transfer review.";return -1;}
    try{
        std::vector<uint64_t> ids;for(auto &r:reviewed.preview.rows)ids.push_back(r.sprite);RampTransferPlan p;if(!plan_ramp_transfer(*this,ids,reviewed.options,p,error))return -1;
        if(reviewed.source_fields!=p.source_fields||reviewed.target_fields!=p.target_fields||reviewed.source_words!=p.source_words||reviewed.preview.target_words!=p.preview.target_words||reviewed.alternates!=p.alternates||reviewed.preview.rows.size()!=p.preview.rows.size()||reviewed.pairs!=p.pairs||reviewed.mapping!=p.mapping){error="The ramp review no longer matches its palettes, pairings or source data. Cancel and reopen it.";return -1;}
        int count=0;for(size_t i=0;i<p.preview.rows.size();++i){auto &r=reviewed.preview.rows[i];if(!equal_row(r,p.preview.rows[i])){error="The ramp review no longer matches a sprite's pixels or metadata. Cancel and reopen it.";return -1;}if(!r.enabled)continue;if(!r.error.empty()){error=r.name+": "+r.error;return -1;}if(r.color_changes&&!allow_color_changes){error="Review the result and enable Allow color changes before applying.";return -1;}++count;}
        if(!count)return 0;
        State before=capture();before.label="Transfer palette by ramps";past.reserve(past.size()+1);
        for(const auto &r:reviewed.preview.rows)if(r.enabled){auto im=sprite(sprite_index(r.sprite));memcpy(im->data_p,r.after.data(),r.after.size());im->palnum=(unsigned short)p.options.target;}
        past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;trim_history();return count;
    }catch(const std::bad_alloc &){error="Not enough memory to transfer palettes. Pixels, assignments and history are unchanged.";return -1;}
}
}
