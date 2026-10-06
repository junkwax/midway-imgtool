/*************************************************************
 * test/video_decode_test.cpp
 *
 * Unit coverage for platform/video_decode.cpp. The parsers are tested
 * directly. When an ffmpeg is installed, a short clip whose every frame is a
 * different flat gray is generated and decoded back, which checks the one
 * property the digitizing workflow cannot do without: asking for frame N
 * returns frame N, not its neighbour. Without ffmpeg that half is skipped.
 *************************************************************/
#include "video_decode.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static void TestParsers(void)
{
    /* ---- Frame rates ---- */
    CHECK(VideoParseRate("30/1") == 30.0);
    CHECK(VideoParseRate("30000/1001") > 29.96 && VideoParseRate("30000/1001") < 29.98);
    CHECK(VideoParseRate("0/0") == 0.0);
    CHECK(VideoParseRate("N/A") == 0.0);
    CHECK(VideoParseRate("25") == 25.0);

    /* ---- Probe output: container count wins, stream duration first ---- */
    {
        VideoInfo vi;
        CHECK(VideoParseProbeOutput(
            "codec_name=h264\nwidth=720\nheight=480\nr_frame_rate=30000/1001\n"
            "avg_frame_rate=30000/1001\nduration=4.004000\nnb_frames=120\n"
            "duration=4.100000\n", &vi));
        CHECK(vi.width == 720 && vi.height == 480);
        CHECK(vi.frame_count == 120);
        CHECK(vi.duration > 4.0 && vi.duration < 4.01);
        CHECK(vi.codec == "h264");
    }
    /* No nb_frames (common for MKV/WebM): count from duration. Phone footage
       stored sideways reports coded size; the displayed size is swapped. */
    {
        VideoInfo vi;
        CHECK(VideoParseProbeOutput(
            "width=1920\r\nheight=1080\r\navg_frame_rate=30/1\r\nnb_frames=N/A\r\n"
            "duration=N/A\r\nrotation=-90\r\nduration=2.000000\r\n", &vi));
        CHECK(vi.width == 1080 && vi.height == 1920);
        CHECK(vi.frame_count == 60);
    }
    /* avg_frame_rate 0/0 falls back to r_frame_rate; no size is a failure. */
    {
        VideoInfo vi;
        CHECK(VideoParseProbeOutput("width=64\nheight=48\navg_frame_rate=0/0\nr_frame_rate=24/1\n", &vi));
        CHECK(vi.fps == 24.0);
        CHECK(!VideoParseProbeOutput("avg_frame_rate=24/1\n", &vi));
    }

    /* ---- PAM headers ---- */
    {
        const char *hdr = "P7\nWIDTH 8\nHEIGHT 6\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
        int w = 0, h = 0, d = 0;
        size_t len = 0;
        CHECK(VideoParsePamHeader((const unsigned char *)hdr, std::strlen(hdr), &w, &h, &d, &len));
        CHECK(w == 8 && h == 6 && d == 4 && len == std::strlen(hdr));
        /* Incomplete: no ENDHDR yet. */
        CHECK(!VideoParsePamHeader((const unsigned char *)hdr, 20, &w, &h, &d, &len));
        /* 16-bit samples are not what was asked for. */
        const char *deep = "P7\nWIDTH 8\nHEIGHT 6\nDEPTH 4\nMAXVAL 65535\nENDHDR\n";
        CHECK(!VideoParsePamHeader((const unsigned char *)deep, std::strlen(deep), &w, &h, &d, &len));
        CHECK(!VideoParsePamHeader((const unsigned char *)"P6\n", 3, &w, &h, &d, &len));
    }

    /* ---- Filter chain ---- */
    {
        VideoDecodeOptions o;
        CHECK(VideoBuildFilter(o, 640, 480).empty());
        o.crop = { 10, 20, 100, 200 };
        CHECK(VideoBuildFilter(o, 640, 480) == "crop=100:200:10:20");
        o.deinterlace = true;
        CHECK(VideoBuildFilter(o, 640, 480) == "yadif=mode=0,crop=100:200:10:20");
        /* Clamped to the frame; a crop of the whole frame is no crop. */
        o.deinterlace = false;
        o.crop = { 600, 400, 100, 200 };
        CHECK(VideoBuildFilter(o, 640, 480) == "crop=40:80:600:400");
        o.crop = { 0, 0, 640, 480 };
        CHECK(VideoBuildFilter(o, 640, 480).empty());
    }

    /* ---- Run planning ---- */
    {
        auto runs = VideoPlanRuns({ 0, 2, 4, 100, 101, 500 }, 60);
        CHECK(runs.size() == 3);
        CHECK(runs[0].first == 0 && runs[0].second == 4);
        CHECK(runs[1].first == 100 && runs[1].second == 101);
        CHECK(runs[2].first == 500 && runs[2].second == 500);
        CHECK(VideoPlanRuns({}, 60).empty());
    }

    /* ---- Windows argument quoting ---- */
    CHECK(VideoQuoteWindowsArg("plain") == "plain");
    CHECK(VideoQuoteWindowsArg("") == "\"\"");
    CHECK(VideoQuoteWindowsArg("C:\\My Videos\\a.mp4") == "\"C:\\My Videos\\a.mp4\"");
    CHECK(VideoQuoteWindowsArg("C:\\dir with space\\") == "\"C:\\dir with space\\\\\"");
    CHECK(VideoQuoteWindowsArg("say \"hi\"") == "\"say \\\"hi\\\"\"");
}

static void TestDecode(void)
{
    const std::string ffmpeg = VideoFindFfmpeg();
    if (ffmpeg.empty()) {
        std::printf("video_decode_test: ffmpeg not found, skipping decode checks\n");
        return;
    }

    /* 30 frames at 10 fps, frame N a flat gray of N*8, losslessly coded. */
    const char *clip = "video_decode_test_clip.mkv";
    std::remove(clip);
    std::string cmd = "\"\"" + ffmpeg + "\" -v error -y -f lavfi "
        "-i \"nullsrc=s=40x30:r=10,geq=r='N*8':g='N*8':b='N*8'\" "
        "-frames:v 30 -c:v ffv1 -pix_fmt bgr0 " + clip + "\"";
#ifndef _WIN32
    cmd = cmd.substr(1, cmd.size() - 2);   /* cmd.exe needs the outer quotes; sh does not */
#endif
    if (std::system(cmd.c_str()) != 0) {
        std::printf("video_decode_test: could not generate a clip, skipping decode checks\n");
        return;
    }

    VideoInfo vi;
    std::string err;
    CHECK(VideoProbe(ffmpeg, clip, &vi, &err));
    CHECK(vi.width == 40 && vi.height == 30);
    CHECK(vi.fps == 10.0);
    CHECK(vi.frame_count == 30);

    /* Two runs (frames 3,5,7 together; 29 after a gap over 2 seconds), out of
       order and with a duplicate and an out-of-range frame thrown in. */
    std::map<int, int> seen_gray;
    int calls = 0, cw = 0, ch = 0;
    VideoDecodeOptions opt;
    opt.crop = { 4, 2, 10, 12 };
    bool ok = VideoDecodeFrames(ffmpeg, clip, vi, { 7, 3, 29, 5, 5, 99 }, opt,
        [&](int frame, const unsigned char *rgba, int w, int h) {
            calls++;
            cw = w; ch = h;
            seen_gray[frame] = rgba[((h / 2) * w + w / 2) * 4];
            CHECK(rgba[3] == 255);
            return true;
        }, &err);
    CHECK(ok);
    CHECK(calls == 4);
    CHECK(cw == 10 && ch == 12);
    CHECK(seen_gray.count(3) && seen_gray[3] == 24);
    CHECK(seen_gray.count(5) && seen_gray[5] == 40);
    CHECK(seen_gray.count(7) && seen_gray[7] == 56);
    CHECK(seen_gray.count(29) && seen_gray[29] == 232);

    /* Frame 0 takes the no-seek path. */
    int g0 = -1;
    CHECK(VideoDecodeFrames(ffmpeg, clip, vi, { 0 }, VideoDecodeOptions(),
        [&](int, const unsigned char *rgba, int, int) { g0 = rgba[0]; return true; }, &err));
    CHECK(g0 == 0);

    /* A sink that says stop ends the decode with no error text. */
    int stops = 0;
    CHECK(!VideoDecodeFrames(ffmpeg, clip, vi, { 1, 2, 3 }, VideoDecodeOptions(),
        [&](int, const unsigned char *, int, int) { stops++; return false; }, &err));
    CHECK(stops == 1);
    CHECK(err.empty());

    /* A file that isn't a video reports why. */
    CHECK(!VideoProbe(ffmpeg, "video_decode_test_missing.mp4", &vi, &err));
    CHECK(!err.empty());

    std::remove(clip);
}

int main(void)
{
    TestParsers();
    TestDecode();

    if (g_fails == 0) {
        std::printf("video_decode_test: all checks passed\n");
        return 0;
    }
    std::printf("video_decode_test: %d failure(s)\n", g_fails);
    return 1;
}
