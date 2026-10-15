#pragma once
#include "img_format.h"
#include <string>
#include <vector>

namespace studio {
// WIMP near records (0x60a..0x631) and the existing far layouts (0x634..0x654).
// Work from the checked byte snapshot, never reopen a mutable source path.
bool inspect_img(const std::vector<unsigned char> &bytes, LIB_HDR &header);
bool normalize_img(const std::vector<unsigned char> &source,
                   std::vector<unsigned char> &modern, std::string &error);
bool project_img_layout(std::vector<unsigned char> &modern, unsigned version,
                        std::string &error);
bool load_img_snapshot(const std::vector<unsigned char> &modern, Document &out,
                       std::string &error);
}
