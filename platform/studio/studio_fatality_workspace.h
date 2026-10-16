#pragma once
#include "studio_fatality.h"
namespace studio {
struct FatalityDocument {
    uint64_t id=0;std::shared_ptr<FatalitySource> source;uint64_t epoch=1;
};
struct FatalityMatch {
    uint64_t document=0,generation=0;int block=-1;
    FatalityKind kind=FatalityKind::Combo;std::string label;uint64_t epoch=0;
};
// Session-owned source files. Save/Undo remain per file; no implicit batch writes.
class FatalityWorkspace {
public:
    static constexpr size_t limit=16;
    const std::vector<FatalityDocument> &documents()const{return entries;}
    std::shared_ptr<FatalitySource> document(uint64_t id)const;
    uint64_t find_path(const std::string &path)const;
    uint64_t add(const std::string &path,std::string &error);
    uint64_t adopt(std::unique_ptr<FatalitySource> source,uint64_t replace,bool discard,std::string &error);
    bool close(uint64_t id,bool discard,std::string &error);
    uint64_t first_dirty()const;size_t dirty_count()const;
    std::vector<FatalityMatch> matches(const std::string &label,FatalityKind kind,uint64_t preferred)const;
    bool resolve(const FatalityMatch &match,std::shared_ptr<FatalitySource> &source,std::string &error)const;
    bool review_save(uint64_t id,const std::string &path,FatalityWriteReview &out,std::string &error)const;
    bool save(uint64_t id,const FatalityWriteReview &review,std::string &error);
private:
    bool destination_available(uint64_t id,const std::string &path,std::string &error)const;
    std::vector<FatalityDocument> entries;uint64_t next_id=1;
};
}
