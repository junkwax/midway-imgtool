#include "studio_tbl_compare.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static void write(const std::filesystem::path &p,const std::string &s){std::ofstream out(p,std::ios::binary);out<<s;}
int main(int argc,char **argv){using namespace studio;namespace fs=std::filesystem;auto art=Artwork::demo();std::string error;TableComparison report;
    auto root=fs::temp_directory_path()/("studio-compare-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);auto path=root/fs::u8path(u8"table \u65e5.TBL");
    std::string text="SENTINEL1:\n.word 48,64,24,60\n.long 03d27000H\n.word 06680H\n.long STUDIOPAL\nSENTINEL2:\n.word 48,64,23,59\nABSENT:\n.word 1,1,0,0\n";write(path,text);
    auto a=art->sprite_id(0),b=art->sprite_id(1),c=art->sprite_id(2),generation=art->generation();
    CHECK(compare_table(*art,{a,b,c},path.u8string(),report,error));CHECK(report.summary.matched==1&&report.summary.differing==1&&report.summary.only_in_tbl==1&&report.summary.only_in_img==1);
    CHECK(report.rows[0].fields.empty()&&report.rows[1].fields.size()==2&&report.sprites==std::vector<uint64_t>({a,b,c}));CHECK(!art->dirty()&&!art->can_undo()&&art->generation()==generation);
    CHECK(report.report.find("informational")!=std::string::npos&&!report.images[0].has_sag&&!report.images[0].has_flags);CHECK(compare_table(*art,{a,a},path.u8string(),report,error)&&report.images.size()==1);
    write(path,"\xef\xbb\xbf"+text);CHECK(compare_table(*art,{a,b,c},path.u8string(),report,error));CHECK(report.source_text.size()==text.size()+3);
    write(path,"BAD:\n.word x,0,0,0\n"+text);CHECK(compare_table(*art,{a},path.u8string(),report,error)&&!report.warnings.empty());
    write(path,text+"sentinel1:\n.word 48,64,24,60\n");CHECK(!compare_table(*art,{a},path.u8string(),report,error)&&error.find("Duplicate")!=std::string::npos);
    for(auto bad:{"X:\n.word 1,1,999999999999999999999,0\n","X:\n.word 0,1,0,0\n","X:\n.word 1,1,65536,0\n","not a table"}){write(path,bad);CHECK(!compare_table(*art,{a},path.u8string(),report,error));}
    write(path,text);CHECK(!compare_table(*art,{},path.u8string(),report,error));CHECK(!compare_table(*art,{0},path.u8string(),report,error));
    // Signed and unsigned 16-bit spellings compare as the same stored word.
    art->anchor(0,-1,-2);art->commit();write(path,"SENTINEL1:\n.word 48,64,65535,65534\n.long STUDIOPAL\n");CHECK(compare_table(*art,{a},path.u8string(),report,error)&&report.summary.matched==1);
    auto duplicate=art->sprite(1);memcpy(duplicate->n_s,art->sprite(0)->n_s,16);CHECK(!compare_table(*art,{a,b},path.u8string(),report,error));
    if(argc>1){auto fixture=fs::u8path(argv[1]);auto img=fixture/"data"/"MK1SKULL.IMG",tbl=fixture/"src"/"MK1SKULL.TBL";
        if(fs::exists(img)&&fs::exists(tbl)){auto real=Artwork::open(img.u8string(),error);CHECK(real);if(real){std::vector<uint64_t> ids;for(int i=0;i<real->count();++i)ids.push_back(real->sprite_id(i));
            CHECK(compare_table(*real,ids,tbl.u8string(),report,error));CHECK(report.summary.matched>0&&report.summary.differing==0&&!real->dirty());
            std::cout<<"MK1SKULL: "<<report.summary.matched<<" matched, "<<report.summary.differing<<" differing\n";}}}
    fs::remove_all(root);std::cout<<"TBL comparison: "<<failures<<" failures\n";return failures?1:0;
}
