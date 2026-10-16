#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
namespace studio {
enum class FatalityKind {Command,Combo,Animation};
struct FatalityBlock {
    FatalityKind kind=FatalityKind::Animation;
    std::string label,summary,combo;
    int first=0,last=0;bool body_ending=false;
};
struct FatalityPatch {
    const class FatalitySource *owner=nullptr;uint64_t generation=0;
    int block=-1;std::string body,before,after;bool changed=false;
};
struct FatalityWriteReview {
    const class FatalitySource *owner=nullptr;uint64_t generation=0;
    std::string path,bytes,previous;bool existed=false;
};
// One source file, preserving untouched bytes. The legacy parser is used only
// for in-memory inspection; its file loading/saving routines are never called.
class FatalitySource {
public:
    static std::unique_ptr<FatalitySource> open(const std::string &path,std::string &error);
    const std::string &path()const{return source_path;}
    const std::string &bytes()const{return text;}
    const std::string &saved_bytes()const{return baseline;}
    const std::vector<FatalityBlock> &blocks()const{return parsed.blocks;}
    std::string header(int block)const;
    std::string body(int block)const;
    bool review(int block,const std::string &body,FatalityPatch &out,std::string &error)const;
    bool apply(const FatalityPatch &patch,std::string &error);
    bool undo(std::string &error);bool redo(std::string &error);
    bool can_undo()const{return !past.empty();}bool can_redo()const{return !future.empty();}
    bool dirty()const{return text!=baseline;}uint64_t generation()const{return serial;}
    bool review_save(const std::string &path,FatalityWriteReview &out,std::string &error)const;
    bool save(const FatalityWriteReview &review,std::string &error);
private:
    struct Line {size_t begin=0,end=0,next=0;};
    struct Parsed {std::vector<Line> lines;std::vector<FatalityBlock> blocks;};
    static bool parse(const std::string &text,Parsed &out,std::string &error);
    bool history(bool redo,std::string &error);
    void trim_history();
    std::string text,baseline,source_path;
    Parsed parsed;std::vector<std::string> past,future;uint64_t serial=1;
};
}
