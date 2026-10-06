/*************************************************************
 * platform/video_decode.cpp
 * Frame grabbing through an ffmpeg child process. See video_decode.h.
 *************************************************************/
#include "video_decode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ---- Pure helpers ------------------------------------------------------- */

double VideoParseRate(const char *s)
{
    if (!s || !*s) return 0.0;
    char *end = nullptr;
    double num = std::strtod(s, &end);
    if (end == s) return 0.0;
    if (*end == '/') {
        const char *dp = end + 1;
        double den = std::strtod(dp, &end);
        if (end == dp || den == 0.0) return 0.0;
        num /= den;
    }
    return (num > 0.0 && std::isfinite(num)) ? num : 0.0;
}

bool VideoParseProbeOutput(const std::string &text, VideoInfo *out)
{
    if (!out) return false;
    VideoInfo info;
    double avg_rate = 0.0, r_rate = 0.0;
    long nb_frames = 0;
    bool have_duration = false;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (val.empty() || val == "N/A") continue;

        if (key == "width")               info.width = std::atoi(val.c_str());
        else if (key == "height")         info.height = std::atoi(val.c_str());
        else if (key == "avg_frame_rate") avg_rate = VideoParseRate(val.c_str());
        else if (key == "r_frame_rate")   r_rate = VideoParseRate(val.c_str());
        else if (key == "nb_frames")      nb_frames = std::atol(val.c_str());
        else if (key == "codec_name")     info.codec = val;
        else if (key == "rotation" || key == "TAG:rotate") info.rotation = std::atoi(val.c_str());
        else if (key == "duration" && !have_duration) {
            /* Stream duration comes first; the container's is the fallback. */
            double d = std::atof(val.c_str());
            if (d > 0.0) { info.duration = d; have_duration = true; }
        }
    }

    info.fps = avg_rate > 0.0 ? avg_rate : r_rate;
    if (info.width <= 0 || info.height <= 0 || info.fps <= 0.0) return false;

    int rot = ((info.rotation % 360) + 360) % 360;
    if (rot == 90 || rot == 270) std::swap(info.width, info.height);

    if (nb_frames > 0)            info.frame_count = (int)nb_frames;
    else if (info.duration > 0.0) info.frame_count = (int)std::lround(info.duration * info.fps);
    if (info.duration <= 0.0 && info.frame_count > 0) info.duration = info.frame_count / info.fps;

    *out = info;
    return true;
}

bool VideoParsePamHeader(const unsigned char *buf, size_t len,
                         int *w, int *h, int *depth, size_t *header_len)
{
    static const char kEnd[] = "ENDHDR\n";
    const size_t kEndLen = sizeof(kEnd) - 1;
    if (!buf || len < 3 || buf[0] != 'P' || buf[1] != '7' || buf[2] != '\n') return false;

    size_t end = 0;
    for (size_t i = 3; i + kEndLen <= len; i++) {
        if (std::memcmp(buf + i, kEnd, kEndLen) == 0 && buf[i - 1] == '\n') {
            end = i + kEndLen;
            break;
        }
    }
    if (!end) return false;

    int pw = 0, ph = 0, pd = 0, maxval = 0;
    size_t pos = 3;
    while (pos < end) {
        size_t eol = pos;
        while (eol < end && buf[eol] != '\n') eol++;
        std::string line((const char *)buf + pos, eol - pos);
        pos = eol + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string key = line.substr(0, sp);
        const int val = std::atoi(line.c_str() + sp + 1);
        if (key == "WIDTH")       pw = val;
        else if (key == "HEIGHT") ph = val;
        else if (key == "DEPTH")  pd = val;
        else if (key == "MAXVAL") maxval = val;
    }
    if (pw <= 0 || ph <= 0 || pd < 1 || pd > 4 || maxval != 255) return false;
    if (w) *w = pw;
    if (h) *h = ph;
    if (depth) *depth = pd;
    if (header_len) *header_len = end;
    return true;
}

std::string VideoBuildFilter(const VideoDecodeOptions &opt, int frame_w, int frame_h)
{
    std::string chain;
    if (opt.deinterlace) chain = "yadif=mode=0";

    const VideoCrop &c = opt.crop;
    if (c.w > 0 && c.h > 0 && frame_w > 0 && frame_h > 0) {
        int x = std::max(0, std::min(c.x, frame_w - 1));
        int y = std::max(0, std::min(c.y, frame_h - 1));
        int cw = std::min(c.w, frame_w - x);
        int ch = std::min(c.h, frame_h - y);
        if (cw > 0 && ch > 0 && (cw != frame_w || ch != frame_h)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "crop=%d:%d:%d:%d", cw, ch, x, y);
            if (!chain.empty()) chain += ',';
            chain += buf;
        }
    }
    return chain;
}

std::vector<std::pair<int, int>> VideoPlanRuns(const std::vector<int> &sorted_frames, int max_gap)
{
    std::vector<std::pair<int, int>> runs;
    for (int f : sorted_frames) {
        if (!runs.empty() && f - runs.back().second <= max_gap) runs.back().second = f;
        else runs.push_back({ f, f });
    }
    return runs;
}

std::string VideoQuoteWindowsArg(const std::string &arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    std::string out = "\"";
    for (size_t i = 0; ; i++) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') { backslashes++; i++; }
        if (i == arg.size()) {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (arg[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(backslashes, '\\');
            out += arg[i];
        }
    }
    out += '"';
    return out;
}

/* ---- Child process -------------------------------------------------------- */

/* Pipe creation and process start are serialized: a child started on one
   thread would otherwise inherit the write end of a pipe another thread is
   setting up, and that pipe would never reach EOF. */
static std::mutex g_spawn_mutex;

namespace {

struct Child {
#ifdef _WIN32
    HANDLE proc = NULL, out = NULL, err = NULL;
#else
    pid_t pid = -1;
    int out = -1, err = -1;
#endif
};

bool FileExists(const std::string &p)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    return access(p.c_str(), X_OK) == 0;
#endif
}

#ifdef _WIN32

bool ChildStart(const std::vector<std::string> &argv, Child *c, std::string *err)
{
    std::lock_guard<std::mutex> lock(g_spawn_mutex);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 1 << 20)) {
        if (err) *err = "Could not create a pipe for ffmpeg.";
        return false;
    }
    if (!CreatePipe(&err_r, &err_w, &sa, 64 << 10)) {
        CloseHandle(out_r); CloseHandle(out_w);
        if (err) *err = "Could not create a pipe for ffmpeg.";
        return false;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul_in = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOA si;
    std::memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul_in;
    si.hStdOutput = out_w;
    si.hStdError = err_w;

    std::string cmd;
    for (size_t i = 0; i < argv.size(); i++) {
        if (i) cmd += ' ';
        cmd += VideoQuoteWindowsArg(argv[i]);
    }

    PROCESS_INFORMATION pi;
    std::memset(&pi, 0, sizeof(pi));
    /* CREATE_NO_WINDOW: imgtool is a GUI-subsystem app, and without it every
       grab would flash a console window. */
    BOOL ok = CreateProcessA(NULL, &cmd[0], NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, NULL, &si, &pi);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
    if (!ok) {
        CloseHandle(out_r); CloseHandle(err_r);
        if (err) *err = "Could not start " + argv[0] + ".";
        return false;
    }
    CloseHandle(pi.hThread);
    c->proc = pi.hProcess;
    c->out = out_r;
    c->err = err_r;
    return true;
}

/* Bytes read, 0 at end of stream. */
size_t ChildRead(Child *c, void *buf, size_t n)
{
    DWORD got = 0;
    if (!ReadFile(c->out, buf, (DWORD)std::min(n, (size_t)1 << 30), &got, NULL)) return 0;
    return got;
}

/* Kill if asked, collect stderr, reap. Returns the exit code. */
int ChildFinish(Child *c, bool kill, std::string *stderr_text)
{
    if (kill) TerminateProcess(c->proc, 1);
    CloseHandle(c->out);
    char buf[1024];
    DWORD got = 0;
    while (ReadFile(c->err, buf, sizeof(buf), &got, NULL) && got > 0)
        if (stderr_text && stderr_text->size() < 8192) stderr_text->append(buf, got);
    CloseHandle(c->err);
    WaitForSingleObject(c->proc, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(c->proc, &code);
    CloseHandle(c->proc);
    *c = Child();
    return (int)code;
}

#else /* POSIX */

bool ChildStart(const std::vector<std::string> &argv, Child *c, std::string *err)
{
    std::lock_guard<std::mutex> lock(g_spawn_mutex);
    int outp[2], errp[2];
    if (pipe(outp) != 0) { if (err) *err = "Could not create a pipe for ffmpeg."; return false; }
    if (pipe(errp) != 0) {
        close(outp[0]); close(outp[1]);
        if (err) *err = "Could not create a pipe for ffmpeg.";
        return false;
    }
    fcntl(outp[0], F_SETFD, FD_CLOEXEC);
    fcntl(errp[0], F_SETFD, FD_CLOEXEC);

    std::vector<char *> args;
    for (const auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        close(outp[0]); close(outp[1]); close(errp[0]); close(errp[1]);
        if (err) *err = "Could not start " + argv[0] + ".";
        return false;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        dup2(outp[1], 1);
        dup2(errp[1], 2);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(outp[1]);
    close(errp[1]);
    c->pid = pid;
    c->out = outp[0];
    c->err = errp[0];
    return true;
}

size_t ChildRead(Child *c, void *buf, size_t n)
{
    for (;;) {
        ssize_t got = read(c->out, buf, n);
        if (got < 0 && errno == EINTR) continue;
        return got > 0 ? (size_t)got : 0;
    }
}

int ChildFinish(Child *c, bool kill, std::string *stderr_text)
{
    if (kill) ::kill(c->pid, SIGTERM);
    close(c->out);
    char buf[1024];
    for (;;) {
        ssize_t got = read(c->err, buf, sizeof(buf));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        if (stderr_text && stderr_text->size() < 8192) stderr_text->append(buf, (size_t)got);
    }
    close(c->err);
    int status = 0;
    while (waitpid(c->pid, &status, 0) < 0 && errno == EINTR) {}
    *c = Child();
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

#endif

bool ReadExact(Child *c, unsigned char *dst, size_t n)
{
    while (n > 0) {
        size_t got = ChildRead(c, dst, n);
        if (got == 0) return false;
        dst += got;
        n -= got;
    }
    return true;
}

/* ffmpeg's last stderr line is the one worth showing. */
std::string LastLine(const std::string &text)
{
    std::string t = text;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    size_t nl = t.find_last_of('\n');
    return nl == std::string::npos ? t : t.substr(nl + 1);
}

} /* namespace */

/* ---- Locating ffmpeg ------------------------------------------------------ */

std::string VideoFindFfmpeg(void)
{
    const char *env = std::getenv("IMGTOOL_FFMPEG");
    if (env && *env && FileExists(env)) return env;

#ifdef _WIN32
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::string dir(exe);
        size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) {
            dir.resize(slash + 1);
            if (FileExists(dir + "ffmpeg.exe")) return dir + "ffmpeg.exe";
            if (FileExists(dir + "ffmpeg\\bin\\ffmpeg.exe")) return dir + "ffmpeg\\bin\\ffmpeg.exe";
        }
    }
    char found[MAX_PATH];
    if (SearchPathA(NULL, "ffmpeg.exe", NULL, MAX_PATH, found, NULL) > 0) return found;

    std::vector<std::string> guesses = { "C:\\ffmpeg\\bin\\ffmpeg.exe" };
    if (const char *pf = std::getenv("ProgramFiles"))
        guesses.push_back(std::string(pf) + "\\ffmpeg\\bin\\ffmpeg.exe");
    if (const char *la = std::getenv("LOCALAPPDATA"))
        guesses.push_back(std::string(la) + "\\Microsoft\\WinGet\\Links\\ffmpeg.exe");
    for (const auto &g : guesses)
        if (FileExists(g)) return g;
#else
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        std::string dir(exe);
        size_t slash = dir.find_last_of('/');
        if (slash != std::string::npos && FileExists(dir.substr(0, slash + 1) + "ffmpeg"))
            return dir.substr(0, slash + 1) + "ffmpeg";
    }
    if (const char *path = std::getenv("PATH")) {
        std::string p(path);
        size_t pos = 0;
        while (pos <= p.size()) {
            size_t colon = p.find(':', pos);
            if (colon == std::string::npos) colon = p.size();
            std::string dir = p.substr(pos, colon - pos);
            if (!dir.empty() && FileExists(dir + "/ffmpeg")) return dir + "/ffmpeg";
            pos = colon + 1;
        }
    }
    for (const char *g : { "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg" })
        if (FileExists(g)) return g;
#endif
    return "";
}

std::string VideoFfprobeFor(const std::string &ffmpeg)
{
    size_t slash = ffmpeg.find_last_of("\\/");
    std::string dir = slash == std::string::npos ? std::string() : ffmpeg.substr(0, slash + 1);
#ifdef _WIN32
    return dir + "ffprobe.exe";
#else
    return dir + "ffprobe";
#endif
}

/* ---- Probe and decode ------------------------------------------------------ */

bool VideoProbe(const std::string &ffmpeg, const char *path, VideoInfo *out, std::string *err)
{
    if (err) err->clear();
    if (ffmpeg.empty()) { if (err) *err = "ffmpeg was not found."; return false; }

    std::vector<std::string> argv = {
        VideoFfprobeFor(ffmpeg), "-v", "error", "-select_streams", "v:0",
        "-show_entries",
        "stream=width,height,avg_frame_rate,r_frame_rate,nb_frames,duration,codec_name"
        ":stream_side_data=rotation:stream_tags=rotate:format=duration",
        "-of", "default=noprint_wrappers=1", path
    };
    Child c;
    if (!ChildStart(argv, &c, err)) return false;
    std::string text;
    char buf[4096];
    size_t got;
    while ((got = ChildRead(&c, buf, sizeof(buf))) > 0 && text.size() < (1 << 20))
        text.append(buf, got);
    std::string stderr_text;
    ChildFinish(&c, false, &stderr_text);

    if (!VideoParseProbeOutput(text, out)) {
        if (err) {
            std::string why = LastLine(stderr_text);
            *err = why.empty() ? "No video stream found." : why;
        }
        return false;
    }
    return true;
}

bool VideoDecodeFrames(const std::string &ffmpeg, const char *path, const VideoInfo &info,
                       const std::vector<int> &frames, const VideoDecodeOptions &opt,
                       const VideoFrameSink &sink, std::string *err)
{
    if (err) err->clear();
    if (ffmpeg.empty()) { if (err) *err = "ffmpeg was not found."; return false; }
    if (info.fps <= 0.0) { if (err) *err = "Unknown frame rate."; return false; }

    std::vector<int> wanted;
    for (int f : frames)
        if (f >= 0 && (info.frame_count <= 0 || f < info.frame_count)) wanted.push_back(f);
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    if (wanted.empty()) { if (err) *err = "No frames requested."; return false; }

    const std::string filter = VideoBuildFilter(opt, info.width, info.height);
    const int max_gap = std::max(1, (int)std::lround(info.fps * 2.0));
    const auto runs = VideoPlanRuns(wanted, max_gap);

    int delivered = 0;
    size_t want_i = 0;
    std::vector<unsigned char> pixels, rgba;
    std::string stderr_text;

    for (const auto &run : runs) {
        const int first = run.first, count = run.second - run.first + 1;

        std::vector<std::string> argv = { ffmpeg, "-hide_banner", "-nostdin", "-v", "error" };
        if (first > 0) {
            /* Half a frame early so rounding in the stream's timestamps can't
               land the seek on the frame after the one asked for. */
            char t[32];
            std::snprintf(t, sizeof(t), "%.6f", (first - 0.5) / info.fps);
            argv.insert(argv.end(), { "-ss", t });
        }
        argv.insert(argv.end(), { "-i", path, "-an", "-sn", "-dn",
                                  "-frames:v", std::to_string(count) });
        if (!filter.empty()) argv.insert(argv.end(), { "-vf", filter });
        argv.insert(argv.end(), { "-vsync", "passthrough", "-pix_fmt", "rgba",
                                  "-c:v", "pam", "-f", "image2pipe", "-" });

        Child c;
        if (!ChildStart(argv, &c, err)) return false;

        bool stopped = false;
        for (int k = 0; k < count; k++) {
            /* The header is under 100 bytes; read it a byte at a time so no
               pixel data is swallowed past ENDHDR. */
            unsigned char hdr[512];
            size_t hlen = 0, header_len = 0;
            int w = 0, h = 0, depth = 0;
            bool ok = false;
            while (hlen < sizeof(hdr)) {
                if (!ReadExact(&c, hdr + hlen, 1)) break;
                hlen++;
                if (hdr[hlen - 1] == '\n' &&
                    VideoParsePamHeader(hdr, hlen, &w, &h, &depth, &header_len)) { ok = true; break; }
            }
            if (!ok) break;   /* end of stream: the frame count was an estimate */

            pixels.resize((size_t)w * h * depth);
            if (!ReadExact(&c, pixels.data(), pixels.size())) break;

            const int frame = first + k;
            if (want_i < wanted.size() && wanted[want_i] == frame) {
                want_i++;
                const unsigned char *src = pixels.data();
                if (depth != 4) {
                    rgba.resize((size_t)w * h * 4);
                    for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
                        const unsigned char *p = pixels.data() + i * depth;
                        unsigned char r = p[0], g = depth >= 3 ? p[1] : p[0], b = depth >= 3 ? p[2] : p[0];
                        unsigned char a = (depth == 2) ? p[1] : 255;
                        rgba[i*4+0] = r; rgba[i*4+1] = g; rgba[i*4+2] = b; rgba[i*4+3] = a;
                    }
                    src = rgba.data();
                }
                delivered++;
                if (!sink(frame, src, w, h)) { stopped = true; break; }
            }
        }

        std::string run_err;
        ChildFinish(&c, true, &run_err);
        if (!run_err.empty()) stderr_text = run_err;
        if (stopped) return false;
        while (want_i < wanted.size() && wanted[want_i] <= run.second) want_i++;
    }

    if (delivered == 0) {
        if (err) {
            std::string why = LastLine(stderr_text);
            *err = why.empty() ? "ffmpeg produced no frames." : why;
        }
        return false;
    }
    return true;
}
