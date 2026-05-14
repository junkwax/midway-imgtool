/*************************************************************
 * test/mk2_roundtrip_test.cpp
 *
 * Defensive parser/writer regression test for mk2_hitbox.
 *
 * Three checks against a given MKSTK.ASM:
 *   1) Load succeeds and produces non-empty records + char_tables.
 *   2) Every parsed strike record has all 8 fields populated with a
 *      line number > 0 (i.e. the parser tracked their location in the
 *      file, so writes can target them).
 *   3) Round-trip is idempotent: load -> save A -> load A -> save B,
 *      and A must match B byte for byte. The first save canonicalizes
 *      line endings (LF) so a CRLF source isn't a false-negative; the
 *      second save must reproduce that canonical form exactly.
 *
 * Path to MKSTK.ASM comes from argv[1] (CMake passes it). The two
 * temp files are written next to the executable.
 *
 * Exit code: 0 on success, 1 on any failure (prints what failed).
 *************************************************************/
#include "mk2_hitbox.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static bool read_all(const char *path, std::vector<unsigned char> *out)
{
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->resize(sz > 0 ? (size_t)sz : 0);
    if (sz > 0) std::fread(out->data(), 1, (size_t)sz, f);
    std::fclose(f);
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path/to/MKSTK.ASM>\n", argv[0]);
        return 1;
    }
    const char *src_path = argv[1];

    mk2::Document doc;
    std::string err;
    if (!mk2::load(&doc, src_path, &err)) {
        std::fprintf(stderr, "FAIL: load %s: %s\n", src_path, err.c_str());
        return 1;
    }
    if (doc.records.empty()) {
        std::fprintf(stderr, "FAIL: zero strike records parsed\n");
        return 1;
    }
    if (doc.char_tables.empty()) {
        std::fprintf(stderr, "FAIL: zero character tables parsed\n");
        return 1;
    }
    std::printf("loaded %d records, %d char tables\n",
                (int)doc.records.size(), (int)doc.char_tables.size());

    /* Check 2: every record has all 8 fields tracked. */
    int missing = 0;
    for (const auto &rec : doc.records) {
        for (int fi = 0; fi < mk2::F_COUNT; fi++) {
            if (rec.fields[fi].line <= 0) {
                if (missing < 5) {
                    std::fprintf(stderr, "FAIL: record %s missing field %s\n",
                                 rec.label.c_str(), mk2::kFieldNames[fi]);
                }
                missing++;
            }
        }
    }
    if (missing > 0) {
        std::fprintf(stderr, "FAIL: %d field(s) untracked across all records\n", missing);
        return 1;
    }

    /* Check 3: idempotent round-trip. */
    const char *path_a = "mk2_roundtrip_A.asm";
    const char *path_b = "mk2_roundtrip_B.asm";

    doc.source_path = path_a;
    if (!mk2::save(&doc, &err)) {
        std::fprintf(stderr, "FAIL: save A: %s\n", err.c_str());
        return 1;
    }

    mk2::Document doc2;
    if (!mk2::load(&doc2, path_a, &err)) {
        std::fprintf(stderr, "FAIL: reload A: %s\n", err.c_str());
        return 1;
    }
    doc2.source_path = path_b;
    if (!mk2::save(&doc2, &err)) {
        std::fprintf(stderr, "FAIL: save B: %s\n", err.c_str());
        return 1;
    }

    std::vector<unsigned char> a, b;
    if (!read_all(path_a, &a) || !read_all(path_b, &b)) {
        std::fprintf(stderr, "FAIL: cannot reread A or B\n");
        return 1;
    }
    if (a.size() != b.size()) {
        std::fprintf(stderr, "FAIL: size diff A=%zu B=%zu\n", a.size(), b.size());
        return 1;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] != b[i]) {
            std::fprintf(stderr, "FAIL: byte diff at offset %zu: A=0x%02X B=0x%02X\n",
                         i, a[i], b[i]);
            return 1;
        }
    }
    /* Also: load the doc again unedited and confirm record counts didn't drift. */
    if (doc.records.size() != doc2.records.size()
        || doc.char_tables.size() != doc2.char_tables.size()) {
        std::fprintf(stderr, "FAIL: structure drift after round-trip\n");
        return 1;
    }

    std::remove(path_a);
    std::remove(path_b);

    std::printf("PASS: load + idempotent round-trip + structure stable\n");
    return 0;
}
