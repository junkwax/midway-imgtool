/*************************************************************
 * platform/tbl_diff.cpp
 * .TBL parsing + IMG-vs-table diff declared in tbl_diff.h.
 *************************************************************/
#include "tbl_diff.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

static std::string tbl_trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) b++;
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

static std::string tbl_upper(const std::string &s)
{
    std::string r = s;
    for (size_t i = 0; i < r.size(); i++)
        r[i] = (char)std::toupper((unsigned char)r[i]);
    return r;
}

/* Split a directive operand list on commas, trimming each field. */
static void tbl_split_commas(const std::string &s, std::vector<std::string> &out)
{
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == ',') {
            out.push_back(tbl_trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
}

/* Signed decimal, the form geometry words use. */
static bool tbl_parse_dec(const std::string &s, int *out)
{
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '+' || s[0] == '-') i = 1;
    if (i >= s.size()) return false;
    for (size_t k = i; k < s.size(); k++)
        if (!std::isdigit((unsigned char)s[k])) return false;
    *out = (int)strtol(s.c_str(), NULL, 10);
    return true;
}

/* TI assembler hex: digits followed by a trailing H/h, conventionally written
   with a leading 0 (03d27000H). Also accepts 0x-prefixed for convenience. */
static bool tbl_parse_hex(const std::string &s, unsigned int *out)
{
    if (s.size() < 2) return false;
    if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
        for (size_t k = 2; k < s.size(); k++)
            if (!std::isxdigit((unsigned char)s[k])) return false;
        *out = (unsigned int)strtoul(s.c_str() + 2, NULL, 16);
        return true;
    }
    char tail = s[s.size() - 1];
    if (tail != 'H' && tail != 'h') return false;
    for (size_t k = 0; k + 1 < s.size(); k++)
        if (!std::isxdigit((unsigned char)s[k])) return false;
    *out = (unsigned int)strtoul(s.substr(0, s.size() - 1).c_str(), NULL, 16);
    return true;
}

static void tbl_reset_entry(TblEntry &e)
{
    e.name.clear();
    e.w = e.h = 0;
    e.anix = e.aniy = 0;
    e.anix2 = e.aniy2 = e.aniz2 = 0;
    e.has_secondary = false;
    e.has_sag = false;
    e.sag = 0;
    e.has_flags = false;
    e.flags = 0;
    e.pal_label.clear();
    e.pal_explicit = false;
    e.line = 0;
}

bool ParseTblText(const std::string &text,
                  std::vector<TblEntry> &out,
                  std::vector<std::string> *warnings,
                  std::string *err)
{
    out.clear();
    if (warnings) warnings->clear();

    TblEntry cur;
    tbl_reset_entry(cur);
    bool have_cur = false;
    bool cur_has_geometry = false;
    /* Real tables inline palette blobs between sprite records — a colour
       count then rows of hex .word data (MKOUTER's OHOUTBLP, MKBGANI's
       OH_TIMEFLR_p). Those are labels with no geometry, which is normal, not
       a malformed record, so they must not produce a warning. */
    bool cur_saw_hex_word_row = false;
    int saw_labels = 0;
    /* The palette in force, carried across entries the way the assembler
       resolves it — a .TBL only emits .long <PAL> when it changes. */
    std::string running_pal;

    auto flush = [&]() {
        if (!have_cur) return;
        if (cur_has_geometry) {
            out.push_back(cur);
        } else if (!cur_saw_hex_word_row && warnings) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "line %d: '%s' has no .word geometry line; skipped",
                     cur.line, cur.name.c_str());
            warnings->push_back(buf);
        }
        tbl_reset_entry(cur);
        have_cur = false;
        cur_has_geometry = false;
        cur_saw_hex_word_row = false;
    };

    size_t pos = 0;
    int line_no = 0;
    std::vector<std::string> parts;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string raw = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        line_no++;

        size_t semi = raw.find(';');
        if (semi != std::string::npos) raw = raw.substr(0, semi);
        /* Strip a stray CR from CRLF sources before trimming does the rest. */
        std::string line = tbl_trim(raw);
        if (line.empty()) continue;

        /* Label: NAME: — possibly with a directive trailing on the same line. */
        size_t colon = line.find(':');
        if (colon != std::string::npos && colon > 0) {
            std::string label = tbl_trim(line.substr(0, colon));
            bool label_ok = !label.empty() &&
                            (std::isalpha((unsigned char)label[0]) || label[0] == '_');
            for (size_t k = 0; label_ok && k < label.size(); k++) {
                char c = label[k];
                if (!std::isalnum((unsigned char)c) && c != '_' && c != '$')
                    label_ok = false;
            }
            if (label_ok) {
                flush();
                saw_labels++;
                cur.name = label;
                cur.line = line_no;
                cur.pal_label = running_pal;
                cur.pal_explicit = false;
                have_cur = true;
                line = tbl_trim(line.substr(colon + 1));
                if (line.empty()) continue;
            }
        }

        /* Directive: .word / .long (case-insensitive), then operands. */
        if (line[0] != '.') continue;
        size_t sp = line.find_first_of(" \t");
        std::string dir = tbl_upper(sp == std::string::npos ? line : line.substr(0, sp));
        std::string ops = (sp == std::string::npos) ? std::string()
                                                    : tbl_trim(line.substr(sp + 1));
        if (ops.empty()) continue;
        if (!have_cur) continue;   /* stray directive before any label */

        tbl_split_commas(ops, parts);

        if (dir == ".WORD") {
            if (!cur_has_geometry && (parts.size() == 4 || parts.size() == 7)) {
                int v[7] = {0, 0, 0, 0, 0, 0, 0};
                bool ok = true;
                for (size_t k = 0; k < parts.size() && ok; k++)
                    ok = tbl_parse_dec(parts[k], &v[k]);
                if (ok) {
                    cur.w = v[0]; cur.h = v[1];
                    cur.anix = v[2]; cur.aniy = v[3];
                    if (parts.size() == 7) {
                        cur.anix2 = v[4];
                        cur.aniy2 = v[5];
                        cur.aniz2 = v[6];
                        cur.has_secondary = true;
                    }
                    cur_has_geometry = true;
                    continue;
                }
            }
            if (parts.size() > 1) {
                unsigned int hv = 0;
                if (tbl_parse_hex(parts[0], &hv)) cur_saw_hex_word_row = true;
                continue;
            }
            if (parts.size() == 1 && !cur.has_flags) {
                unsigned int fv = 0;
                if (tbl_parse_hex(parts[0], &fv)) {
                    cur.flags = fv;
                    cur.has_flags = true;
                    continue;
                }
                int dv = 0;
                if (tbl_parse_dec(parts[0], &dv)) {
                    cur.flags = (unsigned int)dv;
                    cur.has_flags = true;
                    continue;
                }
            }
            continue;
        }

        if (dir == ".LONG") {
            if (parts.size() != 1) continue;
            unsigned int sv = 0;
            if (!cur.has_sag && tbl_parse_hex(parts[0], &sv)) {
                cur.sag = sv;
                cur.has_sag = true;
                continue;
            }
            /* A non-numeric .long is the palette label. "0" means none. */
            if (!parts[0].empty() && parts[0] != "0") {
                unsigned int ignored = 0;
                if (!tbl_parse_hex(parts[0], &ignored)) {
                    running_pal = parts[0];
                    cur.pal_label = parts[0];
                    cur.pal_explicit = true;
                }
            }
            continue;
        }
    }
    flush();

    if (out.empty()) {
        /* Several shipped .TBL files are legitimately not sprite tables:
           MKZIP/MKFLOORS hold only `.set` ROM-address symbols, and
           BGNDTEST/FLAPJACK are an empty .DATA/.TEXT pair. Say which, so the
           user doesn't go hunting for a parse bug. */
        if (err) {
            *err = saw_labels > 0
                 ? "This file has labels but no W,H,ANIX,ANIY records — it is "
                   "not a sprite table."
                 : "No sprite records found. Files like MKZIP.TBL and "
                   "MKFLOORS.TBL hold only .set ROM addresses, and some tables "
                   "are an empty .DATA/.TEXT pair.";
        }
        return false;
    }
    return true;
}

static std::string fmt_int(int v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", v);
    return buf;
}

static std::string fmt_hex(unsigned int v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "0%XH", v);
    return buf;
}

static void push_diff(TblDiffRow &row, const char *field,
                      const std::string &t, const std::string &i)
{
    TblFieldDiff d;
    d.field = field;
    d.tbl_value = t;
    d.img_value = i;
    row.fields.push_back(d);
}

void DiffTblEntries(const std::vector<TblEntry> &tbl,
                    const std::vector<TblEntry> &img,
                    std::vector<TblDiffRow> &out,
                    TblDiffSummary *summary)
{
    out.clear();
    TblDiffSummary sum = {0, 0, 0, 0};

    /* Table labels are assembler symbols, so match them case-sensitively —
       two entries differing only in case are two different symbols. */
    std::map<std::string, int> img_by_name;
    for (size_t i = 0; i < img.size(); i++) {
        if (img_by_name.find(img[i].name) == img_by_name.end())
            img_by_name[img[i].name] = (int)i;
    }
    std::vector<bool> img_used(img.size(), false);

    for (size_t t = 0; t < tbl.size(); t++) {
        const TblEntry &te = tbl[t];
        TblDiffRow row;
        row.name = te.name;
        row.tbl_index = (int)t;
        row.img_index = -1;

        std::map<std::string, int>::const_iterator it = img_by_name.find(te.name);
        if (it == img_by_name.end()) {
            row.kind = TblDiff_OnlyInTbl;
            sum.only_in_tbl++;
            out.push_back(row);
            continue;
        }

        int ii = it->second;
        const TblEntry &ie = img[ii];
        row.img_index = ii;
        img_used[(size_t)ii] = true;

        if (te.w != ie.w) push_diff(row, "W", fmt_int(te.w), fmt_int(ie.w));
        if (te.h != ie.h) push_diff(row, "H", fmt_int(te.h), fmt_int(ie.h));
        if (te.anix != ie.anix)
            push_diff(row, "ANIX", fmt_int(te.anix), fmt_int(ie.anix));
        if (te.aniy != ie.aniy)
            push_diff(row, "ANIY", fmt_int(te.aniy), fmt_int(ie.aniy));

        if (te.has_secondary) {
            if (te.anix2 != ie.anix2)
                push_diff(row, "ANIX2", fmt_int(te.anix2), fmt_int(ie.anix2));
            if (te.aniy2 != ie.aniy2)
                push_diff(row, "ANIY2", fmt_int(te.aniy2), fmt_int(ie.aniy2));
            if (te.aniz2 != ie.aniz2)
                push_diff(row, "ANIZ2", fmt_int(te.aniz2), fmt_int(ie.aniz2));
        }

        /* SAG only when the caller supplied one on the IMG side; imgtool
           cannot derive a ROM bit address from the .IMG alone. */
        if (te.has_sag && ie.has_sag && te.sag != ie.sag)
            push_diff(row, "SAG", fmt_hex(te.sag), fmt_hex(ie.sag));

        if (te.has_flags && ie.has_flags && te.flags != ie.flags)
            push_diff(row, "FLAGS", fmt_hex(te.flags), fmt_hex(ie.flags));

        if (!te.pal_label.empty() && !ie.pal_label.empty() &&
            te.pal_label != ie.pal_label)
            push_diff(row, "PAL", te.pal_label, ie.pal_label);

        if (row.fields.empty()) {
            row.kind = TblDiff_Match;
            sum.matched++;
        } else {
            row.kind = TblDiff_Differs;
            sum.differing++;
        }
        out.push_back(row);
    }

    for (size_t i = 0; i < img.size(); i++) {
        if (img_used[i]) continue;
        TblDiffRow row;
        row.name = img[i].name;
        row.kind = TblDiff_OnlyInImg;
        row.tbl_index = -1;
        row.img_index = (int)i;
        out.push_back(row);
        sum.only_in_img++;
    }

    if (summary) *summary = sum;
}

std::string FormatTblDiffReport(const std::string &tbl_path,
                                const std::vector<TblDiffRow> &rows,
                                const TblDiffSummary &summary)
{
    std::string s;
    char buf[512];

    snprintf(buf, sizeof(buf), "IMGTOOL TBL compare: %s\n", tbl_path.c_str());
    s += buf;
    snprintf(buf, sizeof(buf),
             "%d match, %d differ, %d only in TBL, %d only in IMG\n\n",
             summary.matched, summary.differing,
             summary.only_in_tbl, summary.only_in_img);
    s += buf;

    for (size_t r = 0; r < rows.size(); r++) {
        const TblDiffRow &row = rows[r];
        if (row.kind == TblDiff_Match) continue;
        if (row.kind == TblDiff_OnlyInTbl) {
            snprintf(buf, sizeof(buf), "%-16s only in TBL\n", row.name.c_str());
            s += buf;
            continue;
        }
        if (row.kind == TblDiff_OnlyInImg) {
            snprintf(buf, sizeof(buf), "%-16s only in IMG\n", row.name.c_str());
            s += buf;
            continue;
        }
        snprintf(buf, sizeof(buf), "%-16s differs\n", row.name.c_str());
        s += buf;
        for (size_t f = 0; f < row.fields.size(); f++) {
            snprintf(buf, sizeof(buf), "    %-6s TBL %-12s IMG %s\n",
                     row.fields[f].field.c_str(),
                     row.fields[f].tbl_value.c_str(),
                     row.fields[f].img_value.c_str());
            s += buf;
        }
    }

    if (summary.differing == 0 && summary.only_in_tbl == 0 &&
        summary.only_in_img == 0)
        s += "No drift.\n";
    return s;
}
