/*************************************************************
 * platform/lod_parser.h
 * LOD manifest parser: reads LOAD2 .lod files and resolves
 * referenced IMG file paths.
 *************************************************************/
#ifndef LOD_PARSER_H
#define LOD_PARSER_H

#include <string>
#include <vector>

struct LodManifest {
    struct Entry {
        std::string resolved_path;    /* full path after IMGDIR/wd resolution */
    };
    std::vector<Entry> entries;
    int ppp_value;                    /* from PPP> directive; 0 = auto */
    bool parse_error;
    std::string error_msg;

    LodManifest() : ppp_value(0), parse_error(false) {}
};

/* Parse a .lod file at the given path.  Resolves IMG file references
 * using IMGDIR (environment variable) when no directory is supplied.
 * Returns the parsed manifest. */
LodManifest ParseLodFile(const char *lod_path);

#endif /* LOD_PARSER_H */
