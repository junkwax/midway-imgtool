#include "studio_tbl_compare.h"
#include "anipoint.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
namespace studio {
namespace {std::string upper(std::string s){for(char &c:s)if(c>='a'&&c<='z')c-=32;return s;}}
bool compare_table(const Artwork &art,const std::vector<uint64_t> &ids,const std::string &path,TableComparison &out,std::string &error){
    error.clear();TableComparison report;report.owner=&art;report.generation=art.generation();report.path=path;
    std::ifstream in(std::filesystem::u8path(path),std::ios::binary|std::ios::ate);auto n=in?in.tellg():std::streampos(-1);
    if(n<0||n>4*1024*1024){error="Cannot read TBL file (4 MiB limit).";return false;}report.source_text.resize((size_t)n);in.seekg(0);
    if(n&&!in.read(report.source_text.data(),n)){error="Could not finish reading TBL file.";return false;}
    if(report.source_text.find('\0')!=std::string::npos){error="TBL comparison requires assembly text, not binary data.";return false;}
    std::string text=report.source_text;if(text.size()>=3&&(unsigned char)text[0]==0xef&&(unsigned char)text[1]==0xbb&&(unsigned char)text[2]==0xbf)text.erase(0,3);
    // Bound decimal words before the classic parser can truncate an overflow.
    std::istringstream lines(text);std::string line;int line_count=0;
    while(std::getline(lines,line)){if(++line_count>200000||line.size()>65536){error="TBL text exceeds the line limits.";return false;}auto comment=line.find(';');if(comment!=std::string::npos)line.resize(comment);
        auto directive=upper(line).find(".WORD");if(directive==std::string::npos)continue;std::istringstream fields(line.substr(directive+5));std::string token;
        while(std::getline(fields,token,',')){auto begin=token.find_first_not_of(" \t\r"),end=token.find_last_not_of(" \t\r");if(begin==std::string::npos)continue;token=token.substr(begin,end-begin+1);
            bool decimal=true;size_t start=token[0]=='-'||token[0]=='+'?1:0;if(start==token.size())decimal=false;for(size_t i=start;i<token.size();++i)if(token[i]<'0'||token[i]>'9')decimal=false;
            if(decimal){try{long long value=std::stoll(token);if(value<-32768||value>65535){error="A TBL word exceeds 16-bit range at line "+std::to_string(line_count)+".";return false;}}catch(...){error="An overflowing TBL word appears at line "+std::to_string(line_count)+".";return false;}}
        }
    }
    if(!ParseTblText(text,report.table,&report.warnings,&error))return false;
    if(report.table.size()>32768){error="The TBL has too many sprite records.";return false;}
    std::set<std::string> names;for(auto &entry:report.table){if(!names.insert(upper(entry.name)).second){error="Duplicate TBL sprite label: "+entry.name+". Comparison would be ambiguous.";return false;}
        if(entry.w<1||entry.h<1||entry.w>65535||entry.h>65535){error="Invalid TBL sprite dimensions: "+entry.name;return false;}
        entry.anix=(short)entry.anix;entry.aniy=(short)entry.aniy;entry.anix2=(short)entry.anix2;entry.aniy2=(short)entry.aniy2;entry.aniz2=(short)entry.aniz2;}
    names.clear();std::set<uint64_t> seen;for(auto id:ids){if(!seen.insert(id).second)continue;auto im=art.sprite(art.sprite_index(id));if(!im){error="A chosen IMG sprite is missing.";return false;}
        TblEntry e{};e.name=std::string(im->n_s,strnlen(im->n_s,16));if(!names.insert(upper(e.name)).second){error="Duplicate IMG sprite name: "+e.name+". Comparison would be ambiguous.";return false;}
        e.w=im->w;e.h=im->h;e.anix=(short)im->anix;e.aniy=(short)im->aniy;e.anix2=(short)im->anix2;e.aniy2=(short)im->aniy2;e.aniz2=(short)im->aniz2;e.has_secondary=secondary_anipoint_in_use(im);
        // Editor flags and saved byte offsets are not runtime DMA flags/SAGs.
        e.has_sag=false;e.has_flags=false;auto pal=art.palette(im->palnum);if(pal)e.pal_label=std::string(pal->n_s,strnlen(pal->n_s,16));e.pal_explicit=!e.pal_label.empty();
        report.images.push_back(e);report.sprites.push_back(id);}
    if(report.images.empty()){error="Choose at least one IMG sprite to compare.";return false;}
    DiffTblEntries(report.table,report.images,report.rows,&report.summary);report.report=FormatTblDiffReport(path,report.rows,report.summary);
    report.report+="\nCompared dimensions, animation anchors and palette labels. Runtime DMA flags and ROM SAG addresses are informational; IMG metadata cannot verify them.\n";
    out=std::move(report);return true;
}
}
