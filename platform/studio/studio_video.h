#pragma once
#include "studio_digitize.h"
#include "video_decode.h"
#include <atomic>
#include <set>
#include <thread>
namespace studio {
struct VideoSource {
    std::string path,ffmpeg;VideoInfo info;
    uintmax_t size=0;int64_t modified=0;
};
struct CaptureSettings {
    int first=0,last=0,step=2,hold=4;
    bool manual=false,deinterlace=false,source_timing=true,custom_floor=false;
    std::set<int> picks;VideoCrop crop;
    int floor_x=0,floor_y=0;
    std::string prefix="DIGI";
};
struct CapturePlan {
    std::vector<int> frames,ticks;VideoCrop crop;
    bool deinterlace=false,custom_floor=false;
    int floor_x=0,floor_y=0;std::string prefix;
};
bool is_video_path(const std::string &path);
bool open_video_source(const std::string &path,VideoSource &out,std::string &error,const std::function<bool()> &cancelled={});
bool video_source_matches(const VideoSource &source);
bool plan_video_capture(const VideoSource &source,const CaptureSettings &settings,CapturePlan &out,std::string &error);
bool capture_video(const VideoSource &source,const CapturePlan &plan,std::vector<DigitizeSource> &out,std::string &error,const std::function<bool()> &cancelled={},const std::function<void(int)> &progress={});
enum class VideoJobKind {Probe,Preview,Capture};
// Only atomics may be read until finished. All decoded data belongs to this job.
struct VideoJob {
    std::atomic<bool> cancel{false},finished{false};std::atomic<int> done{0};
    VideoJobKind kind=VideoJobKind::Probe;bool ok=false;int frame=0,preview_w=0,preview_h=0;
    bool deinterlace=false;std::string error;
    VideoSource source;CapturePlan plan;
    std::vector<DigitizeSource> frames;std::vector<unsigned char> preview;
    std::thread thread;
    ~VideoJob();
    void start();
};
}
