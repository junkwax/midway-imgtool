#pragma once
#include "studio_document.h"
namespace studio {
// Editor-only overlay. Index zero is transparent, matching classic imgtool.
size_t checked_layer_bytes(const IMG &image);
unsigned char overlay_pixel(const IMG &image,int x,int y,unsigned char base);
unsigned char visible_pixel(const IMG &image,int x,int y);
// Prepare every allocation before borrowing live pointers. The shared writer
// never sees layer_p, so its best-effort classic flatten hook cannot omit art.
class LayerCompositeScope {
    struct Entry { IMG *image; void *pixels,*layer; std::vector<unsigned char> composite; };
    std::vector<Entry> entries;
public:
    explicit LayerCompositeScope(Document &document);
    ~LayerCompositeScope();
    LayerCompositeScope(const LayerCompositeScope &)=delete;
    LayerCompositeScope &operator=(const LayerCompositeScope &)=delete;
};
}
