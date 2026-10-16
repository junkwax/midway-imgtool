#include "studio_layers.h"
#include "studio_selection_transform.h"
#include "studio_legacy_media.h"
#include "studio_palette.h"
#include "studio_palette_multi_merge.h"
#include "studio_batch.h"
#include "studio_batch_palette.h"
#include "studio_variant.h"
#include "studio_img_codec.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <climits>
#include <new>
static int failures=0;static long fail_after=-1;
void *operator new(size_t n){if(fail_after==0)throw std::bad_alloc();if(fail_after>0)--fail_after;if(auto p=malloc(n?n:1))return p;throw std::bad_alloc();}
void *operator new[](size_t n){return ::operator new(n);}
void operator delete(void *p) noexcept{free(p);}void operator delete[](void *p) noexcept{free(p);}
void operator delete(void *p,size_t) noexcept{free(p);}void operator delete[](void *p,size_t) noexcept{free(p);}
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " ("<<error<<")\n";++failures;}}while(0)
static std::vector<unsigned char> bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(){using namespace studio;namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-layer-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto a=Artwork::demo();int i=a->add_sprite("OVERLAY",5,4,0,error);CHECK(i>=0);auto im=a->sprite(i);memset(im->data_p,8,32);im->anix=19;im->aniy=(unsigned short)-7;im->flags=0x80;
    auto original=a->copy(i,0,0,5,4);auto file=root/"base.IMG";CHECK(a->save(file.u8string(),error));auto base_file=bytes(file);auto identity=a->sprite_id(i);
    Clipboard c;c.w=3;c.h=2;c.colors=original.colors;c.pixels={0,9,10,11,12,13};c.mask={1,1,1,1,0,1};
    CHECK(a->paste_layer(i,c,1,1,error)&&a->dirty());CHECK(a->copy(i,0,0,5,4).pixels==original.pixels&&visible_pixel(*im,1,1)==8&&visible_pixel(*im,2,1)==9&&visible_pixel(*im,2,2)==8&&visible_pixel(*im,3,2)==13);
    CHECK(im->anix==19&&(short)im->aniy==-7&&im->flags==0x80&&a->sprite_id(i)==identity);
    auto n=checked_layer_bytes(*im);CHECK(n==sizeof(SpriteLayer)+8);std::vector<unsigned char> overlay((unsigned char *)im->layer_p,(unsigned char *)im->layer_p+n);auto gen=a->generation();
    CHECK(!a->paste_layer(i,c,0,0,error)&&a->generation()==gen);
    // Save flattens only the output; loaded IMG has no layer, live base is intact.
    auto saved=root/"visible.IMG";CHECK(a->save(saved.u8string(),error)&&!a->dirty());CHECK(a->copy(i,0,0,5,4).pixels==original.pixels&&memcmp(im->layer_p,overlay.data(),n)==0);
    auto reopened=Artwork::open(saved.u8string(),error);CHECK(reopened&&!reopened->sprite(i)->layer_p&&reopened->disk_version()==a->disk_version());if(reopened)for(int y=0;y<4;++y)for(int x=0;x<5;++x)CHECK(visible_pixel(*im,x,y)==visible_pixel(*reopened->sprite(i),x,y));
    CHECK(a->edit_layer(i,-1,2,true,true,false,error));CHECK(img_layer(im)->x==-1&&layer_pixels(img_layer(im))[0]==10&&visible_pixel(*im,0,2)==9&&visible_pixel(*im,1,2)==8);
    a->undo();im=a->sprite(i);CHECK(memcmp(im->layer_p,overlay.data(),n)==0&&!a->dirty());a->redo();im=a->sprite(i);CHECK(img_layer(im)->x==-1&&a->dirty());a->undo();im=a->sprite(i);
    CHECK(a->edit_layer(i,1,1,false,false,false,error)&&visible_pixel(*im,2,1)==8);CHECK(!a->flatten_layer(i,error));CHECK(a->save((root/"hidden.IMG").u8string(),error)&&bytes(root/"hidden.IMG")==base_file);CHECK(im->layer_p&&!img_layer(im)->visible);a->undo();im=a->sprite(i);CHECK(img_layer(im)->visible);
    // Duplicate owns an independent layer; removing either remains undoable.
    int copy=a->duplicate_sprite(i,error);CHECK(copy>=0&&a->sprite(copy)->layer_p!=im->layer_p&&memcmp(a->sprite(copy)->layer_p,overlay.data(),n)==0);a->undo();im=a->sprite(i);
    CHECK(a->remove_layer(i,error)&&!im->layer_p&&a->copy(i,0,0,5,4).pixels==original.pixels);a->undo();im=a->sprite(i);CHECK(im->layer_p);a->redo();CHECK(!a->sprite(i)->layer_p);a->undo();im=a->sprite(i);
    // Image exchange sees visible art; hidden overlays never leak into output.
    IndexedMedia media;CHECK(snapshot_indexed_sprite(*a,i,media,error)&&media.pixels[7]==9&&media.pixels[6]==8);
    CHECK(a->export_png(i,(root/"visible.png").u8string(),error)&&fs::file_size(root/"visible.png")>0);
    // Geometry, index allocation and source palette remapping require flattening.
    CHECK(!a->resize_sprite(i,6,4,false,false,error));CHECK(!a->canvas_size(i,6,4,0,error));CHECK(!a->trim_sprite(i,error));CHECK(!a->transform_sprite(i,Transform::FlipHorizontal,false,error));
    int target=a->duplicate_palette(0,error);CHECK(target>=0);CHECK(!a->assign_palette(i,target,true,error));
    PaletteMergeOptions mo;mo.source=0;mo.target=target;PaletteMergePlan mp;CHECK(!plan_palette_merge(*a,mo,mp,error));
    PaletteCompactionOptions co;co.palette=0;PaletteCompactionPlan cp;CHECK(!plan_palette_compaction(*a,co,cp,error));
    BatchGeometryOptions bo;BatchGeometryPlan bp;CHECK(!plan_batch_geometry(*a,{identity},bo,bp,error));
    BatchPaletteOptions po;po.target=target;BatchPalettePlan pp;CHECK(plan_batch_palette(*a,{identity},po,pp,error)&&!pp.rows[0].error.empty()&&a->apply_batch_palette(pp,true,error)<0);
    SelectionTransform s;CHECK(begin_selection_transform(*a,i,rectangle_selection(5,4,0,0,2,2),s,error));
    s.x=3;s.y=2;SelectionTransformPreview base_preview;CHECK(preview_selection_transform(s,base_preview,error));
    CHECK(a->apply_selection_transform(s,error)&&a->copy(i,0,0,5,4).pixels==base_preview.pixels&&memcmp(im->layer_p,overlay.data(),n)==0);
    CHECK(visible_pixel(*im,2,1)==9&&overlay_pixel(*im,1,1,13)==13&&overlay_pixel(*im,2,1,13)==9);
    a->undo();im=a->sprite(i);CHECK(a->copy(i,0,0,5,4).pixels==original.pixels&&memcmp(im->layer_p,overlay.data(),n)==0);
    auto before_check=a->copy(i,0,0,5,4).pixels;auto layer_address=im->layer_p;auto pixel_address=im->data_p;a->check(6);CHECK(im->layer_p==layer_address&&im->data_p==pixel_address&&a->copy(i,0,0,5,4).pixels==before_check);
    CHECK(!a->save((root/"missing"/"fail.IMG").u8string(),error)&&im->layer_p==layer_address&&im->data_p==pixel_address);
    CHECK(a->flatten_layer(i,error)&&!im->layer_p&&a->copy(i,0,0,5,4).pixels[7]==9&&a->copy(i,0,0,5,4).pixels[6]==8);a->undo();im=a->sprite(i);CHECK(im->layer_p&&a->copy(i,0,0,5,4).pixels==original.pixels);a->redo();CHECK(!a->sprite(i)->layer_p);a->undo();
    // Invalid, empty and stale clipboard requests leave both bytes and history.
    gen=a->generation();CHECK(!a->edit_layer(i,INT_MAX,0,true,false,false,error)&&a->generation()==gen);
    CHECK(a->remove_layer(i,error));gen=a->generation();auto bad=c;bad.pixels[0]=255;CHECK(!a->paste_layer(i,bad,0,0,error)&&a->generation()==gen);bad=c;bad.mask.resize(1);CHECK(!a->paste_layer(i,bad,0,0,error));bad=c;bad.pixels.assign(6,0);CHECK(!a->paste_layer(i,bad,0,0,error));CHECK(!a->paste_layer(i,c,INT_MIN,0,error));
    CHECK(begin_clipboard_transform(*a,i,c,s,error));s.x=1;s.y=1;s.flip_h=true;s.rotation=2;CHECK(a->paste_layer(s,error));CHECK(!a->paste_layer(s,error));a->undo();CHECK(!a->sprite(i)->layer_p);auto foreign=Artwork::demo();CHECK(!foreign->paste_layer(s,error));
    // Explicit transparency in a transformed paste is transparent on a layer,
    // unlike Apply, where covered zero clears the base.
    CHECK(begin_clipboard_transform(*a,i,c,s,error));s.x=1;s.y=1;CHECK(a->paste_layer(s,error));CHECK(visible_pixel(*a->sprite(i),1,1)==8&&visible_pixel(*a->sprite(i),2,1)==9);a->undo();
    // Palette matching preserves opaque black and zeros masked holes.
    auto swapped=c;std::swap(swapped.colors[9],swapped.colors[10]);CHECK(a->paste_layer(i,swapped,0,0,error)&&layer_pixels(img_layer(a->sprite(i)))[1]==10);a->undo();
    // Allocation failures at every staging/history point leave original state.
    int rejected=0,succeeded=0;
    for(int fail=0;fail<110;++fail){auto doc=Artwork::demo();auto clip=doc->copy(0,0,0,4,4);clip.pixels.assign(16,9);auto original_pixels=doc->copy(0,0,0,48,64).pixels;auto g=doc->generation();bool ok=false;fail_after=fail;
        try{ok=doc->paste_layer(0,clip,2,3,error);}catch(const std::bad_alloc &){}fail_after=-1;
        if(ok){++succeeded;doc->undo();CHECK(!doc->sprite(0)->layer_p&&doc->copy(0,0,0,48,64).pixels==original_pixels);}else{++rejected;CHECK(!doc->sprite(0)->layer_p&&doc->generation()==g&&!doc->dirty());}
    }CHECK(rejected>10&&succeeded>0);
    // Two-layer save preparation cannot detach the first layer while allocating
    // the second. Failed flattening cannot change pixels or consume history.
    for(int fail=0;fail<90;++fail){auto doc=Artwork::demo();auto clip=doc->copy(0,0,0,3,2);clip.pixels.assign(6,9);CHECK(doc->paste_layer(0,clip,0,0,error)&&doc->paste_layer(1,clip,0,0,error));
        auto im0=doc->sprite(0),im1=doc->sprite(1);void *data0=im0->data_p,*data1=im1->data_p,*layer0=im0->layer_p,*layer1=im1->layer_p;auto base=doc->copy(0,0,0,48,64).pixels;auto g=doc->generation();
        fail_after=fail;try{LayerCompositeScope scope(doc->data);}catch(const std::bad_alloc &){}fail_after=-1;
        CHECK(im0->data_p==data0&&im1->data_p==data1&&im0->layer_p==layer0&&im1->layer_p==layer1&&doc->generation()==g&&doc->copy(0,0,0,48,64).pixels==base);
        bool ok=false;fail_after=fail;try{ok=doc->flatten_layer(0,error);}catch(const std::bad_alloc &){}fail_after=-1;
        if(ok){doc->undo();CHECK(doc->sprite(0)->layer_p&&doc->copy(0,0,0,48,64).pixels==base);}else{CHECK(im0->layer_p==layer0&&doc->generation()==g&&doc->copy(0,0,0,48,64).pixels==base);}
    }
    // Modern and near-legacy layouts retain their own version and reserved words.
    auto seed=Artwork::demo();CHECK(seed->save((root/"seed.IMG").u8string(),error));auto seedbytes=bytes(root/"seed.IMG");
    for(unsigned version:{0x60au,0x61cu,0x61du,0x631u,0x634u}){auto b=seedbytes;if(version<0x634)CHECK(project_img_layout(b,version,error));auto path=root/(std::to_string(version)+".IMG");{std::ofstream f(path,std::ios::binary);f.write((char *)b.data(),b.size());}auto doc=Artwork::open(path.u8string(),error);CHECK(doc);if(!doc)continue;auto clip=doc->copy(0,0,0,3,2);clip.pixels.assign(6,9);CHECK(doc->paste_layer(0,clip,0,0,error)&&doc->save(path.u8string(),error));auto disk=Artwork::open(path.u8string(),error);CHECK(disk&&disk->disk_version()==version&&!disk->sprite(0)->layer_p&&disk->copy(0,0,0,1,1).pixels[0]==9&&doc->sprite(0)->layer_p);doc->undo();CHECK(doc->save(path.u8string(),error)&&bytes(path)==b);}
    CHECK(g_doc==nullptr);fs::remove_all(root);return failures?1:0;
}
