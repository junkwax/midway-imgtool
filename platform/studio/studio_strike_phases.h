#pragma once
#include <memory>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace studio {
struct PhaseOptions {bool enabled=false,wolf=false;};
struct PhasePatch {
    const class StrikeSource *owner=nullptr;uint64_t generation=0;PhaseOptions options;
    std::string label,before,after;std::vector<std::array<std::string,2>> operands;
    std::vector<std::string> consumers;bool changed=false;
};
struct PhaseEntry {std::string threshold_raw,slot_raw;int threshold=0,slot=0,line=0;bool numeric=false;size_t position=0;};
struct PhaseList {std::string label,conditions,error;int line=0;std::vector<PhaseEntry> entries;};
struct PhaseCatalog {
    struct Data;std::shared_ptr<const Data> data;
    int scan_limit=0;bool limit_known=false;
    std::vector<PhaseList> lists;
};
struct PhasePick {
    bool valid=false,active=false,phased=false,fallback=false;
    int slot=-1,row=-1,line=0;
    size_t position=0;
    std::string character_table,base_record,record,directory,list,note,error;
};
bool read_strike_phases(const std::string &text,PhaseCatalog &out,std::string &error);
std::vector<std::string> phase_characters(const PhaseCatalog &catalog,const PhaseOptions &options,std::string &error);
std::string phase_slot_record(const PhaseCatalog &catalog,int character,int slot,const PhaseOptions &options,std::string &error);
PhasePick pick_strike_phase(const PhaseCatalog &catalog,int character,int base_slot,int remaining,const PhaseOptions &options);
// Default browsing target only; does not infer a move's runtime active window.
bool first_strike_phase(const PhaseCatalog &catalog,int &character,int &slot);
// Existing rows only; validate every active consumer for the explicit preview profile.
bool review_phase_edit(const StrikeSource &source,const std::string &label,
    const std::vector<std::array<std::string,2>> &operands,const PhaseOptions &options,PhasePatch &out,std::string &error);
}
