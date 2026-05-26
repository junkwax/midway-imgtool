/*************************************************************
 * platform/mk2_fatality.h
 * MK2 fatality source editor helpers.
 *
 * This is intentionally source-backed. Fatality commands live mostly in
 * MOVES.ASM, controller sequences live in scom_* tables, and victim/body
 * animation sequences live in the MK*.ASM character files. The UI edits
 * raw assembly lines so the original source remains the source of truth.
 *************************************************************/
#ifndef MK2_FATALITY_H
#define MK2_FATALITY_H

#include <string>
#include <vector>

namespace mk2fatal {

struct SourceFile {
    std::string rel_path;
    std::string full_path;
    std::vector<std::string> lines; /* 1-based via lines[line-1] */
    bool dirty = false;
};

struct CommandBlock {
    std::string label;
    std::string routine;      /* movi <routine>,a7 */
    std::string transfer;     /* fatality_xfer or fatality_xfer_close */
    std::string combo_label;  /* movi <scom_*>,a11 when present */
    std::string trigger;      /* l_* or f*b* token when detected */
    std::string range_note;
    int file_idx = -1;
    int start_line = 0;
    int end_line = 0;
    bool finish_him_only = false;
};

struct ComboBlock {
    std::string label;
    std::string time_token;
    std::vector<std::string> words;
    int file_idx = -1;
    int start_line = 0;
    int end_line = 0;
};

struct AnimationBlock {
    std::string label;
    std::string file_rel;
    int file_idx = -1;
    int start_line = 0;
    int end_line = 0;
    int long_count = 0;
    int word_count = 0;
    int adjust_count = 0;
    bool body_ending = false;
};

struct AssetPlan {
    std::string root_label;
    std::string resolved_label;
    std::string preferred_file;
    std::vector<std::string> animation_labels;
    std::vector<std::string> sprite_labels;
    std::vector<std::string> missing_labels;
    std::vector<std::string> img_files;
};

struct Document {
    std::string root_path;
    std::vector<SourceFile> files;
    std::vector<CommandBlock> commands;
    std::vector<ComboBlock> combos;
    std::vector<AnimationBlock> animations;
    bool dirty = false;
};

void clear(Document *doc);
bool load(Document *doc, const char *root_path, std::string *err);
bool save(Document *doc, std::string *err);
bool reparse(Document *doc, std::string *err);

bool set_line(Document *doc, int file_idx, int line_1based, const char *text);
bool insert_line(Document *doc, int file_idx, int before_line_1based, const char *text);
bool delete_line(Document *doc, int file_idx, int line_1based);

int find_command(const Document *doc, const char *label);
int find_combo(const Document *doc, const char *label);
int find_animation(const Document *doc, const char *label);
bool build_asset_plan(const Document *doc, const char *animation_label,
                      const char *preferred_file, AssetPlan *plan,
                      std::string *err);

const SourceFile *get_file(const Document *doc, int file_idx);

} /* namespace mk2fatal */

#endif /* MK2_FATALITY_H */
