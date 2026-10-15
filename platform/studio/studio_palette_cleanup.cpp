#include "studio_palette_cleanup.h"
#include "studio_palette.h"
#include <algorithm>
namespace studio {
namespace {
bool equal(const PaletteCleanupRow &a,const PaletteCleanupRow &b){return a.index==b.index&&a.target==b.target&&a.primary==b.primary&&a.alternate==b.alternate&&a.remove==b.remove&&a.name==b.name&&a.fields==b.fields&&a.words==b.words;}
}
bool plan_palette_cleanup(const Artwork &art,const std::set<int> &keep,PaletteCleanupPlan &out,std::string &error){
    out={};error.clear();if(art.palette_count()<1||art.palette_count()>4093||art.count()>2000){error="Choose a library with 1 to 4093 palettes and at most 2000 sprites.";return false;}
    for(int index:keep)if(index<0||index>=art.palette_count()){error="A palette selected to keep no longer exists.";return false;}
    PaletteCleanupPlan p;p.owner=&art;p.generation=art.generation();p.keep=keep;
    try{
        for(int i=0;i<art.palette_count();++i){auto pal=art.palette(i);if(!pal||!pal->data_p||pal->numc<1||pal->numc>256){error="A palette has invalid color data. Repair it before cleaning up.";return false;}PaletteCleanupRow row;row.index=i;row.name=palette_name(*pal);memcpy(row.fields.data(),pal,sizeof(PAL));row.words=copy_palette(art,i).words;p.rows.push_back(std::move(row));}
        for(int i=0;i<art.count();++i){auto im=art.sprite(i);if(im->palnum>=p.rows.size()){error="A sprite has an unresolved primary palette. Repair it before cleaning up.";return false;}++p.rows[im->palnum].primary;
            PaletteCleanupSprite sprite;sprite.id=art.sprite_id(i);memcpy(sprite.fields.data(),im,sizeof(IMG));if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;sprite.alternate.assign(refs,refs+16);for(int j=0;j<16&&refs[j]<128;++j){if(refs[j]<NUMDEFPAL)continue;int index=refs[j]-NUMDEFPAL;if(index>=(int)p.rows.size()){error="A sprite has an unresolved alternate palette. Repair it before cleaning up.";return false;}++p.rows[index].alternate;}}p.sprites.push_back(std::move(sprite));
        }
        int next=0;for(auto &row:p.rows){row.remove=!row.primary&&!row.alternate&&!keep.count(row.index);if(row.remove){++p.removed;p.color_bytes+=row.words.size()*2;}else row.target=next++;}
        for(auto &sprite:p.sprites){IMG im;memcpy(&im,sprite.fields.data(),sizeof im);p.primary_remaps+=p.rows[im.palnum].target!=im.palnum;for(auto ref:sprite.alternate){if(ref>=128)break;if(ref>=NUMDEFPAL)p.alternate_remaps+=p.rows[ref-NUMDEFPAL].target!=ref-NUMDEFPAL;}}
    }catch(const std::bad_alloc &){error="Not enough memory to review palette cleanup.";return false;}
    out=std::move(p);return true;
}
int Artwork::apply_palette_cleanup(const PaletteCleanupPlan &p,std::string &error){
    error.clear();if(pending){error="Finish the current edit before removing palettes.";return -1;}if(p.owner!=this||p.generation!=generation()){error="The library changed. Cancel and reopen palette cleanup.";return -1;}
    PaletteCleanupPlan checked;if(!plan_palette_cleanup(*this,p.keep,checked,error))return -1;
    if(p.removed!=checked.removed||p.primary_remaps!=checked.primary_remaps||p.alternate_remaps!=checked.alternate_remaps||p.color_bytes!=checked.color_bytes||p.rows.size()!=checked.rows.size()||p.sprites.size()!=checked.sprites.size()){error="The palette cleanup review changed. Reopen it before applying.";return -1;}
    for(size_t i=0;i<p.rows.size();++i)if(!equal(p.rows[i],checked.rows[i])){error="A palette no longer matches the cleanup review.";return -1;}
    for(size_t i=0;i<p.sprites.size();++i){auto &a=p.sprites[i];auto &b=checked.sprites[i];if(a.id!=b.id||a.fields!=b.fields||a.alternate!=b.alternate){error="Sprite palette references changed after review.";return -1;}}
    if(!p.removed)return 0;if(p.removed>=palette_count()){error="Keep at least one palette in the library.";return -1;}
    std::vector<PAL *> kept,removed;for(auto &row:p.rows)(row.remove?removed:kept).push_back(palette(row.index));
    begin("Remove unused palettes");data.pal_p=kept.front();for(size_t i=0;i<kept.size();++i)kept[i]->nxt_p=i+1<kept.size()?kept[i+1]:nullptr;data.palcnt=(unsigned short)kept.size();
    for(auto pal:removed)FreePal(pal);
    for(int i=0;i<count();++i){auto im=sprite(i);im->palnum=(unsigned short)p.rows[im->palnum].target;if(im->opaltbl_p){auto refs=(unsigned char *)im->opaltbl_p;for(int j=0;j<16&&refs[j]<128;++j)if(refs[j]>=NUMDEFPAL)refs[j]=(unsigned char)(p.rows[refs[j]-NUMDEFPAL].target+NUMDEFPAL);}}
    ++serial;++structure_serial;commit();return p.removed;
}
}
