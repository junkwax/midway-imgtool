#include "studio_document.h"
#include "studio_animation.h"
#include "palette_math.h"
#include "stb_image.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static std::vector<unsigned char> bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static int index_word(const void *p,size_t offset){auto b=(const unsigned char *)p+offset;return b[0]|b[1]<<8;}
int main(){
    namespace fs=std::filesystem;std::string error;
    auto folder=fs::temp_directory_path()/("studio-editing-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(folder);
    auto art=studio::Artwork::demo();auto original=art->copy(0,0,0,48,64).pixels;uint64_t first_id=art->sprite_id(0);
    auto im=art->sprite(0);im->pttbl_p=calloc(1,40);((short *)im->pttbl_p)[0]=-1;((short *)im->pttbl_p)[1]=3;
    im->opaltbl_p=calloc(1,16);((unsigned char *)im->opaltbl_p)[3]=0x71;im->opals=0;
    im->anix2=17;im->aniy2=23;im->aniz2=4;memcpy(im->file_name_raw,im->n_s,16);im->file_name_raw[13]=0x76;
    int duplicate=art->duplicate_sprite(0,error);CHECK(duplicate==8&&art->count()==9);
    uint64_t copy_id=art->sprite_id(8);CHECK(copy_id!=first_id);
    auto groups=studio::infer_preview_sequences(*art);int sequence=-1,frame=-1;
    CHECK(studio::locate_preview_sprite(groups,8,sequence,frame));CHECK(groups[sequence].frames[frame].sprite==8&&groups[sequence].frames[frame].pieces.empty());
    CHECK(art->copy(8,0,0,48,64).pixels==original);CHECK(art->sprite(8)->pttbl_p!=art->sprite(0)->pttbl_p);
    CHECK(memcmp(art->sprite(8)->pttbl_p,art->sprite(0)->pttbl_p,40)==0);CHECK(art->sprite(8)->anix2==17);
    art->select_sprite(copy_id);art->undo();CHECK(art->count()==8&&art->sprite_index(copy_id)==-1&&!art->dirty());CHECK(art->selection_id()==first_id);
    art->redo();CHECK(art->sprite_index(copy_id)==8&&art->selection_id()==copy_id);
    CHECK(art->rename_sprite(8,"COPY",error));CHECK(!art->rename_sprite(8,"SENTINEL1",error));CHECK(!art->rename_sprite(8,"a name with spaces",error));
    art->undo();CHECK(std::string(art->sprite(8)->n_s)!="COPY");art->redo();CHECK(std::string(art->sprite(8)->n_s)=="COPY");
    CHECK(art->move_sprite(8,1,error));CHECK(art->sprite_index(copy_id)==1&&art->sprite_index(first_id)==0);
    CHECK(art->delete_sprite(1,error));CHECK(art->sprite_index(copy_id)==-1);art->undo();CHECK(art->sprite_index(copy_id)==1);art->undo();CHECK(art->sprite_index(copy_id)==8);
    // Geometry commands retain original packing baselines and round-trip all metadata.
    CHECK(art->resize_sprite(0,96,128,false,true,error));CHECK(art->sprite(0)->anix==48&&art->sprite(0)->aniy==120);
    CHECK(art->sprite(0)->anix2==34&&art->sprite(0)->aniy2==46);CHECK(((short *)art->sprite(0)->pttbl_p)[0]==-2);
    CHECK(art->check(6).break_count>0);art->undo();CHECK(art->sprite(0)->w==48&&art->sprite(0)->anix2==17);
    CHECK(art->copy(0,0,0,48,64).pixels==original);CHECK(art->sprite(0)->file_name_raw[13]==0x76);
    CHECK(art->canvas_size(0,64,80,7,error));CHECK(art->sprite(0)->anix==32&&art->sprite(0)->aniy==76);
    CHECK(((short *)art->sprite(0)->pttbl_p)[0]==7);art->undo();CHECK(art->sprite(0)->w==48&&art->sprite(0)->anix==24);
    CHECK(art->trim_sprite(0,error));auto trimmed=art->sprite(0);CHECK(trimmed->w<48&&trimmed->h<64);
    art->undo();CHECK(art->copy(0,0,0,48,64).pixels==original);
    CHECK(art->transform_sprite(0,studio::Transform::FlipHorizontal,true,error));CHECK(art->transform_sprite(0,studio::Transform::FlipHorizontal,true,error));
    CHECK(art->copy(0,0,0,48,64).pixels==original&&art->sprite(0)->anix==24&&art->sprite(0)->anix2==17);
    CHECK(art->transform_sprite(0,studio::Transform::RotateClockwise,false,error));CHECK(art->sprite(0)->w==64&&art->sprite(0)->h==48&&art->sprite(0)->anix==24);
    art->undo();CHECK(art->copy(0,0,0,48,64).pixels==original);
    CHECK(!art->resize_sprite(0,0,128,false,true,error));CHECK(!art->resize_sprite(0,8192,8192,false,true,error));
    int private_pal=art->duplicate_palette(0,error,0);CHECK(private_pal==1&&art->sprite(0)->palnum==1&&art->sprite(1)->palnum==0);
    art->undo();CHECK(art->palette_count()==1&&art->sprite(0)->palnum==0);art->redo();CHECK(art->palette_count()==2&&art->sprite(0)->palnum==1);
    CHECK(art->assign_palette(0,0,true,error));CHECK(art->copy(0,0,0,48,64).pixels==original);

    // Reorder only the image indices in sequences. Scripts still reference sequences.
    auto refs=studio::Artwork::demo();refs->data.seqcnt=1;refs->data.scrcnt=1;refs->data.scrseqbytes=232;refs->data.scrseqmem_p=calloc(1,232);
    auto blob=(unsigned char *)refs->data.scrseqmem_p;memcpy(blob,"WALK",4);blob[16]=0x40;blob[18]=1;blob[102]=0;
    blob[116+18]=1;blob[116+102]=0;blob[116+103]=0; // script index zero must remain zero.
    uint64_t source_id=refs->sprite_id(0);CHECK(refs->move_sprite(0,3,error));CHECK(refs->sprite_index(source_id)==3);
    CHECK(index_word(refs->data.scrseqmem_p,102)==3);CHECK(index_word(refs->data.scrseqmem_p,218)==0);
    CHECK(!refs->delete_sprite(3,error));CHECK(error.find("WALK")!=std::string::npos);
    CHECK(refs->delete_sprite(1,error));CHECK(index_word(refs->data.scrseqmem_p,102)==2);CHECK(refs->sprite_index(source_id)==2);
    refs->undo();CHECK(index_word(refs->data.scrseqmem_p,102)==3);refs->undo();CHECK(index_word(refs->data.scrseqmem_p,102)==0&&refs->sprite_index(source_id)==0&&!refs->dirty());
    auto out=(folder/"references.img").u8string();CHECK(refs->save(out,error));auto reopened=studio::Artwork::open(out,error);CHECK(reopened&&reopened->data.scrseqbytes==232);

    // Export transparency and opaque black; Unicode path support on Windows.
    auto fresh=studio::Artwork::create();CHECK(fresh->count()==1&&!fresh->can_undo());
    CHECK(!fresh->delete_sprite(0,error));fresh->paint(0,1,0,1);fresh->paint(0,2,0,15);fresh->commit();
    auto png=folder/fs::u8path(u8"sprite-\u00e9-\u65e5.png");CHECK(fresh->export_png(0,png.u8string(),error));
    auto encoded=bytes(png);int w,h,c;auto rgba=stbi_load_from_memory(encoded.data(),(int)encoded.size(),&w,&h,&c,4);
    CHECK(rgba&&w==64&&h==64);if(rgba){CHECK(rgba[3]==0);CHECK(rgba[4]==0&&rgba[5]==0&&rgba[6]==0&&rgba[7]==255);CHECK(rgba[11]==255);stbi_image_free(rgba);}
    auto imported=studio::Artwork::demo();int added=imported->import_png(png.u8string(),-1,error);CHECK(added==8&&imported->palette_count()==2);
    CHECK(imported->copy(8,0,0,1,1).pixels[0]==0);CHECK(imported->copy(8,1,0,1,1).pixels[0]!=0);
    uint64_t imported_id=imported->sprite_id(8);imported->undo();CHECK(imported->count()==8&&imported->palette_count()==1);imported->redo();CHECK(imported->sprite_id(8)==imported_id&&imported->palette_count()==2);
    added=imported->import_png(png.u8string(),0,error);CHECK(added==9&&imported->sprite(9)->palnum==0&&imported->palette_count()==2);
    CHECK(imported->copy(9,0,0,1,1).pixels[0]==0&&imported->copy(9,1,0,1,1).pixels[0]!=0);
    {std::ofstream bad(folder/"bad.png");bad<<"not a PNG";}
    int count=imported->count();CHECK(imported->import_png((folder/"bad.png").u8string(),-1,error)<0);CHECK(imported->count()==count);
    out=(folder/"edited.img").u8string();CHECK(art->save(out,error));reopened=studio::Artwork::open(out,error);CHECK(reopened&&reopened->count()==art->count());
    if(reopened){CHECK(reopened->sprite(0)->file_name_raw[13]==0x76);CHECK(reopened->sprite(0)->anix2==17);CHECK(memcmp(reopened->sprite(0)->pttbl_p,art->sprite(0)->pttbl_p,40)==0);}
    auto original_file=bytes(out);CHECK(!art->export_png(0,out,error));CHECK(bytes(out)==original_file);
    fs::remove_all(folder);CHECK(g_doc==nullptr);
    std::cout<<"Studio editing checks: "<<failures<<" failure(s)\n";return failures?1:0;
}
