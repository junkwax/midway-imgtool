#include "studio_points.h"
#include <cstdlib>
#include <cstring>
namespace studio {
bool read_point_table(const Artwork &art,uint64_t id,PointTableEdit &out,std::string &error){
    error.clear();auto im=art.sprite(art.sprite_index(id));if(!im){error="The sprite is missing.";return false;}
    PointTableEdit edit;edit.owner=&art;edit.generation=art.generation();edit.sprite=id;edit.existed=edit.enabled=im->pttbl_p!=nullptr;
    if(im->pttbl_p){memcpy(edit.original.data(),im->pttbl_p,40);for(int i=0;i<20;++i)edit.points[i]=(short)(edit.original[i*2]|(edit.original[i*2+1]<<8));}out=edit;return true;
}
bool apply_point_table(Artwork &art,const PointTableEdit &e,std::string &error){return art.apply_point_table(e,error);}
bool Artwork::apply_point_table(const PointTableEdit &e,std::string &error){
    Artwork &art=*this;
    error.clear();auto im=art.sprite(art.sprite_index(e.sprite));
    if(e.owner!=&art||e.generation!=art.generation()||!im||e.existed!=(im->pttbl_p!=nullptr)||(e.existed&&memcmp(e.original.data(),im->pttbl_p,40))){error="The sprite changed. Reopen its point table before applying.";return false;}
    if(art.disk_version()<0x60a){error="The original IMG layout has no point tables.";return false;}
    std::array<unsigned char,40> bytes{};for(int i=0;i<20;++i){if(e.points[i]<-32768||e.points[i]>32767){error="Point coordinates must fit signed 16-bit values.";return false;}bytes[i*2]=(unsigned char)e.points[i];bytes[i*2+1]=(unsigned char)((unsigned short)e.points[i]>>8);}
    if(e.enabled==e.existed&&(!e.enabled||bytes==e.original))return true;
    void *replacement=nullptr;if(e.enabled){replacement=malloc(40);if(!replacement){error="Could not allocate the point table.";return false;}memcpy(replacement,bytes.data(),40);}
    art.commit();try{art.begin(e.enabled?(e.existed?"Edit IMG point table":"Add IMG point table"):"Remove IMG point table");}catch(...){free(replacement);throw;}
    free(im->pttbl_p);im->pttbl_p=replacement;
    ++serial;art.commit();return true;
}
}
