#pragma once
#include "mk2_hitbox.h"
#include <array>
#include <memory>
namespace studio {
struct PhasePatch;
struct StrikeOperand {std::string raw;int64_t value=0;bool numeric=false;};
struct StrikeRow {
    std::string label,error;int line=0;
    std::array<StrikeOperand,8> fields;
    std::array<size_t,8> begin{},end{};
    std::vector<std::string> aliases;
    bool editable=false;
};
struct StrikePatch {
    const class StrikeSource *owner=nullptr;uint64_t generation=0;int record=-1;
    std::array<std::string,8> operands;
    std::string before,after;
    std::vector<std::string> aliases;
    bool changed=false;
};
struct StrikeWriteReview {
    const class StrikeSource *owner=nullptr;uint64_t generation=0;
    std::string path,bytes,previous;bool existed=false;
};
struct StrikeRect {int x=0,y=0,w=0,h=0;};
bool strike_numeric(const std::string &token,int field,int64_t &value);
std::string strike_literal(const std::string &original,int field,int64_t value);
bool strike_rect(const std::array<StrikeOperand,8> &fields,int x,int y,bool flip,StrikeRect &out);
class StrikeSource {
public:
    static std::unique_ptr<StrikeSource> open(const std::string &path,std::string &error);
    bool review(int record,const std::array<std::string,8> &operands,StrikePatch &out,std::string &error) const;
    bool apply(const StrikePatch &patch,std::string &error);
    bool apply_phase(const PhasePatch &patch,std::string &error);
    bool review_save(const std::string &path,StrikeWriteReview &out,std::string &error) const;
    bool save(const StrikeWriteReview &review,std::string &error);
    bool dirty() const {return text!=baseline;}
    bool can_undo() const {return !past.empty();}bool can_redo() const {return !future.empty();}
    void undo();void redo();uint64_t generation() const {return serial;}
    const std::string &path() const {return source_path;}
    const std::string &bytes() const {return text;}
    const std::string &saved_bytes() const {return baseline;}
    const std::vector<StrikeRow> &records() const {return rows;}
    const std::vector<mk2::CharTable> &characters() const {return tables;}
    const std::vector<std::string> &warnings() const {return notices;}
private:
    bool commit_bytes(const std::string &bytes,std::string &error);
    bool parse(const std::string &bytes,std::string &error);
    std::string text,baseline,source_path;
    std::vector<StrikeRow> rows;std::vector<mk2::CharTable> tables;std::vector<std::string> notices;
    std::vector<std::string> past,future;uint64_t serial=1;
};
}
