#include "studio_palette.h"
#include "studio_layers.h"
#include "palette_math.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static std::vector<unsigned char> bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const std::filesystem::path &p,const std::vector<unsigned char> &b){std::ofstream f(p,std::ios::binary);f.write((const char *)b.data(),b.size());}
int main(int argc,char **argv){
    using namespace studio;namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-palette-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    {
        const std::vector<unsigned short> colors={0xf123,0x0000,0x8000,0x7fff,0xffff,0x4210,0x801f};
        std::array<bool,256> mask{};mask[0]=mask[1]=mask[4]=mask[6]=mask[255]=true;
        auto inverted=palette_color_effect(colors,mask,PaletteColorEffect::Invert);
        CHECK(inverted==std::vector<unsigned short>({0xf123,0x7fff,0x8000,0x7fff,0x8000,0x4210,0xffe0}));
        CHECK(palette_color_effect(inverted,mask,PaletteColorEffect::Invert)==colors);
        auto reversed=palette_color_effect(colors,mask,PaletteColorEffect::Reverse);
        CHECK(reversed==std::vector<unsigned short>({0xf123,0x801f,0x8000,0x7fff,0xffff,0x4210,0x0000}));
        CHECK(palette_color_effect(reversed,mask,PaletteColorEffect::Reverse)==colors);
        mask[2]=true;reversed=palette_color_effect(colors,mask,PaletteColorEffect::Reverse);
        CHECK(reversed==std::vector<unsigned short>({0xf123,0x801f,0xffff,0x7fff,0x8000,0x4210,0x0000}));
        mask.fill(false);mask[0]=mask[255]=true;
        CHECK(palette_color_effect(colors,mask,PaletteColorEffect::Invert)==colors&&palette_color_effect(colors,mask,PaletteColorEffect::Reverse)==colors);
        mask[4]=true;CHECK(palette_color_effect(colors,mask,PaletteColorEffect::Reverse)==colors);
        CHECK(palette_color_effect({},mask,PaletteColorEffect::Invert).empty());
        CHECK(palette_color_effect(std::vector<unsigned short>(257),mask,PaletteColorEffect::Reverse).empty());
        CHECK(palette_color_effect(colors,mask,(PaletteColorEffect)99).empty());
        CHECK(palette_color_effect({0xbeef},mask,PaletteColorEffect::Reverse)==std::vector<unsigned short>({0xbeef}));
        auto fx=Artwork::demo();auto original=copy_palette(*fx,0).words;auto base=fx->copy(0,0,0,48,64).pixels;
        auto image=fx->sprite(0);image->opaltbl_p=malloc(16);memset(image->opaltbl_p,0xff,16);((unsigned char *)image->opaltbl_p)[0]=NUMDEFPAL;
        auto clip=fx->copy(0,0,0,2,2);clip.pixels.assign(4,8);CHECK(fx->paste_layer(0,clip,2,3,error));
        auto path=root/"effects.IMG";CHECK(fx->save(path.u8string(),error));auto exact=bytes(path);
        auto pal=fx->palette(0);auto metadata=*pal;auto buffer=pal->data_p;auto layer=image->layer_p;auto baseline=image->baseline_p;auto id=fx->sprite_id(0);auto generation=fx->generation();
        mask.fill(false);mask[2]=mask[8]=mask[15]=true;
        auto changed=palette_color_effect(original,mask,PaletteColorEffect::Reverse);
        CHECK(fx->generation()==generation&&!fx->dirty()&&copy_palette(*fx,0).words==original);
        CHECK(fx->replace_palette_colors(0,original,changed,error)&&fx->dirty()&&pal->data_p==buffer&&memcmp(pal,&metadata,sizeof(PAL))==0);
        CHECK(image->layer_p==layer&&image->baseline_p==baseline&&fx->sprite_id(0)==id&&image->palnum==0&&((unsigned char *)image->opaltbl_p)[0]==NUMDEFPAL&&fx->copy(0,0,0,48,64).pixels==base);
        CHECK(fx->save(path.u8string(),error));auto disk=Artwork::open(path.u8string(),error);CHECK(disk&&disk->disk_version()==0x634&&copy_palette(*disk,0).words==changed);
        fx->undo();CHECK(copy_palette(*fx,0).words==original&&fx->sprite(0)->layer_p&&fx->save(path.u8string(),error)&&bytes(path)==exact);fx->redo();CHECK(copy_palette(*fx,0).words==changed);fx->undo();
        generation=fx->generation();auto label=std::string(fx->undo_label());
        mask.fill(false);CHECK(fx->replace_palette_colors(0,original,palette_color_effect(original,mask,PaletteColorEffect::Invert),error)&&fx->generation()==generation&&std::string(fx->undo_label())==label);
    }
    auto art=Artwork::demo();auto pixels=art->copy(0,0,0,48,64).pixels;auto original=copy_palette(*art,0);CHECK(original.words.size()==16);CHECK(palette_usage(*art,0).primary==8);
    auto pal=art->palette(0);auto original_bpp=pal->bitspix;pal->flags=0x72;pal->pad=0x3421;pal->file_spare=0x7531;pal->file_colind=17;memcpy(pal->file_name_raw,pal->n_s,10);pal->file_name_raw[9]=0x55;
    auto file=root/"original.IMG";CHECK(art->save(file.u8string(),error));auto original_file=bytes(file);
    CHECK(art->rename_palette(0,original.name,error)&&!art->dirty());CHECK(art->palette(0)->file_name_raw[9]==0x55);
    CHECK(art->rename_palette(0,"RECOLOR",error)&&art->dirty());CHECK(palette_name(*art->palette(0))=="RECOLOR"&&art->palette(0)->file_spare==0x7531);art->undo();CHECK(!art->dirty()&&bytes(file)==original_file&&art->palette(0)->file_name_raw[9]==0x55);
    CHECK(!art->rename_palette(0,"TOO_LONG10",error)&&!art->dirty());CHECK(!art->rename_palette(0,"BAD NAME",error));
    std::array<bool,256> selected{};selected[0]=selected[8]=true;auto baseline=original.words;baseline[0]=0xf123;baseline[8]|=0x8000;
    auto adjusted=adjust_palette(baseline,selected,120,20,5);CHECK(adjusted[0]==baseline[0]&&adjusted[8]!=baseline[8]&&(adjusted[8]&0x8000));for(size_t i=1;i<baseline.size();++i)if(i!=8)CHECK(adjusted[i]==baseline[i]);
    CHECK(adjust_palette(baseline,selected,0,0,0)==baseline);selected.fill(false);selected[0]=true;CHECK(adjust_palette(baseline,selected,30,50,50)==baseline);
    selected.fill(true);auto bright=adjust_palette(original.words,selected,0,0,100);CHECK(bright[0]==original.words[0]);for(size_t i=1;i<bright.size();++i)CHECK((bright[i]&0x7fff)==0x7fff);
    CHECK(art->replace_palette_colors(0,original.words,original.words,error)&&!art->dirty());CHECK(art->replace_palette_colors(0,original.words,bright,error)&&art->dirty());CHECK(art->copy(0,0,0,48,64).pixels==pixels);CHECK(art->palette(0)->pad==0x3421&&art->palette(0)->bitspix==original_bpp); // Do not rewrite packing metadata.
    CHECK(!art->replace_palette_colors(0,original.words,bright,error));art->undo();CHECK(copy_palette(*art,0).words==original.words&&!art->dirty());art->redo();CHECK(copy_palette(*art,0).words==bright);art->undo();
    auto invalid=original.words;invalid[0]^=1;CHECK(!art->replace_palette_colors(0,original.words,invalid,error));invalid.pop_back();CHECK(!art->replace_palette_colors(0,original.words,invalid,error)&&!art->dirty());
    int copied=art->duplicate_palette(0,error,-1,"COPY");CHECK(copied==1&&copy_palette(*art,1).words==original.words);CHECK(art->palette(1)->flags==0x72&&art->palette(1)->pad==0x3421&&art->palette(1)->file_spare==0x7531);art->undo();CHECK(art->palette_count()==1&&!art->dirty());art->redo();CHECK(art->palette_count()==2);
    auto clip=copy_palette(*art,1);auto other=Artwork::demo();int pasted=other->add_palette("PASTED",clip.words,error);CHECK(pasted==1&&copy_palette(*other,1).words==clip.words&&other->palette(1)->bitspix==4);CHECK(other->sprite(0)->palnum==0); // Copy/paste never auto-assigns.
    CHECK(!other->delete_unused_palette(0,error));CHECK(other->delete_unused_palette(1,error)&&other->palette_count()==1);other->undo();CHECK(other->palette_count()==2);other->redo();CHECK(other->palette_count()==1);
    CHECK(other->add_palette("EMPTY",{},error)<0);CHECK(other->add_palette("BIG",std::vector<unsigned short>(257),error)<0);
    int one=other->add_palette("ONE",{0},error);CHECK(one==1);CHECK(!other->assign_palette(0,one,true,error)&&other->copy(0,0,0,48,64).pixels==pixels);CHECK(!other->assign_palette(0,one,false,error));
    // Alternate records store original disk palette indices, including three defaults.
    auto refs=Artwork::demo();CHECK(refs->add_palette("UNUSED",original.words,error)==1);CHECK(refs->add_palette("ALTERNATE",original.words,error)==2);auto im=refs->sprite(0);im->opaltbl_p=malloc(16);memset(im->opaltbl_p,0xee,16);auto alt=(unsigned char *)im->opaltbl_p;alt[0]=2;alt[1]=5;alt[2]=0xff;alt[3]=4;
    CHECK(palette_usage(*refs,2).alternate==1&&palette_usage(*refs,1).alternate==0);CHECK(!refs->delete_unused_palette(2,error));CHECK(refs->delete_unused_palette(1,error));CHECK(((unsigned char *)refs->sprite(0)->opaltbl_p)[1]==4&&((unsigned char *)refs->sprite(0)->opaltbl_p)[3]==4); // Terminator protects opaque trailing bytes.
    refs->undo();CHECK(refs->palette_count()==3&&((unsigned char *)refs->sprite(0)->opaltbl_p)[1]==5);refs->redo();CHECK(refs->palette_count()==2);CHECK(refs->save((root/"alternate.IMG").u8string(),error));auto reopened=Artwork::open((root/"alternate.IMG").u8string(),error);CHECK(reopened&&palette_usage(*reopened,1).alternate==1);refs->undo();
    ((unsigned char *)refs->sprite(0)->opaltbl_p)[1]=77;CHECK(!refs->delete_unused_palette(1,error)&&refs->palette_count()==3);
    // Removal also retains the primary palette identity after index shifts.
    auto ordered=Artwork::demo();CHECK(ordered->add_palette("AFTER",original.words,error)==1);for(int i=0;i<ordered->count();++i)CHECK(ordered->assign_palette(i,1,false,error));CHECK(ordered->delete_unused_palette(0,error));CHECK(ordered->sprite(7)->palnum==0&&palette_name(*ordered->palette(0))=="AFTER");ordered->undo();CHECK(ordered->sprite(7)->palnum==1&&ordered->palette_count()==2);
    // PAL is byte-exact RGB555; ACT converts RGB and retains its count/footer.
    PaletteTransfer transfer{"WORDS",{0x8001,0,0x001f,0x03e0,0x7c00,0x7fff}};auto raw=root/fs::u8path(u8"颜 色.PAL"),act=root/"colors.ACT";CHECK(write_palette_file(raw.u8string(),transfer,false,error));PaletteTransfer read;CHECK(read_palette_file(raw.u8string(),read,error)&&read.words==transfer.words);
    CHECK(write_palette_file(act.u8string(),transfer,true,error));CHECK(bytes(act).size()==772&&bytes(act)[768]==0&&bytes(act)[769]==6);CHECK(read_palette_file(act.u8string(),read,error)&&read.words.size()==6);for(size_t i=0;i<read.words.size();++i)CHECK(read.words[i]==(transfer.words[i]&0x7fff));
    auto flat=bytes(act);flat.resize(768);write(root/"flat.ACT",flat);CHECK(read_palette_file((root/"flat.ACT").u8string(),read,error)&&read.words.size()==256);
    auto full=PaletteTransfer{"FULL",std::vector<unsigned short>(256,0x7c00)};CHECK(write_palette_file((root/"full.ACT").u8string(),full,true,error));CHECK(bytes(root/"full.ACT")[768]==1);auto count0=bytes(root/"full.ACT");count0[768]=count0[769]=0;count0[770]=count0[771]=255;write(root/"count0.ACT",count0);CHECK(read_palette_file((root/"count0.ACT").u8string(),read,error)&&read.words.size()==256);
    auto bad=bytes(act);bad[771]=1;write(root/"bad.ACT",bad);auto retained=read.words;CHECK(!read_palette_file((root/"bad.ACT").u8string(),read,error)&&read.words==retained);bad[771]=0;bad[768]=2;write(root/"bad.ACT",bad);CHECK(!read_palette_file((root/"bad.ACT").u8string(),read,error));
    write(root/"odd.PAL",{1,2,3});CHECK(!read_palette_file((root/"odd.PAL").u8string(),read,error));write(root/"big.PAL",std::vector<unsigned char>(514));CHECK(!read_palette_file((root/"big.PAL").u8string(),read,error));write(root/"short.ACT",{1,2});CHECK(!read_palette_file((root/"short.ACT").u8string(),read,error));
    CHECK(!write_palette_file((root/"missing"/"output.PAL").u8string(),transfer,false,error));auto protected_bytes=bytes(raw);CHECK(!write_palette_file(raw.u8string(),{"BAD",{}},false,error)&&bytes(raw)==protected_bytes);CHECK(!write_palette_file(root.u8string(),transfer,false,error));
    CHECK(art->save(file.u8string(),error));art->undo();CHECK(art->palette_count()==1);CHECK(art->save(file.u8string(),error)&&bytes(file)==original_file);
    for(int i=1;i<argc;++i){auto real=Artwork::open(argv[i],error);CHECK(real!=nullptr);if(!real)continue;auto copy=root/("real-"+std::to_string(i)+".IMG");CHECK(real->save(copy.u8string(),error));auto before=bytes(copy);auto b=copy_palette(*real,0).words;auto changed=b;if(changed.size()>1)changed[1]^=0x7c00;CHECK(real->replace_palette_colors(0,b,changed,error));CHECK(real->save(copy.u8string(),error));auto loaded=Artwork::open(copy.u8string(),error);CHECK(loaded&&copy_palette(*loaded,0).words==changed);real->undo();CHECK(real->save(copy.u8string(),error)&&bytes(copy)==before);std::cout<<fs::u8path(argv[i]).filename().u8string()<<": palette edit/save/reopen/undo passed\n";}
    CHECK(g_doc==nullptr);fs::remove_all(root);return failures?1:0;
}
