#include "studio_animation.h"
#include "anipoint.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <unordered_map>

namespace studio {
namespace {
std::string upper(std::string name) {
    name = trim_sprite_name(name);
    for (char &c : name) c = (char)std::toupper((unsigned char)c);
    return name;
}
// Natural order puts WALK2 before WALK10, with equal numbers kept stable.
bool natural_less(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
            size_t ai = i, bj = j;
            while (i < a.size() && std::isdigit((unsigned char)a[i])) ++i;
            while (j < b.size() && std::isdigit((unsigned char)b[j])) ++j;
            while (ai < i && a[ai] == '0') ++ai;
            while (bj < j && b[bj] == '0') ++bj;
            if (i-ai != j-bj) return i-ai < j-bj;
            int comparison = a.compare(ai,i-ai,b,bj,j-bj);
            if (comparison) return comparison < 0;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i; ++j;
        }
    }
    return i == a.size() && j != b.size();
}
int hold_at(const std::vector<int> &holds, int i) { return std::clamp(holds[i],1,120); }
}

std::vector<PreviewSequence> infer_preview_sequences(const Artwork &art) {
    std::vector<std::string> names;
    std::unordered_map<std::string,int> by_name, candidate_counts;
    std::vector<std::string> parents;
    for (IMG *im=(IMG *)art.data.img_p; im; im=(IMG *)im->nxt_p) {
        names.push_back(upper(im->n_s));
        by_name.emplace(names.back(),(int)names.size()-1);
        parents.push_back(InferSubframeParentName(names.back().c_str()));
        if (!parents.back().empty()) ++candidate_counts[parents.back()];
    }
    std::map<std::string,PreviewFrame> frames;
    for (int i=0; i<(int)names.size(); ++i) {
        bool child = !parents[i].empty() &&
            (by_name.count(parents[i]) || candidate_counts[parents[i]] > 1 ||
             std::isdigit((unsigned char)parents[i].back()));
        std::string key = child ? parents[i] : names[i];
        auto found = frames.find(key);
        if (found == frames.end()) {
            PreviewFrame frame; frame.name = key;
            auto parent = by_name.find(key);
            frame.sprite = parent != by_name.end() ? parent->second : i;
            found = frames.emplace(key,std::move(frame)).first;
        }
        if (child) found->second.pieces.push_back(i);
    }
    std::map<std::string,PreviewSequence> grouped;
    std::vector<bool> included(names.size(),false);
    for (auto &entry:frames) {
        auto frame = std::move(entry.second);
        frame.sprite_id=art.sprite_id(frame.sprite);
        included[frame.sprite]=true;for(int i:frame.pieces)included[i]=true;
        std::string stem;
        if (!strip_trailing_sequence_digits(frame.name,&stem)) stem = frame.name;
        auto &sequence = grouped[stem]; sequence.name = stem;
        sequence.frames.push_back(std::move(frame));
    }
    // Duplicate visible names can have different raw name tails on disk.
    // Preserve each sprite in browsing even when name inference is ambiguous.
    for(int i=0;i<(int)names.size();++i)if(!included[i]){
        std::string stem;if(!strip_trailing_sequence_digits(names[i],&stem))stem=names[i];
        auto &sequence=grouped[stem];sequence.name=stem;
        sequence.frames.push_back({names[i],i,{},art.sprite_id(i)});
    }
    std::vector<PreviewSequence> result;
    for (auto &entry:grouped) {
        auto &sequence = entry.second;
        std::stable_sort(sequence.frames.begin(),sequence.frames.end(),
            [](const PreviewFrame &a,const PreviewFrame &b){return natural_less(a.name,b.name);});
        result.push_back(std::move(sequence));
    }
    return result;
}
bool locate_preview_sprite(const std::vector<PreviewSequence> &sequences, int sprite,
                           int &sequence, int &frame) {
    for (int s=0; s<(int)sequences.size(); ++s)
        for (int f=0; f<(int)sequences[s].frames.size(); ++f) {
            const auto &item=sequences[s].frames[f];
            if (item.sprite==sprite || std::find(item.pieces.begin(),item.pieces.end(),sprite)!=item.pieces.end()) {
                sequence=s; frame=f; return true;
            }
        }
    return false;
}
std::vector<int> preview_pieces(const PreviewFrame &frame, bool multipart) {
    return multipart && !frame.pieces.empty() ? frame.pieces : std::vector<int>{frame.sprite};
}
void PreviewPlayer::seek(int position,int count) {
    frame=count>0?std::clamp(position,0,count-1):0; elapsed_ticks=0;
}
void PreviewPlayer::step(int delta,int count) {
    playing=false;
    seek(count>0?((frame+delta)%count+count)%count:0,count);
}
void PreviewPlayer::advance(double seconds,const std::vector<int> &holds,double hz) {
    if (!playing || holds.empty() || seconds<=0 || !std::isfinite(seconds) || hz<=0 || !std::isfinite(hz)) return;
    frame=std::clamp(frame,0,(int)holds.size()-1);
    elapsed_ticks+=seconds*hz;
    // Skip complete loops so slow frames cannot create an unbounded catch-up.
    if(loop){double duration=0;for(int h:holds)duration+=std::clamp(h,1,120);
        elapsed_ticks=std::fmod(elapsed_ticks,duration);}
    while (elapsed_ticks>=hold_at(holds,frame)) {
        elapsed_ticks-=hold_at(holds,frame);
        if(frame+1<(int)holds.size())++frame;
        else if(loop)frame=0;
        else {playing=false;elapsed_ticks=0;break;}
    }
}
SpritePlacement place_sprite(const IMG &sprite,int x,int y,bool flip,MirrorConvention convention) {
    return {x-anipoint_effective((short)sprite.anix,sprite.w,flip,convention),
            y-(short)sprite.aniy,sprite.w,sprite.h};
}
void shift_frame_anchors(Artwork &art,const std::vector<int> &pieces,int dx,int dy) {
    art.begin("Align multipart frame");
    std::vector<int> unique=pieces;
    std::sort(unique.begin(),unique.end()); unique.erase(std::unique(unique.begin(),unique.end()),unique.end());
    for(int i:unique)if(auto im=art.sprite(i))art.anchor(i,(short)im->anix+dx,(short)im->aniy+dy);
}
}
