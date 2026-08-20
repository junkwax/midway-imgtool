/*************************************************************
 * platform/ui_reactions.cpp
 * See ui_reactions.h. Layout is a fixed-width reaction list on the left and,
 * on the right, the selected reaction's preview above its per-frame anipoint
 * table -- the two things you compare when timing an attack against the hit it
 * causes.
 *************************************************************/
#include "ui_reactions.h"

#include "ui_internal.h"
#include "ui_canvas.h"
#include "reaction_class.h"
#include "world_render.h"     /* doc_get_img / doc_get_pal */
#include "img_io.h"           /* g_restore_msg / g_restore_msg_timer toast */

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* ---- Which ASM the list is showing ---- */
enum ReactSource { ReactSource_Opponent = 0, ReactSource_Player = 1 };

/* One classified row of the source anitab. */
struct ReactRow {
    int          anim_index = -1;   /* into g_asm_opp_anims / g_asm_anims */
    ReactionKind kind = ReactionKind::NotReaction;
    std::string  rule;
    std::string  thrower;           /* spelled out where the code is known */
};

static int   g_react_source   = ReactSource_Opponent;
static int   g_react_sel      = -1;      /* anim index, not row index */
static bool  g_react_show_all = false;
static char  g_react_filter[64] = {0};

/* Playback. Separate from the ASM window's clock on purpose: this tab scrubs a
   reaction frame by frame while that window may be looping something else. */
static bool  g_react_play  = false;
static float g_react_fps   = kMk2TickHz;
static int   g_react_frame = 0;
static float g_react_timer = 0.0f;

/* Preview texture + the canvas bounds it was sized for. */
static SDL_Texture *g_react_tex = NULL;
static int g_react_tex_w = 0, g_react_tex_h = 0;
static int g_react_canvas_w = 0, g_react_canvas_h = 0;
static int g_react_minx = 0, g_react_miny = 0;
static int g_react_last_drawn = -1;
static int g_react_built_sel = -2;       /* which anim the texture belongs to */
/* Number of open document tabs the cached frame was composited from. A piece
   whose IMG tab has closed resolves to nothing and silently drops out of the
   picture, so a frame cached before the close would keep showing art that is
   no longer loaded until the frame index happened to change. */
static int g_react_built_docs = -1;

/* Classified rows, plus the signature of the list they were built from. */
static std::vector<ReactRow> g_react_rows;
static int         g_react_sig_source = -1;
static size_t      g_react_sig_count  = (size_t)-1;
static std::string g_react_sig_front, g_react_sig_back;

/* ---------------------------------------------------------------- helpers */

static std::vector<AsmAnim> &ReactAnims(void)
{
    return (g_react_source == ReactSource_Player) ? g_asm_anims : g_asm_opp_anims;
}

static const char *ReactSourcePath(void)
{
    return (g_react_source == ReactSource_Player) ? AsmAnimPlayerPath()
                                                  : AsmAnimOpponentPath();
}

/* Trailing filename of a path, for the "which ASM is this" label. */
static const char *ReactBaseName(const char *path)
{
    if (!path || !path[0]) return "";
    const char *slash = strrchr(path, '\\');
    const char *fwd   = strrchr(path, '/');
    if (fwd && (!slash || fwd > slash)) slash = fwd;
    return slash ? slash + 1 : path;
}

static void ReactClearTexture(void)
{
    if (g_react_tex) SDL_DestroyTexture(g_react_tex);
    g_react_tex = NULL;
    g_react_tex_w = g_react_tex_h = 0;
    g_react_canvas_w = g_react_canvas_h = 0;
    g_react_last_drawn = -1;
    g_react_built_sel = -2;
    g_react_built_docs = -1;
}

/* Rebuild the classification only when the underlying list actually changed.
   Classifying ~90 labels is cheap, but not at 60 Hz for a panel that is mostly
   idle, and the rows carry selection state that should survive a redraw. */
static void ReactSyncRows(void)
{
    std::vector<AsmAnim> &anims = ReactAnims();
    std::string front = anims.empty() ? std::string() : anims.front().label;
    std::string back  = anims.empty() ? std::string() : anims.back().label;
    if (g_react_sig_source == g_react_source &&
        g_react_sig_count == anims.size() &&
        g_react_sig_front == front && g_react_sig_back == back)
        return;

    g_react_sig_source = g_react_source;
    g_react_sig_count  = anims.size();
    g_react_sig_front  = front;
    g_react_sig_back   = back;

    g_react_rows.clear();
    g_react_rows.reserve(anims.size());
    for (int i = 0; i < (int)anims.size(); i++) {
        const AsmAnim &a = anims[i];
        /* AsmAnim::name is the anitab comment when the entry had one and the
           label otherwise, so it is exactly the "comment" the classifier
           wants -- and passing a label as a comment is harmless, because the
           label rules run first and are strictly more specific. */
        ReactionInfo info = ReactionClassify(a.label.c_str(), a.name.c_str());
        ReactRow row;
        row.anim_index = i;
        row.kind = info.kind;
        row.rule = info.rule;
        if (!info.thrower.empty()) {
            const char *who = ReactionFighterName(info.thrower.c_str());
            row.thrower = who ? who : info.thrower;
        }
        g_react_rows.push_back(row);
    }
    /* A selection from the previous list indexes nothing in this one. */
    if (g_react_sel >= (int)anims.size()) g_react_sel = -1;
    ReactClearTexture();
}

/* Anitab comments arrive as "10 - hit high" or, in Kintaro's table, "c =
   getting hit": a slot number then the description. The slot is the index the
   game itself uses to reach the animation, so it is worth keeping -- but in its
   own column, not welded to the name, because plenty of entries are a bare
   slot with no description at all ("3a -"), which reads as nothing.
   Returns the description, or the label when there is none. */
static std::string ReactSplitName(const AsmAnim &a, std::string *out_slot)
{
    if (out_slot) out_slot->clear();
    const std::string &n = a.name;
    size_t i = 0;
    while (i < n.size() && isxdigit((unsigned char)n[i])) i++;
    if (i > 0) {
        size_t j = i;
        while (j < n.size() && (n[j] == ' ' || n[j] == '	')) j++;
        bool sep = (j < n.size() && (n[j] == '-' || n[j] == '='));
        if (sep || j >= n.size()) {
            if (out_slot) *out_slot = n.substr(0, i);
            size_t k = sep ? j + 1 : j;
            while (k < n.size() && (n[k] == ' ' || n[k] == '	')) k++;
            std::string desc = n.substr(k);
            while (!desc.empty() && (desc.back() == ' ' || desc.back() == '	'))
                desc.pop_back();
            return desc.empty() ? a.label : desc;
        }
    }
    return n.empty() ? a.label : n;
}

/* Case-insensitive substring, for the filter box. */
static bool ReactMatchesFilter(const AsmAnim &a, const ReactRow &row)
{
    if (!g_react_filter[0]) return true;
    std::string needle = g_react_filter;
    for (char &c : needle) c = (char)tolower((unsigned char)c);
    auto hit = [&](const std::string &hay) {
        std::string low = hay;
        for (char &c : low) c = (char)tolower((unsigned char)c);
        return low.find(needle) != std::string::npos;
    };
    return hit(a.label) || hit(a.name) || hit(row.thrower) ||
           hit(ReactionKindName(row.kind));
}

/* Select a reaction, and arm the rest of the app for it: the ASM window's
   selection, the IMG auto-open pass, and the World View lane all key off the
   same globals, so one click here sets up every view that can show it. */
static void ReactSelect(int anim_index)
{
    std::vector<AsmAnim> &anims = ReactAnims();
    if (anim_index < 0 || anim_index >= (int)anims.size()) return;
    g_react_sel = anim_index;
    g_react_frame = 0;
    g_react_timer = 0.0f;
    ReactClearTexture();

    if (g_react_source == ReactSource_Player) {
        AsmAnimSelect(anim_index);          /* resolves + sizes the ASM window */
        g_request_asm_autoload = true;
    } else {
        g_asm_opp_sel = anim_index;
        AsmResolveAnimAgainstDoc(anims[anim_index], NULL);
        g_request_asm_opp_autoload = true;
    }
    WorldMarkedRestart(g_world_marked_state);
}

/* Size the preview canvas to the selected reaction and refill the current
   frame. Bounds span every frame so the sprite does not swim as it plays. */
static void ReactRefillTexture(const AsmAnim &a)
{
    /* Selecting a reaction only REQUESTS the IMGs it needs; the deferred
       autoload opens them a frame or more later. So the bounds have to be
       recomputed when the open-document set changes too, or a canvas sized
       while half the pieces were still unresolved would clip the rest. */
    int docs = document_tab_count();
    if (g_react_built_sel != g_react_sel || g_react_built_docs != docs) {
        g_react_canvas_w = g_react_canvas_h = 0;
        int bx = 0, by = 0, bw = 0, bh = 0;
        if (AsmAnimComputeBounds(a, &bx, &by, &bw, &bh)) {
            /* Widen the art bounds to take in the anipoint itself, which lives
               at (0,0) of this space. MK2 anchors are routinely outside the
               pixel rectangle -- JCSWEEPFALL1A's is 30px above the top of its
               own art -- and a preview cropped to the art alone puts the
               crosshair off-canvas, hiding the one relationship this tab is
               for: where the anchor sits relative to the body. */
            int x0 = (bx < 0) ? bx : 0;
            int y0 = (by < 0) ? by : 0;
            int x1 = (bx + bw > 0) ? bx + bw : 0;
            int y1 = (by + bh > 0) ? by + bh : 0;
            int cw = x1 - x0, ch = y1 - y0;
            if (cw < 1) cw = 1;
            if (ch < 1) ch = 1;
            if (cw > 1024) cw = 1024;
            if (ch > 1024) ch = 1024;
            g_react_minx = x0; g_react_miny = y0;
            g_react_canvas_w = cw; g_react_canvas_h = ch;
            g_react_built_sel = g_react_sel;
        }
        g_react_last_drawn = -1;
    }
    int w = g_react_canvas_w, h = g_react_canvas_h;
    if (w <= 0 || h <= 0 || a.frames.empty()) return;

    int fi = g_react_frame % (int)a.frames.size();
    if (g_react_tex && fi == g_react_last_drawn && docs == g_react_built_docs)
        return;
    g_react_built_docs = docs;

    if (!g_react_tex || g_react_tex_w != w || g_react_tex_h != h) {
        if (g_react_tex) SDL_DestroyTexture(g_react_tex);
        g_react_tex = SDL_CreateTexture(g_imgui_renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!g_react_tex) return;
        SDL_SetTextureBlendMode(g_react_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_react_tex, SDL_ScaleModeNearest);
        g_react_tex_w = w; g_react_tex_h = h;
    }

    void *pixels = NULL; int pitch = 0;
    if (SDL_LockTexture(g_react_tex, NULL, &pixels, &pitch) != 0) return;
    unsigned int *dst = (unsigned int *)pixels;
    int pitch_px = pitch / 4;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            dst[y * pitch_px + x] = 0x00000000u;
    unsigned int fallback = (g_react_source == ReactSource_Player)
                          ? g_asm_anim_doc_uid : g_asm_opp_doc_uid;
    AsmAnimCompositeFrame(a, fi, g_react_minx, g_react_miny, w, h,
                          fallback, dst, pitch_px);
    SDL_UnlockTexture(g_react_tex);
    g_react_last_drawn = fi;
}

/* The art anipoint this frame is anchored by: the first resolved piece's
   anix/aniy, as stored in the IMG. The first piece is the body in every
   multipart MK2 frame -- the later pieces are overlays hung off it. The
   script's cumulative ani_adjustxy is deliberately NOT folded in; the table
   shows it as its own dX/dY columns, because "where the art's anchor is" and
   "how far the script has walked it" are separate things to check.
   Returns false when no piece of the frame resolved to a loaded IMG. */
static bool ReactFrameAnipoint(const AsmAnimFrame &fr, unsigned int fallback_uid,
                               int *out_anix, int *out_aniy, IMG **out_img)
{
    for (size_t p = 0; p < fr.piece_img.size(); p++) {
        if (fr.piece_img[p] < 0) continue;
        unsigned int uid = (p < fr.piece_doc_uid.size() && fr.piece_doc_uid[p])
                         ? fr.piece_doc_uid[p] : fallback_uid;
        IMG *img = doc_get_img(document_from_uid(uid), fr.piece_img[p]);
        if (!img) continue;
        if (out_anix) *out_anix = (int)(short)img->anix;
        if (out_aniy) *out_aniy = (int)(short)img->aniy;
        if (out_img) *out_img = img;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------- list panel */

/* Draw the grouped, filtered reaction list. */
static void ReactDrawList(void)
{
    std::vector<AsmAnim> &anims = ReactAnims();

    /* Reaction families in fight order, with the rejects last and only when
       asked for. */
    static const ReactionKind kOrder[] = {
        ReactionKind::Hit,
        ReactionKind::Stagger,
        ReactionKind::Knockdown,
        ReactionKind::FlippedBy,
        ReactionKind::Getup,
        ReactionKind::Death,
        ReactionKind::NotReaction,
    };

    for (ReactionKind kind : kOrder) {
        if (kind == ReactionKind::NotReaction && !g_react_show_all) continue;

        /* Count matches first so the header can carry the number, and so an
           empty group after filtering disappears instead of showing a header
           with nothing under it. */
        std::vector<const ReactRow *> matches;
        for (const ReactRow &row : g_react_rows) {
            if (row.kind != kind) continue;
            if (row.anim_index >= (int)anims.size()) continue;
            if (!ReactMatchesFilter(anims[row.anim_index], row)) continue;
            matches.push_back(&row);
        }
        if (matches.empty()) continue;

        char header[96];
        snprintf(header, sizeof(header), "%s (%d)###react_grp_%d",
                 ReactionKindName(kind), (int)matches.size(), (int)kind);
        /* Reactions open, rejects closed: the point of the tab is the former. */
        ImGui::SetNextItemOpen(kind != ReactionKind::NotReaction,
                               ImGuiCond_FirstUseEver);
        if (!ImGui::CollapsingHeader(header)) continue;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", ReactionKindHelp(kind));

        for (const ReactRow *row : matches) {
            const AsmAnim &a = anims[row->anim_index];
            ImGui::PushID(row->anim_index);

            /* Label: the human name, with the thrower spelled out for the ten
               near-identical flipped-by rows, and the anitab slot kept in front
               as the index the game reaches this animation by. */
            std::string slot;
            std::string desc = ReactSplitName(a, &slot);
            char label[192];
            const char *shown = row->thrower.empty() ? desc.c_str()
                                                     : row->thrower.c_str();
            if (slot.empty())
                snprintf(label, sizeof(label), "  %s", shown);
            else
                snprintf(label, sizeof(label), "  %-3s %s", slot.c_str(), shown);

            bool selected = (row->anim_index == g_react_sel);
            if (ImGui::Selectable(label, selected)) ReactSelect(row->anim_index);
            /* Captured now: the meta text below becomes the "last item", and
               hovering the row is what should raise the tooltip. */
            bool row_hovered = ImGui::IsItemHovered();
            if (selected && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();

            /* Frame count and unresolved-piece warning ride on the right. */
            char meta[48];
            snprintf(meta, sizeof(meta), "%d fr", (int)a.frames.size());
            float meta_w = ImGui::CalcTextSize(meta).x;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - meta_w + 8.0f);
            if (a.missing > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", meta);
            else
                ImGui::TextDisabled("%s", meta);

            if (row_hovered) {
                ImGui::BeginTooltip();
                ImGui::Text("%s", a.label.c_str());
                if (a.name != a.label) ImGui::TextDisabled("; %s", a.name.c_str());
                if (!slot.empty())
                    ImGui::TextDisabled("anitab slot %s", slot.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("Sorted here by: %s", row->rule.c_str());
                if (a.missing > 0)
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                                       "%d piece(s) have no matching IMG frame",
                                       a.missing);
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
    }
}

/* ------------------------------------------------------- anipoint table */

static void ReactDrawAnipointTable(const AsmAnim &a)
{
    unsigned int fallback = (g_react_source == ReactSource_Player)
                          ? g_asm_anim_doc_uid : g_asm_opp_doc_uid;

    if (!ImGui::BeginTable("##react_anipts", 8,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                           ImGuiTableFlags_SizingStretchProp))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#",     ImGuiTableColumnFlags_WidthFixed, 34.0f);
    ImGui::TableSetupColumn("Piece", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("anix",  ImGuiTableColumnFlags_WidthFixed, 46.0f);
    ImGui::TableSetupColumn("aniy",  ImGuiTableColumnFlags_WidthFixed, 46.0f);
    ImGui::TableSetupColumn("dX",    ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableSetupColumn("dY",    ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableSetupColumn("Size",  ImGuiTableColumnFlags_WidthFixed, 66.0f);
    ImGui::TableSetupColumn("Flip",  ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableHeadersRow();

    int cur = a.frames.empty() ? -1 : (g_react_frame % (int)a.frames.size());

    for (int fi = 0; fi < (int)a.frames.size(); fi++) {
        const AsmAnimFrame &fr = a.frames[fi];
        int anix = 0, aniy = 0;
        IMG *img = NULL;
        bool resolved = ReactFrameAnipoint(fr, fallback, &anix, &aniy, &img);

        ImGui::TableNextRow();
        /* The frame on screen is highlighted, so scrubbing the preview and
           reading the table are the same act. */
        if (fi == cur)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   ImGui::GetColorU32(ImVec4(0.34f, 0.24f, 0.48f, 0.65f)));

        ImGui::TableNextColumn();
        ImGui::PushID(fi);
        char num[16];
        snprintf(num, sizeof(num), "%d", fi + 1);
        if (ImGui::Selectable(num, fi == cur, ImGuiSelectableFlags_SpanAllColumns)) {
            g_react_frame = fi;
            g_react_play = false;
        }
        ImGui::PopID();

        ImGui::TableNextColumn();
        if (!fr.piece_syms.empty()) {
            /* Extra pieces are overlays on the first; name the body and say how
               many rode along rather than wrapping a long symbol list. */
            if (fr.piece_syms.size() > 1)
                ImGui::Text("%s  +%d", fr.piece_syms[0].c_str(),
                            (int)fr.piece_syms.size() - 1);
            else
                ImGui::TextUnformatted(fr.piece_syms[0].c_str());
            if (ImGui::IsItemHovered() && fr.piece_syms.size() > 1) {
                ImGui::BeginTooltip();
                for (size_t p = 0; p < fr.piece_syms.size(); p++) {
                    bool ok = (p < fr.piece_img.size() && fr.piece_img[p] >= 0);
                    if (ok) ImGui::TextUnformatted(fr.piece_syms[p].c_str());
                    else ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                                            "%s  (no IMG frame)",
                                            fr.piece_syms[p].c_str());
                }
                ImGui::EndTooltip();
            }
        } else {
            ImGui::TextDisabled("(empty)");
        }

        if (resolved) {
            ImGui::TableNextColumn(); ImGui::Text("%d", anix);
            ImGui::TableNextColumn(); ImGui::Text("%d", aniy);
        } else {
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "--");
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "--");
        }

        /* dX/dY are the script's cumulative ani_adjustxy at this frame, i.e.
           how far the anchor has travelled from where the animation started. */
        ImGui::TableNextColumn();
        if (fr.dx) ImGui::Text("%+d", fr.dx); else ImGui::TextDisabled("0");
        ImGui::TableNextColumn();
        if (fr.dy) ImGui::Text("%+d", fr.dy); else ImGui::TextDisabled("0");

        ImGui::TableNextColumn();
        if (img) ImGui::TextDisabled("%dx%d", img->w, img->h);
        else     ImGui::TextDisabled("--");

        ImGui::TableNextColumn();
        if (fr.mirror && fr.mirror_v)      ImGui::TextUnformatted("HV");
        else if (fr.mirror)                ImGui::TextUnformatted("H");
        else if (fr.mirror_v)              ImGui::TextUnformatted("V");
        else                               ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
}

/* Everything in the table, as tab-separated text on the clipboard. Lining a
   move up against a reaction usually ends in a spreadsheet or an ASM edit. */
static void ReactCopyAnipoints(const AsmAnim &a)
{
    unsigned int fallback = (g_react_source == ReactSource_Player)
                          ? g_asm_anim_doc_uid : g_asm_opp_doc_uid;
    std::string out = "; " + a.label;
    if (a.name != a.label) out += "  ; " + a.name;
    out += "\nframe\tpiece\tanix\taniy\tdx\tdy\tflip\n";
    for (int fi = 0; fi < (int)a.frames.size(); fi++) {
        const AsmAnimFrame &fr = a.frames[fi];
        int anix = 0, aniy = 0;
        bool resolved = ReactFrameAnipoint(fr, fallback, &anix, &aniy, NULL);
        char ax[16] = "-", ay[16] = "-";
        if (resolved) {
            snprintf(ax, sizeof(ax), "%d", anix);
            snprintf(ay, sizeof(ay), "%d", aniy);
        }
        char line[256];
        snprintf(line, sizeof(line), "%d\t%s\t%s\t%s\t%d\t%d\t%s\n",
                 fi + 1,
                 fr.piece_syms.empty() ? "-" : fr.piece_syms[0].c_str(),
                 ax, ay,
                 fr.dx, fr.dy,
                 fr.mirror && fr.mirror_v ? "HV" : fr.mirror ? "H"
                                          : fr.mirror_v ? "V" : "-");
        out += line;
    }
    ImGui::SetClipboardText(out.c_str());
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Copied %d anipoint row(s) for %s", (int)a.frames.size(),
             a.label.c_str());
    g_restore_msg_timer = 3.0f;
}

/* ----------------------------------------------------------- detail panel */

static void ReactDrawDetail(ImVec2 avail, ImGuiIO &io)
{
    std::vector<AsmAnim> &anims = ReactAnims();
    if (g_react_sel < 0 || g_react_sel >= (int)anims.size()) {
        ImGui::TextDisabled("Pick a reaction on the left to see its frames and "
                            "anipoints.");
        return;
    }
    AsmAnim &a = anims[g_react_sel];
    int nframes = (int)a.frames.size();

    /* ---- Title line ---- */
    std::string slot;
    std::string desc = ReactSplitName(a, &slot);
    ImGui::TextUnformatted(desc.c_str());
    ImGui::SameLine();
    if (slot.empty()) ImGui::TextDisabled("(%s)", a.label.c_str());
    else              ImGui::TextDisabled("(%s · anitab slot %s)",
                                          a.label.c_str(), slot.c_str());

    /* ---- Transport ---- */
    if (nframes > 0) {
        if (g_react_play && g_react_fps > 0.0f) {
            g_react_timer += io.DeltaTime;
            float step = 1.0f / g_react_fps;
            while (g_react_timer >= step) {
                g_react_timer -= step;
                g_react_frame = (g_react_frame + 1) % nframes;
            }
        }
        ImGui::Checkbox("Play", &g_react_play);
        ImGui::SameLine(); ImGui::SetNextItemWidth(110);
        ImGui::SliderFloat("fps", &g_react_fps, 1.0f, 60.0f, "%.1f");
        ImGui::SameLine();
        if (ImGui::SmallButton("Game")) g_react_fps = kMk2TickHz;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("MK2's hardware tick rate, %.1f Hz.", kMk2TickHz);
        ImGui::SameLine(); ImGui::SetNextItemWidth(150);
        int disp = g_react_frame % nframes + 1;
        if (ImGui::SliderInt("##react_frame", &disp, 1, nframes, "frame %d")) {
            g_react_frame = disp - 1;
            g_react_play = false;
        }
    }

    /* ---- Preview ---- */
    ReactRefillTexture(a);
    /* Give the preview a bounded share of the panel: the table below it is the
       reason the tab exists, so the picture must not crowd it out. */
    float preview_h = avail.y * 0.42f;
    if (preview_h < 90.0f) preview_h = 90.0f;
    if (preview_h > 300.0f) preview_h = 300.0f;

    ImGui::BeginChild("##react_preview", ImVec2(0, preview_h), true,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);
    if (g_react_tex && g_react_canvas_w > 0 && g_react_canvas_h > 0) {
        ImVec2 box = ImGui::GetContentRegionAvail();
        float sx = box.x / (float)g_react_canvas_w;
        float sy = box.y / (float)g_react_canvas_h;
        float scale = (sx < sy) ? sx : sy;
        if (scale > 4.0f) scale = 4.0f;
        if (scale < 0.1f) scale = 0.1f;
        ImVec2 sz((float)g_react_canvas_w * scale, (float)g_react_canvas_h * scale);
        /* Centre it, then draw the anipoint crosshair at the anchor the whole
           animation is pinned to -- which is where the attacker has to meet it. */
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 pad((box.x - sz.x) * 0.5f, (box.y - sz.y) * 0.5f);
        if (pad.x < 0.0f) pad.x = 0.0f;
        if (pad.y < 0.0f) pad.y = 0.0f;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad.x, origin.y + pad.y));
        ImVec2 img_pos = ImGui::GetCursorScreenPos();
        ImGui::Image((ImTextureID)(intptr_t)g_react_tex, sz);

        /* The anipoint sits at (-minx, -miny) in canvas space by construction:
           every piece was placed at -anipoint, so the origin of that space is
           the anipoint itself. */
        float cx = img_pos.x + (float)(-g_react_minx) * scale;
        float cy = img_pos.y + (float)(-g_react_miny) * scale;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImU32 col = IM_COL32(0xFF, 0xC0, 0x30, 0xE0);
        dl->AddLine(ImVec2(cx - 7, cy), ImVec2(cx + 7, cy), col, 1.0f);
        dl->AddLine(ImVec2(cx, cy - 7), ImVec2(cx, cy + 7), col, 1.0f);
        dl->AddCircle(ImVec2(cx, cy), 3.0f, col, 8, 1.0f);
    } else if (a.missing > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                           "No frame of this reaction resolves to a loaded IMG.");
        ImGui::TextDisabled("Selecting it asks imgtool to open the sprite files "
                            "it needs; if they are not beside the ASM, open them "
                            "as tabs yourself.");
    } else {
        ImGui::TextDisabled("Nothing to preview.");
    }
    ImGui::EndChild();

    /* ---- Frame / anipoint summary ---- */
    ImGui::Text("%d frame%s", nframes, nframes == 1 ? "" : "s");
    if (a.missing > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                           "· %d unresolved piece%s", a.missing,
                           a.missing == 1 ? "" : "s");
    }
    if (!a.control.empty()) {
        std::string ctl;
        for (const std::string &c : a.control) {
            if (!ctl.empty()) ctl += ", ";
            ctl += c;
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "· %s", ctl.c_str());
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Copy anipoints")) ReactCopyAnipoints(a);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every row below as tab-separated text.");

    ImGui::SameLine();
    bool *lane = (g_react_source == ReactSource_Player) ? &g_asm_lane_enabled
                                                        : &g_asm_opp_enabled;
    if (ImGui::Checkbox("World View lane", lane) && *lane) {
        g_world_state.enabled = true;
        g_world_marked_state.marked_play = true;
        WorldMarkedRestart(g_world_marked_state);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Play this reaction as a World View lane, so you can "
                          "stage an attack against it.\n"
                          "Ticking it switches the canvas to World View.");

    ReactDrawAnipointTable(a);
}

/* -------------------------------------------------------------- workspace */

bool DrawReactionWorkspace(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io)
{
    if (avail.x < 64.0f || avail.y < 64.0f) return true;
    ImGui::SetCursorScreenPos(img_pos);

    ReactSyncRows();

    /* ---- Source bar ---- */
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Reactions of:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190);
    const char *src_label = (g_react_source == ReactSource_Player)
                          ? "Player ASM" : "Opponent ASM";
    if (ImGui::BeginCombo("##react_src", src_label)) {
        /* Opponent first: this tab exists to look at what you are hitting. */
        if (ImGui::Selectable("Opponent ASM", g_react_source == ReactSource_Opponent)) {
            g_react_source = ReactSource_Opponent;
            g_react_sel = -1;
            ReactClearTexture();
        }
        if (ImGui::Selectable("Player ASM", g_react_source == ReactSource_Player)) {
            g_react_source = ReactSource_Player;
            g_react_sel = -1;
            ReactClearTexture();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Which loaded character ASM to list reactions from.\n"
                          "Both are the ones the ASM Animations window loaded.");

    ImGui::SameLine();
    const char *path = ReactSourcePath();
    if (path && path[0]) {
        ImGui::TextDisabled("%s", ReactBaseName(path));
    } else {
        ImGui::TextDisabled("(none loaded)");
    }

    ImGui::SameLine();
    if (ImGui::Button(g_react_source == ReactSource_Player ? "Load Character ASM..."
                                                           : "Load Opponent ASM...")) {
        if (g_react_source == ReactSource_Player) g_request_load_asm = true;
        else                                      g_request_load_opp_asm = true;
    }

    std::vector<AsmAnim> &anims = ReactAnims();
    if (anims.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped(
            "No character ASM loaded for this side yet.\n\n"
            "Load a per-character ASM (MKJC.ASM for Johnny Cage, MKLK.ASM for "
            "Liu Kang, and so on). imgtool reads its anitab, sorts out the "
            "animations that happen TO that fighter -- hits, staggers, "
            "knockdowns, throws, getups and fatality bodies -- and lists each "
            "one's frames with the anipoint the game anchors them at.");
        return true;
    }

    /* Reaction/total count and the two list controls. */
    int reactions = 0;
    for (const ReactRow &row : g_react_rows)
        if (row.kind != ReactionKind::NotReaction) reactions++;
    ImGui::TextDisabled("%d reaction%s of %d animation%s",
                        reactions, reactions == 1 ? "" : "s",
                        (int)anims.size(), anims.size() == 1 ? "" : "s");
    ImGui::SameLine();
    ImGui::Checkbox("Show all animations", &g_react_show_all);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add a 'Not a reaction' group holding everything the "
                          "sort left out.\nThe sort reads 1993 comments and "
                          "label spellings, so check here if\nsomething you "
                          "expected is missing.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##react_filter", "filter", g_react_filter,
                             sizeof(g_react_filter));

    ImGui::Separator();

    /* ---- Two panes: list, then the selected reaction ---- */
    ImVec2 body = ImGui::GetContentRegionAvail();
    if (body.x < 32.0f || body.y < 32.0f) return true;
    float list_w = body.x * 0.30f;
    if (list_w < 180.0f) list_w = 180.0f;
    if (list_w > 340.0f) list_w = 340.0f;
    if (list_w > body.x - 200.0f) list_w = body.x * 0.5f;

    ImGui::BeginChild("##react_list", ImVec2(list_w, body.y), true);
    ReactDrawList();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##react_detail", ImVec2(0, body.y), true);
    ReactDrawDetail(ImGui::GetContentRegionAvail(), io);
    ImGui::EndChild();

    return true;
}
