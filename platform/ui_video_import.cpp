/*************************************************************
 * platform/ui_video_import.cpp
 * "Import Video" dialog — the frame grabber. See ui_video_import.h.
 *
 * Owns scrubbing, frame picking, crop, and the floor point; decoding is
 * video_decode.h, and everything from keying onward is the digitize wizard.
 * The grab itself runs on a worker thread so a long pull from HD footage
 * shows progress and can be cancelled instead of freezing the window.
 *************************************************************/
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <SDL.h>

#include "ui_video_import.h"
#include "ui_internal.h"
#include "video_decode.h"
#include "digitize_import.h"
#include "img_io.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

const char *const kVideoImportExtensions =
    "MP4;MOV;M4V;AVI;MKV;WEBM;MPG;MPEG;WMV;MTS;M2TS;DV;FLV;GIF";

bool VideoImportIsVideoExt(const std::string &ext)
{
    std::string up;
    for (char c : ext) up += (char)std::toupper((unsigned char)c);
    if (up.empty() || up == "GIF") return false;   /* GIFs have their own importer */
    const std::string list = std::string(";") + kVideoImportExtensions + ";";
    return list.find(";" + up + ";") != std::string::npos;
}

/* ---- State ----------------------------------------------------------------- */

namespace {

struct GrabJob {
    std::thread thread;
    std::atomic<int>  done{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    int total = 0;
    /* Written by the worker, read by the UI only once `finished` is set. */
    bool ok = false;
    std::string err;
    std::vector<DigitizeSourceFrame> frames;
};

enum MouseMode { Mouse_Crop = 0, Mouse_Floor };

struct VideoImportState {
    bool open = false;
    std::string path, file_name, ffmpeg;
    VideoInfo info;

    int cur = 0;                      /* frame under the playhead */
    int shown = -1;                   /* frame the preview texture holds */
    bool shown_deinterlaced = false;
    int prev_w = 0, prev_h = 0;
    double last_decode_ms = 0.0;
    std::string preview_err;
    bool scrubbing = false;

    int in_frame = 0, out_frame = 0, step = 2;
    std::set<int> picks;

    VideoCrop crop;                   /* w/h 0 = whole frame */
    bool deinterlace = false;
    int anchor_x = -1, anchor_y = -1; /* floor point, full-frame pixels */
    int mouse_mode = Mouse_Crop;
    bool dragging = false;
    int drag_x0 = 0, drag_y0 = 0;

    char prefix[9] = "";

    GrabJob *job = nullptr;
};

} /* namespace */

static VideoImportState g_vi;
static SDL_Texture *g_vi_tex = nullptr;
static int g_vi_tex_w = 0, g_vi_tex_h = 0;

/* ---- Helpers ----------------------------------------------------------------- */

static VideoCrop EffectiveCrop(void)
{
    VideoCrop c = g_vi.crop;
    if (c.w <= 0 || c.h <= 0) c = { 0, 0, g_vi.info.width, g_vi.info.height };
    return c;
}

static void FormatTime(int frame, char *buf, size_t n)
{
    double t = g_vi.info.fps > 0.0 ? frame / g_vi.info.fps : 0.0;
    int m = (int)(t / 60.0);
    std::snprintf(buf, n, "%d:%05.2f", m, t - m * 60.0);
}

static void UploadPreview(const unsigned char *rgba, int w, int h)
{
    if (!g_vi_tex || g_vi_tex_w != w || g_vi_tex_h != h) {
        if (g_vi_tex) SDL_DestroyTexture(g_vi_tex);
        g_vi_tex = SDL_CreateTexture(g_imgui_renderer, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STATIC, w, h);
        if (g_vi_tex) SDL_SetTextureScaleMode(g_vi_tex, SDL_ScaleModeLinear);
        g_vi_tex_w = w; g_vi_tex_h = h;
    }
    if (g_vi_tex) SDL_UpdateTexture(g_vi_tex, NULL, rgba, w * 4);
    g_vi.prev_w = w; g_vi.prev_h = h;
}

/* Decode the playhead frame for the preview. Synchronous: a single frame is
   one seek plus at most a GOP of decoding. */
static void LoadPreview(int frame)
{
    auto t0 = std::chrono::steady_clock::now();
    VideoDecodeOptions o;
    o.deinterlace = g_vi.deinterlace;
    bool got = false;
    std::string err;
    VideoDecodeFrames(g_vi.ffmpeg, g_vi.path.c_str(), g_vi.info, { frame }, o,
        [&](int, const unsigned char *rgba, int w, int h) {
            UploadPreview(rgba, w, h);
            got = true;
            return true;
        }, &err);
    g_vi.last_decode_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_vi.shown = frame;
    g_vi.shown_deinterlaced = g_vi.deinterlace;
    g_vi.preview_err = got ? std::string() : (err.empty() ? "Could not decode this frame." : err);
}

static void SetCur(int f)
{
    const int last = std::max(0, g_vi.info.frame_count - 1);
    g_vi.cur = std::max(0, std::min(f, last));
}

static void TogglePick(int f)
{
    if (!g_vi.picks.erase(f)) g_vi.picks.insert(f);
}

static void PickRange(void)
{
    const int a = std::min(g_vi.in_frame, g_vi.out_frame);
    const int b = std::max(g_vi.in_frame, g_vi.out_frame);
    const int step = std::max(1, g_vi.step);
    g_vi.picks.clear();
    for (int f = a; f <= b; f += step) g_vi.picks.insert(f);
}

static void FinishJob(bool close_dialog)
{
    GrabJob *job = g_vi.job;
    if (!job) return;
    job->cancel = true;
    if (job->thread.joinable()) job->thread.join();
    g_vi.job = nullptr;
    delete job;
    if (close_dialog) g_vi.open = false;
}

static void StartGrab(void)
{
    if (g_vi.job || g_vi.picks.empty()) return;

    GrabJob *job = new GrabJob;
    job->total = (int)g_vi.picks.size();
    g_vi.job = job;

    const std::string ffmpeg = g_vi.ffmpeg, path = g_vi.path, prefix = g_vi.prefix;
    const VideoInfo info = g_vi.info;
    const std::vector<int> frames(g_vi.picks.begin(), g_vi.picks.end());
    VideoDecodeOptions opt;
    opt.crop = EffectiveCrop();
    opt.deinterlace = g_vi.deinterlace;

    job->thread = std::thread([job, ffmpeg, path, prefix, info, frames, opt]() {
        std::string err;
        int seq = 0;
        bool ok = VideoDecodeFrames(ffmpeg, path.c_str(), info, frames, opt,
            [&](int, const unsigned char *rgba, int w, int h) {
                if (job->cancel) return false;
                DigitizeSourceFrame f;
                f.name = prefix + std::to_string(++seq);
                f.w = w; f.h = h;
                f.rgba.assign(rgba, rgba + (size_t)w * h * 4);
                job->frames.push_back(std::move(f));
                job->done++;
                return !job->cancel;
            }, &err);
        job->ok = ok;
        job->err = err;
        job->finished = true;
    });
}

/* The worker is done: hand its frames to the digitize wizard. */
static void CompleteGrab(void)
{
    GrabJob *job = g_vi.job;
    if (job->thread.joinable()) job->thread.join();

    if (!job->ok || job->frames.empty()) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Video grab failed: %.100s",
                 job->err.empty() ? "no frames decoded" : job->err.c_str());
        g_restore_msg_timer = 5.0f;
        g_vi.job = nullptr;
        delete job;
        return;
    }

    DigitizeHandoff h;
    h.frames = std::move(job->frames);
    h.palette_name = std::string(g_vi.prefix) + "P";
    if (g_vi.anchor_x >= 0 && g_vi.anchor_y >= 0) {
        const VideoCrop c = EffectiveCrop();
        h.anchor_x = std::max(0, g_vi.anchor_x - c.x);
        h.anchor_y = std::max(0, g_vi.anchor_y - c.y);
    }
    g_vi.job = nullptr;
    delete job;
    g_vi.open = false;
    OpenDigitizeImportDialogFrames(std::move(h));
}

/* ---- Open ------------------------------------------------------------------- */

void OpenVideoImportDialog(const std::string &path)
{
    FinishJob(false);
    std::string ffmpeg = VideoFindFfmpeg();
    if (ffmpeg.empty()) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Video import needs ffmpeg: install it on PATH or set IMGTOOL_FFMPEG.");
        g_restore_msg_timer = 6.0f;
        return;
    }
    VideoInfo info;
    std::string err;
    if (!VideoProbe(ffmpeg, path.c_str(), &info, &err) || info.frame_count <= 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Can't read video: %.100s",
                 err.empty() ? "no frames" : err.c_str());
        g_restore_msg_timer = 5.0f;
        return;
    }

    g_vi = VideoImportState();
    g_vi.path = path;
    g_vi.ffmpeg = ffmpeg;
    g_vi.info = info;
    size_t slash = path.find_last_of("\\/");
    g_vi.file_name = slash == std::string::npos ? path : path.substr(slash + 1);
    g_vi.out_frame = info.frame_count - 1;

    /* Sprite names in the Midway style: an upper-case move name and a frame
       number (LKWALK1, LKWALK2, ...). Seeded from the file name. */
    size_t n = 0;
    for (char c : g_vi.file_name) {
        if (c == '.') break;
        if (std::isalnum((unsigned char)c) && n < sizeof(g_vi.prefix) - 1)
            g_vi.prefix[n++] = (char)std::toupper((unsigned char)c);
    }
    if (n == 0) std::strcpy(g_vi.prefix, "VID");

    g_vi.open = true;
    LoadPreview(0);
}

/* ---- Drawing ------------------------------------------------------------------ */

/* The full-frame preview with the crop and floor point over it. */
static void DrawPreview(ImVec2 size)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 pane0 = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(pane0, pane0 + size, IM_COL32(20, 20, 22, 255));

    const int fw = g_vi.info.width, fh = g_vi.info.height;
    if (fw <= 0 || fh <= 0) { ImGui::Dummy(size); return; }
    float scale = std::min(size.x / fw, size.y / fh);
    if (scale <= 0.0f) { ImGui::Dummy(size); return; }
    const ImVec2 img_size(fw * scale, fh * scale);
    const ImVec2 o = pane0 + (size - img_size) * 0.5f;

    if (g_vi_tex && g_vi.prev_w > 0)
        dl->AddImage((ImTextureID)(intptr_t)g_vi_tex, o, o + img_size);
    if (!g_vi.preview_err.empty())
        dl->AddText(o + ImVec2(8, 8), IM_COL32(255, 120, 120, 255), g_vi.preview_err.c_str());

    /* Dim what the crop throws away. */
    const VideoCrop c = EffectiveCrop();
    const ImVec2 c0 = o + ImVec2(c.x * scale, c.y * scale);
    const ImVec2 c1 = o + ImVec2((c.x + c.w) * scale, (c.y + c.h) * scale);
    const ImU32 dim = IM_COL32(0, 0, 0, 150);
    dl->AddRectFilled(o, ImVec2(o.x + img_size.x, c0.y), dim);
    dl->AddRectFilled(ImVec2(o.x, c1.y), o + img_size, dim);
    dl->AddRectFilled(ImVec2(o.x, c0.y), ImVec2(c0.x, c1.y), dim);
    dl->AddRectFilled(ImVec2(c1.x, c0.y), ImVec2(o.x + img_size.x, c1.y), dim);
    dl->AddRect(c0, c1, IM_COL32(255, 220, 60, 255), 0.0f, 0, 1.5f);

    {
        float ax = g_vi.anchor_x >= 0 ? g_vi.anchor_x + 0.5f : c.x + c.w * 0.5f;
        float ay = g_vi.anchor_y >= 0 ? g_vi.anchor_y + 0.5f : c.y + c.h - 0.5f;
        const ImVec2 p = o + ImVec2(ax * scale, ay * scale);
        const ImU32 col = g_vi.anchor_x >= 0 ? IM_COL32(255, 80, 255, 240) : IM_COL32(255, 80, 255, 120);
        dl->AddLine(p - ImVec2(10, 0), p + ImVec2(11, 0), col, 1.5f);
        dl->AddLine(p - ImVec2(0, 10), p + ImVec2(0, 11), col, 1.5f);
        dl->AddCircle(p, 4.0f, col);
    }

    ImGui::SetCursorScreenPos(o);
    ImGui::InvisibleButton("##video_preview", img_size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 m = ImGui::GetIO().MousePos;
    int px = (int)std::floor((m.x - o.x) / scale), py = (int)std::floor((m.y - o.y) / scale);
    px = std::max(0, std::min(px, fw - 1));
    py = std::max(0, std::min(py, fh - 1));

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_vi.dragging = true;
        g_vi.drag_x0 = px; g_vi.drag_y0 = py;
    }
    if (g_vi.dragging) {
        if (g_vi.mouse_mode == Mouse_Floor) {
            g_vi.anchor_x = px; g_vi.anchor_y = py;
        } else {
            const int x0 = std::min(g_vi.drag_x0, px), x1 = std::max(g_vi.drag_x0, px);
            const int y0 = std::min(g_vi.drag_y0, py), y1 = std::max(g_vi.drag_y0, py);
            /* A plain click (no real drag) leaves the crop alone. */
            if (x1 - x0 >= 4 && y1 - y0 >= 4) g_vi.crop = { x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_vi.dragging = false;
    }
    if (hovered) ImGui::SetTooltip("%d, %d", px, py);

    ImGui::SetCursorScreenPos(pane0);
    ImGui::Dummy(size);
}

/* Scrub bar: in/out range shaded, a tick per picked frame, the playhead. */
static void DrawTimeline(float width)
{
    const int count = std::max(1, g_vi.info.frame_count);
    const float h = 26.0f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1 = p0 + ImVec2(width, h);
    auto x_of = [&](int f) { return p0.x + (count > 1 ? (float)f / (count - 1) : 0.0f) * width; };

    dl->AddRectFilled(p0, p1, IM_COL32(36, 36, 40, 255));
    const int a = std::min(g_vi.in_frame, g_vi.out_frame), b = std::max(g_vi.in_frame, g_vi.out_frame);
    dl->AddRectFilled(ImVec2(x_of(a), p0.y), ImVec2(std::max(x_of(b), x_of(a) + 1.0f), p1.y),
                      IM_COL32(70, 90, 130, 255));
    for (int f : g_vi.picks) {
        const float x = x_of(f);
        dl->AddLine(ImVec2(x, p0.y + 4), ImVec2(x, p1.y - 4), IM_COL32(120, 230, 140, 255), 1.0f);
    }
    const float cx = x_of(g_vi.cur);
    dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p1.y), IM_COL32(255, 220, 60, 255), 2.0f);
    dl->AddRect(p0, p1, IM_COL32(90, 90, 96, 255));

    ImGui::InvisibleButton("##video_scrub", ImVec2(width, h));
    g_vi.scrubbing = ImGui::IsItemActive();
    if (g_vi.scrubbing) {
        const float t = (ImGui::GetIO().MousePos.x - p0.x) / std::max(1.0f, width);
        SetCur((int)std::lround(std::max(0.0f, std::min(1.0f, t)) * (count - 1)));
    }
}

void DrawVideoImportDialog(void)
{
    if (!g_vi.open) return;
    ImGui::OpenPopup("Import Video");
    ImGui::SetNextWindowSize(ImVec2(1040, 700), ImGuiCond_Once);
    bool keep_open = true;
    if (!ImGui::BeginPopupModal("Import Video", &keep_open, ImGuiWindowFlags_NoSavedSettings)) {
        if (!keep_open) FinishJob(true);
        return;
    }
    if (!keep_open) {
        FinishJob(true);
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const bool busy = g_vi.job != nullptr;
    if (busy && g_vi.job->finished) {
        CompleteGrab();
        if (!g_vi.open) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
    }

    const VideoInfo &vi = g_vi.info;
    ImGui::Text("%s", g_vi.file_name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%dx%d  %.3g fps  %d frames  %s", vi.width, vi.height, vi.fps,
                        vi.frame_count, vi.codec.c_str());

    /* Keyboard, NLE-style: arrows step, I/O set in and out, P picks. */
    ImGuiIO &io = ImGui::GetIO();
    if (!busy && !io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        const int big = io.KeyShift ? 10 : 1;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))  SetCur(g_vi.cur - big);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) SetCur(g_vi.cur + big);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false))      SetCur(0);
        if (ImGui::IsKeyPressed(ImGuiKey_End, false))       SetCur(vi.frame_count - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_I, false))         g_vi.in_frame = g_vi.cur;
        if (ImGui::IsKeyPressed(ImGuiKey_O, false))         g_vi.out_frame = g_vi.cur;
        if (ImGui::IsKeyPressed(ImGuiKey_P, false))         TogglePick(g_vi.cur);
    }

    /* Decode lazily. While scrubbing, only keep up live if decoding is fast
       enough to feel live; otherwise wait for the release. */
    if (!busy && (g_vi.shown != g_vi.cur || g_vi.shown_deinterlaced != g_vi.deinterlace)) {
        if (!g_vi.scrubbing || g_vi.last_decode_ms < 80.0) LoadPreview(g_vi.cur);
    }

    const float kSide = 300.0f;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("##video_left", ImVec2(-kSide, -footer), false);
    {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const float timeline_h = 26.0f + ImGui::GetFrameHeightWithSpacing() + 10.0f;
        DrawPreview(ImVec2(avail.x, std::max(80.0f, avail.y - timeline_h)));
        ImGui::BeginDisabled(busy);
        DrawTimeline(avail.x);

        if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) SetCur(g_vi.cur - 1);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##next", ImGuiDir_Right)) SetCur(g_vi.cur + 1);
        ImGui::SameLine();
        char t[32];
        FormatTime(g_vi.cur, t, sizeof(t));
        ImGui::Text("Frame %d / %d   %s", g_vi.cur, vi.frame_count - 1, t);
        ImGui::SameLine();
        const bool picked = g_vi.picks.count(g_vi.cur) != 0;
        if (ImGui::Button(picked ? "Unpick (P)" : "Pick (P)")) TogglePick(g_vi.cur);
        ImGui::SameLine();
        if (g_vi.shown == g_vi.cur && g_vi.last_decode_ms > 0.0)
            ImGui::TextDisabled("decode %.0f ms", g_vi.last_decode_ms);
        else
            ImGui::TextDisabled("(release to load)");
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##video_side", ImVec2(kSide - 8.0f, -footer), true);
    ImGui::BeginDisabled(busy);
    {
        ImGui::TextUnformatted("Frames");
        const int last = std::max(0, vi.frame_count - 1);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputInt("##in", &g_vi.in_frame)) g_vi.in_frame = std::max(0, std::min(g_vi.in_frame, last));
        ImGui::SameLine();
        if (ImGui::Button("In (I)")) g_vi.in_frame = g_vi.cur;
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputInt("##out", &g_vi.out_frame)) g_vi.out_frame = std::max(0, std::min(g_vi.out_frame, last));
        ImGui::SameLine();
        if (ImGui::Button("Out (O)")) g_vi.out_frame = g_vi.cur;

        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputInt("##step", &g_vi.step)) g_vi.step = std::max(1, std::min(g_vi.step, 120));
        ImGui::SameLine();
        ImGui::TextUnformatted("every Nth frame");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The arcade never showed every frame of the tape. A move\n"
                              "was a handful of chosen poses — keep every 2nd or 3rd\n"
                              "frame of 30 fps footage, then thin it by hand.");
        const int a = std::min(g_vi.in_frame, g_vi.out_frame), b = std::max(g_vi.in_frame, g_vi.out_frame);
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "Pick In..Out (%d frames)", (b - a) / std::max(1, g_vi.step) + 1);
        if (ImGui::Button(lbl, ImVec2(-1.0f, 0.0f))) PickRange();
        if (ImGui::Button("Clear Picks", ImVec2(-1.0f, 0.0f))) g_vi.picks.clear();
        ImGui::TextDisabled("%d frame(s) picked", (int)g_vi.picks.size());

        if (!g_vi.picks.empty() && ImGui::BeginListBox("##picks", ImVec2(-1.0f, 90.0f))) {
            for (int f : g_vi.picks) {
                char row[48];
                std::snprintf(row, sizeof(row), "frame %d", f);
                if (ImGui::Selectable(row, f == g_vi.cur)) SetCur(f);
            }
            ImGui::EndListBox();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Source");
        ImGui::RadioButton("Drag crop", &g_vi.mouse_mode, Mouse_Crop);
        ImGui::SameLine();
        ImGui::RadioButton("Floor point", &g_vi.mouse_mode, Mouse_Floor);
        const VideoCrop c = EffectiveCrop();
        ImGui::TextDisabled("Crop %d,%d  %dx%d", c.x, c.y, c.w, c.h);
        ImGui::SameLine();
        if (ImGui::SmallButton("Full")) g_vi.crop = VideoCrop();
        if (g_vi.anchor_x >= 0) {
            ImGui::TextDisabled("Floor %d,%d", g_vi.anchor_x, g_vi.anchor_y);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) g_vi.anchor_x = g_vi.anchor_y = -1;
        } else {
            ImGui::TextDisabled("Floor: bottom center of crop");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click the floor between the actor's feet. Every grabbed\n"
                              "frame gets its anipoint on this one spot, so with a\n"
                              "camera that never moved the move comes in registered.");
        ImGui::Checkbox("Deinterlace", &g_vi.deinterlace);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("For tape and DV transfers: removes the comb where the two\n"
                              "fields of a moving limb don't line up. Leave off for\n"
                              "phone or camera footage, which is progressive.");

        ImGui::Separator();
        ImGui::TextUnformatted("Names");
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputText("##prefix", g_vi.prefix, sizeof(g_vi.prefix),
                             ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank)) {}
        ImGui::SameLine();
        ImGui::TextDisabled("%s1.. / %sP", g_vi.prefix, g_vi.prefix);

        ImGui::Separator();
        const double mb = (double)g_vi.picks.size() * c.w * c.h * 4.0 / (1024.0 * 1024.0);
        const bool too_big = mb > (sizeof(void *) == 4 ? 600.0 : 3000.0);
        if (mb > 1000.0 || too_big)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%.0f MB of source frames%s", mb,
                               too_big ? " - crop tighter or pick fewer" : "");
        else
            ImGui::TextDisabled("%.0f MB of source frames", mb);
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    ImGui::Separator();
    if (busy) {
        GrabJob *job = g_vi.job;
        char overlay[48];
        std::snprintf(overlay, sizeof(overlay), "%d / %d", job->done.load(), job->total);
        ImGui::ProgressBar(job->total ? (float)job->done.load() / job->total : 0.0f,
                           ImVec2(-120.0f, 0.0f), overlay);
        ImGui::SameLine();
        if (ImGui::Button("Cancel Grab", ImVec2(110, 0))) FinishJob(false);
    } else {
        const VideoCrop c = EffectiveCrop();
        const double mb = (double)g_vi.picks.size() * c.w * c.h * 4.0 / (1024.0 * 1024.0);
        const bool too_big = mb > (sizeof(void *) == 4 ? 600.0 : 3000.0);
        ImGui::BeginDisabled(g_vi.picks.empty() || too_big || !g_vi.prefix[0]);
        char go[64];
        std::snprintf(go, sizeof(go), "Grab %d Frame(s) -> Digitize", (int)g_vi.picks.size());
        if (ImGui::Button(go, ImVec2(240, 0))) StartGrab();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            g_vi.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Next: key the backdrop, seed materials on frame 1, fit ramps.");
    }

    ImGui::EndPopup();
}
