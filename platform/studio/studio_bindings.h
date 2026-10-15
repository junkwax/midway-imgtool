#pragma once
#include "strike_bind.h"
#include <memory>
#include <optional>
#include <cstdint>
namespace studio {
struct BindingsWriteReview {
    const class StrikeBindings *owner=nullptr;uint64_t generation=0;
    std::string path,bytes,previous;bool existed=false;
};
// Owned classic .imgtool sidecar. '-' is an explicit disabled association.
class StrikeBindings {
public:
    static std::unique_ptr<StrikeBindings> open_for(const std::string &strike_source,std::string &error);
    std::optional<std::string> lookup(const std::string &animation) const;
    bool set(const std::string &animation,const std::optional<std::string> &strike,std::string &error);
    bool review_save(BindingsWriteReview &out,std::string &error) const;
    bool save(const BindingsWriteReview &review,std::string &error);
    void undo();void redo();void discard();
    bool dirty() const {return text!=baseline;}
    bool can_undo() const {return !past.empty();}bool can_redo() const {return !future.empty();}
    uint64_t generation() const {return serial;}
    const std::string &path() const {return sidecar_path;}
    const std::string &source() const {return source_path;}
    const std::string &bytes() const {return text;}
    const std::string &saved_bytes() const {return baseline;}
    const std::vector<strike_bind::Binding> &bindings() const {return pairs;}
    const std::vector<std::string> &warnings() const {return notices;}
private:
    struct Row {std::string animation;size_t first=0,last=0,begin=0,end=0;bool editable=false;};
    bool parse(const std::string &bytes,std::string &error);
    void trim_history();
    std::string text,baseline,sidecar_path,source_path;
    bool existed=false;uint64_t serial=1;
    std::vector<Row> rows;std::vector<strike_bind::Binding> pairs;
    std::vector<std::string> notices,past,future;
};
}
