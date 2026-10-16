#pragma once
#include "studio_asm.h"
namespace studio {
struct AsmAssetRow {
    std::string symbol,source;int first_frame=0,line=0,uses=0;
    AsmPiece resolved;std::vector<AsmPiece> candidates;
};
struct AsmAssetReview {
    std::vector<AsmAssetRow> rows;int resolved=0,missing=0,ambiguous=0,stale=0;
};
// Complete selected-animation inventory, first-use order. No document mutation.
// Refuses over 32768 symbols, 262144 references, or 65536 matching candidates.
// Failed reviews leave out unchanged. Source line/path describe the first operand.
bool review_asm_assets(const AsmSourceAnimation &animation,const std::vector<AsmLibrary> &libraries,
                       uint64_t preferred,const AsmArtworkChoices &choices,AsmAssetReview &out,std::string &error);
}
