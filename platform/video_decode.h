/*************************************************************
 * platform/video_decode.h
 *
 * Pull frames out of a video file (MP4, MOV, AVI, MKV, ...) for the
 * digitizing pipeline.
 *
 * Midway's fighters began as video tape: an actor performed a move against a
 * backdrop, the tape was frame-grabbed, and an artist picked the handful of
 * fields that would become the animation. This module is the frame grabber.
 * It does no keying, scaling, or palette work — those are digitize_matte's
 * and the digitize wizard's job — it only gets exact frames out as RGBA.
 *
 * Decoding is delegated to an ffmpeg executable run as a child process, with
 * frames streamed back over a pipe as PAM images. That keeps every codec
 * ffmpeg knows available without linking any of it into imgtool; the cost is
 * that ffmpeg must be installed (see VideoFindFfmpeg for where it is looked
 * for). Frame numbers are positions in the stream at the average frame rate,
 * and a seek to frame N returns frame N exactly — ffmpeg decodes forward from
 * the previous keyframe and drops everything before the requested time.
 *
 * No g_doc, no UI, no SDL. Safe to call from a worker thread.
 *************************************************************/
#ifndef PLATFORM_VIDEO_DECODE_H
#define PLATFORM_VIDEO_DECODE_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

struct VideoInfo {
    int    width = 0, height = 0;   /* as displayed: rotation metadata applied */
    double fps = 0.0;               /* average frame rate */
    int    frame_count = 0;         /* container's count, else duration * fps */
    double duration = 0.0;          /* seconds */
    int    rotation = 0;            /* degrees, as the container states it */
    std::string codec;
};

/* A rectangle in displayed-frame pixels. w or h <= 0 means the whole frame. */
struct VideoCrop {
    int x = 0, y = 0, w = 0, h = 0;
};

struct VideoDecodeOptions {
    VideoCrop crop;
    /* Interpolate out the comb between the two fields of interlaced footage
       (yadif, one output frame per input frame so frame numbers hold). Tape
       transfers and DV are interlaced; phone and camera footage is not. */
    bool deinterlace = false;
};

/* Called once per requested frame, in ascending frame order. `rgba` is w*h*4,
   straight alpha (always opaque for video), and is only valid for the call.
   Return false to stop decoding; VideoDecodeFrames then returns false with an
   empty error. */
typedef std::function<bool(int frame, const unsigned char *rgba, int w, int h)> VideoFrameSink;

/* Path to an ffmpeg executable, or "" if none was found. Looked for, in order:
   the IMGTOOL_FFMPEG environment variable (a full path), next to imgtool's
   own executable, on PATH, then the usual install locations. */
std::string VideoFindFfmpeg(void);

/* The ffprobe that ships beside `ffmpeg`. */
std::string VideoFfprobeFor(const std::string &ffmpeg);

/* Read dimensions, frame rate, and length. */
bool VideoProbe(const std::string &ffmpeg, const char *path, VideoInfo *out, std::string *err);

/* Decode exactly the frames listed in `frames` (any order; duplicates and
   out-of-range numbers are dropped) and hand each to `sink`. Nearby frames are
   decoded in one pass; a gap longer than a couple of seconds starts a fresh
   seek instead of decoding through footage nobody asked for. */
bool VideoDecodeFrames(const std::string &ffmpeg, const char *path, const VideoInfo &info,
                       const std::vector<int> &frames, const VideoDecodeOptions &opt,
                       const VideoFrameSink &sink, std::string *err);

/* ---- Pieces exposed for unit tests ------------------------------------ */

/* Fill `out` from ffprobe's `-of default=noprint_wrappers=1` key=value text.
   Returns false if no usable width/height/frame rate was found. */
bool VideoParseProbeOutput(const std::string &text, VideoInfo *out);

/* "30000/1001" -> 29.97. 0 for "0/0", "N/A", or garbage. */
double VideoParseRate(const char *s);

/* Parse a PAM (P7) header at the start of `buf`. On success sets the
   dimensions, channel depth, and the byte length of the header including the
   ENDHDR line. Returns false if the header is malformed or not yet complete. */
bool VideoParsePamHeader(const unsigned char *buf, size_t len,
                         int *w, int *h, int *depth, size_t *header_len);

/* The -vf filter chain for `opt` ("" when nothing applies). The crop is
   clamped to `frame_w` x `frame_h`. */
std::string VideoBuildFilter(const VideoDecodeOptions &opt, int frame_w, int frame_h);

/* Split sorted, unique frame numbers into runs worth decoding in one pass:
   a new run starts wherever the gap to the previous frame exceeds `max_gap`.
   Each run is [first, last] inclusive. */
std::vector<std::pair<int, int>> VideoPlanRuns(const std::vector<int> &sorted_frames, int max_gap);

/* Quote one argument for a Windows command line so the C runtime's parser
   hands it back unchanged (embedded quotes, trailing backslashes, spaces). */
std::string VideoQuoteWindowsArg(const std::string &arg);

#endif /* PLATFORM_VIDEO_DECODE_H */
