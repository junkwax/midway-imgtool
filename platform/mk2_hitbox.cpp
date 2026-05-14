/*************************************************************
 * platform/mk2_hitbox.cpp
 * MK2 strike-table parser/writer for MKSTK.ASM.
 * See mk2_hitbox.h for the data model.
 *************************************************************/
#include "mk2_hitbox.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>

namespace mk2 {

const char *kFieldNames[F_COUNT] = {
    "x_offset", "y_offset", "x_size", "y_size",
    "strike_routine", "damage", "score", "sound"
};

bool field_is_long(int idx) { return idx == F_SCORE; }

/* GSPA-style literal parser:
     0XXh / 00ah / 0FFFFh   → hex
     -00ah                  → signed hex
     0xNN                   → C-style hex
     123                    → decimal
     -123                   → signed decimal
   Anything else (symbolic) leaves has_value=false. */
static void parse_literal(const std::string &raw, int32_t *out, bool *ok)
{
    *out = 0; *ok = false;
    std::string s = raw;
    /* trim */
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && (std::isspace((unsigned char)s.back()) || s.back() == ',')) s.pop_back();
    if (s.empty()) return;

    int sign = 1;
    if (s[0] == '-') { sign = -1; s.erase(s.begin()); }
    else if (s[0] == '+') s.erase(s.begin());
    if (s.empty()) return;

    /* GSPA hex: <digits>h, optional leading 0 */
    if (s.size() >= 2 && (s.back() == 'h' || s.back() == 'H')) {
        std::string body = s.substr(0, s.size() - 1);
        if (body.empty()) return;
        for (char c : body) if (!std::isxdigit((unsigned char)c)) return;
        long v = std::strtol(body.c_str(), nullptr, 16);
        *out = (int32_t)(sign * v); *ok = true; return;
    }
    /* C-style 0x */
    if (s.size() >= 3 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (size_t i = 2; i < s.size(); i++)
            if (!std::isxdigit((unsigned char)s[i])) return;
        long v = std::strtol(s.c_str() + 2, nullptr, 16);
        *out = (int32_t)(sign * v); *ok = true; return;
    }
    /* Decimal */
    bool all_digit = !s.empty();
    for (char c : s) if (!std::isdigit((unsigned char)c)) { all_digit = false; break; }
    if (all_digit) { *out = (int32_t)(sign * std::strtol(s.c_str(), nullptr, 10)); *ok = true; }
}

/* Format an int back to a GSPA hex literal (e.g. 0XXh) using the field's
   width. Negative values are two's-complement encoded. */
static std::string format_hex(int32_t v, int bits)
{
    uint32_t mask  = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    uint32_t ival  = (uint32_t)v & mask;
    char buf[24];
    /* Match hitbox_edit.py: 0<hex>h with width_bits/4 digits */
    int width = bits / 4;
    if (width < 1) width = 1;
    std::snprintf(buf, sizeof(buf), "0%0*xh", width, (unsigned)ival);
    return std::string(buf);
}

/* Detect whether an existing raw literal used hex notation (ends with h/H
   or starts with 0x). Decimal-style literals stay decimal on edit. */
static bool raw_looks_hex(const std::string &raw)
{
    std::string s = raw;
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    while (!s.empty() && (s.back() == ',' || std::isspace((unsigned char)s.back()))) s.pop_back();
    if (s.empty()) return true;  /* default to hex */
    if (s.back() == 'h' || s.back() == 'H') return true;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return true;
    return false;
}

static void split_lines(const std::string &text, std::vector<std::string> *out)
{
    out->clear();
    std::string cur;
    cur.reserve(128);
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (c == '\r') continue; /* normalize CRLF → LF; we'll re-emit LF on save */
        if (c == '\n') { out->push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out->push_back(cur);
}

bool load(Document *doc, const char *path, std::string *err)
{
    doc->raw_lines.clear();
    doc->records.clear();
    doc->char_tables.clear();
    doc->dirty = false;
    doc->source_path = path ? path : "";

    FILE *f = std::fopen(path, "rb");
    if (!f) {
        if (err) { *err = "cannot open "; *err += path ? path : "(null)"; }
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string text;
    if (sz > 0) { text.resize((size_t)sz); std::fread(&text[0], 1, (size_t)sz, f); }
    std::fclose(f);
    split_lines(text, &doc->raw_lines);

    /* Patterns matched against each line (a la hitbox_edit.py). */
    static const std::regex re_stk_label   (R"(^(stk_[A-Za-z0-9_]+)\s*$)");
    static const std::regex re_char_table  (R"(^([a-z]{2})_strikes\s*$)");
    static const std::regex re_field       (R"(^\s*\.(word|long)\s+([^;]+?)\s*(?:;.*)?$)");
    static const std::regex re_char_long   (R"(^\s*\.long\s+(stk_[A-Za-z0-9_]+)\s*(?:;.*)?$)");

    /* State for strike-record parsing */
    int    rec_field_idx = 0;
    int    cur_record    = -1;
    /* State for character-table parsing */
    int    cur_table     = -1;

    for (int li = 0; li < (int)doc->raw_lines.size(); li++) {
        const std::string &line = doc->raw_lines[li];
        std::smatch m;

        /* --- Strike record label --- */
        if (std::regex_match(line, m, re_stk_label)) {
            StrikeRecord rec;
            rec.label      = m[1].str();
            rec.label_line = li + 1;
            doc->records.push_back(rec);
            cur_record    = (int)doc->records.size() - 1;
            rec_field_idx = 0;
            cur_table     = -1; /* labels close any open table */
            continue;
        }

        /* --- Character table head --- */
        if (std::regex_match(line, m, re_char_table)) {
            CharTable t;
            t.name = m[1].str();
            t.line = li + 1;
            doc->char_tables.push_back(t);
            cur_table  = (int)doc->char_tables.size() - 1;
            cur_record = -1;
            continue;
        }

        /* --- Inside a char table: collect .long stk_*** --- */
        if (cur_table >= 0) {
            if (std::regex_match(line, m, re_char_long)) {
                doc->char_tables[cur_table].moves.push_back(m[1].str());
                continue;
            }
            /* Blank or pure comment doesn't close the table. */
            bool blank = true;
            for (char c : line) if (!std::isspace((unsigned char)c)) { blank = false; break; }
            if (blank) continue;
            /* Comment-only line (starts with * or ;) doesn't close. */
            std::string trimmed = line;
            while (!trimmed.empty() && std::isspace((unsigned char)trimmed.front())) trimmed.erase(trimmed.begin());
            if (!trimmed.empty() && (trimmed[0] == '*' || trimmed[0] == ';')) continue;
            /* Anything else closes the table. */
            cur_table = -1;
        }

        /* --- Inside a strike record: collect 8 fields --- */
        if (cur_record >= 0 && rec_field_idx < F_COUNT) {
            if (std::regex_match(line, m, re_field)) {
                std::string raw = m[2].str();
                StrikeField &fld = doc->records[cur_record].fields[rec_field_idx];
                fld.raw  = raw;
                fld.line = li + 1;
                int32_t v = 0; bool ok = false;
                parse_literal(raw, &v, &ok);
                fld.value     = v;
                fld.has_value = ok;
                rec_field_idx++;
                if (rec_field_idx >= F_COUNT) cur_record = -1;
                continue;
            }
            /* blank/comment within a record — skip without consuming a slot */
        }
    }

    if (err) err->clear();
    return true;
}

bool save(Document *doc, std::string *err)
{
    if (doc->source_path.empty()) {
        if (err) *err = "no source path";
        return false;
    }
    FILE *f = std::fopen(doc->source_path.c_str(), "wb");
    if (!f) {
        if (err) { *err = "cannot write "; *err += doc->source_path; }
        return false;
    }
    for (size_t i = 0; i < doc->raw_lines.size(); i++) {
        const std::string &l = doc->raw_lines[i];
        std::fwrite(l.data(), 1, l.size(), f);
        std::fputc('\n', f);
    }
    std::fclose(f);
    doc->dirty = false;
    if (err) err->clear();
    return true;
}

/* Substitute the literal on a `.word X` or `.long Y` line, preserving the
   directive, indentation, and trailing comment. */
static void rewrite_field_line(std::string *line, const std::string &new_lit)
{
    static const std::regex re(R"(^(\s*\.(?:word|long)\s+)([^;]+?)(\s*(?:;.*)?)$)");
    std::smatch m;
    if (!std::regex_match(*line, m, re)) return;
    *line = m[1].str() + new_lit + m[3].str();
}

void set_value(Document *doc, int record_idx, int field_idx, int32_t value)
{
    if (record_idx < 0 || record_idx >= (int)doc->records.size()) return;
    if (field_idx  < 0 || field_idx  >= F_COUNT) return;
    StrikeField &fld = doc->records[record_idx].fields[field_idx];
    bool was_hex = raw_looks_hex(fld.raw);
    int bits = field_is_long(field_idx) ? 32 : 16;
    std::string new_raw;
    if (was_hex) {
        new_raw = format_hex(value, bits);
    } else {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%d", (int)value);
        new_raw = buf;
    }
    if (new_raw == fld.raw && fld.has_value && fld.value == value) return;
    fld.raw       = new_raw;
    fld.value     = value;
    fld.has_value = true;
    if (fld.line > 0 && fld.line <= (int)doc->raw_lines.size())
        rewrite_field_line(&doc->raw_lines[fld.line - 1], new_raw);
    doc->dirty = true;
}

void set_raw(Document *doc, int record_idx, int field_idx, const char *raw)
{
    if (record_idx < 0 || record_idx >= (int)doc->records.size()) return;
    if (field_idx  < 0 || field_idx  >= F_COUNT) return;
    if (!raw) return;
    StrikeField &fld = doc->records[record_idx].fields[field_idx];
    std::string s = raw;
    if (s == fld.raw) return;
    fld.raw = s;
    parse_literal(s, &fld.value, &fld.has_value);
    if (fld.line > 0 && fld.line <= (int)doc->raw_lines.size())
        rewrite_field_line(&doc->raw_lines[fld.line - 1], s);
    doc->dirty = true;
}

int find_char_table(const Document *doc, const char *name)
{
    if (!name) return -1;
    for (size_t i = 0; i < doc->char_tables.size(); i++)
        if (doc->char_tables[i].name == name) return (int)i;
    return -1;
}

int find_record(const Document *doc, const char *label)
{
    if (!label) return -1;
    for (size_t i = 0; i < doc->records.size(); i++)
        if (doc->records[i].label == label) return (int)i;
    return -1;
}

/* Snapshot record_idx's current state (fields + .asm line text) into out. */
static void capture_record(const Document *doc, int record_idx, UndoEntry *out)
{
    out->record_idx = record_idx;
    const StrikeRecord &rec = doc->records[record_idx];
    for (int i = 0; i < F_COUNT; i++) {
        out->fields[i] = rec.fields[i];
        out->line_text[i].clear();
        if (rec.fields[i].line > 0 && rec.fields[i].line <= (int)doc->raw_lines.size())
            out->line_text[i] = doc->raw_lines[rec.fields[i].line - 1];
    }
}

/* Apply the snapshot in `e` back onto its record. */
static void apply_snapshot(Document *doc, const UndoEntry &e)
{
    if (e.record_idx < 0 || e.record_idx >= (int)doc->records.size()) return;
    StrikeRecord &rec = doc->records[e.record_idx];
    for (int i = 0; i < F_COUNT; i++) {
        rec.fields[i] = e.fields[i];
        if (e.fields[i].line > 0 && e.fields[i].line <= (int)doc->raw_lines.size()
            && !e.line_text[i].empty())
            doc->raw_lines[e.fields[i].line - 1] = e.line_text[i];
    }
}

void undo_push(Document *doc, int record_idx, bool coalesce_with_prev)
{
    if (record_idx < 0 || record_idx >= (int)doc->records.size()) return;
    /* During a continuous drag we don't want one snapshot per pixel. If
       the caller asks to coalesce and the top of the stack already
       points at the same record, keep the older snapshot — it's the
       pre-drag state. */
    if (coalesce_with_prev && !doc->undo_stack.empty()
        && doc->undo_stack.back().record_idx == record_idx) return;

    UndoEntry e;
    capture_record(doc, record_idx, &e);
    /* Bound the stack so a long session doesn't grow without limit. */
    const size_t kMaxUndo = 200;
    if (doc->undo_stack.size() >= kMaxUndo)
        doc->undo_stack.erase(doc->undo_stack.begin());
    doc->undo_stack.push_back(std::move(e));
    /* New edit branch — invalidate redo. */
    doc->redo_stack.clear();
}

int undo_pop(Document *doc)
{
    if (doc->undo_stack.empty()) return -1;
    UndoEntry e = std::move(doc->undo_stack.back());
    doc->undo_stack.pop_back();
    if (e.record_idx < 0 || e.record_idx >= (int)doc->records.size()) return -1;

    /* Capture current state into the redo stack before restoring the
       snapshot, so Ctrl+Shift+Z can return here. */
    UndoEntry r;
    capture_record(doc, e.record_idx, &r);
    doc->redo_stack.push_back(std::move(r));

    apply_snapshot(doc, e);
    /* Restoring doesn't clean dirty: the file on disk still differs
       until Save is invoked or the user reloads. */
    return e.record_idx;
}

int redo_pop(Document *doc)
{
    if (doc->redo_stack.empty()) return -1;
    UndoEntry e = std::move(doc->redo_stack.back());
    doc->redo_stack.pop_back();
    if (e.record_idx < 0 || e.record_idx >= (int)doc->records.size()) return -1;

    /* Push current state back onto undo so the next Ctrl+Z returns here. */
    UndoEntry u;
    capture_record(doc, e.record_idx, &u);
    doc->undo_stack.push_back(std::move(u));

    apply_snapshot(doc, e);
    return e.record_idx;
}

} // namespace mk2
