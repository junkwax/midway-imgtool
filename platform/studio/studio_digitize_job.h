#pragma once
#include "studio_digitize.h"
#include <atomic>
#include <thread>
namespace studio {
enum class DigitizeJobKind {Load,Matte,Fit,Reseed};
struct DigitizeJob {
    DigitizeJobKind kind=DigitizeJobKind::Matte;
    uint64_t owner=0,generation=0,epoch=0;
    std::unique_ptr<DigitizeSession> input,output;
    std::vector<std::string> paths;
    std::vector<unsigned short> target;
    std::atomic<bool> cancel{false},finished{false};
    std::atomic<int> done{0},total{1};std::atomic<DigitizeStage> stage{DigitizeStage::Matte};
    bool ok=false;std::string error;
    std::thread thread;
    ~DigitizeJob();
    void start();
    bool matches(uint64_t uid,uint64_t art_generation,uint64_t review_epoch)const;
};
const char *digitize_stage_label(DigitizeStage stage);
}
