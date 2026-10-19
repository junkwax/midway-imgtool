#include "studio_palette.h"
#include "palette_math.h"
#include <algorithm>
namespace studio {
namespace {
// Ported from classic ui_palette: pixel-weighted median cut followed by up to
// eight weighted nearest-centroid refinements, with stable dark-to-light order.
struct PaletteReduceColor {
    int old_idx;
    unsigned char r, g, b;
    double weight;
};

struct PaletteReduceBox {
    std::vector<int> colors;
    int rmin, rmax, gmin, gmax, bmin, bmax;
    double weight;
};

struct PaletteReduceRep {
    unsigned char r, g, b;
    int old_min;
    int luma;
    int sat;
    int hue;
    int family;
};

static int palette_sort_hue(int r,int g,int b){int hi=std::max({r,g,b}),lo=std::min({r,g,b}),delta=hi-lo;if(!delta)return 0;int hue=hi==r?256*(g-b)/delta:hi==g?512+256*(b-r)/delta:1024+256*(r-g)/delta;if(hue<0)hue+=1536;if(hue>=1536)hue-=1536;return hue;}
static void palette_reduce_update_rep_fields(PaletteReduceRep *rep)
{
    if (!rep) return;
    rep->luma = rep->r * 54 + rep->g * 183 + rep->b * 19;
    int maxv = rep->r > rep->g ? (rep->r > rep->b ? rep->r : rep->b) : (rep->g > rep->b ? rep->g : rep->b);
    int minv = rep->r < rep->g ? (rep->r < rep->b ? rep->r : rep->b) : (rep->g < rep->b ? rep->g : rep->b);
    rep->sat = maxv - minv;
    rep->hue = palette_sort_hue(rep->r, rep->g, rep->b);
    rep->family = (rep->sat <= 18) ? 0 : 1 + ((rep->hue + 32) % 1536) / 64;
}

static void palette_reduce_compute_box(PaletteReduceBox *box,
                                       const std::vector<PaletteReduceColor> &colors)
{
    if (!box || box->colors.empty()) return;
    const PaletteReduceColor &first = colors[box->colors[0]];
    box->rmin = box->rmax = first.r;
    box->gmin = box->gmax = first.g;
    box->bmin = box->bmax = first.b;
    box->weight = 0.0;

    for (int ci : box->colors) {
        const PaletteReduceColor &c = colors[ci];
        if (c.r < box->rmin) box->rmin = c.r;
        if (c.r > box->rmax) box->rmax = c.r;
        if (c.g < box->gmin) box->gmin = c.g;
        if (c.g > box->gmax) box->gmax = c.g;
        if (c.b < box->bmin) box->bmin = c.b;
        if (c.b > box->bmax) box->bmax = c.b;
        box->weight += c.weight;
    }
}

static double palette_reduce_box_score(const PaletteReduceBox &box)
{
    if (box.colors.size() <= 1) return -1.0;
    int rr = box.rmax - box.rmin;
    int gr = box.gmax - box.gmin;
    int br = box.bmax - box.bmin;
    int range = rr > gr ? (rr > br ? rr : br) : (gr > br ? gr : br);
    if (range <= 0) return -1.0;
    return (double)range * (box.weight > 1.0 ? box.weight : 1.0);
}

static bool palette_reduce_split_box(std::vector<PaletteReduceBox> &boxes,
                                     const std::vector<PaletteReduceColor> &colors,
                                     int box_idx)
{
    if (box_idx < 0 || box_idx >= (int)boxes.size()) return false;
    PaletteReduceBox box = boxes[box_idx];
    if (box.colors.size() <= 1) return false;

    int rr = box.rmax - box.rmin;
    int gr = box.gmax - box.gmin;
    int br = box.bmax - box.bmin;
    int channel = 0;
    if (gr >= rr && gr >= br) channel = 1;
    else if (br >= rr && br >= gr) channel = 2;

    std::stable_sort(box.colors.begin(), box.colors.end(),
        [&](int ai, int bi) {
            const PaletteReduceColor &a = colors[ai];
            const PaletteReduceColor &b = colors[bi];
            int av = channel == 0 ? a.r : (channel == 1 ? a.g : a.b);
            int bv = channel == 0 ? b.r : (channel == 1 ? b.g : b.b);
            if (av != bv) return av < bv;
            return a.old_idx < b.old_idx;
        });

    double half = box.weight * 0.5;
    double acc = 0.0;
    int split = (int)box.colors.size() / 2;
    for (int i = 0; i < (int)box.colors.size(); i++) {
        acc += colors[box.colors[i]].weight;
        if (acc >= half) {
            split = i + 1;
            break;
        }
    }
    if (split <= 0) split = 1;
    if (split >= (int)box.colors.size()) split = (int)box.colors.size() - 1;

    PaletteReduceBox a = {};
    PaletteReduceBox b = {};
    a.colors.assign(box.colors.begin(), box.colors.begin() + split);
    b.colors.assign(box.colors.begin() + split, box.colors.end());
    palette_reduce_compute_box(&a, colors);
    palette_reduce_compute_box(&b, colors);

    boxes[box_idx] = a;
    boxes.push_back(b);
    return true;
}

static void palette_reduce_make_reps(const std::vector<PaletteReduceBox> &boxes,
                                     const std::vector<PaletteReduceColor> &colors,
                                     std::vector<PaletteReduceRep> &reps)
{
    reps.clear();
    reps.reserve(boxes.size());
    for (const PaletteReduceBox &box : boxes) {
        if (box.colors.empty()) continue;
        double rs = 0.0, gs = 0.0, bs = 0.0, ws = 0.0;
        int old_min = 999;
        for (int ci : box.colors) {
            const PaletteReduceColor &c = colors[ci];
            double w = c.weight > 0.0 ? c.weight : 1.0;
            rs += (double)c.r * w;
            gs += (double)c.g * w;
            bs += (double)c.b * w;
            ws += w;
            if (c.old_idx < old_min) old_min = c.old_idx;
        }
        if (ws <= 0.0) ws = 1.0;
        int r = (int)(rs / ws + 0.5);
        int g = (int)(gs / ws + 0.5);
        int b = (int)(bs / ws + 0.5);
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;

        PaletteReduceRep rep = {};
        rep.r = (unsigned char)r;
        rep.g = (unsigned char)g;
        rep.b = (unsigned char)b;
        rep.old_min = old_min;
        palette_reduce_update_rep_fields(&rep);
        reps.push_back(rep);
    }

    std::sort(reps.begin(), reps.end(), [](const PaletteReduceRep &a, const PaletteReduceRep &b) {
        if (a.luma != b.luma) return a.luma < b.luma;
        if (a.sat != b.sat) return a.sat < b.sat;
        if (a.family != b.family) return a.family < b.family;
        if (a.hue != b.hue) return a.hue < b.hue;
        return a.old_min < b.old_min;
    });
}

static int palette_reduce_nearest_rep(const std::vector<PaletteReduceRep> &reps,
                                      unsigned char r, unsigned char g, unsigned char b)
{
    int best = 0;
    int best_d = 0x7fffffff;
    for (int i = 0; i < (int)reps.size(); i++) {
        int dr = (int)r - (int)reps[i].r;
        int dg = (int)g - (int)reps[i].g;
        int db = (int)b - (int)reps[i].b;
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

static void palette_reduce_refine_reps(std::vector<PaletteReduceRep> &reps,
                                       const std::vector<PaletteReduceColor> &colors)
{
    int n = (int)reps.size();
    if (n <= 0 || colors.empty()) return;

    for (int iter = 0; iter < 8; iter++) {
        std::vector<double> rs(n, 0.0), gs(n, 0.0), bs(n, 0.0), ws(n, 0.0);
        std::vector<int> old_min(n, 999);

        for (const PaletteReduceColor &c : colors) {
            int best = palette_reduce_nearest_rep(reps, c.r, c.g, c.b);
            double w = c.weight > 0.0 ? c.weight : 1.0;
            rs[best] += (double)c.r * w;
            gs[best] += (double)c.g * w;
            bs[best] += (double)c.b * w;
            ws[best] += w;
            if (c.old_idx < old_min[best]) old_min[best] = c.old_idx;
        }

        bool changed = false;
        for (int i = 0; i < n; i++) {
            if (ws[i] <= 0.0) continue;
            int r = (int)(rs[i] / ws[i] + 0.5);
            int g = (int)(gs[i] / ws[i] + 0.5);
            int b = (int)(bs[i] / ws[i] + 0.5);
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            if (reps[i].r != (unsigned char)r ||
                reps[i].g != (unsigned char)g ||
                reps[i].b != (unsigned char)b) {
                changed = true;
            }
            reps[i].r = (unsigned char)r;
            reps[i].g = (unsigned char)g;
            reps[i].b = (unsigned char)b;
            if (old_min[i] != 999) reps[i].old_min = old_min[i];
            palette_reduce_update_rep_fields(&reps[i]);
        }
        if (!changed) break;
    }
}

bool same_sources(const PaletteCompactionPlan &a,const PaletteCompactionPlan &b){
    if(a.palette_fields!=b.palette_fields||a.original!=b.original||a.source_sprites.size()!=b.source_sprites.size())return false;
    for(size_t i=0;i<a.source_sprites.size();++i){auto &x=a.source_sprites[i];auto &y=b.source_sprites[i];if(x.index!=y.index||x.id!=y.id||x.fields!=y.fields||x.pixels!=y.pixels||x.alternate!=y.alternate||x.layer!=y.layer)return false;}return true;
}
}
bool plan_palette_reduction(const Artwork &art,const PaletteReductionOptions &o,PaletteReductionPlan &out,std::string &error){
    error.clear();auto pal=art.palette(o.palette);
    if(!pal||!pal->data_p||!pal->numc||pal->numc>256||pal->bitspix>8||o.depth<4||o.depth>8||o.ppp<0||o.ppp>8){error="Choose a valid palette, target depth from 4 to 8, and packing PPP from 0 to 8.";return false;}
    try{
        size_t snapshot_bytes=0;
        for(int i=0;i<art.count();++i){auto im=art.sprite(i);bool primary=im->palnum==o.palette,alternate=false,active=false;if(im->opaltbl_p){auto refs=(const unsigned char *)im->opaltbl_p;active=refs[0]<128;for(int a=0;a<16&&refs[a]<128;++a)alternate|=refs[a]==o.palette+NUMDEFPAL;}
            if(alternate||(primary&&active)){error="Active alternate palettes share these indices. Remove those references before downscaling.";return false;}if(!primary)continue;
            if(im->layer_p){error="Flatten or remove overlays using this palette before downscaling.";return false;}
            if(!im->data_p||!im->w||!im->h||im->w>8192||im->h>8192||(size_t)im->w*im->h>4u*1024u*1024u){error="A sprite using this palette has invalid dimensions or pixels.";return false;}
            snapshot_bytes+=(size_t)((im->w+3)&~3)*im->h;if(snapshot_bytes>256u*1024u*1024u){error="Downscale review exceeds the 256 MiB pixel budget. Split the library before reviewing.";return false;}
        }
        PaletteReductionPlan p;p.options=o;
        if(!plan_palette_compaction(art,{o.palette,o.ppp,true,false,PaletteColorOrder::Keep,false},p.preview,error))return false;
        auto &v=p.preview;const bool unused=v.primary_sprites.empty();int limit=1<<o.depth;
        std::vector<PaletteReduceColor> colors;
        for(size_t c=1;c<v.original.size();++c)if(unused||v.primary[c]){unsigned short w=v.original[c];unsigned char raw[]={(unsigned char)w,(unsigned char)(w>>8)},r,g,b;pal_word_to_rgb8(raw,&r,&g,&b);colors.push_back({(int)c,r,g,b,unused?1.0:(double)v.primary[c]});}
        p.active_colors=(int)colors.size();v.mapping.fill(0);v.words.clear();v.words.push_back(v.original[0]);
        if((int)v.original.size()<=limit){v.words=v.original;for(size_t c=1;c<v.words.size();++c)v.mapping[c]=(unsigned char)c;}
        else if(p.active_colors<=limit-1){for(const auto &c:colors){v.mapping[c.old_idx]=(unsigned char)v.words.size();v.words.push_back(v.original[c.old_idx]);}}
        else{
            p.quantized=true;std::vector<PaletteReduceBox> boxes;PaletteReduceBox root{};for(int i=0;i<(int)colors.size();++i)root.colors.push_back(i);palette_reduce_compute_box(&root,colors);boxes.push_back(std::move(root));
            while((int)boxes.size()<limit-1){int best=-1;double score=-1;for(int i=0;i<(int)boxes.size();++i){double candidate=palette_reduce_box_score(boxes[i]);if(candidate>score){score=candidate;best=i;}}if(best<0||!palette_reduce_split_box(boxes,colors,best))break;}
            std::vector<PaletteReduceRep> reps;palette_reduce_make_reps(boxes,colors,reps);palette_reduce_refine_reps(reps,colors);
            std::sort(reps.begin(),reps.end(),[](const PaletteReduceRep &a,const PaletteReduceRep &b){if(a.luma!=b.luma)return a.luma<b.luma;if(a.sat!=b.sat)return a.sat<b.sat;if(a.family!=b.family)return a.family<b.family;if(a.hue!=b.hue)return a.hue<b.hue;return a.old_min<b.old_min;});
            for(const auto &rep:reps){unsigned char raw[2];rgb8_to_pal_word(rep.r,rep.g,rep.b,raw);v.words.push_back((unsigned short)(raw[0]|(raw[1]<<8)));}
            // Match actual stored RGB555 words, rather than unquantized RGB8
            // centroids: the preview and applied nearest colors then agree.
            for(size_t c=1;c<v.original.size();++c){int best=1,distance=PaletteColorDistance5(v.original[c],v.words[1]);for(size_t d=2;d<v.words.size();++d){int delta=PaletteColorDistance5(v.original[c],v.words[d]);if(delta<distance){best=(int)d;distance=delta;}}v.mapping[c]=(unsigned char)best;}
        }
        v.index_changes=0;for(size_t c=1;c<v.original.size();++c){if(v.mapping[c]!=c)v.index_changes+=v.primary[c];if(!unused&&!v.primary[c])continue;int mapped=v.mapping[c];if(mapped){int d=PaletteColorDistance5(v.original[c],v.words[mapped]);if(d){++p.approximate_colors;p.color_changes+=v.primary[c];p.max_distance=std::max(p.max_distance,d);}}}
        v.depth=o.depth;v.removed=(int)v.original.size()-(int)v.words.size();v.ppp_changed=o.ppp&&(((int)v.original.size()>(1<<o.ppp))!=((int)v.words.size()>(1<<o.ppp)));
        p.changed=v.original!=v.words||pal->bitspix!=o.depth||v.index_changes;out=std::move(p);return true;
    }catch(const std::bad_alloc &){error="Not enough memory to review palette downscaling. The library is unchanged.";return false;}
}
bool Artwork::reduce_palette_colors(const PaletteReductionPlan &reviewed,bool allow_approximation,std::string &error){
    error.clear();auto &v=reviewed.preview;if(pending||v.owner!=this||v.generation!=generation()){error="The library changed or an edit is pending. Cancel and reopen the downscale review.";return false;}
    PaletteReductionPlan p;if(!plan_palette_reduction(*this,reviewed.options,p,error))return false;
    if(!same_sources(v,p.preview)){error="Palette colors, metadata or sprite data changed. Cancel and reopen the downscale review.";return false;}
    if(p.approximate_colors&&!allow_approximation){error="Review the before/after result and allow approximate colors before downscaling.";return false;}if(!p.changed)return true;
    try{
        auto &result=p.preview;auto pal=palette(p.options.palette);
        std::unique_ptr<unsigned char,decltype(&free)> colors((unsigned char *)malloc(result.words.size()*2),free);if(!colors)throw std::bad_alloc();for(size_t i=0;i<result.words.size();++i){colors.get()[i*2]=(unsigned char)result.words[i];colors.get()[i*2+1]=(unsigned char)(result.words[i]>>8);}
        State before=capture();before.label="Downscale palette colors";past.reserve(past.size()+1);
        free(pal->data_p);pal->data_p=colors.release();pal->numc=(unsigned short)result.words.size();pal->bitspix=(unsigned char)p.options.depth;
        for(int index:result.primary_sprites){auto im=sprite(index);int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){auto pixel=(unsigned char *)im->data_p+y*stride+x;*pixel=result.mapping[*pixel];}}
        past.push_back(std::move(before));future.clear();revision=next_revision++;++serial;trim_history();return true;
    }catch(const std::bad_alloc &){error="Not enough memory to downscale colors. Palette, pixels and history are unchanged.";return false;}
}
}

