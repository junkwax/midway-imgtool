/*************************************************************
 * platform/mk2_fatality.cpp
 * Source-backed MK2 fatality browser/editor.
 *************************************************************/
#include "mk2_fatality.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace mk2fatal {

static std::string trim_copy(const std::string &s)
{
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string lower_copy(std::string s)
{
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool contains_icase(const std::string &haystack, const char *needle)
{
    if (!needle || !needle[0]) return true;
    std::string h = lower_copy(haystack);
    std::string n = lower_copy(std::string(needle));
    return h.find(n) != std::string::npos;
}

static bool starts_with_icase(const std::string &s, const char *prefix)
{
    if (!prefix) return false;
    size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower((unsigned char)s[i]) != std::tolower((unsigned char)prefix[i]))
            return false;
    return true;
}

static bool is_path_sep(char c)
{
    return c == '/' || c == '\\';
}

static std::string join_path(const std::string &a, const std::string &b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (is_path_sep(a[a.size() - 1])) return a + b;
    return a + "/" + b;
}

static bool file_exists(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

static void split_lines(const std::string &text, std::vector<std::string> *out)
{
    out->clear();
    std::string cur;
    cur.reserve(128);
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n') { out->push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out->push_back(cur);
}

static bool read_text_file(const std::string &path, std::vector<std::string> *lines, std::string *err)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) { *err = "cannot open "; *err += path; }
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string text;
    if (sz > 0) {
        text.resize((size_t)sz);
        size_t got = std::fread(&text[0], 1, (size_t)sz, f);
        if (got < (size_t)sz) text.resize(got);
    }
    std::fclose(f);
    split_lines(text, lines);
    return true;
}

static bool write_text_file(const std::string &path, const std::vector<std::string> &lines, std::string *err)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (err) { *err = "cannot write "; *err += path; }
        return false;
    }
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &l = lines[i];
        if (!l.empty()) std::fwrite(l.data(), 1, l.size(), f);
        std::fputc('\n', f);
    }
    std::fclose(f);
    return true;
}

static bool label_from_line(const std::string &line, std::string *label)
{
    if (line.empty()) return false;
    if (std::isspace((unsigned char)line[0])) return false;
    std::string t = trim_copy(line);
    if (t.empty()) return false;
    if (t[0] == ';' || t[0] == '*' || t[0] == '.' || t[0] == '=') return false;

    size_t p = 0;
    while (p < t.size() && !std::isspace((unsigned char)t[p]) && t[p] != ':') p++;
    if (p == 0) return false;
    std::string tok = t.substr(0, p);
    if (tok.empty()) return false;
    if (std::isdigit((unsigned char)tok[0])) return false;
    if (tok.find('=') != std::string::npos || tok.find(',') != std::string::npos) return false;
    for (char c : tok) {
        if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    }
    if (label) *label = tok;
    return true;
}

struct LabelSpan {
    std::string label;
    int line = 0;      /* 1-based */
    int end_line = 0;  /* inclusive */
};

static void collect_labels(const SourceFile &sf, std::vector<LabelSpan> *labels)
{
    labels->clear();
    for (int i = 0; i < (int)sf.lines.size(); i++) {
        std::string label;
        if (!label_from_line(sf.lines[i], &label)) continue;
        LabelSpan span;
        span.label = label;
        span.line = i + 1;
        labels->push_back(span);
    }
    for (size_t i = 0; i < labels->size(); i++) {
        int next = (i + 1 < labels->size()) ? (*labels)[i + 1].line : ((int)sf.lines.size() + 1);
        (*labels)[i].end_line = next - 1;
    }
}

static std::string strip_comment(const std::string &line)
{
    size_t semi = line.find(';');
    if (semi == std::string::npos) return line;
    return line.substr(0, semi);
}

static void split_tokens(const std::string &raw, std::vector<std::string> *out)
{
    out->clear();
    std::string cur;
    for (size_t i = 0; i < raw.size(); i++) {
        char c = raw[i];
        if (c == ',' || std::isspace((unsigned char)c)) {
            if (!cur.empty()) { out->push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out->push_back(cur);
}

static std::string directive_scan_text(const std::string &line)
{
    std::string s = trim_copy(strip_comment(line));
    if (s.empty()) return s;
    if (s[0] == '.') return s;

    std::string label;
    if (!label_from_line(line, &label)) return s;
    size_t p = s.find(label);
    if (p == std::string::npos) return s;
    p += label.size();
    if (p < s.size() && s[p] == ':') p++;
    while (p < s.size() && std::isspace((unsigned char)s[p])) p++;
    return s.substr(p);
}

static bool parse_directive_tokens(const std::string &line, const char *directive, std::vector<std::string> *tokens)
{
    std::string s = directive_scan_text(line);
    if (!starts_with_icase(s, directive)) return false;
    size_t n = std::strlen(directive);
    if (s.size() > n && !std::isspace((unsigned char)s[n])) return false;
    split_tokens(s.substr(n), tokens);
    return true;
}

static std::string token_before_register(const std::string &line, const char *reg)
{
    std::string s = trim_copy(strip_comment(line));
    if (!starts_with_icase(s, "movi")) return std::string();
    std::vector<std::string> toks;
    split_tokens(s.substr(4), &toks);
    if (toks.size() < 2) return std::string();
    if (lower_copy(toks[1]) == lower_copy(std::string(reg))) return toks[0];
    return std::string();
}

static bool block_contains(const SourceFile &sf, int start_line, int end_line, const char *needle)
{
    for (int line = start_line; line <= end_line && line <= (int)sf.lines.size(); line++) {
        if (contains_icase(sf.lines[line - 1], needle)) return true;
    }
    return false;
}

static std::string detect_transfer(const SourceFile &sf, int start_line, int end_line)
{
    if (block_contains(sf, start_line, end_line, "fatality_xfer_close")) return "fatality_xfer_close";
    if (block_contains(sf, start_line, end_line, "fatality_xfer")) return "fatality_xfer";
    return std::string();
}

static std::string detect_range_note(const SourceFile &sf, int start_line, int end_line)
{
    bool saw_dist = false;
    std::string out;
    for (int line = start_line; line <= end_line && line <= (int)sf.lines.size(); line++) {
        std::string s = lower_copy(strip_comment(sf.lines[line - 1]));
        if (s.find("get_x_dist") != std::string::npos) {
            saw_dist = true;
            continue;
        }
        if (!saw_dist) continue;
        if (s.find("cmpi") != std::string::npos && s.find(",a3") != std::string::npos) {
            std::vector<std::string> toks;
            split_tokens(strip_comment(sf.lines[line - 1]), &toks);
            if (toks.size() >= 2) {
                if (!out.empty()) out += ", ";
                out += toks[1];
            }
        }
    }
    if (!out.empty()) out = "x distance checks: " + out;
    return out;
}

static bool is_command_candidate(const SourceFile &sf, const LabelSpan &span)
{
    if (starts_with_icase(span.label, "fatality_xfer")) return false;
    if (starts_with_icase(span.label, "do_fatality")) return false;
    if (starts_with_icase(span.label, "restricted_xfer")) return false;
    if (starts_with_icase(span.label, "airborn_xfer")) return false;
    if (span.label == "button_bit_check") return false;
    std::string transfer = detect_transfer(sf, span.line, span.end_line);
    if (transfer.empty()) return false;
    if (block_contains(sf, span.line, span.end_line, "winner_status") ||
        block_contains(sf, span.line, span.end_line, "do_fatal") ||
        block_contains(sf, span.line, span.end_line, "_fatal") ||
        block_contains(sf, span.line, span.end_line, "fatality")) {
        return true;
    }
    return false;
}

static void parse_commands(Document *doc)
{
    int moves_idx = -1;
    for (int i = 0; i < (int)doc->files.size(); i++) {
        if (contains_icase(doc->files[i].rel_path, "MOVES.ASM")) { moves_idx = i; break; }
    }
    if (moves_idx < 0) return;

    const SourceFile &sf = doc->files[moves_idx];
    std::vector<LabelSpan> labels;
    collect_labels(sf, &labels);

    for (const LabelSpan &span : labels) {
        if (!is_command_candidate(sf, span)) continue;
        CommandBlock cmd;
        cmd.label = span.label;
        cmd.file_idx = moves_idx;
        cmd.start_line = span.line;
        cmd.end_line = span.end_line;
        cmd.transfer = detect_transfer(sf, span.line, span.end_line);
        cmd.finish_him_only =
            block_contains(sf, span.line, span.end_line, "winner_status") &&
            block_contains(sf, span.line, span.end_line, "cmpi");
        cmd.range_note = detect_range_note(sf, span.line, span.end_line);

        for (int line = span.line; line <= span.end_line && line <= (int)sf.lines.size(); line++) {
            std::string r = token_before_register(sf.lines[line - 1], "a7");
            if (!r.empty()) cmd.routine = r;
            std::string combo = token_before_register(sf.lines[line - 1], "a11");
            if (!combo.empty()) cmd.combo_label = combo;

            std::string a0 = token_before_register(sf.lines[line - 1], "a0");
            if (!a0.empty() && cmd.trigger.empty() &&
                (contains_icase(a0, "l_") || contains_icase(a0, "f1b") || contains_icase(a0, "f2b")))
                cmd.trigger = a0;
        }
        doc->commands.push_back(cmd);
    }
}

static bool is_combo_candidate(const std::string &label)
{
    return starts_with_icase(label, "scom_");
}

static void parse_combos(Document *doc)
{
    int moves_idx = -1;
    for (int i = 0; i < (int)doc->files.size(); i++) {
        if (contains_icase(doc->files[i].rel_path, "MOVES.ASM")) { moves_idx = i; break; }
    }
    if (moves_idx < 0) return;

    const SourceFile &sf = doc->files[moves_idx];
    std::vector<LabelSpan> labels;
    collect_labels(sf, &labels);
    for (const LabelSpan &span : labels) {
        if (!is_combo_candidate(span.label)) continue;
        ComboBlock combo;
        combo.label = span.label;
        combo.file_idx = moves_idx;
        combo.start_line = span.line;
        combo.end_line = span.end_line;
        for (int line = span.line + 1; line <= span.end_line && line <= (int)sf.lines.size(); line++) {
            std::vector<std::string> toks;
            if (!parse_directive_tokens(sf.lines[line - 1], ".word", &toks)) continue;
            for (const std::string &tok : toks) {
                if (combo.time_token.empty()) combo.time_token = tok;
                combo.words.push_back(tok);
            }
        }
        doc->combos.push_back(combo);
    }
}

static bool is_animation_candidate(const std::string &label)
{
    std::string l = lower_copy(label);
    if (starts_with_icase(label, "a_")) return true;
    const char *terms[] = {
        "fatal", "torso", "decap", "headhole", "head", "ripped", "swipe", "burn", "freeze"
    };
    for (const char *term : terms)
        if (l.find(term) != std::string::npos) return true;
    return false;
}

static bool is_body_ending_label(const std::string &label)
{
    std::string l = lower_copy(label);
    const char *terms[] = {
        "torso", "decap", "head", "headhole", "ripped", "swipe", "skel", "burn"
    };
    for (const char *term : terms)
        if (l.find(term) != std::string::npos) return true;
    return false;
}

static bool vector_has_icase(const std::vector<std::string> &items, const std::string &needle)
{
    std::string n = lower_copy(needle);
    for (const std::string &item : items)
        if (lower_copy(item) == n) return true;
    return false;
}

static void push_unique_icase(std::vector<std::string> *items, const std::string &value)
{
    if (value.empty()) return;
    if (vector_has_icase(*items, value)) return;
    items->push_back(value);
}

static bool token_is_number(const std::string &tok)
{
    if (tok.empty()) return false;
    size_t i = 0;
    if (tok[i] == '-' || tok[i] == '+') i++;
    if (i >= tok.size()) return false;
    if (tok.size() - i > 1 && tok[i] == '0' && (tok[i + 1] == 'x' || tok[i + 1] == 'X')) {
        i += 2;
        if (i >= tok.size()) return false;
        for (; i < tok.size(); i++)
            if (!std::isxdigit((unsigned char)tok[i])) return false;
        return true;
    }
    bool saw_digit = false;
    for (; i < tok.size(); i++) {
        char c = tok[i];
        if (c == 'h' || c == 'H') return saw_digit;
        if (!std::isxdigit((unsigned char)c)) return false;
        if (std::isdigit((unsigned char)c)) saw_digit = true;
    }
    return saw_digit;
}

static bool token_is_ignored_anim_op(const std::string &tok)
{
    if (tok.empty()) return true;
    if (token_is_number(tok)) return true;
    if (starts_with_icase(tok, "ani_")) return true;
    static const char *ops[] = {
        "change_to_bloody_pal", "current_proc", "tsound", "rsnd", "shake_a11",
        "make_ochar_skel", "make_skeleton", "ground_player", "stop_me",
        "ochar_jump", "db1_table", "db2_table"
    };
    for (const char *op : ops)
        if (lower_copy(tok) == lower_copy(std::string(op))) return true;
    return false;
}

static bool token_looks_sprite(const std::string &tok)
{
    if (tok.empty()) return false;
    bool has_upper = false;
    bool has_alpha = false;
    for (char c : tok) {
        if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
        if (std::isalpha((unsigned char)c)) {
            has_alpha = true;
            if (std::isupper((unsigned char)c)) has_upper = true;
        }
    }
    return has_alpha && has_upper;
}

static bool file_matches_preference(const SourceFile &sf, const std::string &preferred_file)
{
    if (preferred_file.empty()) return false;
    return contains_icase(sf.rel_path, preferred_file.c_str()) ||
           contains_icase(sf.full_path, preferred_file.c_str());
}

static bool find_label_span(const Document *doc, const std::string &label,
                            const std::string &preferred_file,
                            int *file_idx, LabelSpan *out)
{
    int fallback_file = -1;
    LabelSpan fallback_span;
    bool have_fallback = false;

    for (int fi = 0; fi < (int)doc->files.size(); fi++) {
        const SourceFile &sf = doc->files[fi];
        std::vector<LabelSpan> labels;
        collect_labels(sf, &labels);
        for (const LabelSpan &span : labels) {
            if (lower_copy(span.label) != lower_copy(label)) continue;
            if (!have_fallback) {
                fallback_file = fi;
                fallback_span = span;
                have_fallback = true;
            }
            if (file_matches_preference(sf, preferred_file)) {
                if (file_idx) *file_idx = fi;
                if (out) *out = span;
                return true;
            }
        }
    }

    if (have_fallback) {
        if (file_idx) *file_idx = fallback_file;
        if (out) *out = fallback_span;
        return true;
    }
    return false;
}

static void collect_asset_refs(const Document *doc, const std::string &label,
                               const std::string &preferred_file,
                               AssetPlan *plan, int depth)
{
    if (!doc || !plan || label.empty()) return;
    if (depth > 64) {
        push_unique_icase(&plan->missing_labels, label + " (depth limit)");
        return;
    }
    if (vector_has_icase(plan->animation_labels, label)) return;

    int file_idx = -1;
    LabelSpan span;
    if (!find_label_span(doc, label, preferred_file, &file_idx, &span)) {
        if (!token_is_ignored_anim_op(label) && token_looks_sprite(label))
            push_unique_icase(&plan->sprite_labels, label);
        else if (!token_is_ignored_anim_op(label))
            push_unique_icase(&plan->missing_labels, label);
        return;
    }

    const SourceFile &sf = doc->files[file_idx];
    if (plan->resolved_label.empty()) {
        plan->resolved_label = span.label;
        plan->preferred_file = sf.rel_path;
    }
    push_unique_icase(&plan->animation_labels, span.label);

    for (int line = span.line; line <= span.end_line && line <= (int)sf.lines.size(); line++) {
        std::vector<std::string> toks;
        if (!parse_directive_tokens(sf.lines[line - 1], ".long", &toks)) continue;
        for (const std::string &tok : toks) {
            if (token_is_ignored_anim_op(tok)) continue;
            int child_file = -1;
            LabelSpan child_span;
            if (find_label_span(doc, tok, preferred_file, &child_file, &child_span)) {
                collect_asset_refs(doc, tok, preferred_file, plan, depth + 1);
            } else if (token_looks_sprite(tok)) {
                push_unique_icase(&plan->sprite_labels, tok);
            } else {
                push_unique_icase(&plan->missing_labels, tok);
            }
        }
    }
}

static void parse_animations(Document *doc)
{
    for (int fi = 0; fi < (int)doc->files.size(); fi++) {
        const SourceFile &sf = doc->files[fi];
        if (contains_icase(sf.rel_path, "MOVES.ASM")) continue;

        std::vector<LabelSpan> labels;
        collect_labels(sf, &labels);
        for (const LabelSpan &span : labels) {
            if (!is_animation_candidate(span.label)) continue;

            AnimationBlock anim;
            anim.label = span.label;
            anim.file_idx = fi;
            anim.file_rel = sf.rel_path;
            anim.start_line = span.line;
            anim.end_line = span.end_line;
            anim.body_ending = is_body_ending_label(span.label);
            for (int line = span.line; line <= span.end_line && line <= (int)sf.lines.size(); line++) {
                std::vector<std::string> toks;
                if (parse_directive_tokens(sf.lines[line - 1], ".long", &toks)) {
                    anim.long_count += (int)toks.size();
                    for (const std::string &tok : toks)
                        if (contains_icase(tok, "ani_adjustxy")) anim.adjust_count++;
                } else if (parse_directive_tokens(sf.lines[line - 1], ".word", &toks)) {
                    anim.word_count += (int)toks.size();
                }
            }
            doc->animations.push_back(anim);
        }
    }
}

void clear(Document *doc)
{
    if (!doc) return;
    doc->root_path.clear();
    doc->files.clear();
    doc->commands.clear();
    doc->combos.clear();
    doc->animations.clear();
    doc->dirty = false;
}

static std::string resolve_source_file(const std::string &root, const char *name, std::string *rel_out)
{
    std::string rel = join_path("src", name);
    std::string p = join_path(root, rel);
    if (file_exists(p)) {
        if (rel_out) *rel_out = rel;
        return p;
    }
    p = join_path(root, name);
    if (file_exists(p)) {
        if (rel_out) *rel_out = name;
        return p;
    }
    if (rel_out) *rel_out = rel;
    return join_path(root, rel);
}

bool load(Document *doc, const char *root_path, std::string *err)
{
    if (!doc) return false;
    clear(doc);
    std::string root = root_path && root_path[0] ? root_path : ".";
    doc->root_path = root;

    static const char *kFiles[] = {
        "MOVES.ASM",
        "MKFN.ASM", "MKNJ.ASM", "MKJC.ASM", "MKJX.ASM", "MKLK.ASM",
        "MKRD.ASM", "MKSA.ASM", "MKSK.ASM", "MKST.ASM", "MKHH.ASM",
        "MKBABY.ASM", "MKFRIEND.ASM", "MKDEATH.ASM", "MKBLOOD.ASM",
        "MKDIE.ASM", "MKFALL.ASM", "MKREACT.ASM", "MKFX.ASM", "MKPROP.ASM"
    };

    for (const char *name : kFiles) {
        std::string rel;
        std::string path = resolve_source_file(root, name, &rel);
        if (!file_exists(path)) {
            if (std::strcmp(name, "MOVES.ASM") == 0) {
                if (err) { *err = "MOVES.ASM not found under "; *err += root; }
                clear(doc);
                return false;
            }
            continue;
        }
        SourceFile sf;
        sf.rel_path = rel;
        sf.full_path = path;
        if (!read_text_file(path, &sf.lines, err)) {
            clear(doc);
            return false;
        }
        doc->files.push_back(sf);
    }

    std::string parse_err;
    if (!reparse(doc, &parse_err)) {
        if (err) *err = parse_err;
        clear(doc);
        return false;
    }
    doc->dirty = false;
    for (SourceFile &sf : doc->files) sf.dirty = false;
    if (err) err->clear();
    return true;
}

bool reparse(Document *doc, std::string *err)
{
    if (!doc) return false;
    doc->commands.clear();
    doc->combos.clear();
    doc->animations.clear();
    parse_commands(doc);
    parse_combos(doc);
    parse_animations(doc);
    std::sort(doc->animations.begin(), doc->animations.end(),
              [](const AnimationBlock &a, const AnimationBlock &b) {
                  if (a.file_rel != b.file_rel) return a.file_rel < b.file_rel;
                  return a.start_line < b.start_line;
              });
    if (err) err->clear();
    return true;
}

bool save(Document *doc, std::string *err)
{
    if (!doc) return false;
    if (doc->files.empty()) {
        if (err) *err = "no MK2 source loaded";
        return false;
    }
    for (SourceFile &sf : doc->files) {
        if (!sf.dirty) continue;
        if (!write_text_file(sf.full_path, sf.lines, err)) return false;
    }
    for (SourceFile &sf : doc->files) sf.dirty = false;
    doc->dirty = false;
    if (err) err->clear();
    return true;
}

bool set_line(Document *doc, int file_idx, int line_1based, const char *text)
{
    if (!doc) return false;
    if (file_idx < 0 || file_idx >= (int)doc->files.size()) return false;
    SourceFile &sf = doc->files[file_idx];
    if (line_1based <= 0 || line_1based > (int)sf.lines.size()) return false;
    std::string s = text ? text : "";
    if (sf.lines[line_1based - 1] == s) return false;
    sf.lines[line_1based - 1] = s;
    sf.dirty = true;
    doc->dirty = true;
    return true;
}

bool insert_line(Document *doc, int file_idx, int before_line_1based, const char *text)
{
    if (!doc) return false;
    if (file_idx < 0 || file_idx >= (int)doc->files.size()) return false;
    SourceFile &sf = doc->files[file_idx];
    if (before_line_1based <= 0) before_line_1based = 1;
    if (before_line_1based > (int)sf.lines.size() + 1) before_line_1based = (int)sf.lines.size() + 1;
    sf.lines.insert(sf.lines.begin() + (before_line_1based - 1), text ? text : "");
    sf.dirty = true;
    doc->dirty = true;
    return true;
}

bool delete_line(Document *doc, int file_idx, int line_1based)
{
    if (!doc) return false;
    if (file_idx < 0 || file_idx >= (int)doc->files.size()) return false;
    SourceFile &sf = doc->files[file_idx];
    if (line_1based <= 0 || line_1based > (int)sf.lines.size()) return false;
    sf.lines.erase(sf.lines.begin() + (line_1based - 1));
    sf.dirty = true;
    doc->dirty = true;
    return true;
}

int find_command(const Document *doc, const char *label)
{
    if (!doc || !label) return -1;
    for (int i = 0; i < (int)doc->commands.size(); i++)
        if (doc->commands[i].label == label) return i;
    return -1;
}

int find_combo(const Document *doc, const char *label)
{
    if (!doc || !label) return -1;
    for (int i = 0; i < (int)doc->combos.size(); i++)
        if (doc->combos[i].label == label) return i;
    return -1;
}

int find_animation(const Document *doc, const char *label)
{
    if (!doc || !label) return -1;
    for (int i = 0; i < (int)doc->animations.size(); i++)
        if (doc->animations[i].label == label) return i;
    return -1;
}

bool build_asset_plan(const Document *doc, const char *animation_label,
                      const char *preferred_file, AssetPlan *plan,
                      std::string *err)
{
    if (!doc || !plan) return false;
    *plan = AssetPlan();
    plan->root_label = animation_label ? animation_label : "";
    std::string pref = preferred_file ? preferred_file : "";
    if (plan->root_label.empty()) {
        if (err) *err = "no animation label";
        return false;
    }

    collect_asset_refs(doc, plan->root_label, pref, plan, 0);
    if (plan->animation_labels.empty() && plan->sprite_labels.empty()) {
        if (err) { *err = "could not resolve animation label "; *err += plan->root_label; }
        return false;
    }

    bool wants_jc = contains_icase(pref, "MKJC") || contains_icase(pref, "CAGE") ||
                    contains_icase(plan->preferred_file, "MKJC");
    if (wants_jc) {
        static const char *kCageImgs[] = {
            "data/CAGE1.IMG", "data/CAGE2.IMG", "data/CAGE3.IMG",
            "data/CAGE4.IMG", "data/CAGE5.IMG", "data/CAGE6.IMG",
            "data/CAGE7.IMG", "data/CAGE8.IMG", "data/CAGE9.IMG",
            "data/CAGE10.IMG"
        };
        for (const char *img : kCageImgs) plan->img_files.push_back(img);
    }

    if (err) err->clear();
    return true;
}

const SourceFile *get_file(const Document *doc, int file_idx)
{
    if (!doc) return nullptr;
    if (file_idx < 0 || file_idx >= (int)doc->files.size()) return nullptr;
    return &doc->files[file_idx];
}

} /* namespace mk2fatal */
