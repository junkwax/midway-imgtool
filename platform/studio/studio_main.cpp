#define IMGUI_DEFINE_MATH_OPERATORS
#include "studio_document.h"
#include "studio_layers.h"
#include "studio_img_codec.h"
#include "studio_theme.h"
#include "studio_animation.h"
#include "studio_asm.h"
#include "studio_asm_assets.h"
#include "studio_asm_artwork_library.h"
#include "studio_reactions.h"
#include "studio_fatality.h"
#include "studio_fatality_workspace.h"
#include <tuple>
#include "studio_bindings.h"
#include "studio_strike_phases.h"
#include "strike_bind.h"
#include "studio_anchor_link.h"
#include "studio_points.h"
#include "studio_tbl_compare.h"
#include "studio_strikes.h"
#include "studio_project.h"
#include "studio_subframe_swap.h"
#include "studio_waterline.h"
#include "studio_dummy_body.h"
#include "studio_sequences.h"
#include "studio_embedded_metadata.h"
#include "studio_opacity_gradient.h"
#include "studio_sprite_ramp.h"
#include "studio_palette.h"
#include "studio_palette_transparency.h"
#include "studio_palette_removal.h"
#include "studio_palette_zero.h"
#include "studio_palette_move.h"
#include "studio_palette_isolation.h"
#include "studio_isolation_frames.h"
#include "studio_region_remap.h"
#include "studio_palette_swatches.h"
#include "studio_palette_multi_merge.h"
#include "studio_palette_ramps.h"
#include "studio_import.h"
#include "studio_legacy_media.h"
#include "studio_sheet.h"
#include "studio_digitize.h"
#include "studio_digitize_job.h"
#include "studio_video.h"
#include "studio_selection.h"
#include "studio_regions.h"
#include "studio_overlay_frame.h"
#include "studio_selection_transform.h"
#include "studio_paint.h"
#include "studio_filters.h"
#include "studio_repair.h"
#include "studio_batch.h"
#include "studio_batch_rename.h"
#include "studio_batch_palette.h"
#include "studio_ramp_transfer.h"
#include "studio_costume.h"
#include "studio_costume_presets.h"
#include "material_mask.h"
#include "studio_batch_delete.h"
#include "studio_library_copy.h"
#include "studio_library_split_animations.h"
#include "studio_animation_copy.h"
#include "studio_animation_clipboard.h"
#include "studio_sprite_clipboard.h"
#include "studio_palette_cleanup.h"
#include "studio_restore.h"
#include "studio_lod.h"
#include "studio_export.h"
#include "studio_variant.h"
#include "anipoint.h"
#include "anipoint_mirror.h"
#include "palette_math.h"
#include "stb_image_write.h"
#include <SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include "../../test/video_fixture.h"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <SDL_syswm.h>
#endif

#include "studio_startup.h"

namespace studio {
namespace fs=std::filesystem;
enum class Tool { Select, Pencil, Erase, Fill, Pick, Pan, Wand, Lasso, Move, Line, Replace, SmartErase, Clone, Variant, Blur, Smudge, Heal, Defringe };
struct ToolSpec {const char *name,*key,*glyph,*fallback,*tip;};
static const ToolSpec tool_specs[]={
    {"Select","R","\xee\xbd\x92","R","Drag a rectangle to select pixels."},
    {"Pencil","P","\xee\x8f\x89","P","Draw with the selected palette index. [ / ] changes brush size."},
    {"Erase","Shift+E","\xee\x9b\x90","E","Paint transparency inside the selection."},
    {"Fill","G","\xee\x88\xba","G","Fill matching pixels; choose connected or global in tool settings."},
    {"Pick","I","\xef\x8d\x91","I","Pick a palette index from the sprite. Right-click also picks."},
    {"Pan","Shift+H","\xee\xa4\xa5","H","Drag to pan; middle-drag works with any tool."},
    {"Wand","W","\xef\x8c\x9f","W","Select matching colors, respecting transparency."},
    {"Lasso","L","\xee\xac\x83","L","Draw a freehand selection."},
    {"Move","Shift+V","\xee\xa2\x9f","V","Drag selected pixels. Enter applies; Escape cancels."},
    {"Line","Shift+K","\xee\xa4\x99","K","Preview a straight line. Shift snaps; release applies; Escape cancels."},
    {"Replace","B","\xee\xa2\x81","B","Replace the color clicked at the start of a stroke."},
    {"Smart erase","E / U","\xee\x9b\x90","U","Erase matching colors inside the selection."},
    {"Clone","C","\xee\x85\x8d","C","Alt-click a source, then drag to clone its pixels."},
    {"Variant","V / T","\xee\x90\x8a","T","Recolor pixels while retaining alternate palette appearances. Preview before applying."},
    {"Blur","O","\xee\x8e\xa5","O","Soften colors inside the selection. Each stroke is one pass; strength controls the blend."},
    {"Smudge","S","\xee\x85\x95","S","Drag colors along the stroke inside the selection. Transparency and outlines stay intact."},
    {"Heal","J","\xee\x99\xa2","J","Repair a brushed blemish from surrounding colors. Pixels without a usable rim stay unchanged."},
    {"Defringe","F","\xee\x8f\x83","F","Clean outline color spill using interior colors. Transparency and thin features stay intact."}
};
struct WheelState {int palette=-1,slot=-1,last_opaque=1;uint64_t serial=0;float rgb[3]={};bool editing=false;};
struct BatchUi {
    bool open=false,dirty=true,valid=false,preview_dirty=true,preview_ok=false,before=false;uint64_t doc=0,generation=0;int scope=0,selected=0;
    BatchGeometryOptions options;BatchGeometryPlan plan;std::vector<uint64_t> excluded;std::string error;
    SDL_Texture *before_texture=nullptr,*after_texture=nullptr;ImVec2 apply_point,cancel_point,before_point,after_point,row_points[2]{},library_point,mark_point,scope_point,kind_point,scope_choices[3]{},kind_choices[4]{};
};
struct BatchRenameUi {
    bool open=false,valid=false,dirty=true;uint64_t doc=0,generation=0;int scope=0,changed=0;
    BatchRenameOptions options;BatchRenamePlan plan;char text[129]="SPRITE";std::unordered_set<uint64_t> excluded;
    std::vector<std::string> issues;std::string error;ImVec2 apply_point,cancel_point,text_point,scope_point,kind_point,scope_choices[3]{},kind_choices[3]{},row_points[3]{};
};
struct BatchPaletteUi {
    bool open=false,dirty=true,valid=false,preview_dirty=true,preview_ok=false,allow_approximation=false;
    uint64_t doc=0,generation=0;int scope=0,selected=0;BatchPaletteOptions options;BatchPalettePlan plan;
    std::unordered_set<uint64_t> excluded;std::string error;SDL_Texture *before_texture=nullptr,*after_texture=nullptr;
    ImVec2 apply_point,cancel_point,scope_point,target_point,match_point,indices_point,approximate_point,scope_choices[4]{},target_choices[4]{},row_points[3]{},name_points[3]{};
};
struct RampTransferUi {
    bool open=false,dirty=true,valid=false,preview_dirty=true,preview_ok=false,allow_colors=false;
    uint64_t doc=0,generation=0,origin=0;int scope=2,selected=0;RampTransferOptions options;RampTransferPlan plan;
    std::unordered_set<uint64_t> excluded;std::string error;SDL_Texture *before_texture=nullptr,*after_texture=nullptr;
    ImVec2 apply_point,cancel_point,scope_point,target_point,mode_point,approve_point,reset_point;
    ImVec2 scope_choices[3]{},target_choices[8]{},mode_choices[2]{},row_points[8]{},name_points[8]{},pair_points[8]{},pair_choices[8]{},fallback_point;
};
struct CostumeSectionUi {char name[33]="Costume",destination[1024]="1-3",reference[1024]="1-3";bool enabled=true;};
struct CostumePresetsUi {
    bool open=false,valid=false,replace=false,loaded[2]{};int kind=1,side=0,selected=-1;
    CostumePresetFile files[2];CostumeMappingReview review;char name[32]={};std::string error,message,load_errors[2];
    ImVec2 kind_points[2]{},side_points[2]{},choice_point,choices[8]{},name_point,replace_point,save_point,delete_point,use_point,close_point,reload_point;
};
struct CostumeUi {
    bool open=false,valid=false,dirty=true,preview_dirty=true,preview_ok=false,manual=false;
    uint64_t doc=0,generation=0,origin=0;int scope=0,preview_sprite=-1;float split=30,color[3]={220/255.f,45/255.f,75/255.f};
    CostumeOptions options;CostumePlan plan;CostumePresetsUi presets;std::vector<unsigned short> words;
    std::vector<PaletteRampBlock> blocks;std::vector<char> picked;
    std::vector<CostumeSectionUi> sections{CostumeSectionUi{}};int section=0;
    ImVec2 mode_point,mode_choices[2]{},reference_point,reference_choices[8]{},coverage_point,coverage_choices[3]{},section_point,section_choices[16]{},section_name_point,destination_ranges_point,reference_ranges_point,add_section_point,remove_section_point,enable_section_point;
    char name[10]={},ranges[1024]={};std::string error;SDL_Texture *before_texture=nullptr,*after_texture=nullptr;
    ImVec2 create_point,cancel_point,source_point,name_point,scope_point,manual_point,ranges_point,strength_point,split_point;
    ImVec2 source_choices[8]{},scope_choices[4]{},ramp_points[8]{},preset_points[4]{},preview_point,saved_point;
};
struct PaletteCleanupUi {
    bool open=false,valid=false,dirty=true,only_unused=true,duplicates=false;uint64_t doc=0,generation=0;int selected=0;
    PaletteCleanupPlan plan;std::set<int> keep;std::string error;
    ImVec2 apply_point,cancel_point,all_point,none_point,filter_point,row_points[8]{},name_points[8]{};
};
struct BatchDeleteUi {
    bool open=false,valid=false,dirty=true,preview_dirty=true,include_children=false;uint64_t doc=0,generation=0;int scope=0,selected=0;
    BatchDeletePlan plan;std::unordered_set<uint64_t> excluded;std::vector<std::string> scene_uses;std::string error;SDL_Texture *preview=nullptr;
    ImVec2 apply_point,cancel_point,scope_point,exclude_point,children_point,scope_choices[3]{},row_points[3]{},name_points[3]{};
};
struct AnimationCopyUi {
    bool open=false,valid=false,dirty=true,capture_clipboard=false,paste_clipboard=false,split_library=false;uint64_t source=0,target=0,source_generation=0,target_generation=0;int selected=0;
    char prefix[17]={},sprite_prefix[16]={},binding_filter[64]={};AnimationCopyOptions options;AnimationCopyPlan plan;std::string error;
    ImVec2 apply_point,cancel_point,target_point,prefix_point,sprite_prefix_point,all_point,none_point,target_choices[4]{},row_points[6]{},name_points[6]{};
    std::unique_ptr<Artwork> capture_target;std::shared_ptr<const AnimationClipboard> held;
    ImVec2 binding_points[6]{},binding_choices[8]{},binding_filter_point,automatic_point,reset_bindings_point,missing_point;
};
struct LibraryCopyUi {
    bool open=false,valid=false,dirty=true,preview_dirty=true,show_target=true,capture_clipboard=false,paste_clipboard=false,split_library=false,move_source=false,split_animations=false,artwork_return=false,reuse_artwork=false;uint64_t source=0,target=0,source_generation=0,target_generation=0;
    LibrarySplitAnimationPlan split_animation_plan;
    BatchDeletePlan split_removal;std::vector<std::string> split_scene_uses;
    std::unique_ptr<Artwork> capture_target;std::shared_ptr<const SpriteClipboard> held;std::shared_ptr<const AsmArtworkLibrary> held_artwork;
    int scope=0,selected=0;char prefix[16]={};LibraryCopyOptions options;LibraryCopyPlan plan;std::string error;SDL_Texture *preview=nullptr;
    ImVec2 apply_point,cancel_point,prefix_point,target_point,scope_point,reuse_point,children_point,move_point,animations_point,animation_details_point,exclude_refs_point,reuse_artwork_point,target_choices[4]{},scope_choices[3]{},row_points[3]{},name_points[3]{};
};
struct RestoreUi {
    bool open=false,build=false,valid=false,preview_dirty=true,preview_ok=false,changes=true;uint64_t doc=0,generation=0;int scope=0,selected=0,view=1;
    RestoreOptions options;RestorePlan plan;std::unordered_set<uint64_t> excluded,included;std::string error;char pattern[129]="^(.+)[A-Z]$",parent_filter[64]={};
    SDL_Texture *before_texture=nullptr,*after_texture=nullptr,*parent_texture=nullptr,*changes_texture=nullptr;
    ImVec2 build_point,apply_point,cancel_point,mode_point,mode_choices[4]{},row_points[3]{},child_points[3]{},view_points[3]{},changes_point,names_point,manual_point;
};
struct LodUi {
    bool open=false,valid=false,settings_changed=false;int selected=0,preview_row=-1;
    std::string path,error;char override_folder[32768]={},img_directory[32768]={};LodReview review;SDL_Texture *preview=nullptr;
    ImVec2 apply_point,cancel_point,rebuild_point,row_points[4]{},name_points[4]{};
};
struct ExportUi {
    bool open=false,valid=false,build=false,destination_valid=false;uint64_t doc=0;int scope=0,selected=0;
    ExportOptions options;ExportPlan plan;ExportDestination destination;std::unordered_set<uint64_t> excluded;
    char path[32768]={};std::string error;std::vector<std::string> lines;
    ImVec2 build_point,apply_point,cancel_point,kind_point,kind_choices[3]{},scope_point,scope_choices[4]{},depth_point,depth_choices[3]{},align_point,align_choices[3]{},row_points[4]{},name_points[4]{},mk3_point,bpp_min_point;
};
struct SheetUi {
    bool open=false,valid=false,build=false,destination_valid=false;uint64_t doc=0;int scope=0,selected=0;
    SheetOptions options;SheetPlan plan;std::vector<SheetDestination> destinations;std::unordered_set<uint64_t> excluded;
    char path[32768]={};std::string error;SDL_Texture *preview=nullptr;float zoom=0;ImVec2 pan;
    ImVec2 apply_point,build_point,cancel_point,anf_point,rgba_point,row_points[4]{},name_points[4]{};
};
struct ToolsUi {ImVec2 points[18]{},wheel_origin,detail_point,transparent_point,edges_point,cleanup_point;float wheel_size=0;};
enum class Action { Open, OpenLod, ExportTbl, ExportIrw, ExportAnilst, ImportIndexed, ExportLbm, ExportTga, ExportSheet, Save, SaveAs, Undo, Redo, Copy, Paste, Clear, Fit, ZoomIn, ZoomOut, Demo, Close, Quit,
    NewLibrary, NewSprite, Rename, Duplicate, Delete, MoveUp, MoveDown, Resize, CanvasSize, Trim, Transform, ImportPng, ExportPng,
    NewProject, OpenProject, SaveProject, SaveProjectAs, CloseProject, SaveArtwork, SaveArtworkAs, ExportScene, ManagePalettes, ImportMedia, Digitize, CaptureVideo,
    Cut,SelectAll,Deselect,InvertSelection,SelectOpaque,ExtractSelection,SplitOverlay,CopyOverlay,SplitBody,TransformSelection,BatchGeometry,BatchRename,BatchPalette,RampTransfer,Costume,BatchDelete,LibraryCopy,AnimationCopy,SplitAnimations,CopyAnimations,PasteAnimations,ClearAnimationClipboard,SplitLibrary,CopySprites,PasteSprites,ClearSpriteClipboard,PaletteCleanup,PaletteDuplicates,RestoreChildren, Help,Diagnostics,Points,CopyNew,CutNew,PasteNew,Mark,MarkAll,UnmarkAll,PaletteAssign,PaletteAssignMarked,PaletteRename,PaletteMerge,ReduceMarked,PreviewEarlier,PreviewLater };
#include "studio_shortcut_bindings.inl"
#include "studio_help_topics.inl"
enum class EditDialog { None, EmbeddedMetadata, NewSprite, Rename, Delete, Resize, CanvasSize, Transform, ImportPng, ExportPng, OpacityGradient, SpriteRamp, OverlayFrame, SubframeSwap, Waterline, DummyBody };
using Selection=PixelSelection;
struct Texture { SDL_Texture *value=nullptr; uint64_t revision=0; };
enum class SceneCommand { None, AddFrame, DuplicateFrame, MoveLeft, MoveRight, DeleteFrame, AddLane, RemoveLane, ReplaceFrame, FollowFrame, DetachTrigger };
struct SceneUi {
    int lane=0,frame=0;
    double tick=0;
    bool playing=false,dragging=false;
    float zoom=0;
    ImVec2 pan{0,0},drag_mouse,probe;
    int drag_x=0,drag_y=0;
    uint64_t source_doc=0,source_sprite=0;
    SceneCommand command=SceneCommand::None;
    bool smoke=false;
    bool motion_smoke=false,subframe_smoke=false;
    ImVec2 show_here_point;
    ImVec2 magnification_point,magnification_choices[4]{},subframe_swap_point,waterline_point,dummy_body_point;
    uint64_t trigger_source_lane=0,trigger_source_frame=0;
    bool trigger_smoke=false;
    ImVec2 follow_point,detach_point;
    float probe_zoom=1;
};
struct AlignmentView {
    bool enabled=false,onion=true,multipart=true,move_frame=false;
    int facing=0,sequence=0,world_w=400,world_h=254,origin_x=200,origin_y=20,floor_y=234;
    bool placed=false;
    float zoom=0;
    ImVec2 pan{0,0};
    PreviewPlayer player;
    std::vector<PreviewSequence> sequences;
    std::vector<std::vector<int>> holds;
    double tick_hz=mk2_tick_hz;
    uint64_t reference_doc=0,reference_id=0;
    int reference_sprite=0,reference_x=100,reference_y=0;
    bool reference_flip=true;
    bool dragging=false;
    int drag_kind=0,drag_dx=0,drag_dy=0;
    ImVec2 drag_mouse;
    std::vector<int> drag_pieces;
    void initialize(const Artwork &art){
        std::unordered_map<uint64_t,int> timing;
        for(size_t s=0;s<sequences.size();++s)for(size_t f=0;f<sequences[s].frames.size();++f)timing[sequences[s].frames[f].sprite_id]=holds[s][f];
        sequences=infer_preview_sequences(art);holds.clear();
        for(auto &s:sequences){holds.emplace_back();for(auto &f:s.frames){auto found=timing.find(f.sprite_id);holds.back().push_back(found==timing.end()?default_preview_hold:found->second);}}
    }
    const PreviewFrame *frame() const {
        if(sequence<0||sequence>=(int)sequences.size())return nullptr;
        auto &frames=sequences[sequence].frames;
        return player.frame>=0&&player.frame<(int)frames.size()?&frames[player.frame]:nullptr;
    }
};
enum class EmbeddedCommand {None,NewSequence,NewScript,Append,Duplicate,Delete,Left,Right,Scene};
struct EmbeddedView {
    bool enabled=false,scripts=false,playing=false,loop=true,rename_focus=false;
    int record=-1,entry=0,target=0;
    double tick=0;
    float zoom=0;
    ImVec2 pan{0,0},append_point,duplicate_point,open_point,metadata_point;
    int origin_x=200,origin_y=20,floor=234;
    uint64_t seen=0;
    std::vector<EmbeddedRecord> records;
    std::string error;
    ImGuiTextFilter filter;
    EmbeddedCommand command=EmbeddedCommand::None;
};
struct EmbeddedMetadataUi {
    bool valid=false;std::shared_ptr<const EmbeddedMetadataSource> source;EmbeddedMetadataValues values;EmbeddedMetadataPlan plan;std::string error;
    ImVec2 flags_point,x_point,y_point,spare_points[3]{},damage_points[6]{},none_choice,damage_choices[4]{},apply_point,cancel_point;
};
struct OpacityGradientUi {
    uint64_t origin=0,generation=0;int scope=0,row=0;bool valid=false,dirty=true,capture_dirty=true,preview_dirty=true,preview_ok=true;
    PixelSelection selection;OpacityGradientOptions options;std::shared_ptr<const OpacityGradientSource> source;OpacityGradientPlan plan;std::unordered_set<uint64_t> excluded;std::string error;
    SDL_Texture *before=nullptr,*after=nullptr;
    ImVec2 scope_point,scope_choices[3]{},direction_point,direction_choices[7]{},pattern_point,pattern_choices[3]{},start_point,end_point,swap_point,reset_point,limit_point,depth_point,bounds_point,feather_point,seed_point,trim_point,apply_point,cancel_point,row_points[8]{},include_points[8]{};
};
struct SpriteRampUi {
    uint64_t origin=0,generation=0;int scope=0,row=0,panel=0,active_stop=0,preset=-1,feather=5,depth=8;
    bool valid=false,dirty=true,capture_dirty=true,preview_dirty=true,preview_ok=true,seed_pending=true,limit=false;
    PixelSelection selection;SpriteRampOptions options;std::shared_ptr<const SpriteRampSource> source;SpriteRampPlan plan;std::unordered_set<uint64_t> excluded;std::string error,preset_error,preset_message;char preset_name[65]={};
    SDL_Texture *before=nullptr,*after=nullptr;
    ImVec2 scope_point,scope_choices[3]{},panel_points[3]{},bpp_point,count_point,fill_point,add_point,remove_point,reverse_point,seed_point,direction_point,direction_choices[7]{},from_point,to_point,swap_point,shade_point,limit_point,band_point,dither_point,dither_choices[4]{},noise_point,ppp_point,preset_point,preset_choices[8]{},preset_name_point,save_point,apply_point,cancel_point,row_points[8]{},include_points[8]{};
};
struct SubframeSwapUi {
    uint64_t doc=0,lane=0,frame=0;int tick=0;bool valid=false,dirty=false;SubframeSwapPlan plan;std::string error;
    ImVec2 tick_point,now_point,apply_point,cancel_point;
};
struct WaterlineUi {
    uint64_t doc=0;WaterlineOptions options;WaterlinePlan plan;int preview_tick=0;
    bool valid=false,dirty=false;std::string error;ImGuiTextFilter filter;
    ImVec2 line_point,floor_point,library_point,parent_point,apply_point,cancel_point,seek_point,library_choices[8]{},parent_choices[4]{};
};
struct DummyBodyUi {
    uint64_t doc=0,selected_lane=0;DummyBodyOptions options;DummyBodyPlan plan;
    std::vector<DummyBodyFamily> families;int tick=0;bool replace=false,valid=false,dirty=false;std::string error;
    ImVec2 library_point,family_point,replace_point,facing_point,apply_point,cancel_point,seek_point,library_choices[8]{},family_choices[8]{};
};
struct OverlayFrameUi {
    uint64_t source=0,generation=0;bool split=false,valid=false,dirty=false,preview_dirty=true,preview_ok=false;
    PixelSelection selection;OverlayFramePlan plan;std::string error;char name[16]={};
    SDL_Texture *textures[3]{};ImVec2 copy_point,split_point,name_point,apply_point,cancel_point;
};

enum class PaletteCommand {None,Apply,Reset,New,Duplicate,Paste,Delete,Rename,Assign,Private,Undo,Redo,Import,Export,Merge,Compact,Ramp,Swatches,Inherit,Reduce};
struct PaletteTransparencyUi {
    bool open=false,valid=false,dirty=true,preview_dirty=true,preview_ok=false;uint64_t generation=0,origin=0;
    int scope=0,row=0;PaletteTransparencyOptions options;PaletteTransparencyPlan plan;PixelSelection selection;
    std::vector<unsigned short> words;std::unordered_set<uint64_t> excluded;std::string error;
    SDL_Texture *before=nullptr,*after=nullptr;
    ImVec2 scope_point,scope_choices[4]{},apply_point,cancel_point,row_points[8]{},name_points[8]{},slot_points[256]{},all_point,none_point;
};
struct PaletteRemovalUi {
    bool open=false,valid=false,dirty=true,preview_dirty=true,preview_ok=false;uint64_t generation=0;
    int row=0;PaletteRemovalOptions options;PaletteRemovalPlan plan;std::vector<unsigned short> words;std::string error;
    SDL_Texture *before=nullptr,*after=nullptr;
    ImVec2 apply_point,cancel_point,slot_points[256]{},row_points[8]{},all_point,none_point,ppp_point;
};
struct PaletteZeroUi {
    bool open=false,valid=false,dirty=true,preview_dirty=true,preview_ok=false,automatic=true,allow_recolor=false;uint64_t generation=0;
    int row=0,scope=0,slot=1;PaletteZeroOptions options;PaletteZeroPlan plan;PixelSelection selection;std::string error;
    SDL_Texture *before=nullptr,*after=nullptr;
    ImVec2 apply_point,cancel_point,scope_point,scope_choices[3]{},automatic_point,slot_point,approve_point,row_points[8]{},ppp_point;
};
struct PaletteMoveUi {
    bool open=false,valid=false,dirty=true,preview_dirty=true,preview_ok=false;uint64_t generation=0;
    int row=0;PaletteMoveOptions options;PaletteMovePlan plan;std::vector<unsigned short> words;std::string error;
    SDL_Texture *before=nullptr,*after=nullptr;
    ImVec2 apply_point,cancel_point,slot_points[256]{},row_points[8]{},all_point,none_point;
};
struct PaletteIsolationUi {
    bool open=false,valid=false,dirty=true,preview_dirty=true,preview_ok=false,allow=false,outline=true;uint64_t generation=0;
    int row=0;PaletteIsolationOptions options;PaletteIsolationPlan plan;std::string error;
    SDL_Texture *before=nullptr,*after=nullptr;
    ImVec2 apply_point,cancel_point,ppp_point,outline_point,allow_point,user_point,row_points[8]{};
};
struct IsolationFramesUi {
    bool open=false,valid=false,dirty=true,preview_dirty=true,preview_ok=false,allow=false,outline=true;uint64_t generation=0;
    int row=0,ppp=6;std::shared_ptr<const IsolationFramesSource> captured;std::vector<uint64_t> included;IsolationFramesPlan plan;PixelSelection feature;std::string error;
    SDL_Texture *before=nullptr,*after=nullptr;
    ImVec2 apply_point,cancel_point,all_point,source_point,matched_point,ppp_point,allow_point,outline_point,row_points[8]{},include_points[8]{};
};
struct RegionRemapUi {
    bool open=false,valid=false,dirty=true,capture_dirty=true,preview_dirty=true,preview_ok=false,outline=true;uint64_t generation=0;
    int row=0;RegionRemapOptions options;std::shared_ptr<const RegionRemapSource> captured;std::vector<uint64_t> included;std::vector<unsigned short> words;RegionRemapPlan plan;PixelSelection feature;std::string error;
    SDL_Texture *before=nullptr,*after=nullptr;
    ImVec2 apply_point,cancel_point,all_point,none_point,source_point,matched_point,destination_point,destination_choices[256]{},outline_point,row_points[8]{},include_points[8]{};
};
struct PaletteUi {
    RegionRemapUi remap;ImVec2 remap_point;
    IsolationFramesUi frames;ImVec2 frames_point;
    PaletteIsolationUi isolation;ImVec2 isolation_point;
    PaletteMoveUi move;ImVec2 move_point,move_menu_point;
    PaletteZeroUi zero;ImVec2 zero_point,zero_menu_point,zero_here_point;
    PaletteRemovalUi removal;ImVec2 remove_slots_point,remove_slots_menu_point;
    PaletteTransparencyUi transparency;ImVec2 transparency_point,transparency_menu_point;
    bool inherit_mode=false,inherit_valid=false,inherit_allow=false,inherit_dirty=false;
    uint64_t inherit_generation=0,inherit_revision=0,tex_inherit_revision=0;bool tex_inherit=false;
    PaletteInheritanceOptions inherit_options;PaletteInheritancePlan inherit_plan;std::string inherit_error;
    ImVec2 inherit_point,inherit_approximate_point,inherit_perceptual_point,cancel_inherit_point;

    bool swatch_mode=false,swatch_valid=false;uint64_t swatch_generation=0;int swatch_kind=0,swatch_slot=1;
    PaletteSwatchClipboard swatch_source;PaletteSwatchPlan swatch_plan;std::string swatch_error;
    ImVec2 swatches_menu_point,copy_one_point,copy_swatches_point,paste_one_point,paste_swatches_point,append_swatches_point,clear_swatches_point,swatch_same_point,swatch_append_point,cancel_swatches_point;
    bool open=false,compare=true,match=true,export_act=false,rename_focus=false,palette_reveal=false;
    uint64_t doc=0,seen=0;int index=0,slot=1,preview_sprite=0,new_count=64,hue=0,saturation=0,lightness=0;
    std::array<bool,256> selected{};
    std::vector<unsigned short> baseline,manual;
    ImGuiTextFilter filter;
    char name[10]={},path[2048]={};std::string error;
    PaletteCommand command=PaletteCommand::None;
    SDL_Texture *preview=nullptr,*original=nullptr;int tex_w=0,tex_h=0;
    uint64_t tex_revision=0,tex_doc=0;int tex_sprite=-1;std::vector<unsigned short> tex_words,tex_baseline;
    ImVec2 clear_clipboard_point,copy_selected_point;
    ImVec2 invert_colors_point,reverse_colors_point;
    ImVec2 apply_point,reset_point,copy_point,paste_point,undo_point,assign_point,hue_point,close_point,swatch_point;
    bool merge_mode=false,merge_valid=false,allow_approximation=false,tex_merge=false;
    bool marked_merge=false,marked_dirty=true;uint64_t marked_generation=0;
    std::vector<int> marked_candidates;std::set<int> marked_excluded;
    PaletteMultiMergeOptions marked_options;PaletteMultiMergePlan marked_plan;
    ImVec2 palette_mark_points[8]{},palette_row_points[8]{},marked_row_points[8]{},remove_sources_point;
    PaletteMergeOptions merge_options;
    PaletteMergePlan merge_plan;
    std::string merge_error;
    uint64_t merge_revision=0,tex_merge_revision=0;
    ImVec2 merge_point,grow_point,approximate_point,cancel_merge_point;
    bool reduce_mode=false,reduce_dirty=false,reduce_allow=false;
    PaletteReductionOptions reduce_options;PaletteReductionPlan reduce_plan;
    ImVec2 reduce_point,reduce_depth_point,reduce_choices[5]{},reduce_approximate_point,cancel_reduce_point;
    bool usage_mode=false,usage_valid=false,tex_usage=false;
    PaletteCompactionOptions usage_options;
    PaletteCompactionPlan usage_plan;
    std::string usage_error;
    uint64_t usage_revision=0,tex_usage_revision=0,usage_generation=0;
    ImVec2 order_point,order_choices[3]{};
    ImVec2 usage_point,pack_point,depth_point,cancel_usage_point,clean_copy_point;
    bool ramp_mode=false,ramp_valid=false,presets_loaded=false,presets_writable=true;
    PaletteRampOptions ramp_options;
    std::vector<unsigned short> ramp_words;
    std::vector<GradientPreset> presets;
    int active_stop=0,preset=-1;
    char preset_name[65]={};std::string ramp_error,preset_error;
    ImVec2 ramps_point,gradient_point,reverse_point,strength_point,cancel_ramp_point,save_preset_point;
    std::array<ImVec2,256> swatch_points;
    ImVec2 ramp_sample_point;
};
struct LegacyMediaUi {
    bool open=false,exporting=false,valid=false,dirty=false;LegacyMediaKind kind=LegacyMediaKind::Lbm;
    uint64_t doc=0,sprite=0,seen=0;int palette=-1,anchor[2]={};
    char path[32768]={},name[16]={};std::string error;
    IndexedMedia media;ImportBatch batch;IndexedExportReview review;SDL_Texture *preview=nullptr;
    ImVec2 apply_point,rebuild_point,cancel_point,palette_point;
};
struct ImportUi {
    bool open=false,dirty=true,valid=false,playing=false,source_view=true;
    uint64_t doc=0,revision=0,tex_revision=0;int selected=0,tex_selected=-1;double tick=0;
    ImportMedia media;
    ImportOptions options;
    ImportBatch all,batch;
    char path[2048]={},prefix[12]="FRAME";
    std::string error;
    SDL_Texture *source_texture=nullptr,*frame_texture=nullptr;
    int tex_w=0,tex_h=0;
    ImVec2 apply_point,cancel_point,rebuild_point,grid_point,sequence_point,palette_point,anchor_point;
};
struct DigitizeUi {
    bool open=false,matte_dirty=false,dirty=true,valid=false,erase=false,brush=false,playing=false;
    uint64_t doc=0,tex_revision=0;int frame=0,material=-1,view=0,radius=2,tex_view=-1,tex_frame=-1;double tick=0;
    DigitizeSession session;ImportBatch batch;
    char path[32768]={},prefix[12]="DIGI";std::string error;
    SDL_Texture *texture=nullptr;
    ImVec2 matte_point,add_point,fit_point,apply_point,cancel_point,seed_point,brush_point;
    ImVec2 preview_origin;float preview_zoom=1;
    int first=0,last=1999,every=1;
    std::unique_ptr<DigitizeJob> job;
    bool initial_matte=false,pending=false,closing=false,previous_valid=false,previous_dirty=true,previous_matte_dirty=true;
    DigitizeJobKind pending_kind=DigitizeJobKind::Matte;
    uint64_t epoch=0;ImVec2 cancel_job_point;
};
struct VideoUi {
    bool open=false,ready=false,closing=false,playing=false,dragging=false;
    uint64_t doc=0;int current=0,shown=-1,attempted=-1;bool shown_deinterlace=false,attempted_deinterlace=false;
    double tick=0;int drag_x=0,drag_y=0;
    VideoSource source;CaptureSettings settings;std::unique_ptr<VideoJob> job;
    char path[2048]={},prefix[12]="DIGI";std::string error;
    SDL_Texture *texture=nullptr;
    ImVec2 preview_origin;float preview_zoom=1;
    ImVec2 capture_point,cancel_point,pick_point,in_point,out_point;
    bool plan_dirty=true,plan_valid=false,capture_pending=false;
    CapturePlan planned;std::string plan_error;
    float zoom=0;ImVec2 pan{0,0};bool panning=false;
    ImVec2 fit_point;
};
struct PaintLineUi {
    bool active=false,valid=false,dirty=true;uint64_t doc=0;
    PaintLineReview review;std::vector<unsigned char> pixels;int changed=0;
    SDL_Texture *texture=nullptr;std::string error;
};
struct WelcomeUi {
    struct Recent {std::string path,name,directory;bool available=false;};
    std::vector<std::string> paths;std::vector<Recent> rows;int last_frame=-2;
    ImVec2 open_point,new_point,sample_point,project_point,refresh_point;
    std::vector<ImVec2> recent_points;
};
struct VariantUi {
    bool active=false,dragging=false,dirty=true,valid=false,show_original=false;uint64_t doc=0;
    VariantSource source;VariantPlan plan;std::vector<unsigned char> coverage;
    PaintBrush brush;int last_x=0,last_y=0,preview_palette=0;
    SDL_Texture *texture=nullptr;std::string error;ImVec2 apply_point,cancel_point,selection_point,original_point;
};
struct FloatingUi {
    bool active=false,dirty=true,valid=false,dragging=false,ratio=true;
    uint64_t doc=0;SelectionTransform stage;SelectionTransformPreview preview;PixelSelection prior_selection;
    SDL_Texture *texture=nullptr;std::string error;
    int drag_kind=-1,drag_x=0,drag_y=0,drag_w=0,drag_h=0;ImVec2 drag_mouse;
    ImVec2 apply_point,cancel_point,rotate_point,flip_point,copy_point;
};
struct RegionUi {
    bool open=false,dirty=true,valid=false,trim=true,cut=false,fallback=false,dragging=false;
    uint64_t doc=0,source=0;int selected=0,view=0,drag_kind=0;
    std::vector<RegionRequest> requests;RegionPlan plan;std::string error;
    char name[16]={};PixelSelection drag_start;ImVec2 drag_mouse;
    SDL_Texture *preview=nullptr;float zoom=0;ImVec2 pan{0,0};
    ImVec2 preview_origin,apply_point,cancel_point,cut_point;float preview_zoom=1;
};
struct Tab {
    std::shared_ptr<const StageBackdrop> stage;
    StageView stage_view;
    SDL_Texture *stage_texture=nullptr;
    std::string stage_texture_error;
    std::unique_ptr<Artwork> art;
    uint64_t uid=0;
    uint64_t selected_id=0,seen_structure=0;
    int selected=0,color=8,radius=1;
    BrushShape brush_shape=BrushShape::Round;
    bool match_colors=false,fill_connected=true,replace_all=false;
    int paint_tolerance=0;
    int blur_strength=50,smudge_strength=65;
    int heal_passes=16,edge_width=1;bool erase_cleanup=true;
    WheelState wheel;
    uint64_t clone_sprite=0;int clone_w=0,clone_h=0,clone_x=0,clone_y=0,clone_dx=0,clone_dy=0;
    bool clone_offset=false,clone_aligned=true,clone_transparent=false;
    float zoom=0;
    ImVec2 pan{0,0};
    bool flip=false,grid=false,anchors=true,reveal_selection=false;
    bool match_palette=true,move_overlay=false;
    int view_w=0,view_h=0;
    MirrorConvention convention=MirrorConvention_Ani2;
    std::set<int> palette_marks;uint64_t palette_marks_structure=0;std::vector<const PAL *> palette_mark_order;
    std::unordered_set<uint64_t> marks;
    Selection selection;
    int selection_operation=0,wand_tolerance=0;
    bool wand_contiguous=true;
    ImVec2 canvas_origin;float canvas_zoom=1;
    ImGuiTextFilter filter;
    std::unordered_map<int,Texture> textures;
    uint64_t check_revision=0;
    int ppp=6, checked_ppp=-1;
    bool export_initialized=false;int export_scope=3;ExportOptions export_options;std::unordered_set<uint64_t> export_excluded;
    L2Report report;
    AlignmentView alignment;
    std::unique_ptr<SceneProject> project;
    SceneUi scene;
    EmbeddedView embedded;
    ~Tab(){if(stage_texture)SDL_DestroyTexture(stage_texture);for(auto &v:textures)if(v.second.value)SDL_DestroyTexture(v.second.value);}
    std::string untitled="Sample library";
    std::string name() const {return art->path().empty()?untitled:fs::u8path(art->path()).filename().u8string();}
};

struct AnchorLinkUi {
    bool open=false,dragging=false,reference_picked=false,target_picked=false,valid=false;
    uint64_t owner=0,reference_doc=0,reference_id=0,target_doc=0,target_id=0;
    AnchorLinkOptions options;AnchorLinkPlan plan;std::string error;
    int alpha=145,picking=0,origin_x=200,origin_y=100;
    float zoom=0;ImVec2 pan,stage_origin;float stage_zoom=1;
    ImVec2 apply_point,undo_point,redo_point,close_point,reset_point,reference_point,target_point;
};
struct PointsUi {
    bool open=false,valid=false,dragging=false,polygon=false;
    uint64_t doc=0;int selected=0;PointTableEdit edit;std::string error;
    float zoom=0,preview_zoom=1;ImVec2 pan,preview_origin;
    ImVec2 enable_point,apply_point,cancel_point,reload_point,handles[10]{};
};
struct TableCompareUi {
    bool open=false,valid=false,matches=false;uint64_t doc=0;int scope=2,selected=-1;
    char path[32768]={};std::string error;TableComparison report;
    ImVec2 compare_point,close_point,row_points[4]{};
};
struct StrikesUi {
    bool open=false,valid=false,dirty_preview=false,saving=false,save_valid=false,flip=false,dragging=false;
    int record=0,character=-1,guard=0; // 1 close, 2 replace source, 3 quit.
    int origin_x=200,origin_y=100,drag_kind=-1,drag_rect[4]={};
    uint64_t preview_doc=0,preview_sprite=0;
    bool animation=false;
    char path[32768]={},save_path[32768]={},operands[8][129]{};
    ImGuiTextFilter filter;std::unique_ptr<StrikeSource> source,pending_source;
    StrikePatch patch;StrikeWriteReview write_review;std::string error,save_error,diff;
    float zoom=0,preview_zoom=1;ImVec2 pan,preview_origin,drag_mouse;
    ImVec2 load_point,apply_point,close_point,undo_point,redo_point,save_point,write_point,cancel_save_point,guard_cancel_point,guard_discard_point,guard_save_point,corner_points[4]{};
};

static constexpr ImGuiWindowFlags panel_flags=ImGuiWindowFlags_NoSavedSettings;
struct ReactionsUi {
    int family=-1,selected=-1,cache_animation=-1;bool show_all=false,cache_facing=false;
    uint64_t cache_preferred=0;
    char search[96]={};std::string error;
    std::vector<ReactionAnimation> catalogue;std::vector<int> visible;
    ReactionAnchorReview anchors;
    std::vector<std::tuple<uint64_t,uint64_t,int>> signature;
    ImVec2 mode_point,search_point,family_point,show_all_point,open_point,refresh_point,facing_point;
    ImVec2 choice_points[8]{},row_points[8]{},family_choices[8]{};
};
struct AsmAssetsUi {
    bool open=false,valid=false,unresolved=false;int selected=-1,cache_animation=-1;uint64_t cache_preferred=0;
    bool library_review=false,allow_partial=false;int library_row=0;
    std::shared_ptr<const AsmArtworkLibrary> library_snapshot;std::unique_ptr<Tab> library_preview;std::string library_error;
    ImVec2 prepare_library_point,create_library_point,cancel_library_point,copy_library_point,partial_point,library_row_points[8]{};
    AsmAssetReview review;AsmArtworkChoices choices;std::string error;ImGuiTextFilter filter;
    std::vector<std::pair<uint64_t,uint64_t>> signature;
    ImVec2 show_point,close_point,open_point,frame_point,choice_point,auto_point,filter_point,copy_point,unresolved_point;
    ImVec2 row_points[8]{},choice_points[8]{};
};
struct AsmUi {
    bool open=false,facing=false,overlay=true,reaction_mode=false;int selected=0,hold=4;
    ReactionsUi reactions;AsmAssetsUi assets;
    int origin_x=200,origin_y=100;uint64_t preferred=0;
    std::unique_ptr<AsmSource> source;PreviewPlayer player;
    char path[32768]={};std::string error;ImGuiTextFilter filter;
    int cache_anim=-1,cache_frame=-1;uint64_t cache_preferred=0;
    std::vector<std::pair<uint64_t,uint64_t>> cache_libraries;
    std::vector<AsmPiece> pieces;float zoom=0;ImVec2 pan;
    ImVec2 next_point,previous_point,play_point,close_point,bind_point,floor_point;
};
struct FatalityUi {
    bool open=false,saving=false,valid=false,dirty_preview=false,save_valid=false,return_preview=false,body_only=false;
    int guard=0,block=-1,kind=0;uint64_t seen=0;
    FatalityWorkspace workspace;uint64_t document=0,guard_document=0,pending_document=0;
    std::shared_ptr<FatalitySource> source;std::unique_ptr<FatalitySource> pending_source;
    std::map<uint64_t,int> selected_blocks;
    char path[32768]={},save_path[32768]={};std::vector<char> body;
    std::string error,save_error,diff,body_baseline;FatalityPatch patch;FatalityWriteReview write_review;
    ImGuiTextFilter filter;
    ImVec2 load_point,apply_point,reset_point,undo_point,redo_point,save_point,write_point,cancel_save_point,close_point;
    ImVec2 guard_save_point,guard_discard_point,guard_cancel_point,preview_point,back_point,body_point,row_points[8]{},combo_point,documents_point,document_choices[16]{},reload_point,close_source_point,combo_choices[16]{};
};
struct BindingsUi {
    bool open=false,saving=false,valid=false,return_asm=false;int guard=0;
    std::map<std::string,std::unique_ptr<StrikeBindings>> documents;
    std::unique_ptr<StrikeBindings> pending_reload;
    std::string selected,failed,error,save_error;
    char animation[129]={},strike[129]={};
    BindingsWriteReview review;
    ImVec2 save_point,write_point,cancel_point,undo_point,redo_point,reload_point,done_point;
    ImVec2 guard_save_point,guard_discard_point,guard_cancel_point,apply_point;
};
struct StrikePhasesUi {
    bool open=false,valid=false,playing=false,animation=false,flip=false,return_strikes=false;
    int character=0,base_slot=0,window=6,remaining=6,origin_x=200,origin_y=100;
    uint64_t generation=0,preview_doc=0,preview_sprite=0;
    const StrikeSource *owner=nullptr;PhaseOptions options;PhaseCatalog catalog;PhasePick pick;
    std::string error;double elapsed=0;float zoom=0;ImVec2 pan;
    bool editing=false,draft_valid=false;std::string draft_label,draft_error;PhasePatch patch;PhaseCatalog draft_catalog;
    std::vector<std::array<int,2>> draft;ImVec2 rows_edit_point,rows_apply_point,rows_cancel_point,source_undo_point,source_redo_point,source_save_point;
    ImVec2 enable_point,wolf_point,next_point,previous_point,play_point,close_point,edit_point;
};
static std::string native_file(SDL_Window *window,bool save,const std::string &initial,bool png=false,bool project=false,bool palette=false,bool act=false,bool media=false,bool video=false,bool manifest=false,int pipeline=-1,int legacy=-1) {
#ifdef _WIN32
    wchar_t name[32768]={}; auto value=fs::u8path(initial).wstring();
    if(save && value.size()<32768)std::copy(value.begin(),value.end(),name);
    OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.lpstrFile=name;ofn.nMaxFile=32768;
    ofn.lpstrFilter=manifest?L"LOAD2 manifests\0*.LOD;*.lod\0":video?L"Video clips\0*.mp4;*.mov;*.m4v;*.avi;*.mkv;*.webm;*.mpg;*.mpeg;*.wmv;*.mts;*.m2ts;*.dv;*.flv\0":media?L"Sprite sheets / animations\0*.png;*.PNG;*.gif;*.GIF;*.bmp;*.BMP;*.tga;*.TGA;*.jpg;*.jpeg\0":palette?(save?(act?L"Adobe ACT palette\0*.ACT;*.act\0":L"Midway PAL palette\0*.PAL;*.pal\0"):L"Palette files\0*.PAL;*.pal;*.ACT;*.act\0"):project?(save?L"Studio project\0*.ISTUDIO;*.istudio\0":L"Studio / classic projects\0*.ISTUDIO;*.istudio;*.WAX;*.wax;*.WVP;*.wvp\0"):(png?L"PNG image\0*.png;*.PNG\0":(save?L"Midway IMG library\0*.IMG;*.img\0All files\0*.*\0":L"IMG libraries / LOD manifests\0*.IMG;*.img;*.LOD;*.lod\0All files\0*.*\0"));
    ofn.lpstrDefExt=manifest?L"LOD":(media||video)?nullptr:palette?(act?L"ACT":L"PAL"):project?L"ISTUDIO":png?L"png":L"img";ofn.lpstrTitle=manifest?L"Open LOD libraries - Imgtool Studio":video?L"Capture video frames - Imgtool Studio":media?L"Import sprite sheet / GIF - Imgtool Studio":palette?(save?L"Export palette - Imgtool Studio":L"Import palette - Imgtool Studio"):project?(save?L"Save Studio project":L"Open Studio project / import WAX"):(png?(save?L"Export sprite PNG - Imgtool Studio":L"Import sprite PNG - Imgtool Studio"):(save?L"Save IMG library - Imgtool Studio":L"Open IMG library - Imgtool Studio"));
    if(pipeline==4){ofn.lpstrFilter=L"Assembly strike source\0*.ASM;*.asm\0";ofn.lpstrDefExt=L"ASM";ofn.lpstrTitle=save?L"Save strike source As - Imgtool Studio":L"Open MKSTK.ASM - Imgtool Studio";}
    if(pipeline==5){ofn.lpstrFilter=L"Assembly animations\0*.ASM;*.asm\0";ofn.lpstrDefExt=nullptr;ofn.lpstrTitle=L"Open animation ASM - Imgtool Studio";}
    if(pipeline==6){ofn.lpstrFilter=L"IMG sprite libraries\0*.IMG;*.img\0";ofn.lpstrDefExt=nullptr;ofn.lpstrTitle=L"Open animation artwork - Imgtool Studio";}
    if(pipeline==3){ofn.lpstrFilter=L"Existing image tables\0*.TBL;*.tbl\0";ofn.lpstrDefExt=nullptr;ofn.lpstrTitle=L"Compare existing TBL - Imgtool Studio";}
    if(pipeline>=0&&pipeline<3){const wchar_t *filters[]={L"Midway TBL table\0*.TBL;*.tbl\0All files\0*.*\0",L"Midway IRW pixels\0*.IRW;*.irw\0All files\0*.*\0",L"ANILST assembly sprite list\0*.ASM;*.asm\0All files\0*.*\0"};const wchar_t *extensions[]={L"TBL",L"IRW",L"ASM"};ofn.lpstrFilter=filters[pipeline];ofn.lpstrDefExt=extensions[pipeline];ofn.lpstrTitle=L"Export pipeline file - Imgtool Studio";}
    if(legacy>=0){ofn.lpstrFilter=legacy==2?L"Indexed LBM / IFF / TGA\0*.LBM;*.lbm;*.IFF;*.iff;*.TGA;*.tga\0":legacy==0?L"IFF PBM image\0*.LBM;*.lbm\0":L"Indexed TGA image\0*.TGA;*.tga\0";ofn.lpstrDefExt=save?(legacy==0?L"LBM":L"TGA"):nullptr;ofn.lpstrTitle=save?L"Export indexed sprite - Imgtool Studio":L"Import indexed artwork - Imgtool Studio";}
    if(legacy==3){ofn.lpstrFilter=L"TGA sprite sheet\0*.TGA;*.tga\0";ofn.lpstrTitle=L"Export TGA sprite sheet - Imgtool Studio";}
    if(legacy==4){ofn.lpstrFilter=L"Stage scenery\0*.BDD;*.bdd;*.BDB;*.bdb\0";ofn.lpstrDefExt=nullptr;ofn.lpstrTitle=L"Load reference scenery - Imgtool Studio";}
    ofn.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    SDL_SysWMinfo wm{};SDL_VERSION(&wm.version);
    if(SDL_GetWindowWMInfo(window,&wm))ofn.hwndOwner=wm.info.win.window;
    if(save?GetSaveFileNameW(&ofn):GetOpenFileNameW(&ofn))return fs::path(name).u8string();
#else
    (void)window;(void)save;(void)initial;(void)png;(void)project;(void)palette;(void)act;(void)media;(void)video;(void)manifest;(void)pipeline;(void)legacy;
#endif
    return {};
}
static std::vector<std::string> native_digitize_files(SDL_Window *window){
    std::vector<std::string> paths;
#ifdef _WIN32
    wchar_t names[32768]={};OPENFILENAMEW o{};o.lStructSize=sizeof o;o.lpstrFile=names;o.nMaxFile=32768;o.lpstrFilter=L"Digitize source images\0*.png;*.PNG;*.gif;*.GIF;*.bmp;*.BMP;*.tga;*.TGA;*.jpg;*.jpeg\0";o.lpstrTitle=L"Select digitized frames - Imgtool Studio";o.Flags=OFN_EXPLORER|OFN_ALLOWMULTISELECT|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST;SDL_SysWMinfo wm{};SDL_VERSION(&wm.version);if(SDL_GetWindowWMInfo(window,&wm))o.hwndOwner=wm.info.win.window;if(GetOpenFileNameW(&o)){fs::path first=names;auto p=names+first.native().size()+1;if(!*p)paths.push_back(first.u8string());else while(*p){fs::path file=p;paths.push_back((first/file).u8string());p+=file.native().size()+1;}}
#else
    (void)window;
#endif
    return paths;
}

class App {
public:
    SDL_Window *window=nullptr;
    SDL_Renderer *renderer=nullptr;
    ImFont *home_title=nullptr,*home_section=nullptr,*tool_font=nullptr;
    ToolsUi tools_ui;
    AnchorLinkUi anchor_link;
    PointsUi points;
    TableCompareUi table_compare;
    StrikesUi strikes;
    AsmUi asm_ui;
    FatalityUi fatality;
    BindingsUi binding_ui;
    StrikePhasesUi strike_phases;
    bool stage_dialog_open=false;uint64_t stage_doc=0;
    char stage_path[32768]={};std::string stage_error;
    ImVec2 stage_load_point,stage_clear_point,stage_close_point,stage_visible_point,stage_align_point;
    WelcomeUi home;
    std::vector<std::unique_ptr<Tab>> tabs;
    int active=-1;
    uint64_t next_uid=1,close_uid=0,remove_uid=0,select_uid=0;
    Tool tool=Tool::Select;
    Clipboard clipboard;std::shared_ptr<const SpriteClipboard> sprite_clipboard;std::shared_ptr<const AnimationClipboard> animation_clipboard;
    SDL_Texture *paste_texture=nullptr;
    uint64_t paste_generation=0,paste_uid=0;
    int paste_sprite=-1;
    ImGuiTextFilter help_search;int help_topic=0;ImVec2 help_done_point,help_search_point;
    std::array<ImVec2,sizeof(help_topics)/sizeof(help_topics[0])> help_topic_points{};
    int paste_x=0,paste_y=0;uint64_t space_mark_doc=0,space_mark_sprite=0;
    bool running=true,quitting=false,help=false,diagnostics=false,paste_mode=false,stroke=false,select_drag=false,anchor_drag=false;
    ImVec2 anchor_drag_mouse;
    int anchor_start_x=0,anchor_start_y=0;
    int start_x=0,start_y=0,last_x=0,last_y=0;
    PixelSelection selection_base;
    SelectionOp selection_operation=SelectionOp::Replace;
    std::vector<SelectionPoint> lasso_points;
    PaintBrush stroke_brush;
    CloneStroke clone;
    FilterStroke filter;
    RepairStroke repair;
    BatchUi batch;BatchRenameUi renaming;BatchPaletteUi batch_palette;RampTransferUi ramp_transfer;CostumeUi costume;BatchDeleteUi deleting;LibraryCopyUi library_copy;PaletteCleanupUi cleanup;AnimationCopyUi animation_copy;
    RestoreUi restoring;
    LodUi lod;
    ExportUi exporting;
    SheetUi sheet;
    std::string error,status="Open an IMG library or explore the sample.",prefs,ini;
    std::vector<std::string> recent;
    bool path_dialog=false,path_save=false;
    uint64_t path_uid=0;
    char path_input[2048]={};
    // Hidden rendering checks use the evaluated placement to drive real gestures.
    bool gesture_smoke=false;
    ImVec2 gesture_point;
    float gesture_zoom=1;
    SpriteRampUi sprite_ramp;
    OverlayFrameUi overlay_frame;
    SubframeSwapUi subframe_swap;
    WaterlineUi waterline;
    DummyBodyUi dummy_body;
    OpacityGradientUi opacity_gradient;
    EmbeddedMetadataUi embedded_metadata;
    EditDialog edit_dialog=EditDialog::None;
    uint64_t edit_doc=0,edit_sprite=0;
    char edit_name[16]={},edit_path[2048]={};
    int edit_w=64,edit_h=64,edit_placement=4,edit_transform=0,edit_palette=0;
    bool edit_quality=false,edit_anchors=true,edit_ratio=true,import_match=false;
    float edit_aspect=1;
    std::string edit_error;
    Action deferred_action=Action::Open;
    uint64_t deferred_sprite=0;
    bool have_deferred=false;
    ImVec2 edit_cancel_point,edit_apply_point;
    std::unique_ptr<SceneProject> pending_project;
    bool pending_scene_new=false;
    std::vector<std::unique_ptr<Tab>> staged_scene_tabs;
    std::vector<SceneIssue> scene_issues;
    std::vector<std::string> scene_relocations;
    uint64_t pending_scene_owner=0,scene_guard_uid=0,scene_path_uid=0;
    int scene_guard_reason=0; // 1=close project, 2=close tab, 3=replace, 4=quit.
    bool scene_path_dialog=false,scene_path_save=false,scene_path_export=false;
    char scene_path_input[2048]={};
    std::string scene_error;
    ImVec2 scene_cancel_point,scene_sources_cancel_point;
    PaletteUi palettes;
    PaletteTransfer palette_clipboard;
    PaletteSwatchClipboard swatch_clipboard;
    ImportUi importing;
    LegacyMediaUi legacy_media;
    DigitizeUi digitizing;
    uint64_t next_digitize_epoch=1;
    VideoUi video;
    RegionUi regions;
    FloatingUi floating;
    PaintLineUi paint_line;
    VariantUi variant;

    Tab *tab(){return active>=0 && active<(int)tabs.size()?tabs[active].get():nullptr;}
    Tab *find(uint64_t uid){for(auto &t:tabs)if(t->uid==uid)return t.get();return nullptr;}
    void finish(){if(layer_drag.active)cancel_layer_drag();if(variant.active)cancel_variant();if(paint_line.active)cancel_paint_line();if(floating.active)cancel_floating();if(tab()){if(tab()->wheel.editing)ImGui::ClearActiveID();tab()->wheel.editing=false;tab()->wheel.serial=0;tab()->art->commit();tab()->alignment.dragging=false;if(tab()->project)tab()->project->commit();tab()->scene.dragging=false;}stroke=false;clone={};filter={};repair={};select_drag=false;anchor_drag=false;selection_base={};lasso_points.clear();}
    void recent_add(const std::string &path){
        recent.erase(std::remove(recent.begin(),recent.end(),path),recent.end());recent.insert(recent.begin(),path);
        if(recent.size()>10)recent.resize(10);
        std::ofstream f(fs::u8path(prefs)/"recent.txt");for(auto &p:recent)f<<p<<'\n';
    }
    void open(const std::string &path){
        if(is_video_path(path)){if(!tab())new_library();request_video(path);return;}
        auto extension=fs::u8path(path).extension().u8string();for(char &c:extension)if(c>='A'&&c<='Z')c+=32;
        if(extension==".bdd"||extension==".bdb"){if(!tab())new_library();show_stage_dialog(*tab());snprintf(stage_path,sizeof stage_path,"%s",path.c_str());load_stage(*tab(),path);return;}
        if(extension==".asm"){std::string message;auto source=AsmSource::open(path,message);if(source){show_asm();install_asm(std::move(source));}else{show_strikes();load_strikes(path);}return;}
        if(extension==".tbl"){if(!tab())new_library();show_table_compare(*tab());snprintf(table_compare.path,sizeof table_compare.path,"%s",path.c_str());rebuild_table_compare();return;}
        if(extension==".lod"){open_lod(path);return;}
        if(extension==".lbm"||extension==".iff"){if(!tab())new_library();request_indexed_import(path);return;}
        if(extension==".tga"){std::ifstream source(fs::u8path(path),std::ios::binary);unsigned char header[3]={};if(source.read((char *)header,3)&&header[1]==1&&(header[2]==1||header[2]==9)){if(!tab())new_library();request_indexed_import(path);return;}}
        if(extension==".png"||extension==".gif"||extension==".bmp"||extension==".jpg"||extension==".jpeg"||extension==".tga"){if(!tab())new_library();request_media_import(path);return;}
        if(extension==".istudio"||extension==".wax"||extension==".wvp"){open_scene(path);return;}
        finish();paste_mode=false;
        std::error_code ec;auto absolute=fs::absolute(fs::u8path(path),ec).lexically_normal();
        if(ec){error=ec.message();return;}
        for(int i=0;i<(int)tabs.size();++i)if(!tabs[i]->art->path().empty() && fs::equivalent(absolute,fs::u8path(tabs[i]->art->path()),ec)){active=i;select_uid=tabs[i]->uid;return;}
        auto doc=Artwork::open(absolute.u8string(),error);if(!doc)return;
        auto t=std::make_unique<Tab>();t->uid=next_uid++;t->art=std::move(doc);t->color=1;t->selected_id=t->art->sprite_id(0);
        t->alignment.initialize(*t->art);sync_alignment(*t);
        tabs.push_back(std::move(t));active=(int)tabs.size()-1;select_uid=tabs.back()->uid;recent_add(absolute.u8string());status="Opened "+absolute.filename().u8string();if(tabs.back()->art->disk_version()<0x634)status+=" — original IMG layout preserved on save.";
    }
    void demo(){finish();paste_mode=false;auto t=std::make_unique<Tab>();t->uid=next_uid++;t->art=Artwork::demo();t->selected_id=t->art->sprite_id(0);t->alignment.initialize(*t->art);sync_alignment(*t);tabs.push_back(std::move(t));active=(int)tabs.size()-1;select_uid=tabs.back()->uid;status="Sample artwork. Save As to create your own IMG.";}
    void synchronize(Tab &t){
        bool structural=t.seen_structure!=t.art->structure_generation();
        if(!structural&&t.selected_id==t.art->selection_id())return;
        int found=t.art->sprite_index(t.art->selection_id());t.selected=found>=0?found:std::clamp(t.selected,0,t.art->count()-1);t.selected_id=t.art->sprite_id(t.selected);t.art->select_sprite(t.selected_id);
        t.alignment.player.playing=false;if(structural)t.alignment.initialize(*t.art);sync_alignment(t);
        if(structural){for(auto &cache:t.textures)if(cache.second.value)SDL_DestroyTexture(cache.second.value);t.textures.clear();}
        auto im=t.art->sprite(t.selected);if(t.view_w!=im->w||t.view_h!=im->h)t.zoom=0;
        validate_clone_source(t);
        t.view_w=im->w;t.view_h=im->h;t.selection={};t.reveal_selection=true;t.seen_structure=t.art->structure_generation();
    }
    void new_library(){finish();auto t=std::make_unique<Tab>();t->uid=next_uid++;t->untitled="Untitled IMG";t->art=Artwork::create();t->selected_id=t->art->sprite_id(0);t->alignment.initialize(*t->art);
        tabs.push_back(std::move(t));active=(int)tabs.size()-1;select_uid=tabs.back()->uid;status="New library. Save As to choose an IMG filename.";}
    void request_edit(EditDialog mode){
        auto t=tab();if(!t)return;finish();t->alignment.player.playing=false;edit_dialog=mode;edit_doc=t->uid;edit_sprite=t->art->sprite_id(t->selected);edit_error.clear();
        auto im=t->art->sprite(t->selected);snprintf(edit_name,sizeof edit_name,"%s",mode==EditDialog::NewSprite?"NEWSPRITE":im->n_s);
        edit_w=im->w;edit_h=im->h;edit_palette=im->palnum;edit_aspect=(float)im->w/im->h;edit_ratio=true;edit_quality=false;edit_anchors=true;edit_placement=4;edit_transform=0;edit_path[0]=0;import_match=false;
        if(mode==EditDialog::ImportPng||mode==EditDialog::ExportPng){
#ifdef _WIN32
            auto path=native_file(window,mode==EditDialog::ExportPng,mode==EditDialog::ExportPng?std::string(im->n_s)+".png":"",true);
            if(path.empty()){edit_dialog=EditDialog::None;return;}if(path.size()>=sizeof edit_path){error="This path is too long for the current dialog.";edit_dialog=EditDialog::None;return;}snprintf(edit_path,sizeof edit_path,"%s",path.c_str());
#endif
        }
    }
    void choose_open(){
#ifdef _WIN32
        auto path=native_file(window,false,{});if(!path.empty())open(path);
#else
        path_save=false;path_dialog=true;path_input[0]=0;
#endif
    }
    bool save(Tab &t,bool as){
        t.art->commit();std::string path=t.art->path();
        if(as || path.empty()) {
#ifdef _WIN32
            path=native_file(window,true,path.empty()?"studio-artwork.img":path);if(path.empty())return false;
#else
            path_save=true;path_dialog=true;path_uid=t.uid;snprintf(path_input,sizeof(path_input),"%s",path.c_str());return false;
#endif
        }
        if(!t.art->save(path,error))return false;
        recent_add(t.art->path());status="Saved "+t.name();return true;
    }
    bool enabled(Action a){
        if(a==Action::Help||a==Action::Diagnostics)return true;
        if(layer_drag.active||floating.active||paint_line.active||variant.active)return a==Action::Undo||a==Action::Quit||a==Action::Fit||a==Action::ZoomIn||a==Action::ZoomOut;
        auto t=tab();if(a==Action::Open||a==Action::OpenLod||a==Action::OpenProject||a==Action::Demo||a==Action::Quit||a==Action::NewLibrary)return true;
        if(a==Action::ClearSpriteClipboard)return sprite_clipboard!=nullptr;
        if(a==Action::ClearAnimationClipboard)return animation_clipboard!=nullptr;
        if(!t)return false;
        if(a==Action::PasteNew)return !clipboard.pixels.empty()&&!t->embedded.enabled&&!t->alignment.enabled&&!t->flip;
        if(a==Action::CopyNew||a==Action::CutNew)return !t->embedded.enabled&&!t->alignment.enabled&&!t->flip;
        if(a==Action::Points||a==Action::Mark||a==Action::MarkAll||a==Action::UnmarkAll||a==Action::PaletteAssign||a==Action::PaletteAssignMarked||a==Action::PaletteRename||a==Action::PaletteMerge||a==Action::ReduceMarked)return !t->embedded.enabled&&!t->alignment.enabled&&!t->flip;
        if(a==Action::ExportSheet)return true;
        if(a==Action::ExportLbm||a==Action::ExportTga)return !t->embedded.enabled;
        if(a==Action::ExportTbl||a==Action::ExportIrw||a==Action::ExportAnilst)return true;
        if(a==Action::PasteSprites)return sprite_clipboard&&!t->alignment.enabled&&!t->embedded.enabled&&!t->flip;
        if(a==Action::PasteAnimations)return animation_clipboard!=nullptr;
        if(a==Action::SplitAnimations||a==Action::CopyAnimations)return t->art->data.seqcnt+t->art->data.scrcnt>0;
        if(a==Action::AnimationCopy)return tabs.size()>1&&(t->art->data.seqcnt+t->art->data.scrcnt)>0;
        if(a==Action::LibraryCopy)return tabs.size()>1&&!t->alignment.enabled&&!t->embedded.enabled&&!t->flip;
        if(a==Action::SplitLibrary||a==Action::CopySprites||a==Action::BatchGeometry||a==Action::BatchRename||a==Action::BatchPalette||a==Action::RampTransfer||a==Action::Costume||a==Action::PaletteCleanup||a==Action::PaletteDuplicates||a==Action::BatchDelete||a==Action::RestoreChildren)return !t->alignment.enabled&&!t->embedded.enabled&&!t->flip;
        if(a==Action::TransformSelection)return !t->alignment.enabled&&!t->embedded.enabled&&!t->flip&&(t->selection.valid||paste_mode);
        if(a==Action::ExtractSelection||a==Action::SplitOverlay||a==Action::CopyOverlay||a==Action::SplitBody)return !t->alignment.enabled&&!t->embedded.enabled&&!t->flip&&(a==Action::SplitBody||t->selection.valid);
        if(a==Action::OpenProject)return true;
        if(a==Action::Undo&&t->wheel.editing)return true;
        if(a==Action::Undo)return t->alignment.enabled&&t->project?t->project->can_undo():t->art->can_undo();
        if(a==Action::Redo)return t->alignment.enabled&&t->project?t->project->can_redo():t->art->can_redo();
        if(a==Action::SaveProject||a==Action::SaveProjectAs||a==Action::CloseProject||a==Action::ExportScene)return t->project!=nullptr;
        bool selection_action=a==Action::Cut||a==Action::SelectAll||a==Action::Deselect||a==Action::InvertSelection||a==Action::SelectOpaque;
        if(selection_action)return !t->embedded.enabled&&!t->alignment.enabled&&!t->flip&&(a!=Action::Cut||t->selection.valid);
        if(t->embedded.enabled&&(a==Action::Copy||a==Action::Paste||a==Action::Clear))return false;
        if(t->embedded.enabled){auto r=embedded_record(*t);if(a==Action::Rename)return r!=nullptr;if(a==Action::Duplicate)return r&&!r->entries.empty();if(a==Action::NewProject)return r&&!r->script&&!r->entries.empty();if(a==Action::ImportPng||a==Action::ExportPng)return false;}
        if(a==Action::Copy)return !t->project||!t->alignment.enabled;
        if(a==Action::Paste)return !clipboard.pixels.empty()&&!t->flip&&!t->alignment.enabled;
        if(a==Action::Clear)return t->selection.valid&&!t->flip&&!t->alignment.enabled;
        if(a==Action::MoveUp)return t->selected>0;
        if(a==Action::MoveDown)return t->selected+1<t->art->count();
        if(a==Action::Delete)return t->art->count()>1;
        return true;
    }
    void action(Action a){
        if(a==Action::Quit){dummy_body.plan={};waterline.plan={};subframe_swap.plan={};clear_overlay_frame_textures();overlay_frame.plan={};clear_sprite_ramp_textures();sprite_ramp.plan={};sprite_ramp.source.reset();clear_opacity_gradient_textures();opacity_gradient.plan={};opacity_gradient.source.reset();palettes.remap.open=false;clear_region_remap_textures();palettes.remap.plan={};palettes.remap.captured.reset();palettes.frames.open=false;clear_isolation_frames_textures();palettes.frames.plan={};palettes.frames.captured.reset();palettes.isolation.open=false;clear_palette_isolation_textures();palettes.isolation.plan={};palettes.move.open=false;clear_palette_move_textures();palettes.move.plan={};palettes.zero.open=false;clear_palette_zero_textures();palettes.zero.plan={};palettes.removal.open=false;clear_palette_removal_textures();palettes.removal.plan={};palettes.transparency.open=false;clear_palette_transparency_textures();palettes.transparency.plan={};animation_copy.open=false;animation_copy.plan={};animation_copy.held.reset();animation_copy.capture_target.reset();cleanup.open=false;cleanup.plan={};renaming.open=false;costume.open=false;clear_costume_textures();costume.plan={};ramp_transfer.open=false;clear_ramp_transfer_textures();ramp_transfer.plan={};batch_palette.open=false;clear_batch_palette_textures();batch_palette.plan={};deleting.open=false;clear_batch_delete_preview();deleting.plan={};library_copy.open=false;clear_library_copy_preview();library_copy.plan={};library_copy.split_removal={};library_copy.split_animation_plan={};library_copy.split_scene_uses.clear();library_copy.held.reset();library_copy.held_artwork.reset();library_copy.artwork_return=false;library_copy.capture_target.reset();}
        if(a==Action::Quit&&fatality.workspace.first_dirty()){finish();asm_ui.open=false;asm_ui.player.playing=false;uint64_t id=fatality.source&&fatality.source->dirty()?fatality.document:fatality.workspace.first_dirty();if(id!=fatality.document)activate_fatality(id);fatality.open=true;fatality.guard=3;fatality.guard_document=id;return;}
        if(a==Action::Quit&&dirty_bindings()){finish();request_bindings_quit();return;}
        if(a==Action::Quit&&strikes.source&&strikes.source->dirty()){finish();strike_phases.open=false;strike_phases.playing=false;asm_ui.open=false;asm_ui.player.playing=false;strikes.guard=3;strikes.open=true;return;}
        if(!enabled(a))return;if(a==Action::Undo&&tab()&&tab()->wheel.editing){auto mask=tab()->selection;tab()->art->cancel();tab()->wheel=WheelState{};synchronize(*tab());auto im=tab()->art->sprite(tab()->selected);if(mask.fits(im->w,im->h))tab()->selection=std::move(mask);ImGui::ClearActiveID();status="Cancelled palette color edit.";return;}if(layer_drag.active&&a==Action::Undo){cancel_layer_drag();return;}if(variant.active&&a==Action::Undo){cancel_variant();return;}if(paint_line.active&&a==Action::Undo){cancel_paint_line();return;}if(floating.active&&a==Action::Undo){cancel_floating();return;}if((!floating.active&&!paint_line.active&&!variant.active)||a==Action::Quit)finish();auto t=tab();
        if(t&&a!=Action::Fit&&a!=Action::ZoomIn&&a!=Action::ZoomOut){t->alignment.player.playing=false;t->scene.playing=false;t->embedded.playing=false;}
        switch(a){
        case Action::PreviewEarlier:case Action::PreviewLater:reorder_preview(*t,a==Action::PreviewEarlier?-1:1);break;
        case Action::Help:help=true;break;
        case Action::Diagnostics:diagnostics=!diagnostics;break;
        case Action::Points:show_points(*t);break;
        case Action::CopyNew:case Action::CutNew:{if(!t->selection.valid){auto im=t->art->sprite(t->selected);t->selection=rectangle_selection(im->w,im->h,0,0,im->w,im->h);}request_regions(false);regions.cut=a==Action::CutNew;regions.dirty=true;break;}
        case Action::PasteNew:paste_new_sprite(*t);break;
        case Action::Mark:mark_sprite(*t,t->selected_id,!marked(*t,t->selected_id));break;
        case Action::MarkAll:for(int i=0;i<t->art->count();++i)mark_sprite(*t,t->art->sprite_id(i),true);break;
        case Action::UnmarkAll:t->marks.clear();break;
        case Action::PaletteAssign:case Action::PaletteAssignMarked:open_batch_palette(*t);batch_palette.scope=a==Action::PaletteAssignMarked?0:3;batch_palette.options.match=false;batch_palette.dirty=true;break;
        case Action::PaletteRename:open_palettes(*t);palettes.rename_focus=true;break;
        case Action::PaletteMerge:open_palettes(*t);start_palette_merge(*t);break;
        case Action::ReduceMarked:open_batch(*t);batch.scope=0;batch.options.kind=BatchGeometryKind::Resize;batch.options.relative=true;batch.options.percent=50;batch.options.quality=true;batch.dirty=true;break;
        case Action::Open:choose_open();break;
        case Action::OpenLod:choose_lod();break;
        case Action::ExportTbl:case Action::ExportIrw:case Action::ExportAnilst:open_export(*t,a==Action::ExportTbl?ExportKind::Tbl:a==Action::ExportIrw?ExportKind::Irw:ExportKind::Anilst);break;
        case Action::Demo:demo();break;
        case Action::NewLibrary:new_library();break;
        case Action::NewProject:if(t->embedded.enabled)t->embedded.command=EmbeddedCommand::Scene;else new_scene(*t);break;
        case Action::OpenProject:choose_scene();break;
        case Action::SaveProject:save_scene(*t,false);break;
        case Action::SaveProjectAs:save_scene(*t,true);break;
        case Action::CloseProject:request_close_scene(*t);break;
        case Action::SaveArtwork:save(*t,false);break;
        case Action::SaveArtworkAs:save(*t,true);break;
        case Action::ExportScene:export_scene(*t);break;
        case Action::Costume:open_costume(*t);break;
        case Action::RampTransfer:open_ramp_transfer(*t);break;
        case Action::ManagePalettes:open_palettes(*t);break;
        case Action::ExportSheet:open_sheet(*t);break;
        case Action::ImportIndexed:request_indexed_import({});break;
        case Action::ExportLbm:case Action::ExportTga:request_indexed_export(a==Action::ExportLbm?LegacyMediaKind::Lbm:LegacyMediaKind::Tga);break;
        case Action::ImportMedia:request_media_import({});break;
        case Action::Digitize:request_digitize({});break;
        case Action::CaptureVideo:request_video({});break;
        case Action::SplitOverlay:case Action::CopyOverlay:show_overlay_frame(*t,a==Action::SplitOverlay);break;
        case Action::ExtractSelection:case Action::SplitBody:request_regions(a==Action::SplitBody);break;
        case Action::TransformSelection:if(paste_mode)begin_clipboard_floating(*t);else begin_floating(*t);break;
        case Action::NewSprite:request_edit(EditDialog::NewSprite);break;
        case Action::Rename:if(t->embedded.enabled)t->embedded.rename_focus=true;else request_edit(EditDialog::Rename);break;
        case Action::Delete:request_edit(EditDialog::Delete);break;
        case Action::Resize:request_edit(EditDialog::Resize);break;
        case Action::CanvasSize:request_edit(EditDialog::CanvasSize);break;
        case Action::Transform:request_edit(EditDialog::Transform);break;
        case Action::ImportPng:request_edit(EditDialog::ImportPng);break;
        case Action::ExportPng:request_edit(EditDialog::ExportPng);break;
        case Action::Duplicate:{if(t->embedded.enabled){t->embedded.command=EmbeddedCommand::Duplicate;break;}int added=t->art->duplicate_sprite(t->selected,error);if(added>=0){t->selected=added;t->selected_id=t->art->sprite_id(added);t->art->select_sprite(t->selected_id);}break;}
        case Action::RestoreChildren:open_restore(*t);break;
        case Action::BatchGeometry:open_batch(*t);break;
        case Action::BatchRename:open_batch_rename(*t);break;
        case Action::BatchPalette:open_batch_palette(*t);break;
        case Action::PaletteCleanup:open_palette_cleanup(*t);break;
        case Action::PaletteDuplicates:open_palette_cleanup(*t,true);break;
        case Action::BatchDelete:open_batch_delete(*t);break;
        case Action::LibraryCopy:open_library_copy(*t);break;
        case Action::AnimationCopy:open_animation_copy(*t);break;
        case Action::CopyAnimations:open_animation_capture(*t);break;
        case Action::SplitAnimations:open_animation_split(*t);break;
        case Action::PasteAnimations:open_animation_paste(*t);break;
        case Action::ClearAnimationClipboard:animation_clipboard.reset();status="Animation clipboard cleared.";break;
        case Action::SplitLibrary:open_library_split(*t);break;
        case Action::CopySprites:open_sprite_capture(*t);break;
        case Action::PasteSprites:open_sprite_paste(*t);break;
        case Action::ClearSpriteClipboard:sprite_clipboard.reset();status="Sprite clipboard cleared.";break;
        case Action::Trim:t->art->trim_sprite(t->selected,error);break;
        case Action::MoveUp:case Action::MoveDown:t->art->move_sprite(t->selected,t->selected+(a==Action::MoveUp?-1:1),error);break;
        case Action::Save:if(t->project&&t->alignment.enabled)save_scene(*t,false);else save(*t,false);break;
        case Action::SaveAs:if(t->project&&t->alignment.enabled)save_scene(*t,true);else save(*t,true);break;
        case Action::Undo:if(t->project&&t->alignment.enabled){t->scene.playing=false;t->project->undo();clamp_scene_ui(*t);}else t->art->undo();break;
        case Action::Redo:if(t->project&&t->alignment.enabled){t->scene.playing=false;t->project->redo();clamp_scene_ui(*t);}else t->art->redo();break;
        case Action::Copy:case Action::Cut:{auto im=t->art->sprite(t->selected);auto &r=t->selection;
            if(paste_texture){SDL_DestroyTexture(paste_texture);paste_texture=nullptr;}
            clipboard=r.valid?t->art->copy_selection(t->selected,r):t->art->copy(t->selected,0,0,im->w,im->h);
            if(a==Action::Cut)t->art->clear_selection(t->selected,r,error);
            status="Copied pixels and palette. Paste, then click the canvas to place.";break;}
        case Action::Paste:paste_mode=true;paste_x=paste_y=0;status="Click to place. Escape cancels. Colors map to the destination palette.";break;
        case Action::Clear:t->art->clear_selection(t->selected,t->selection,error);break;
        case Action::SelectAll:{auto im=t->art->sprite(t->selected);t->selection=rectangle_selection(im->w,im->h,0,0,im->w,im->h);break;}
        case Action::Deselect:t->selection={};break;
        case Action::InvertSelection:{auto im=t->art->sprite(t->selected);t->selection=invert_selection(t->selection,im->w,im->h);break;}
        case Action::SelectOpaque:t->selection=opaque_selection(*t->art->sprite(t->selected));break;
        case Action::Fit:if(t->embedded.enabled){t->embedded.zoom=0;t->embedded.pan=ImVec2();}else if(t->project&&t->alignment.enabled){t->scene.zoom=0;t->scene.pan=ImVec2();}else if(t->alignment.enabled){t->alignment.zoom=0;t->alignment.pan=ImVec2();}else{t->zoom=0;t->pan=ImVec2();}break;
        case Action::ZoomIn:case Action::ZoomOut:{float &zoom=t->alignment.enabled?t->alignment.zoom:t->zoom;
            if(t->embedded.enabled)t->embedded.zoom=std::clamp((t->embedded.zoom>0?t->embedded.zoom:1)*(a==Action::ZoomIn?1.25f:.8f),.1f,32.f);else if(t->project&&t->alignment.enabled){t->scene.zoom=std::clamp((t->scene.zoom>0?t->scene.zoom:1)*(a==Action::ZoomIn?1.25f:.8f),.25f,32.f);}else zoom=std::clamp((zoom>0?zoom:1)*(a==Action::ZoomIn?1.25f:.8f),t->alignment.enabled?.25f:1.f,64.f);break;}
        case Action::Close:close_uid=t->uid;break;
        case Action::Quit:fatality.open=false;fatality.saving=false;fatality.guard=0;fatality.pending_source.reset();strike_phases.open=false;strike_phases.playing=false;binding_ui.open=false;binding_ui.saving=false;binding_ui.guard=0;binding_ui.pending_reload.reset();asm_ui.open=false;asm_ui.player.playing=false;strikes.open=false;strikes.guard=0;strikes.saving=false;strikes.pending_source.reset();table_compare.open=false;points.open=false;anchor_link.open=false;stage_dialog_open=false;sheet.open=false;legacy_media.open=false;exporting.open=false;lod.open=false;restoring.open=false;batch.open=false;regions.open=false;edit_dialog=EditDialog::None;help=false;path_dialog=false;palettes.open=false;importing.open=false;digitizing.open=false;digitizing.pending=false;if(digitizing.job)digitizing.job->cancel=true;video.open=false;if(video.job)video.job->cancel=true;quitting=true;close_uid=0;break;
        }
    }
    void item(const char *label,const char *shortcut,Action a){for(const auto &binding:shortcut_bindings)if(binding.action==a){shortcut=binding.label;break;}if(ImGui::MenuItem(label,shortcut,false,enabled(a)))action(a);}
    void button(const char *label,Action a){ImGui::BeginDisabled(!enabled(a));if(ImGui::Button(label))action(a);ImGui::EndDisabled();}
    void sync_alignment(Tab &t){
        auto &v=t.alignment;int sequence,frame;
        if(locate_preview_sprite(v.sequences,t.selected,sequence,frame)){
            v.sequence=sequence;v.player.seek(frame,(int)v.sequences[sequence].frames.size());
            auto &item=v.sequences[sequence].frames[frame];
            if(v.enabled)v.multipart=std::find(item.pieces.begin(),item.pieces.end(),t.selected)!=item.pieces.end();
        }
    }
    void select(int index){finish();if(auto t=tab()){t->selected=index;t->selected_id=t->art->sprite_id(index);t->art->select_sprite(t->selected_id);t->reveal_selection=true;t->selection={};paste_mode=false;t->alignment.player.playing=false;sync_alignment(*t);if(t->project&&t->alignment.enabled){t->scene.source_doc=t->uid;t->scene.source_sprite=t->selected_id;status="Source sprite selected. Use Add frame or Replace frame in the inspector.";}auto im=t->art->sprite(index);auto p=t->art->palette(im->palnum);t->color=std::min(t->color,(int)p->numc-1);}}
    void select_frame(Tab &t,int index,bool pause=true){
        auto &v=t.alignment;finish();
        if(pause)v.player.playing=false;
        v.player.seek(index,(int)v.sequences[v.sequence].frames.size());
        if(auto frame=v.frame()){auto pieces=preview_pieces(*frame,v.multipart);t.selected=pieces.front();t.selected_id=t.art->sprite_id(t.selected);t.art->select_sprite(t.selected_id);t.reveal_selection=true;t.selection={};}
    }
    void workspace(Tab &t,bool alignment){
        finish();paste_mode=false;t.alignment.player.playing=false;t.scene.playing=false;t.embedded.playing=false;t.embedded.enabled=false;t.alignment.enabled=alignment;
        if(alignment){select_frame(t,t.alignment.player.frame);if(!t.alignment.placed)feet_to_floor(t);}
    }
    void toggle_playback(Tab &t){
        finish();auto &v=t.alignment;
        if(!v.player.playing&&!v.player.loop&&v.player.frame==(int)v.sequences[v.sequence].frames.size()-1)select_frame(t,0);
        v.player.playing=!v.player.playing;
    }
    void feet_to_floor(Tab &t){
        auto &v=t.alignment;if(auto frame=v.frame()){
            int bottom=-32768;for(int i:preview_pieces(*frame,v.multipart))if(auto im=t.art->sprite(i))bottom=std::max(bottom,(int)im->h-(short)im->aniy);
            v.origin_y=v.floor_y-bottom;v.placed=true;
        }
    }

    SDL_Texture *texture(Tab &t,int i){
        auto im=t.art->sprite(i);auto pal=t.art->palette(im->palnum);auto &cache=t.textures[i];
        if(cache.value && cache.revision==t.art->generation())return cache.value;
        if(!cache.value){cache.value=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,im->w,im->h);if(!cache.value)return nullptr;SDL_SetTextureBlendMode(cache.value,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(cache.value,SDL_ScaleModeNearest);}
        checked_layer_bytes(*im);std::vector<unsigned char> rgba((size_t)im->w*im->h*4);int stride=(im->w+3)&~3;
        for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){int ci=visible_pixel(*im,x,y);auto q=&rgba[((size_t)y*im->w+x)*4];
            if(ci<pal->numc)pal_word_to_rgb8((unsigned char *)pal->data_p+ci*2,q,q+1,q+2);else {q[0]=255;q[1]=0;q[2]=255;}
            q[3]=ci?255:0;
        }
        SDL_UpdateTexture(cache.value,nullptr,rgba.data(),im->w*4);cache.revision=t.art->generation();return cache.value;
    }
    SDL_Texture *paste_preview(Tab &t){
        if(paste_texture&&paste_generation==t.art->generation()&&paste_uid==t.uid&&paste_sprite==t.selected)return paste_texture;
        if(paste_texture)SDL_DestroyTexture(paste_texture);
        paste_texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,clipboard.w,clipboard.h);
        if(!paste_texture)return nullptr;
        auto p=t.art->palette(t.art->sprite(t.selected)->palnum);
        bool same=clipboard.colors.size()==p->numc;
        for(size_t i=0;same&&i<clipboard.colors.size();++i)same=clipboard.colors[i]==palette_word_at((unsigned char *)p->data_p,(int)i);
        unsigned char map[256]={};for(size_t i=1;i<clipboard.colors.size();++i)map[i]=same?(unsigned char)i:nearest_palette_index_for_word(clipboard.colors[i],p);
        std::vector<unsigned char> rgba(clipboard.pixels.size()*4);
        for(size_t i=0;i<clipboard.pixels.size();++i){auto c=clipboard.pixels[i];auto out=rgba.data()+i*4;pal_word_to_rgb8((unsigned char *)p->data_p+map[c]*2,out,out+1,out+2);bool covered=clipboard.mask.empty()||clipboard.mask[i];out[3]=c&&covered?190:0;if(!c&&covered&&!clipboard.mask.empty()){out[0]=180;out[1]=60;out[2]=70;out[3]=90;}}
        SDL_SetTextureBlendMode(paste_texture,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(paste_texture,SDL_ScaleModeNearest);
        SDL_UpdateTexture(paste_texture,nullptr,rgba.data(),clipboard.w*4);
        paste_generation=t.art->generation();paste_uid=t.uid;paste_sprite=t.selected;return paste_texture;
    }
    void menu(){
        if(!ImGui::BeginMenuBar())return;
        ImGui::TextColored(accent_surface(1),"imgtool");ImGui::SameLine();ImGui::TextDisabled("STUDIO");ImGui::Separator();
        if(ImGui::BeginMenu("File")){item("New IMG library","Ctrl+N",Action::NewLibrary);item("Open IMG...","Ctrl+O",Action::Open);item("Open LOD libraries...",nullptr,Action::OpenLod);
            item("Open project / import WAX...","Ctrl+Shift+O",Action::OpenProject);
            if(ImGui::BeginMenu("Open recent",!recent.empty())){auto list=recent;for(auto &p:list)if(ImGui::MenuItem(p.c_str()))open(p);ImGui::EndMenu();}
            item("Save","Ctrl+S",Action::Save);item("Save As...","Ctrl+Shift+S",Action::SaveAs);ImGui::Separator();
            item("Import PNG sprite...",nullptr,Action::ImportPng);item("Import sprite sheet / GIF...",nullptr,Action::ImportMedia);item("Digitize frame(s)...",nullptr,Action::Digitize);item("Capture video frames...",nullptr,Action::CaptureVideo);item("Import indexed LBM / TGA...",nullptr,Action::ImportIndexed);item("Export selected PNG...",nullptr,Action::ExportPng);item("Export selected LBM...",nullptr,Action::ExportLbm);item("Export selected TGA...",nullptr,Action::ExportTga);item("Export TGA sprite sheet...",nullptr,Action::ExportSheet);ImGui::Separator();
            if(ImGui::BeginMenu("Export pipeline",tab()!=nullptr)){item("TBL image table...",nullptr,Action::ExportTbl);item("IRW raw pixels...",nullptr,Action::ExportIrw);item("ANILST sprite list...",nullptr,Action::ExportAnilst);ImGui::EndMenu();}
            if(tab()&&ImGui::MenuItem("Compare existing TBL..."))show_table_compare(*tab());
            item("Copy IMG animations to library...",nullptr,Action::AnimationCopy);item("Split IMG animations to new library...",nullptr,Action::SplitAnimations);item("Copy IMG animations to clipboard...",nullptr,Action::CopyAnimations);animation_paste_item("Paste IMG animations from clipboard...");
            item("Save IMG artwork",nullptr,Action::SaveArtwork);item("Save IMG artwork As...",nullptr,Action::SaveArtworkAs);ImGui::Separator();
            item("Explore sample",nullptr,Action::Demo);item("Close library","Ctrl+W",Action::Close);item("Quit",nullptr,Action::Quit);ImGui::EndMenu();}
        if(ImGui::BeginMenu("Edit")){item("Undo","Ctrl+Z",Action::Undo);item("Redo","Ctrl+Y",Action::Redo);ImGui::Separator();item("Copy pixels","Ctrl+C",Action::Copy);item("Cut pixels","Ctrl+X",Action::Cut);item("Paste pixels","Ctrl+V",Action::Paste);if(ImGui::MenuItem("Keep paste as overlay","L",false,tab()&&(paste_mode||(floating.active&&floating.stage.clipboard))))drop_paste_layer();item("Clear selection","Delete",Action::Clear);item("Copy selection to new sprite...",nullptr,Action::CopyNew);item("Cut selection to new sprite...",nullptr,Action::CutNew);item("Paste pixels as new sprite",nullptr,Action::PasteNew);item("Transform selection...","Ctrl+T",Action::TransformSelection);ImGui::Separator();item("Select all","Ctrl+A",Action::SelectAll);item("Select artwork",nullptr,Action::SelectOpaque);item("Invert selection","Ctrl+Shift+I",Action::InvertSelection);item("Deselect","Ctrl+D",Action::Deselect);ImGui::Separator();item("Clear sprite clipboard",nullptr,Action::ClearSpriteClipboard);item("Clear animation clipboard",nullptr,Action::ClearAnimationClipboard);ImGui::EndMenu();}
        if(ImGui::BeginMenu("Project")){
            item("Create from current preview",nullptr,Action::NewProject);item("Open / import WAX...",nullptr,Action::OpenProject);item("Save project",nullptr,Action::SaveProject);item("Save project As...",nullptr,Action::SaveProjectAs);item("Export current scene PNG...",nullptr,Action::ExportScene);if(ImGui::MenuItem("Build dummy body...",nullptr,false,tab()&&tab()->project!=nullptr))show_dummy_body(*tab());item("Close project",nullptr,Action::CloseProject);ImGui::EndMenu();}
        if(ImGui::BeginMenu("Sprite",tab()!=nullptr&&!tab()->embedded.enabled)){
            item("New sprite...",nullptr,Action::NewSprite);item("Rename...","F2",Action::Rename);item("Batch rename...",nullptr,Action::BatchRename);item("Duplicate","Ctrl+J",Action::Duplicate);item("Delete sprite...",nullptr,Action::Delete);item("Delete multiple sprites...",nullptr,Action::BatchDelete);item("Copy / move to another library...",nullptr,Action::LibraryCopy);if(tabs.size()<2&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("Open a second IMG library to copy sprites into it.");
            item("Split to new library...",nullptr,Action::SplitLibrary);
            ImGui::Separator();item("Copy sprites to clipboard...",nullptr,Action::CopySprites);item("Paste sprites from clipboard...",nullptr,Action::PasteSprites);if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){if(sprite_clipboard)ImGui::SetTooltip("%d sprites from %s. Available until replaced, cleared or Studio closes.",sprite_clipboard->artwork().count(),sprite_clipboard->label().c_str());else ImGui::SetTooltip("Copy sprites to the sprite clipboard first. Pixel Copy/Paste uses a separate clipboard.");}item("Clear sprite clipboard",nullptr,Action::ClearSpriteClipboard);
            ImGui::Separator();item("Move up",nullptr,Action::MoveUp);item("Move down",nullptr,Action::MoveDown);ImGui::Separator();
            item("Resize artwork...",nullptr,Action::Resize);item("Canvas size...",nullptr,Action::CanvasSize);item("Trim transparent bounds",nullptr,Action::Trim);item("Rotate / flip...",nullptr,Action::Transform);item("Batch geometry...",nullptr,Action::BatchGeometry);if(ImGui::MenuItem("Opacity gradient...",nullptr,false,enabled(Action::Resize)))show_opacity_gradient(*tab());if(ImGui::MenuItem("Sprite Ramp Gradient...",nullptr,false,enabled(Action::Resize)))show_sprite_ramp(*tab());item("Restore child sprites...",nullptr,Action::RestoreChildren);ImGui::Separator();item("Extract selection...",nullptr,Action::ExtractSelection);item("Split Selection to Overlay Frame...",nullptr,Action::SplitOverlay);item("Copy Selection to Overlay Frame...",nullptr,Action::CopyOverlay);item("Split body parts...",nullptr,Action::SplitBody);ImGui::Separator();item("Point table...",nullptr,Action::Points);ImGui::EndMenu();
        }
        if(ImGui::BeginMenu("Palette",tab()!=nullptr)){item("Manage palettes...",nullptr,Action::ManagePalettes);if(ImGui::MenuItem("Isolate selection colors...",nullptr,false,enabled(Action::ManagePalettes)&&tab()->selection.valid&&!tab()->alignment.enabled&&!tab()->embedded.enabled&&!tab()->flip&&!paste_mode))begin_selection_isolation(*tab());if(ImGui::MenuItem("Isolate selection across frames...",nullptr,false,enabled(Action::ManagePalettes)&&tab()->selection.valid&&!tab()->alignment.enabled&&!tab()->embedded.enabled&&!tab()->flip&&!paste_mode))begin_isolation_frames(*tab());if(ImGui::MenuItem("Remap Similar Regions...",nullptr,false,enabled(Action::ManagePalettes)&&tab()->selection.valid&&!tab()->alignment.enabled&&!tab()->embedded.enabled&&!tab()->flip&&!paste_mode&&tab()->color>0))begin_region_remap(*tab());item("Assign to multiple sprites...",nullptr,Action::BatchPalette);item("Transfer palette by ramps...",nullptr,Action::RampTransfer);item("Alternate costume...",nullptr,Action::Costume);item("Remove unused palettes...",nullptr,Action::PaletteCleanup);item("Merge duplicate palettes...",nullptr,Action::PaletteDuplicates);item("Assign palette to marked...","[",Action::PaletteAssignMarked);item("Assign palette to selected...","]",Action::PaletteAssign);item("Rename palette...",nullptr,Action::PaletteRename);item("Merge palettes...","*",Action::PaletteMerge);ImGui::EndMenu();}
        if(tab()&&tab()->embedded.enabled&&ImGui::BeginMenu("Animation")){auto &e=tab()->embedded;
            item("Copy to another library...",nullptr,Action::AnimationCopy);item("Split to new library...",nullptr,Action::SplitAnimations);item("Copy to animation clipboard...",nullptr,Action::CopyAnimations);animation_paste_item("Paste from animation clipboard...");item("Clear animation clipboard",nullptr,Action::ClearAnimationClipboard);ImGui::Separator();if(ImGui::MenuItem("New sequence"))e.command=EmbeddedCommand::NewSequence;if(ImGui::MenuItem("New script"))e.command=EmbeddedCommand::NewScript;ImGui::Separator();item("Rename animation","F2",Action::Rename);item("Duplicate entry","Ctrl+J",Action::Duplicate);ImGui::EndMenu();}
        if(ImGui::BeginMenu("View")){item("Fit artwork","Ctrl+0",Action::Fit);item("Zoom in","Ctrl++",Action::ZoomIn);item("Zoom out","Ctrl+-",Action::ZoomOut);if(tab()){ImGui::MenuItem("Pixel grid",nullptr,&tab()->grid);ImGui::MenuItem("Animation anchor",nullptr,&tab()->anchors);ImGui::Separator();if(ImGui::MenuItem("Stage reference..."))show_stage_dialog(*tab());if(ImGui::MenuItem("Link animation anchors..."))show_anchor_link(*tab());}ImGui::EndMenu();}
        if(ImGui::BeginMenu("Tools")){if(ImGui::MenuItem("ASM animations..."))show_asm();if(ImGui::MenuItem("Reactions..."))show_reactions();if(ImGui::MenuItem("Fatality source..."))show_fatality();if(ImGui::MenuItem("MK2 strike tables..."))show_strikes();if(ImGui::MenuItem("Strike associations..."))show_bindings();if(ImGui::MenuItem("Strike phase inspector..."))show_strike_phases();ImGui::EndMenu();}
        if(ImGui::BeginMenu("Help")){item("Using Studio / keyboard reference","H / F1",Action::Help);item("Diagnostics","F9",Action::Diagnostics);ImGui::EndMenu();}
        ImGui::EndMenuBar();
    }
    #include "studio_help_ui.inl"
    #include "studio_shortcut_actions.inl"
    #include "studio_layers_ui.inl"
    void shortcuts(){
        if(ImGui::GetIO().WantTextInput || ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel)){space_mark_doc=0;return;}
        const auto route=ImGuiInputFlags_RouteGlobal;
        if(layer_drag.active){if(ImGui::IsKeyPressed(ImGuiKey_Escape)||(ImGui::GetIO().KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_Z)))cancel_layer_drag();return;}
        // A paste owns transform/placement keys before global Help or frame navigation.
        if(paste_mode&&tab()&&!ImGui::GetIO().KeyCtrl&&!ImGui::GetIO().KeyAlt&&!ImGui::GetIO().KeyShift&&!ImGui::GetIO().KeySuper&&ImGui::IsKeyPressed(ImGuiKey_L)){drop_paste_layer();return;}
        if(paste_mode&&tab()){
            auto &io=ImGui::GetIO();bool plain=!io.KeyCtrl&&!io.KeyAlt&&!io.KeySuper;
            bool control=io.KeyCtrl&&!io.KeyAlt&&!io.KeyShift&&!io.KeySuper;
            bool intercept=(plain&&!io.KeyShift&&(ImGui::IsKeyPressed(ImGuiKey_H)||ImGui::IsKeyPressed(ImGuiKey_V)||ImGui::IsKeyPressed(ImGuiKey_Enter)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))||(plain&&(ImGui::IsKeyPressed(ImGuiKey_LeftArrow)||ImGui::IsKeyPressed(ImGuiKey_RightArrow)||ImGui::IsKeyPressed(ImGuiKey_UpArrow)||ImGui::IsKeyPressed(ImGuiKey_DownArrow)))||(control&&(ImGui::IsKeyPressed(ImGuiKey_T)||ImGui::IsKeyPressed(ImGuiKey_E)||ImGui::IsKeyPressed(ImGuiKey_J)));
            if(intercept){
                bool open_transform=control&&ImGui::IsKeyPressed(ImGuiKey_T);begin_clipboard_floating(*tab());refresh_floating();
                // First-use keys are already pressed before a floating preview can
                // register next-frame Shortcut routing, so consume this event here.
                if(floating.active&&!open_transform){auto &u=floating;
                    if(control&&ImGui::IsKeyPressed(ImGuiKey_J))duplicate_floating();
                    else if((control&&ImGui::IsKeyPressed(ImGuiKey_E))||ImGui::IsKeyPressed(ImGuiKey_Enter)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))apply_floating();
                    else if(ImGui::IsKeyPressed(ImGuiKey_H)){u.stage.flip_h=!u.stage.flip_h;u.dirty=true;}
                    else if(ImGui::IsKeyPressed(ImGuiKey_V)){u.stage.flip_v=!u.stage.flip_v;u.dirty=true;}
                    else{int step=io.KeyShift?10:1;int dx=(ImGui::IsKeyPressed(ImGuiKey_RightArrow)?step:0)-(ImGui::IsKeyPressed(ImGuiKey_LeftArrow)?step:0),dy=(ImGui::IsKeyPressed(ImGuiKey_DownArrow)?step:0)-(ImGui::IsKeyPressed(ImGuiKey_UpArrow)?step:0);u.stage.x=std::clamp(u.stage.x+dx,0,u.stage.canvas_w-u.stage.w);u.stage.y=std::clamp(u.stage.y+dy,0,u.stage.canvas_h-u.stage.h);u.dirty=true;}
                }return;
            }
        }
        for(const auto &k:shortcut_bindings)if(!(floating.active&&k.key==ImGuiKey_H&&k.action==Action::Help)&&(!k.artwork||(tab()&&!tab()->alignment.enabled&&!tab()->embedded.enabled))&&ImGui::Shortcut(k.key,route)){
            auto command=k.action;if(command==Action::Mark){if(enabled(command)&&tab()){space_mark_doc=tab()->uid;space_mark_sprite=tab()->selected_id;}continue;}if(command==Action::Clear&&k.key==ImGuiKey_Delete&&tab()&&!tab()->selection.valid)command=Action::Delete;action(command);
        }
        if(ImGui::IsKeyReleased(ImGuiKey_Space)&&space_mark_doc){if(tab()&&tab()->uid==space_mark_doc&&tab()->selected_id==space_mark_sprite)action(Action::Mark);space_mark_doc=0;}
        auto t=tab();if(!t)return;
        if(variant.active){if(ImGui::IsKeyPressed(ImGuiKey_Escape))cancel_variant();else if(ImGui::Shortcut(ImGuiKey_Enter,route)||ImGui::Shortcut(ImGuiKey_KeypadEnter,route))apply_variant();return;}
        if(paint_line.active){if(ImGui::IsKeyPressed(ImGuiKey_Escape))cancel_paint_line();return;}
        if(floating.active){floating_shortcuts(route);return;}
        if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_Tab,route)||ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiMod_Shift|ImGuiKey_Tab,route)){finish();active=(active+(ImGui::GetIO().KeyShift?(int)tabs.size()-1:1))%(int)tabs.size();select_uid=tabs[active]->uid;paste_mode=false;return;}
        if(t->embedded.enabled){
            if(ImGui::Shortcut(ImGuiKey_Tab,route))workspace(*t,false);
            if(ImGui::Shortcut(ImGuiKey_Space,route)||ImGui::Shortcut(ImGuiKey_K,route))embedded_toggle_play(*t);
            if(ImGui::Shortcut(ImGuiKey_LeftArrow,route)||ImGui::Shortcut(ImGuiKey_UpArrow,route))embedded_step(*t,-1);
            if(ImGui::Shortcut(ImGuiKey_RightArrow,route)||ImGui::Shortcut(ImGuiKey_DownArrow,route))embedded_step(*t,1);
            return;
        }
        if(t->project&&t->alignment.enabled){
            if(ImGui::Shortcut(ImGuiKey_Escape,route)){t->project->cancel();t->scene.dragging=false;}
            if(ImGui::Shortcut(ImGuiKey_Tab,route))workspace(*t,false);
            if(ImGui::Shortcut(ImGuiKey_Space,route)||ImGui::Shortcut(ImGuiKey_K,route))toggle_scene_playback(*t);
            if(ImGui::Shortcut(ImGuiKey_LeftArrow,route)||ImGui::Shortcut(ImGuiKey_UpArrow,route))scene_step(*t,-1);
            if(ImGui::Shortcut(ImGuiKey_RightArrow,route)||ImGui::Shortcut(ImGuiKey_DownArrow,route))scene_step(*t,1);
            return;
        }
        if(ImGui::IsKeyPressed(ImGuiKey_Escape)&&t->wheel.editing){auto mask=t->selection;t->art->cancel();t->wheel=WheelState{};synchronize(*t);auto im=t->art->sprite(t->selected);if(mask.fits(im->w,im->h))t->selection=std::move(mask);ImGui::ClearActiveID();status="Cancelled palette color edit.";return;}
        if(ImGui::IsKeyPressed(ImGuiKey_Escape)){t->art->cancel();t->alignment.dragging=false;stroke=false;t->selection=(clone.owner||filter.owner||repair.owner)?t->selection:select_drag?selection_base:PixelSelection{};clone={};filter={};repair={};select_drag=false;lasso_points.clear();anchor_drag=false;paste_mode=false;}
        if(ImGui::Shortcut(ImGuiKey_Tab,route))workspace(*t,!t->alignment.enabled);
        if(t->alignment.enabled){
            auto &v=t->alignment;
            if(ImGui::Shortcut(ImGuiKey_Space,route)||ImGui::Shortcut(ImGuiKey_K,route))toggle_playback(*t);
            if(ImGui::Shortcut(ImGuiKey_LeftArrow,route)||ImGui::Shortcut(ImGuiKey_UpArrow,route))select_frame(*t,v.player.frame? v.player.frame-1:(int)v.sequences[v.sequence].frames.size()-1);
            if(ImGui::Shortcut(ImGuiKey_RightArrow,route)||ImGui::Shortcut(ImGuiKey_DownArrow,route))select_frame(*t,(v.player.frame+1)%(int)v.sequences[v.sequence].frames.size());
            return;
        }
        if(layer_move_keys(*t))return;
        if(ImGui::Shortcut(ImGuiKey_K,route)){workspace(*t,true);toggle_playback(*t);return;}
        if(ImGui::Shortcut(ImGuiKey_LeftArrow,route)){select_frame(*t,t->alignment.player.frame? t->alignment.player.frame-1:(int)t->alignment.sequences[t->alignment.sequence].frames.size()-1);return;}
        if(ImGui::Shortcut(ImGuiKey_RightArrow,route)){select_frame(*t,(t->alignment.player.frame+1)%(int)t->alignment.sequences[t->alignment.sequence].frames.size());return;}
        if(ImGui::Shortcut(ImGuiKey_PageUp,route))select(std::max(0,t->selected-10));
        if(ImGui::Shortcut(ImGuiKey_PageDown,route))select(std::min(t->art->count()-1,t->selected+10));
        if(ImGui::Shortcut(ImGuiKey_UpArrow,route))select((t->selected+t->art->count()-1)%t->art->count());
        if(ImGui::Shortcut(ImGuiKey_DownArrow,route))select((t->selected+1)%t->art->count());
        const ImGuiKey arrows[]={ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_UpArrow,ImGuiKey_DownArrow};
        const int dx[]={-1,1,0,0},dy[]={0,0,-1,1};
        for(int i=0;i<4;++i)for(int fast=0;fast<2;++fast)if(ImGui::Shortcut(ImGuiMod_Alt|(fast?ImGuiMod_Shift:0)|arrows[i],route))nudge_selection(*t,dx[i]*(fast?10:1),dy[i]*(fast?10:1));
        if(t->flip)return;
        for(const auto &k:tool_bindings)if(ImGui::Shortcut(k.key,route)){finish();tool=k.tool;paste_mode=false;}
        if(tool==Tool::Pencil||tool==Tool::Erase||tool==Tool::Replace||tool==Tool::Line||tool==Tool::Clone||tool==Tool::Variant||tool==Tool::Blur||tool==Tool::Smudge||tool==Tool::Heal||tool==Tool::Defringe){if(ImGui::Shortcut(ImGuiKey_LeftBracket,route))t->radius=std::max(1,t->radius-1);if(ImGui::Shortcut(ImGuiKey_RightBracket,route))t->radius=std::min(16,t->radius+1);}
        else{if(ImGui::Shortcut(ImGuiKey_LeftBracket,route))action(Action::PaletteAssignMarked);if(ImGui::Shortcut(ImGuiKey_RightBracket,route))action(Action::PaletteAssign);}
        bool merge=ImGui::Shortcut(ImGuiKey_KeypadMultiply,route);for(ImWchar c:ImGui::GetIO().InputQueueCharacters)merge|=c=='*';if(merge)action(Action::PaletteMerge);
    }
    #include "studio_batch_ui.inl"
    #include "studio_batch_rename_ui.inl"
    #include "studio_batch_palette_ui.inl"
#include "studio_ramp_transfer_ui.inl"
#include "studio_costume_presets_ui.inl"
#include "studio_costume_ui.inl"
    void sprite_scope_summary(const SpriteScope &scope){int added=0;for(auto &row:scope.rows)added+=row.parent!=0;ImGui::Text("%d scope sprites + %d child pieces",(int)scope.rows.size()-added,added);if(scope.ambiguous){ImGui::TextWrapped("%d ambiguous child matches skipped (hover for names). Select them explicitly to include them.",scope.ambiguous);if(ImGui::IsItemHovered()){ImGui::BeginTooltip();for(size_t i=0;i<std::min(size_t(12),scope.skipped.size());++i)ImGui::TextUnformatted(scope.skipped[i].c_str());if(scope.skipped.size()>12)ImGui::Text("... and %d more",(int)scope.skipped.size()-12);ImGui::EndTooltip();}}}
    void sprite_scope_origin(const SpriteScopeRow &row){if(row.parent)ImGui::TextWrapped("Child of %s / added by the child-piece option",row.parent_name.c_str());}
    #include "studio_palette_transparency_ui.inl"
    #include "studio_palette_removal_ui.inl"
    #include "studio_palette_zero_ui.inl"
    #include "studio_palette_move_ui.inl"
    #include "studio_palette_isolation_ui.inl"
    #include "studio_isolation_frames_ui.inl"
    #include "studio_region_remap_ui.inl"
    #include "studio_palette_cleanup_ui.inl"
    #include "studio_batch_delete_ui.inl"
    #include "studio_library_copy_ui.inl"
    #include "studio_animation_copy_ui.inl"
    #include "studio_restore_ui.inl"
    #include "studio_lod_ui.inl"
    #include "studio_export_ui.inl"
    void library(Tab &t){
        if(t.embedded.enabled){embedded_library(t);return;}
        ImGui::TextUnformatted("SPRITES");ImGui::SameLine();ImGui::TextDisabled("%d",t.art->count());
        ImGui::SetNextItemWidth(-1);if(ImGui::InputTextWithHint("##search","Search sprites...",t.filter.InputBuf,sizeof t.filter.InputBuf))t.filter.Build();
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Search sprite names. Commas include alternatives; -name excludes.");
        if(!t.alignment.enabled){ImGui::BeginDisabled(!enabled(Action::BatchGeometry));if(ImGui::SmallButton("Batch..."))action(Action::BatchGeometry);batch.library_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();if(ImGui::SmallButton("Marks"))ImGui::OpenPopup("Batch marks");ImGui::SameLine();ImGui::TextDisabled("%d",mark_count(t));if(ImGui::BeginPopup("Batch marks")){if(ImGui::MenuItem("Mark shown sprites"))for(int i=0;i<t.art->count();++i)if(t.filter.PassFilter(t.art->sprite(i)->n_s))mark_sprite(t,t.art->sprite_id(i),true);if(ImGui::MenuItem("Clear all marks"))t.marks.clear();ImGui::EndPopup();}}
        else ImGui::TextDisabled("Up / Down to browse");ImGui::Separator();
        std::vector<int> visible;for(int i=0;i<t.art->count();++i)if(t.filter.PassFilter(t.art->sprite(i)->n_s))visible.push_back(i);
        ImGui::BeginChild("sprite-list",ImVec2(0,0),ImGuiChildFlags_None,panel_flags);
        if(t.reveal_selection){auto row=std::find(visible.begin(),visible.end(),t.selected);
            if(row!=visible.end()){float y=(float)(row-visible.begin())*65,scroll=ImGui::GetScrollY(),height=ImGui::GetWindowHeight();
                if(y<scroll||y+65>scroll+height)ImGui::SetScrollY(std::max(0.f,y-height*.5f));}
            t.reveal_selection=false;
        }
        ImGuiListClipper clip;clip.Begin((int)visible.size(),65);
        while(clip.Step())for(int j=clip.DisplayStart;j<clip.DisplayEnd;++j){int i=visible[j];auto im=t.art->sprite(i);ImGui::PushID(i);
            ImVec2 pos=ImGui::GetCursorScreenPos();float width=ImGui::GetContentRegionAvail().x;
            if(!t.alignment.enabled){bool value=marked(t,t.art->sprite_id(i));if(ImGui::Checkbox("##batch-mark",&value))mark_sprite(t,t.art->sprite_id(i),value);if(i==0)batch.mark_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;if(ImGui::IsItemHovered())ImGui::SetTooltip("Include this sprite in a batch. Marks do not edit the IMG or select canvas pixels.");pos.x+=26;width-=26;ImGui::SetCursorScreenPos(pos);}
            if(ImGui::Selectable("##row",t.selected==i,0,ImVec2(width,58)))select(i);
            if(ImGui::BeginPopupContextItem("sprite-actions")){
                auto queue=[&](const char *label,Action a){if(ImGui::MenuItem(label)){deferred_action=a;deferred_sprite=t.art->sprite_id(i);have_deferred=true;}};
                queue("Rename...",Action::Rename);queue("Duplicate",Action::Duplicate);queue("Resize artwork...",Action::Resize);queue("Canvas size...",Action::CanvasSize);queue("Trim transparent bounds",Action::Trim);queue("Rotate / flip...",Action::Transform);
                ImGui::Separator();queue("Move up",Action::MoveUp);queue("Move down",Action::MoveDown);queue("Export PNG...",Action::ExportPng);queue("Delete sprite...",Action::Delete);ImGui::EndPopup();
            }
            auto dl=ImGui::GetWindowDrawList();ImVec2 a=pos+ImVec2(5,5),b=a+ImVec2(48,48);
            dl->AddRectFilled(a,b,IM_COL32(20,23,29,255),3);
            float scale=std::min(44.f/im->w,44.f/im->h);ImVec2 size(im->w*scale,im->h*scale);auto tex=texture(t,i);
            if(tex)dl->AddImage((ImTextureID)(intptr_t)tex,a+(ImVec2(48,48)-size)*.5f,a+(ImVec2(48,48)+size)*.5f);
            dl->PushClipRect(pos,pos+ImVec2(width,58),true);
            dl->AddText(pos+ImVec2(63,10),ImGui::GetColorU32(ImGuiCol_Text),im->n_s);
            char info[70];snprintf(info,sizeof info,"%d x %d   |   %s",im->w,im->h,t.art->palette(im->palnum)->n_s);
            dl->AddText(pos+ImVec2(63,33),ImGui::GetColorU32(ImGuiCol_TextDisabled),info);dl->PopClipRect();
            if(t.selected==i)dl->AddRectFilled(pos,pos+ImVec2(3,58),ImGui::GetColorU32(ImGuiCol_CheckMark),1);
            ImGui::PopID();
        }
        if(visible.empty())ImGui::TextDisabled("No matching sprites.");
        ImGui::EndChild();
    }
    #include "studio_tools_ui.inl"
    #include "studio_selection_ui.inl"
    #include "studio_selection_transform_ui.inl"
    #include "studio_paint_ui.inl"
    #include "studio_clone_ui.inl"
    #include "studio_variant_ui.inl"
    #include "studio_filters_ui.inl"
    #include "studio_repair_ui.inl"
    void canvas(Tab &t,float height){
        ImGui::BeginChild("canvas",ImVec2(0,height),ImGuiChildFlags_Border,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        auto im=t.art->sprite(t.selected);auto io=ImGui::GetIO();auto pos=ImGui::GetCursorScreenPos();auto available=ImGui::GetContentRegionAvail();
        validate_clone_source(t);
        t.view_w=im->w;t.view_h=im->h;
        t.color=std::clamp(t.color,0,(int)t.art->palette(im->palnum)->numc-1);
        available.x=std::max(10.f,available.x);available.y=std::max(10.f,available.y);
        ImGui::InvisibleButton("##surface",available,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle|ImGuiButtonFlags_MouseButtonRight);
        bool hovered=ImGui::IsItemHovered(),held=ImGui::IsItemActive();
        float fit=std::max(1.f,std::floor(std::min((available.x-60)/im->w,(available.y-60)/im->h)));
        float z=t.zoom>0?t.zoom:std::min(16.f,fit);
        if(!layer_drag.active&&hovered && io.MouseWheel!=0){t.zoom=std::clamp(z*(io.MouseWheel>0?1.25f:.8f),1.f,64.f);z=t.zoom;}
        if(!layer_drag.active&&hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))t.pan+=io.MouseDelta;
        if(held && tool==Tool::Pan && !ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDragging(ImGuiMouseButton_Left))t.pan+=io.MouseDelta;
        bool space_down=ImGui::IsKeyDown(ImGuiKey_Space)&&!pixel_preview();
        if(held&&space_down&&ImGui::IsMouseDragging(ImGuiMouseButton_Left)){if(space_mark_doc)finish();space_mark_doc=0;t.pan+=io.MouseDelta;}

        ImVec2 size(im->w*z,im->h*z),origin=pos+(available-size)*.5f+t.pan;
        t.canvas_origin=origin;t.canvas_zoom=z;
        ImVec2 anchor=origin+ImVec2((short)im->anix*z,(short)im->aniy*z);
        if(t.flip)origin.x=anchor.x-anipoint_effective((short)im->anix,im->w,true,t.convention)*z;
        layer_drag_input(t,origin,z,hovered&&!space_down);
        floating_input(t,origin,z,hovered&&!space_down&&!layer_move_selected(t));
        paint_line_input(t,origin,z,hovered&&!space_down);
        variant_input(t,origin,z,hovered&&!space_down);
        auto dl=ImGui::GetWindowDrawList();dl->PushClipRect(pos,pos+available,true);
        dl->AddRectFilled(pos,pos+available,IM_COL32(18,21,27,255));
        ImVec2 ca=ImMax(pos,origin),cb=ImMin(pos+available,origin+size);
        for(float yy=ca.y;yy<cb.y;yy+=12)for(float xx=ca.x;xx<cb.x;xx+=12){int cellx=(int)std::floor((xx-origin.x)/12),celly=(int)std::floor((yy-origin.y)/12);dl->AddRectFilled(ImVec2(xx,yy),ImMin(ImVec2(xx+12,yy+12),cb),((cellx+celly)&1)?IM_COL32(41,44,51,255):IM_COL32(33,36,43,255));}
        auto canvas_tex=layer_drag.active&&layer_drag.texture?layer_drag.texture:variant.active&&variant.texture?variant.texture:paint_line.active&&paint_line.valid?paint_line.texture:floating.active&&floating.valid?floating.texture:texture(t,t.selected);
        if(canvas_tex)dl->AddImage((ImTextureID)(intptr_t)canvas_tex,origin,origin+size,ImVec2(t.flip?1.f:0.f,0),ImVec2(t.flip?0.f:1.f,1));
        dl->AddRect(origin,origin+size,IM_COL32(90,96,106,255));
        if(auto layer=img_layer(im);layer&&layer->visible&&!t.flip&&!floating.active){dl->PushClipRect(origin,origin+size,true);dl->AddRect(origin+ImVec2((layer_drag.active?layer_drag.x:layer->x)*z,(layer_drag.active?layer_drag.y:layer->y)*z),origin+ImVec2(((layer_drag.active?layer_drag.x:layer->x)+layer->w)*z,((layer_drag.active?layer_drag.y:layer->y)+layer->h)*z),IM_COL32(235,100,110,220),0,0,1.5f);dl->PopClipRect();}
        if(t.grid && z>=6){for(int x=0;x<=im->w;++x)if(origin.x+x*z>=pos.x&&origin.x+x*z<=pos.x+available.x)dl->AddLine(origin+ImVec2(x*z,0),origin+ImVec2(x*z,size.y),IM_COL32(10,12,16,90));for(int y=0;y<=im->h;++y)if(origin.y+y*z>=pos.y&&origin.y+y*z<=pos.y+available.y)dl->AddLine(origin+ImVec2(0,y*z),origin+ImVec2(size.x,y*z),IM_COL32(10,12,16,90));}
        if(t.anchors){dl->AddLine(anchor-ImVec2(12,0),anchor+ImVec2(12,0),IM_COL32(239,193,94,255),1.5f);dl->AddLine(anchor-ImVec2(0,12),anchor+ImVec2(0,12),IM_COL32(239,193,94,255),1.5f);dl->AddCircle(anchor,4,IM_COL32(239,193,94,255));}
        int x=(int)std::floor((io.MousePos.x-origin.x)/z),y=(int)std::floor((io.MousePos.y-origin.y)/z);
        bool inside=x>=0&&y>=0&&x<im->w&&y<im->h;
        bool on_anchor=!floating.active&&tool==Tool::Select&&t.anchors&&!t.flip&&!io.KeyShift&&!io.KeyAlt&&std::abs(io.MousePos.x-anchor.x)<12&&std::abs(io.MousePos.y-anchor.y)<12;
        if(hovered&&on_anchor&&!space_down&&!paste_mode&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
            finish();anchor_drag=true;anchor_drag_mouse=io.MousePos;anchor_start_x=(short)im->anix;anchor_start_y=(short)im->aniy;t.art->begin("Move animation anchor");
        }
        if(anchor_drag&&ImGui::IsMouseDown(ImGuiMouseButton_Left))t.art->anchor(t.selected,
            anchor_start_x+(int)std::round((io.MousePos.x-anchor_drag_mouse.x)/z),anchor_start_y+(int)std::round((io.MousePos.y-anchor_drag_mouse.y)/z));
        if(hovered&&on_anchor)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if(!variant.active&&hovered && inside && ImGui::IsMouseClicked(ImGuiMouseButton_Right))t.color=((unsigned char *)im->data_p)[y*((im->w+3)&~3)+(t.flip?im->w-1-x:x)];
        if(hovered && inside && !space_down && !t.flip && !anchor_drag && !layer_move_selected(t) && !floating.active && !paint_line.active && !variant.active && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
            if(paste_mode){t.art->paste(t.selected,clipboard,x,y);paste_mode=false;}
            else if(tool==Tool::Clone)start_clone(t,x,y,io.KeyAlt);
            else if(tool==Tool::Blur||tool==Tool::Smudge)start_filter(t,x,y);
            else if(tool==Tool::Heal||tool==Tool::Defringe)start_repair(t,x,y);
            else if(tool==Tool::Pick)t.color=((unsigned char *)im->data_p)[y*((im->w+3)&~3)+x];
            else if(tool==Tool::SmartErase)t.art->smart_erase(t.selected,x,y,t.match_colors,t.paint_tolerance,t.fill_connected,t.erase_cleanup,t.selection,error);
            else if(tool==Tool::Fill)t.art->fill_matching(t.selected,x,y,t.color,t.match_colors,t.paint_tolerance,t.fill_connected,&t.selection,error);
            else if(tool==Tool::Replace&&t.replace_all)t.art->fill_matching(t.selected,x,y,t.color,t.match_colors,t.paint_tolerance,false,&t.selection,error);
            else if(tool==Tool::Wand){finish();t.selection=combine_selection(t.selection,wand_selection(*im,*t.art->palette(im->palnum),x,y,t.wand_tolerance,t.wand_contiguous),selection_op(t),im->w,im->h);}
            else if(tool==Tool::Select||tool==Tool::Lasso){finish();select_drag=true;start_x=x;start_y=y;selection_base=t.selection;selection_operation=selection_op(t);lasso_points.clear();if(tool==Tool::Lasso)append_lasso(im->w,im->h,(io.MousePos.x-origin.x)/z,(io.MousePos.y-origin.y)/z);}
            else if(tool==Tool::Pencil||tool==Tool::Erase||tool==Tool::Replace){stroke=true;last_x=x;last_y=y;stroke_brush=paint_brush(t);if(tool==Tool::Replace)stroke_brush.source=((unsigned char *)im->data_p)[y*((im->w+3)&~3)+x];t.art->begin(tool==Tool::Replace?"Replace matching pixels":tool==Tool::Erase?"Erase pixels":"Paint pixels");}
        }
        if(stroke && (ImGui::IsMouseDown(ImGuiMouseButton_Left)||ImGui::IsMouseReleased(ImGuiMouseButton_Left)) && !t.flip){
            // Interpolate within the image; cap out-of-canvas coordinates so a
            // fast drag cannot turn into an unbounded loop.
            int nx=std::clamp(x,-32,(int)im->w+32),ny=std::clamp(y,-32,(int)im->h+32);
            if(repair.owner){if((nx!=last_x||ny!=last_y)&&t.art->repair_stroke(repair,nx,ny,false,error)<0){stroke=false;repair={};t.art->commit();}}
            else if(filter.owner){if((nx!=last_x||ny!=last_y)&&t.art->filter_stroke(filter,nx,ny,error)<0){stroke=false;filter={};t.art->commit();}}
            else if(clone.owner){if((nx!=last_x||ny!=last_y)&&t.art->clone_stroke(clone,last_x,last_y,nx,ny,error)<0){stroke=false;clone={};t.art->commit();}}
            else t.art->paint_stroke(t.selected,last_x,last_y,nx,ny,stroke_brush,&t.selection,error);
            last_x=nx;last_y=ny;
        }
        if(select_drag&&(ImGui::IsMouseDown(ImGuiMouseButton_Left)||ImGui::IsMouseReleased(ImGuiMouseButton_Left))){
            if(tool==Tool::Lasso){append_lasso(im->w,im->h,(io.MousePos.x-origin.x)/z,(io.MousePos.y-origin.y)/z);if(ImGui::IsMouseReleased(ImGuiMouseButton_Left))t.selection=combine_selection(selection_base,lasso_selection(im->w,im->h,lasso_points),selection_operation,im->w,im->h);}
            else{int xx=std::clamp(x,0,(int)im->w-1),yy=std::clamp(y,0,(int)im->h-1);t.selection=combine_selection(selection_base,rectangle_selection(im->w,im->h,std::min(start_x,xx),std::min(start_y,yy),std::abs(start_x-xx)+1,std::abs(start_y-yy)+1),selection_operation,im->w,im->h);}}
        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)){if(repair.owner){status="Repair finished. Undo restores the complete edit.";if(repair.unresolved)status+=" "+std::to_string(repair.unresolved)+" pixels stayed unchanged without usable surrounding colors.";}if(filter.owner)status="Filter stroke finished. Undo restores the complete edit.";if(clone.owner)status="Clone stroke finished. Alt-click chooses a new source.";if(stroke||anchor_drag)t.art->commit();stroke=false;clone={};filter={};repair={};select_drag=false;anchor_drag=false;lasso_points.clear();selection_base={};}
        if(floating.active)draw_floating(dl,origin,z,ca,cb);else if(t.selection.valid&&!t.flip&&!layer_move_selected(t))draw_selection(t.selection,dl,origin,z,ca,cb);
        if(paint_line.active){const auto &line=paint_line.review;dl->AddCircle(origin+ImVec2((line.x0+.5f)*z,(line.y0+.5f)*z),3,IM_COL32(239,193,94,255));dl->AddCircle(origin+ImVec2((line.x1+.5f)*z,(line.y1+.5f)*z),3,IM_COL32(239,193,94,255));}
        if(select_drag&&tool==Tool::Lasso){for(size_t i=1;i<lasso_points.size();++i)dl->AddLine(origin+ImVec2(lasso_points[i-1].x*z,lasso_points[i-1].y*z),origin+ImVec2(lasso_points[i].x*z,lasso_points[i].y*z),IM_COL32(236,218,162,255),1.5f);}
        if(paste_mode&&hovered&&!t.flip){paste_x=x;paste_y=y;ImVec2 a=origin+ImVec2(x*z,y*z),b=a+ImVec2(clipboard.w*z,clipboard.h*z);
            dl->PushClipRect(ImMax(pos,origin),ImMin(pos+available,origin+size),true);
            if(auto tex=paste_preview(t))dl->AddImage((ImTextureID)(intptr_t)tex,a,b);
            dl->PopClipRect();dl->AddRect(a,b,ImGui::GetColorU32(ImGuiCol_CheckMark),0,0,2);}
        if(hovered&&inside&&!t.flip&&(tool==Tool::Pencil||tool==Tool::Erase||tool==Tool::Replace||tool==Tool::Line||tool==Tool::Clone||tool==Tool::Variant||tool==Tool::Blur||tool==Tool::Smudge||tool==Tool::Heal||tool==Tool::Defringe)){auto center=origin+ImVec2((x+.5f)*z,(y+.5f)*z);float r=(t.radius-.5f)*z;if(t.brush_shape==BrushShape::Square||t.radius==1)dl->AddRect(center-ImVec2(r,r),center+ImVec2(r,r),IM_COL32(255,255,255,230));else dl->AddCircle(center,r,IM_COL32(255,255,255,230));}
        draw_clone_source(t,dl,origin,z,x,y,hovered&&inside);
        char info[96];snprintf(info,sizeof info,"%.0f%%   |   %d x %d pixels",z*100,im->w,im->h);dl->AddText(pos+ImVec2(10,8),IM_COL32(155,166,182,255),info);
        dl->PopClipRect();ImGui::EndChild();
    }
    void alignment_controls(Tab &t){
        auto &v=t.alignment;
        ImGui::SetNextItemWidth(std::max(110.f,ImGui::GetContentRegionAvail().x-155));
        if(ImGui::BeginCombo("##sequence",v.sequences[v.sequence].name.c_str())){
            for(int i=0;i<(int)v.sequences.size();++i){
                std::string label=v.sequences[i].name+"  ("+std::to_string(v.sequences[i].frames.size())+")";
                if(ImGui::Selectable(label.c_str(),i==v.sequence)){finish();v.sequence=i;select_frame(t,0);}
            }ImGui::EndCombo();
        }
        ImGui::SameLine();if(ImGui::Button("Fit"))action(Action::Fit);
        ImGui::SameLine();if(ImGui::Button("Feet to floor")){finish();feet_to_floor(t);}
        ImGui::TextDisabled("Name-based preview  |  drag artwork to align");
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Frames are inferred from sprite names, not game scripts. Anchor edits save to IMG; preview timing and placement stay in this session.");
    }
    void draw_world_pieces(Tab &t,const std::vector<int> &pieces,ImDrawList *dl,
                           ImVec2 stage,float zoom,int x,int y,bool flip,MirrorConvention convention,ImU32 tint){
        for(int i:pieces)if(auto im=t.art->sprite(i)){
            auto p=place_sprite(*im,x,y,flip,convention);
            ImVec2 a=stage+ImVec2(p.x*zoom,p.y*zoom),b=a+ImVec2(p.w*zoom,p.h*zoom);
            if(auto tex=texture(t,i))dl->AddImage((ImTextureID)(intptr_t)tex,a,b,ImVec2(flip?1.f:0.f,0),ImVec2(flip?0.f:1.f,1),tint);
        }
    }
    void alignment_canvas(Tab &t,float height){
        auto &v=t.alignment;auto frame=v.frame();if(!frame)return;
        ImGui::BeginChild("world-canvas",ImVec2(0,height),ImGuiChildFlags_Border,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        auto &io=ImGui::GetIO();ImVec2 pos=ImGui::GetCursorScreenPos(),available=ImGui::GetContentRegionAvail();
        available=ImMax(available,ImVec2(10,10));
        ImGui::InvisibleButton("##world-surface",available,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);
        bool hovered=ImGui::IsItemHovered();
        float fit=std::max(.25f,std::min((available.x-24)/v.world_w,(available.y-45)/v.world_h));
        float z=v.zoom>0?v.zoom:fit;
        if(hovered&&io.MouseWheel){v.zoom=std::clamp(z*(io.MouseWheel>0?1.25f:.8f),.25f,32.f);z=v.zoom;}
        if(hovered&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle))v.pan+=io.MouseDelta;
        ImVec2 stage=pos+(available-ImVec2(v.world_w*z,v.world_h*z))*.5f+v.pan;
        ImVec2 end=stage+ImVec2(v.world_w*z,v.world_h*z);
        auto dl=ImGui::GetWindowDrawList();dl->PushClipRect(pos,pos+available,true);
        dl->AddRectFilled(pos,pos+available,IM_COL32(18,21,27,255));
        dl->AddRectFilled(stage,end,IM_COL32(28,32,40,255));
        dl->PushClipRect(ImMax(pos,stage),ImMin(pos+available,end),true);
        draw_stage(t,dl,stage,z);
        for(int x=0;x<=v.world_w;x+=50)dl->AddLine(stage+ImVec2(x*z,0),stage+ImVec2(x*z,v.world_h*z),IM_COL32(49,55,65,130));
        for(int y=0;y<=v.world_h;y+=50)dl->AddLine(stage+ImVec2(0,y*z),stage+ImVec2(v.world_w*z,y*z),IM_COL32(49,55,65,130));
        ImVec2 anchor=stage+ImVec2(v.origin_x*z,v.origin_y*z);
        float floor=stage.y+v.floor_y*z;
        dl->AddLine(ImVec2(stage.x,floor),ImVec2(end.x,floor),IM_COL32(172,151,108,220));
        dl->AddLine(ImVec2(anchor.x,stage.y),ImVec2(anchor.x,end.y),IM_COL32(145,154,169,80));
        // References are display-only, including their placement and facing.
        if(auto reference=find(v.reference_doc)){int index=reference->art->sprite_index(v.reference_id);if(index>=0)
            draw_world_pieces(*reference,{index},dl,stage,z,v.origin_x+v.reference_x,v.origin_y+v.reference_y,v.reference_flip,t.convention,IM_COL32(180,195,220,150));}
        bool flip=v.facing==1;
        if(v.onion&&v.sequences[v.sequence].frames.size()>1){
            int prev=(v.player.frame+(int)v.sequences[v.sequence].frames.size()-1)%(int)v.sequences[v.sequence].frames.size();
            draw_world_pieces(t,preview_pieces(v.sequences[v.sequence].frames[prev],v.multipart),dl,stage,z,v.origin_x,v.origin_y,flip,t.convention,IM_COL32(167,191,221,65));
        }
        auto pieces=preview_pieces(*frame,v.multipart);
        if(v.facing==2)draw_world_pieces(t,pieces,dl,stage,z,v.origin_x,v.origin_y,true,t.convention,IM_COL32(255,152,163,100));
        draw_world_pieces(t,pieces,dl,stage,z,v.origin_x,v.origin_y,flip,t.convention,IM_COL32(255,255,255,255));
        // Indicate exactly which piece the inspector and drag affect.
        auto selected=t.art->sprite(t.selected);
        bool visible=std::find(pieces.begin(),pieces.end(),t.selected)!=pieces.end();
        if(gesture_smoke&&selected&&visible){auto p=place_sprite(*selected,v.origin_x,v.origin_y,flip,t.convention);gesture_zoom=z;
            bool found=false;for(int y=0;y<selected->h&&!found;++y)for(int x=0;x<selected->w;++x)
                if(((unsigned char *)selected->data_p)[y*((selected->w+3)&~3)+x]){
                    gesture_point=stage+ImVec2((p.x+(flip?selected->w-1-x:x)+1.5f)*z,(p.y+y+1.5f)*z);found=true;break;
                }
        }
        if(selected&&visible){auto p=place_sprite(*selected,v.origin_x,v.origin_y,flip,t.convention);
            dl->AddRect(stage+ImVec2(p.x*z,p.y*z),stage+ImVec2((p.x+p.w)*z,(p.y+p.h)*z),IM_COL32(226,86,104,190));
        }
        dl->AddLine(anchor-ImVec2(10,0),anchor+ImVec2(10,0),IM_COL32(239,193,94,255),1.5f);
        dl->AddLine(anchor-ImVec2(0,10),anchor+ImVec2(0,10),IM_COL32(239,193,94,255),1.5f);
        dl->PopClipRect();dl->AddRect(stage,end,IM_COL32(90,96,106,255));
        char label[160];snprintf(label,sizeof label,"WORLD  %d x %d  |  %.0f%%  |  %s",v.world_w,v.world_h,z*100,frame->name.c_str());
        dl->AddText(pos+ImVec2(10,6),IM_COL32(165,176,192,255),label);
        dl->PopClipRect();

        if(hovered&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
            float wx=(io.MousePos.x-stage.x)/z,wy=(io.MousePos.y-stage.y)/z;
            int hit=-1;
            // Last visible piece wins, following draw order; ignore transparent pixels.
            for(auto it=pieces.rbegin();it!=pieces.rend();++it)if(auto im=t.art->sprite(*it)){
                auto p=place_sprite(*im,v.origin_x,v.origin_y,flip,t.convention);
                int x=(int)std::floor(wx-p.x),y=(int)std::floor(wy-p.y);
                if(x>=0&&y>=0&&x<im->w&&y<im->h&&((unsigned char *)im->data_p)[y*((im->w+3)&~3)+(flip?im->w-1-x:x)]){hit=*it;break;}
            }
            if(hit>=0){finish();t.selected=hit;t.selected_id=t.art->sprite_id(hit);t.art->select_sprite(t.selected_id);t.reveal_selection=true;v.player.playing=false;v.dragging=true;v.drag_mouse=io.MousePos;v.drag_dx=v.drag_dy=0;
                v.drag_pieces=v.move_frame?pieces:std::vector<int>{hit};t.art->begin(v.move_frame?"Align multipart frame":"Align sprite in world");}
        }
        if(v.dragging&&ImGui::IsMouseDown(ImGuiMouseButton_Left)){
            int dx=(int)std::round((io.MousePos.x-v.drag_mouse.x)/z),dy=(int)std::round((io.MousePos.y-v.drag_mouse.y)/z);
            // A flipped placement responds to X-anchor changes in the opposite direction.
            if(dx!=v.drag_dx||dy!=v.drag_dy)shift_frame_anchors(*t.art,v.drag_pieces,(flip?1:-1)*(dx-v.drag_dx),-(dy-v.drag_dy));
            v.drag_dx=dx;v.drag_dy=dy;
        }
        if(v.dragging&&ImGui::IsMouseReleased(ImGuiMouseButton_Left)){t.art->commit();v.dragging=false;status="Aligned artwork. Anchor changes save to this IMG.";}
        if(hovered)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        ImGui::EndChild();
    }
    void animation_timeline(Tab &t,float height){
        auto &v=t.alignment;auto &sequence=v.sequences[v.sequence];
        ImGui::BeginChild("timeline",ImVec2(0,height),ImGuiChildFlags_Border);
        if(ImGui::Button(v.player.playing?"Pause":"Play"))toggle_playback(t);
        ImGui::SameLine();if(ImGui::Button("<"))select_frame(t,(v.player.frame+(int)sequence.frames.size()-1)%(int)sequence.frames.size());
        ImGui::SameLine();if(ImGui::Button(">"))select_frame(t,(v.player.frame+1)%(int)sequence.frames.size());
        ImGui::SameLine();ImGui::Checkbox("Loop",&v.player.loop);
        ImGui::SameLine();ImGui::TextDisabled("%d / %d",v.player.frame+1,(int)sequence.frames.size());
        if(ImGui::GetContentRegionAvail().x>100){ImGui::SameLine();ImGui::SetNextItemWidth(-1);int seek=v.player.frame+1;
            if(ImGui::SliderInt("##scrub",&seek,1,(int)sequence.frames.size(),"Frame %d"))select_frame(t,seek-1);}
        int hold=v.holds[v.sequence][v.player.frame];ImGui::SetNextItemWidth(95);
        if(ImGui::InputInt("Ticks/frame",&hold)){v.player.playing=false;v.holds[v.sequence][v.player.frame]=std::clamp(hold,1,120);v.player.elapsed_ticks=0;}
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Preview duration for this frame. Project > Create from current preview keeps this timing in a Studio project.");
        ImGui::SameLine();ImGui::TextDisabled("%.1f Hz preview",v.tick_hz);
        ImGui::BeginChild("frame-strip",ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
        for(int f=0;f<(int)sequence.frames.size();++f){if(f)ImGui::SameLine(0,6);ImGui::PushID(f);
            auto &item=sequence.frames[f];ImVec2 a=ImGui::GetCursorScreenPos();
            if(ImGui::Selectable("##frame",v.player.frame==f,0,ImVec2(85,85)))select_frame(t,f);
            auto dl=ImGui::GetWindowDrawList();auto pieces=preview_pieces(item,v.multipart);
            int left=32767,top=32767,right=-32768,bottom=-32768;
            for(int i:pieces)if(auto im=t.art->sprite(i)){auto p=place_sprite(*im,0,0,false,t.convention);left=std::min(left,p.x);top=std::min(top,p.y);right=std::max(right,p.x+p.w);bottom=std::max(bottom,p.y+p.h);}
            float z=std::min(68.f/std::max(1,right-left),42.f/std::max(1,bottom-top));
            ImVec2 origin=a+ImVec2((85-(right-left)*z)*.5f,(48-(bottom-top)*z)*.5f)-ImVec2(left*z,top*z);
            dl->PushClipRect(a,a+ImVec2(85,85),true);
            draw_world_pieces(t,pieces,dl,origin,z,0,0,false,t.convention,IM_COL32(255,255,255,255));
            dl->AddText(a+ImVec2(5,49),ImGui::GetColorU32(ImGuiCol_Text),item.name.c_str());
            char ticks[24];snprintf(ticks,sizeof ticks,"%d ticks",v.holds[v.sequence][f]);
            dl->AddText(a+ImVec2(5,67),ImGui::GetColorU32(ImGuiCol_TextDisabled),ticks);dl->PopClipRect();
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s | %d piece(s) | %d preview ticks",item.name.c_str(),(int)pieces.size(),v.holds[v.sequence][f]);
            ImGui::PopID();
        }ImGui::EndChild();ImGui::EndChild();
    }
    void alignment_inspector(Tab &t){
        auto &v=t.alignment;auto frame=v.frame();if(!frame)return;
        ImGui::SeparatorText("Frame preview");
        if(ImGui::Button("Stage reference..."))show_stage_dialog(t);
        if(ImGui::Button("Link animation anchors..."))show_anchor_link(t);
        if(ImGui::Checkbox("Reconstruct subframes",&v.multipart))select_frame(t,v.player.frame);
        ImGui::Checkbox("Previous-frame onion skin",&v.onion);
        const char *facing[]={"Normal","Flipped","Both facings (ghost)"};ImGui::SetNextItemWidth(-1);
        if(ImGui::Combo("##facing",&v.facing,facing,3))finish();
        if(!frame->pieces.empty()){
            ImGui::TextDisabled("%d subframe pieces",(int)frame->pieces.size());
            if(v.multipart)for(int i:frame->pieces){auto im=t.art->sprite(i);if(ImGui::Selectable(im->n_s,i==t.selected)){finish();v.player.playing=false;t.selected=i;t.selected_id=t.art->sprite_id(i);t.art->select_sprite(t.selected_id);t.reveal_selection=true;}}
        }
        ImGui::Checkbox("Drag the whole frame",&v.move_frame);
        ImGui::TextWrapped("Drag artwork to change its IMG anchor. Escape cancels; undo restores the drag.");
        if(ImGui::CollapsingHeader("Playfield & timing")){
            int size[]={v.world_w,v.world_h};ImGui::SetNextItemWidth(-1);
            if(ImGui::InputInt2("##world-size",size)){v.world_w=std::clamp(size[0],64,2048);v.world_h=std::clamp(size[1],64,2048);}
            ImGui::TextDisabled("Width / Height (preview pixels)");
            int origin[]={v.origin_x,v.origin_y};ImGui::SetNextItemWidth(-1);
            if(ImGui::InputInt2("##world-origin",origin)){v.origin_x=std::clamp(origin[0],-4096,4096);v.origin_y=std::clamp(origin[1],-4096,4096);}
            ImGui::TextDisabled("Anchor target X / Y");ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##floor",&v.floor_y,0,v.world_h,"Floor: %d");
            const char *profiles[]={"MK2 (54.7068 Hz)","60 Hz preview"};int profile=v.tick_hz==mk2_tick_hz?0:1;
            ImGui::SetNextItemWidth(-1);if(ImGui::Combo("##clock",&profile,profiles,2)){v.tick_hz=profile?60:mk2_tick_hz;v.player.elapsed_ticks=0;}
            ImGui::TextWrapped("Groups and timing are preview settings. Embedded IMG scripts remain intact.");
        }
        if(ImGui::CollapsingHeader("Reference actor")){
            auto reference=find(v.reference_doc);
            std::string reference_name=reference?reference->name():"None";
            ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##ref-library",reference_name.c_str())){
                if(ImGui::Selectable("None",!reference))v.reference_doc=0;
                for(auto &candidate:tabs)if(ImGui::Selectable((candidate->name()+"###ref"+std::to_string(candidate->uid)).c_str(),candidate->uid==v.reference_doc)){
                    v.reference_doc=candidate->uid;v.reference_sprite=candidate->selected;v.reference_id=candidate->art->sprite_id(v.reference_sprite);
                    auto im=candidate->art->sprite(v.reference_sprite);v.reference_y=v.floor_y-v.origin_y-((int)im->h-(short)im->aniy);
                }ImGui::EndCombo();
            }
            if((reference=find(v.reference_doc))){
                v.reference_sprite=reference->art->sprite_index(v.reference_id);
                auto im=reference->art->sprite(v.reference_sprite);ImGui::SetNextItemWidth(-1);
                if(ImGui::BeginCombo("##ref-sprite",im?im->n_s:"Choose sprite")){
                    for(int i=0;i<reference->art->count();++i)if(ImGui::Selectable((std::string(reference->art->sprite(i)->n_s)+"###ref-sprite"+std::to_string(i)).c_str(),i==v.reference_sprite)){v.reference_sprite=i;v.reference_id=reference->art->sprite_id(i);}
                    ImGui::EndCombo();
                }
                int xy[]={v.reference_x,v.reference_y};ImGui::SetNextItemWidth(-1);
                if(ImGui::InputInt2("##ref-position",xy)){v.reference_x=std::clamp(xy[0],-4096,4096);v.reference_y=std::clamp(xy[1],-4096,4096);}
                ImGui::TextDisabled("Offset X / Y from actor");ImGui::Checkbox("Flip reference",&v.reference_flip);
                if(ImGui::Button("Reference feet to floor"))if((im=reference->art->sprite(v.reference_sprite)))v.reference_y=v.floor_y-v.origin_y-((int)im->h-(short)im->aniy);
            }
            ImGui::TextWrapped("Reference placement is preview-only. Closing its library removes it from view.");
        }
    }
    void palette_strip(Tab &t){
        auto im=t.art->sprite(t.selected);auto p=t.art->palette(im->palnum);
        ImGui::TextUnformatted("PALETTE");ImGui::SameLine();ImGui::TextDisabled("%s  /  %d colors",p->n_s,p->numc);
        ImGui::SameLine();if(ImGui::SmallButton("Manage..."))open_palettes(t);
        ImGui::BeginChild("swatches",ImVec2(0,0),ImGuiChildFlags_None);
        int columns=std::max(1,(int)(ImGui::GetContentRegionAvail().x/27));
        for(int i=0;i<p->numc;++i){if(i%columns)ImGui::SameLine(0,3);ImGui::PushID(i);
            unsigned char r,g,b;pal_word_to_rgb8((unsigned char *)p->data_p+i*2,&r,&g,&b);
            ImVec2 a=ImGui::GetCursorScreenPos();if(ImGui::ColorButton("##swatch",ImVec4(r/255.f,g/255.f,b/255.f,i?1.f:0.f),ImGuiColorEditFlags_NoTooltip,ImVec2(24,24))){if(t.wheel.editing)finish();else t.art->commit();t.color=i;}
            if(t.color==i)ImGui::GetWindowDrawList()->AddRect(a-ImVec2(1,1),a+ImVec2(25,25),IM_COL32(255,235,210,255),2,0,2);
            if(ImGui::IsItemHovered())ImGui::SetTooltip(i?"Index %d  |  RGB %d, %d, %d":"Index 0  |  Transparent",i,r,g,b);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    void inspector(Tab &t){
        if(t.embedded.enabled){embedded_inspector(t);return;}
        if(t.project&&t.alignment.enabled){project_inspector(t);return;}
        auto im=t.art->sprite(t.selected);auto p=t.art->palette(im->palnum);
        ImGui::TextUnformatted("SPRITE INSPECTOR");ImGui::Separator();ImGui::TextUnformatted(im->n_s);
        ImGui::TextDisabled("%d x %d pixels  |  %d of %d",im->w,im->h,t.selected+1,t.art->count());
        if(layer_drag.active){ImGui::SeparatorText("Move overlay preview");ImGui::Text("X: %d / Y: %d",layer_drag.x,layer_drag.y);ImGui::TextWrapped("Release to apply one Undo step. Hold Shift to lock an axis. Escape or Undo cancels. Artwork and anchors stay unchanged during this preview.");return;}
        if(variant.active){variant_inspector(t);return;}
        if(paint_line.active){ImGui::SeparatorText("Line preview");ImGui::Text("%d pixels change",paint_line.changed);ImGui::TextWrapped("Release to draw the line. Shift snaps horizontal, vertical or diagonal. Escape or Undo cancels without changing artwork.");return;}
        if(floating.active){floating_inspector(t);return;}
        layer_inspector(t);
        if(!t.alignment.enabled){
            ImGui::Spacing();if(ImGui::Button("Rename"))request_edit(EditDialog::Rename);ImGui::SameLine();if(ImGui::Button("Resize"))request_edit(EditDialog::Resize);ImGui::SameLine();if(ImGui::Button("More..."))ImGui::OpenPopup("sprite-more");
            if(ImGui::BeginPopup("sprite-more")){
                auto queue=[&](const char *label,Action a){if(ImGui::MenuItem(label,nullptr,false,enabled(a))){deferred_action=a;deferred_sprite=t.selected_id;have_deferred=true;}};
                queue("New sprite...",Action::NewSprite);queue("Duplicate",Action::Duplicate);queue("Canvas size...",Action::CanvasSize);queue("Trim bounds",Action::Trim);if(ImGui::MenuItem("Opacity gradient...",nullptr,false,enabled(Action::Resize)))show_opacity_gradient(t);if(ImGui::MenuItem("Sprite Ramp Gradient...",nullptr,false,enabled(Action::Resize)))show_sprite_ramp(t);queue("Rotate / flip...",Action::Transform);queue("Split body parts...",Action::SplitBody);queue("Export PNG...",Action::ExportPng);queue("Delete...",Action::Delete);ImGui::EndPopup();}
        }
        ImGui::Spacing();if(ImGui::Button("Point table..."))show_points(t);
        ImGui::SeparatorText("Animation anchor");
        int xy[]={(short)im->anix,(short)im->aniy};ImGui::SetNextItemWidth(-1);
        if(ImGui::InputInt2("##anchor",xy)){t.alignment.player.playing=false;t.art->anchor(t.selected,xy[0],xy[1]);}if(ImGui::IsItemDeactivatedAfterEdit())t.art->commit();
        ImGui::TextDisabled("X / Y  |  this sprite only");
        if(!t.alignment.enabled){ImGui::Checkbox("Show anchor",&t.anchors);ImGui::Checkbox("Flipped preview",&t.flip);}
        const char *conventions[]={"Multipart (ani2)","Single part (ganiof)"};int conv=(int)t.convention;ImGui::SetNextItemWidth(-1);
        if(ImGui::Combo("##mirror",&conv,conventions,2))t.convention=(MirrorConvention)conv;
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Preview uses the selected engine's anchor convention. This does not change the IMG.");
        if(t.alignment.enabled){alignment_inspector(t);return;}
        ImGui::Spacing();ImGui::SeparatorText("Active tool");
        if(tool==Tool::Heal||tool==Tool::Defringe)repair_controls(t);
        else if(tool==Tool::Blur||tool==Tool::Smudge)filter_controls(t);
        else if(tool==Tool::Variant)variant_controls(t);
        else if(tool==Tool::Clone)clone_controls(t);
        else if(tool==Tool::Pencil||tool==Tool::Erase||tool==Tool::Replace||tool==Tool::Line)paint_brush_controls(t);
        else if(tool==Tool::Select)ImGui::TextWrapped("Drag a rectangle. Copy with Ctrl+C; Delete clears its pixels.");
        else if(tool==Tool::Lasso)ImGui::TextWrapped("Drag around the pixels to select, then release to close the shape.");
        else if(tool==Tool::Move)ImGui::TextWrapped(layer_move_selected(t)?(layer_move_target(t)?"Move targets the overlay. Drag and release to apply; Shift locks an axis. Choose Base selection above to move selected base pixels.":"The overlay is hidden. Show it to drag, or choose Base selection above to move base pixels."):"Drag selected base pixels to preview a move. Enter applies; Escape cancels. An existing overlay stays in place.");
        else if(tool==Tool::Wand)ImGui::TextWrapped("Click a color to select matching pixels.");
        else if(tool==Tool::Fill||tool==Tool::SmartErase)paint_match_controls(t);
        else if(tool==Tool::Pick)ImGui::TextWrapped("Click to pick a color. Right-click also picks with any tool.");
        else ImGui::TextWrapped("Drag to pan. The mouse wheel zooms.");
        ImGui::Checkbox("Pixel grid",&t.grid);
        selection_controls(t);
        ImGui::SeparatorText("Sprite palette");ImGui::SetNextItemWidth(-1);
        if(ImGui::BeginCombo("##sprite-palette",p->n_s)){
            for(int i=0;i<t.art->palette_count();++i)if(ImGui::Selectable((std::string(t.art->palette(i)->n_s)+"###pal"+std::to_string(i)).c_str(),i==im->palnum)){
                finish();t.art->assign_palette(t.selected,i,t.match_palette,error);
            }ImGui::EndCombo();
        }
        ImGui::Checkbox("Match existing colors",&t.match_palette);
        if(ImGui::IsItemHovered())ImGui::SetTooltip("On: remap pixels to their nearest colors. Off: keep pixel indices to recolor the sprite.");
        if(ImGui::Button("Make palette private")){finish();t.art->duplicate_palette(im->palnum,error,t.selected);}
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Copies and assigns this palette to just this sprite, in one undo step.");
        p=t.art->palette(im->palnum);
        ImGui::Spacing();
        if(ImGui::CollapsingHeader("Packing checks")){
            ImGui::SetNextItemWidth(-1);ImGui::SliderInt("##ppp",&t.ppp,0,8,"PPP: %d (0 = auto)");
            if(!stroke&&(t.check_revision!=t.art->generation()||t.checked_ppp!=t.ppp)){t.report=t.art->check(t.ppp);t.check_revision=t.art->generation();t.checked_ppp=t.ppp;}
            ImGui::TextWrapped("%d packing changes / %d warnings",t.report.break_count,t.report.warn_count);
            ImGui::TextDisabled("Compared with the loaded IMG");
            for(auto &issue:t.report.issues){ImGui::PushID(&issue);if(ImGui::Selectable(issue.img_name.c_str()))select(issue.img_idx);ImGui::TextWrapped("%s",issue.message.c_str());ImGui::PopID();}
        }
    }
    #include "studio_welcome_ui.inl"
    #include "studio_subframe_swap_ui.inl"
    #include "studio_waterline_ui.inl"
    #include "studio_dummy_body_ui.inl"
    #include "studio_overlay_frame_ui.inl"
    #include "studio_sprite_ramp_ui.inl"
    #include "studio_opacity_gradient_ui.inl"
    #include "studio_embedded_metadata_ui.inl"
    void editing_dialog(){
        if(edit_dialog==EditDialog::SubframeSwap){subframe_swap_dialog();return;}
        if(edit_dialog==EditDialog::Waterline){waterline_dialog();return;}
        if(edit_dialog==EditDialog::DummyBody){dummy_body_dialog();return;}
        if(edit_dialog==EditDialog::OverlayFrame){overlay_frame_dialog();return;}
        if(edit_dialog==EditDialog::SpriteRamp){sprite_ramp_dialog();return;}
        if(edit_dialog==EditDialog::OpacityGradient){opacity_gradient_dialog();return;}
        if(edit_dialog==EditDialog::EmbeddedMetadata){embedded_metadata_dialog();return;}
        if(edit_dialog!=EditDialog::None)ImGui::OpenPopup("Edit sprite");
        ImGui::SetNextWindowSizeConstraints(ImVec2(480,0),ImVec2(620,900));
        if(!ImGui::BeginPopupModal("Edit sprite",nullptr,ImGuiWindowFlags_AlwaysAutoResize))return;
        auto target=find(edit_doc);int index=target?target->art->sprite_index(edit_sprite):-1;
        auto im=target?target->art->sprite(index):nullptr;
        const char *titles[]={"","Animation metadata","New sprite","Rename sprite","Delete sprite","Resize artwork","Canvas size","Rotate / flip artwork","Import PNG sprite","Export PNG sprite","Sprite opacity gradient","Sprite Ramp Gradient","Selection to Overlay Frame","Swap with subframes","Cut at waterline","Build dummy body"};
        ImGui::TextUnformatted(titles[(int)edit_dialog]);ImGui::Separator();
        if(im)ImGui::TextDisabled("%s / %s",target->name().c_str(),im->n_s);
        bool name=edit_dialog==EditDialog::NewSprite||edit_dialog==EditDialog::Rename;
        if(name){if(ImGui::IsWindowAppearing())ImGui::SetKeyboardFocusHere();ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##sprite-name","IMG name (max 15 characters)",edit_name,sizeof edit_name,ImGuiInputTextFlags_AutoSelectAll);}
        bool size=edit_dialog==EditDialog::NewSprite||edit_dialog==EditDialog::Resize||edit_dialog==EditDialog::CanvasSize;
        if(size){
            ImGui::SetNextItemWidth(170);if(ImGui::InputInt("Width",&edit_w)){edit_w=std::clamp(edit_w,1,8192);if(edit_dialog==EditDialog::Resize&&edit_ratio)edit_h=std::clamp((int)std::round(edit_w/edit_aspect),1,8192);}
            ImGui::SetNextItemWidth(170);if(ImGui::InputInt("Height",&edit_h)){edit_h=std::clamp(edit_h,1,8192);if(edit_dialog==EditDialog::Resize&&edit_ratio)edit_w=std::clamp((int)std::round(edit_h*edit_aspect),1,8192);}
        }
        if(edit_dialog==EditDialog::Resize){ImGui::Checkbox("Keep proportions",&edit_ratio);ImGui::Checkbox("Smooth sampling (remap to palette)",&edit_quality);ImGui::Checkbox("Scale anchors and point coordinates",&edit_anchors);}
        if(edit_dialog==EditDialog::CanvasSize){
            ImGui::TextUnformatted("Keep art anchored at:");const char *labels[]={"Top left","Top","Top right","Left","Center","Right","Bottom left","Bottom","Bottom right"};
            for(int i=0;i<9;++i){if(i%3)ImGui::SameLine();bool selected=edit_placement==i;if(selected)ImGui::PushStyleColor(ImGuiCol_Button,accent_surface(.5f));
                if(ImGui::Button(labels[i],ImVec2(135,28)))edit_placement=i;if(selected)ImGui::PopStyleColor();}
            ImGui::TextWrapped("Artwork keeps its size. Cropped edges are discarded; anchors and points move with the content.");
        }
        if(edit_dialog==EditDialog::Transform){
            const char *ops[]={"Flip horizontal","Flip vertical","Rotate 90 clockwise","Rotate 90 counterclockwise","Rotate 180"};ImGui::SetNextItemWidth(-1);
            if(ImGui::Combo("##transform",&edit_transform,ops,5))edit_anchors=edit_transform!=2&&edit_transform!=3;
            ImGui::Checkbox("Transform anchors and point coordinates",&edit_anchors);
            ImGui::TextWrapped("Anchor transforms use classic IMG edge coordinates. Quarter-turns keep anchors by default.");
        }
        if(edit_dialog==EditDialog::ImportPng||edit_dialog==EditDialog::ExportPng){ImGui::SetNextItemWidth(-1);ImGui::InputText("##png-path",edit_path,sizeof edit_path);}
        if(edit_dialog==EditDialog::ImportPng){ImGui::Checkbox("Match an existing palette",&import_match);ImGui::TextWrapped(import_match?"Maps opaque pixels to the closest palette colors; transparent pixels stay transparent.":"Creates a palette with up to 255 opaque RGB555 colors plus transparency, using the classic median-cut quantizer.");}
        if((edit_dialog==EditDialog::NewSprite||(edit_dialog==EditDialog::ImportPng&&import_match))&&target){
            edit_palette=std::clamp(edit_palette,0,target->art->palette_count()-1);ImGui::SetNextItemWidth(-1);
            if(ImGui::BeginCombo("##new-palette",target->art->palette(edit_palette)->n_s)){
                for(int i=0;i<target->art->palette_count();++i)if(ImGui::Selectable((std::string(target->art->palette(i)->n_s)+"###new-pal"+std::to_string(i)).c_str(),i==edit_palette))edit_palette=i;ImGui::EndCombo();}
        }
        if(edit_dialog==EditDialog::Delete)ImGui::TextWrapped("Delete this sprite? Undo restores its artwork and metadata. Sprites used by embedded animations must have those references removed first.");
        if(edit_dialog==EditDialog::Resize||edit_dialog==EditDialog::CanvasSize||edit_dialog==EditDialog::Transform)ImGui::TextWrapped("Changes this sprite only. Geometry or silhouette changes can affect LOAD2 packing; check the report before exporting to a game build.");
        if(!edit_error.empty()){ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+440);ImGui::TextColored(accent_surface(1),"%s",edit_error.c_str());ImGui::PopTextWrapPos();}
        ImGui::Spacing();bool applied=false;
        bool apply_pressed=ImGui::Button(edit_dialog==EditDialog::Delete?"Delete":edit_dialog==EditDialog::ExportPng?"Export":"Apply",ImVec2(110,0));
        edit_apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
        if(apply_pressed&&target){
            int added=-1;switch(edit_dialog){
            case EditDialog::NewSprite:added=target->art->add_sprite(edit_name,edit_w,edit_h,edit_palette,edit_error);applied=added>=0;break;
            case EditDialog::Rename:applied=target->art->rename_sprite(index,edit_name,edit_error);break;
            case EditDialog::Delete:applied=target->art->delete_sprite(index,edit_error);break;
            case EditDialog::Resize:applied=target->art->resize_sprite(index,edit_w,edit_h,edit_quality,edit_anchors,edit_error);break;
            case EditDialog::CanvasSize:applied=target->art->canvas_size(index,edit_w,edit_h,edit_placement,edit_error);break;
            case EditDialog::Transform:applied=target->art->transform_sprite(index,(Transform)edit_transform,edit_anchors,edit_error);break;
            case EditDialog::ImportPng:added=target->art->import_png(edit_path,import_match?edit_palette:-1,edit_error);applied=added>=0;break;
            case EditDialog::ExportPng:
#ifndef _WIN32
                if(fs::exists(fs::u8path(edit_path)))edit_error="Choose a new PNG filename to avoid overwriting an existing file.";else
#endif
                applied=target->art->export_png(index,edit_path,edit_error);break;
            case EditDialog::None:break;
            }
            if(added>=0){target->selected=added;target->selected_id=target->art->sprite_id(added);target->art->select_sprite(target->selected_id);target->alignment.enabled=false;}
            if(applied){status="Completed "+std::string(titles[(int)edit_dialog]);edit_dialog=EditDialog::None;ImGui::CloseCurrentPopup();}
        }
        ImGui::SameLine();if(ImGui::Button("Cancel",ImVec2(110,0))){edit_dialog=EditDialog::None;edit_error.clear();ImGui::CloseCurrentPopup();}edit_cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
        ImGui::EndPopup();
    }
    void dialogs(){
        if(animation_copy.open){animation_copy_dialog();return;}
        if(cleanup.open){palette_cleanup_dialog();return;}
        if(renaming.open){batch_rename_dialog();return;}
        if(costume.open){costume_dialog();return;}
        if(ramp_transfer.open){ramp_transfer_dialog();return;}
        if(batch_palette.open){batch_palette_dialog();return;}
        if(deleting.open){batch_delete_dialog();return;}
        if(library_copy.open){library_copy_dialog();return;}
        if(fatality.open||fatality.saving||fatality.guard){fatality_dialog();return;}
        if(bindings_dialogs())return;
        if(strike_phases.open){strike_phases_dialog();return;}
        strikes_dialog();
        asm_dialog();
        stage_dialog();
        anchor_link_dialog();
        points_dialog();
        table_compare_dialog();
        export_dialog();
        lod_dialog();
        restore_dialog();
        batch_dialog();
        region_dialog();
        video_dialog();
        digitize_dialog();
        media_import_dialog();
        indexed_media_dialog();
        sheet_dialog();
        palette_dialog();
        project_dialogs();
        editing_dialog();
        if(!error.empty()&&!close_uid)ImGui::OpenPopup("Could not complete action");
        if(ImGui::BeginPopupModal("Could not complete action",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+460);ImGui::TextUnformatted(error.c_str());ImGui::PopTextWrapPos();if(ImGui::Button("OK",ImVec2(100,0))){error.clear();ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
        help_dialog();
        diagnostics_dialog();
        if(path_dialog)ImGui::OpenPopup(path_save?"Save IMG path":"Open IMG path");
        if(ImGui::BeginPopupModal(path_save?"Save IMG path":"Open IMG path",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::SetNextItemWidth(500);ImGui::InputText("Path",path_input,sizeof path_input);
            if(ImGui::Button(path_save?"Save":"Open")){
                auto target=find(path_uid);
                if(path_save && target){if(fs::exists(fs::u8path(path_input)) && target->art->path()!=path_input)error="Choose a new filename, or use Save for this library's existing file.";
                    else if(target->art->save(path_input,error)){recent_add(target->art->path());status="Saved "+target->name();}}
                else open(path_input);
                path_dialog=false;ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();if(ImGui::Button("Cancel")){path_dialog=false;ImGui::CloseCurrentPopup();}ImGui::EndPopup();
        }
        if(close_uid&&!path_dialog&&!scene_path_dialog&&!scene_guard_uid){auto t=find(close_uid);if(!t){close_uid=0;}else if(!t->art->dirty()){remove_uid=close_uid;close_uid=0;}else ImGui::OpenPopup("Save changes?");}
        if(ImGui::BeginPopupModal("Save changes?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            auto t=find(close_uid);ImGui::Text("Save changes to %s?",t?t->name().c_str():"this library");
            if(!error.empty()){ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+420);ImGui::TextColored(accent_surface(1),"%s",error.c_str());ImGui::PopTextWrapPos();}
            if(ImGui::Button("Save",ImVec2(100,0))&&t){if(save(*t,false)){remove_uid=close_uid;close_uid=0;ImGui::CloseCurrentPopup();}else if(path_dialog)ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button("Discard",ImVec2(100,0))){remove_uid=close_uid;close_uid=0;error.clear();ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button("Cancel",ImVec2(100,0))){close_uid=0;quitting=false;error.clear();ImGui::CloseCurrentPopup();}ImGui::EndPopup();
        }
    }
    void frame(){
        if(!library_copy.open)clear_library_copy_preview();
        if(!asm_ui.assets.library_review&&!asm_ui.assets.open){asm_ui.assets.library_preview.reset();asm_ui.assets.library_snapshot.reset();}
        for(auto &t:tabs)synchronize(*t);
        validate_layer_drag();
        validate_floating();
        validate_paint_line();
        validate_variant();
        for(auto &t:tabs)refresh_embedded(*t);
        shortcuts();
        for(auto &t:tabs)synchronize(*t);
        if(auto t=tab())if(t->embedded.enabled){
            if(!exporting.open&&!lod.open&&!restoring.open&&!batch.open&&!renaming.open&&!costume.open&&!ramp_transfer.open&&!batch_palette.open&&!deleting.open&&!library_copy.open&&!animation_copy.open&&!cleanup.open&&!video.open&&!digitizing.open&&!importing.open&&!palettes.open&&!close_uid&&!help&&error.empty()&&!path_dialog&&edit_dialog==EditDialog::None)embedded_advance(*t,ImGui::GetIO().DeltaTime);
        }else if(t->project&&t->alignment.enabled){
            clamp_scene_ui(*t);if(t->scene.playing&&!t->scene.dragging&&!exporting.open&&!lod.open&&!restoring.open&&!batch.open&&!renaming.open&&!costume.open&&!ramp_transfer.open&&!batch_palette.open&&!deleting.open&&!library_copy.open&&!animation_copy.open&&!cleanup.open&&!video.open&&!digitizing.open&&!importing.open&&!palettes.open&&!pending_project&&!close_uid&&!scene_guard_uid&&!help&&error.empty()&&!scene_path_dialog&&!path_dialog&&edit_dialog==EditDialog::None){auto &s=t->project->state;t->scene.tick+=ImGui::GetIO().DeltaTime*s.tick_hz;int duration=scene_duration(s);if(t->scene.tick>=duration){if(s.loop)t->scene.tick=std::fmod(t->scene.tick,duration);else{t->scene.tick=duration;t->scene.playing=false;}}}
        }else if(t->alignment.enabled&&!t->alignment.dragging&&!exporting.open&&!lod.open&&!restoring.open&&!batch.open&&!renaming.open&&!costume.open&&!ramp_transfer.open&&!batch_palette.open&&!deleting.open&&!library_copy.open&&!animation_copy.open&&!cleanup.open&&!video.open&&!digitizing.open&&!importing.open&&!palettes.open&&!close_uid&&!help&&error.empty()&&!path_dialog&&edit_dialog==EditDialog::None){
            auto &v=t->alignment;int before=v.player.frame;
            v.player.advance(ImGui::GetIO().DeltaTime,v.holds[v.sequence],v.tick_hz);
            if(v.player.frame!=before){auto pieces=preview_pieces(*v.frame(),v.multipart);t->selected=pieces.front();t->selected_id=t->art->sprite_id(t->selected);t->art->select_sprite(t->selected_id);t->reveal_selection=true;t->selection={};}
        }
        ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Studio",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_MenuBar|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        menu();
        if(tabs.empty())welcome();
        else {
            button("Open...",Action::Open);ImGui::SameLine();button(tab()&&tab()->project&&tab()->alignment.enabled?"Save project":"Save IMG",Action::Save);ImGui::SameLine();button("Undo",Action::Undo);ImGui::SameLine();button("Redo",Action::Redo);
            if(auto t=tab()){
                ImGui::SameLine();ImGui::TextDisabled("|");ImGui::SameLine();
                bool artwork=!t->alignment.enabled&&!t->embedded.enabled;
                if(artwork)ImGui::PushStyleColor(ImGuiCol_Button,accent_surface(.45f));
                if(ImGui::Button("Artwork"))workspace(*t,false);
                if(artwork)ImGui::PopStyleColor();ImGui::SameLine();
                bool alignment=t->alignment.enabled;if(alignment)ImGui::PushStyleColor(ImGuiCol_Button,accent_surface(.45f));
                if(ImGui::Button("Animation & alignment"))workspace(*t,true);
                if(alignment)ImGui::PopStyleColor();ImGui::SameLine();
                bool embedded=t->embedded.enabled;if(embedded)ImGui::PushStyleColor(ImGuiCol_Button,accent_surface(.45f));
                if(ImGui::Button("IMG animations"))embedded_workspace(*t);
                if(embedded)ImGui::PopStyleColor();
            }
            if(ImGui::BeginTabBar("libraries",ImGuiTabBarFlags_FittingPolicyScroll)){
                for(int i=0;i<(int)tabs.size();++i){auto &t=*tabs[i];std::string label=t.name()+"###library"+std::to_string(t.uid);bool opened=true;
                    ImGuiTabItemFlags flags=t.art->dirty()||(t.project&&t.project->dirty())?ImGuiTabItemFlags_UnsavedDocument:0;
                    // ImGui applies queued selection on its next layout. Keep the requested
                    // document active until that tab becomes visible.
                    if(select_uid==t.uid)flags|=ImGuiTabItemFlags_SetSelected;
                    if(ImGui::BeginTabItem(label.c_str(),&opened,flags)){if(select_uid==t.uid)select_uid=0;if(active!=i&&(!select_uid||select_uid==t.uid)){if(tab()){tab()->alignment.player.playing=false;tab()->scene.playing=false;tab()->embedded.playing=false;}finish();active=i;paste_mode=false;}ImGui::EndTabItem();}
                    if(!opened){finish();close_uid=t.uid;}
                }ImGui::EndTabBar();
            }
            if(auto t=tab()){
                synchronize(*t);
                ImGui::PushID((int)t->uid);
                float height=std::max(250.f,ImGui::GetContentRegionAvail().y-31);
                if(ImGui::BeginTable("workspace-with-tools",4,ImGuiTableFlags_Resizable|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp,ImVec2(0,height))){
                    bool compact=ImGui::GetIO().DisplaySize.x<1100;
                    ImGui::TableSetupColumn("Tools",ImGuiTableColumnFlags_WidthFixed|ImGuiTableColumnFlags_NoResize,160);
                    ImGui::TableSetupColumn("Library",ImGuiTableColumnFlags_WidthFixed,compact?190.f:245.f);
                    ImGui::TableSetupColumn("Canvas",ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Inspector",ImGuiTableColumnFlags_WidthFixed,compact?250.f:270.f);ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);tools_rail(*t,height);
                    ImGui::TableSetColumnIndex(1);ImGui::BeginChild("library",ImVec2(0,height),ImGuiChildFlags_None);library(*t);ImGui::EndChild();
                    ImGui::TableSetColumnIndex(2);ImGui::BeginChild("editor",ImVec2(0,height),ImGuiChildFlags_None);
                    if(t->embedded.enabled){embedded_controls(*t);float canvas_h=std::max(120.f,ImGui::GetContentRegionAvail().y-265);embedded_canvas(*t,canvas_h);embedded_timeline(*t,ImGui::GetContentRegionAvail().y);}
                    else if(t->alignment.enabled&&t->project){
                        t->project->begin("Edit project");project_controls(*t);float canvas_h=std::max(120.f,ImGui::GetContentRegionAvail().y-215);
                        project_canvas(*t,canvas_h);project_timeline(*t,ImGui::GetContentRegionAvail().y);
                    }else if(t->alignment.enabled){
                        alignment_controls(*t);float canvas_h=std::max(120.f,ImGui::GetContentRegionAvail().y-215);
                        alignment_canvas(*t,canvas_h);animation_timeline(*t,ImGui::GetContentRegionAvail().y);
                    }else{
                        tool_header(*t);float canvas_h=std::max(120.f,ImGui::GetContentRegionAvail().y-145);
                        canvas(*t,canvas_h);ImGui::BeginChild("palette",ImVec2(0,0),ImGuiChildFlags_Border);ImGui::BeginDisabled(variant.active);palette_strip(*t);ImGui::EndDisabled();ImGui::EndChild();
                    }ImGui::EndChild();
                    ImGui::TableSetColumnIndex(3);ImGui::BeginChild("inspector",ImVec2(0,height),ImGuiChildFlags_None);inspector(*t);ImGui::EndChild();ImGui::EndTable();
                }
                ImGui::Separator();
                if(t->art->disk_version()<0x634){ImGui::TextDisabled("IMG 0x%03X | original format",t->art->disk_version());ImGui::SameLine();ImGui::TextDisabled(" | ");ImGui::SameLine();}
                ImGui::TextDisabled("%s",status.c_str());
                ImGui::PopID();
            }
        }
        dialogs();ImGui::End();
        if(auto t=tab())if(t->embedded.enabled){apply_embedded_command(*t);if(ImGui::IsAnyItemActive())t->embedded.playing=false;else t->art->commit();}
        if(auto t=tab())if(t->project){apply_scene_command(*t);if(ImGui::IsAnyItemActive())t->scene.playing=false;else if(!t->scene.dragging)t->project->commit();}
        if(have_deferred){have_deferred=false;if(auto t=tab()){int index=t->art->sprite_index(deferred_sprite);if(index>=0){select(index);action(deferred_action);}}}
        if(remove_uid){for(auto it=tabs.begin();it!=tabs.end();++it)if((*it)->uid==remove_uid){int erased=(int)(it-tabs.begin());tabs.erase(it);if(active>=erased)--active;active=std::clamp(active,0,std::max(0,(int)tabs.size()-1));break;}remove_uid=0;paste_mode=false;}
        if(quitting&&!close_uid&&!scene_guard_uid&&!fatality.guard&&!fatality.saving&&!strikes.guard&&!strikes.saving&&!binding_ui.guard&&!binding_ui.saving){bool projects=false;for(auto &t:tabs)if(t->project){projects=true;break;}if(!projects){if(tabs.empty())running=false;else close_uid=tabs.back()->uid;}}
    }
#include "studio_project_ui.inl"
#include "studio_stage_ui.inl"
#include "studio_anchor_link_ui.inl"
#include "studio_points_ui.inl"
#include "studio_tbl_compare_ui.inl"
#include "studio_strikes_ui.inl"
#include "studio_asm_artwork_library_ui.inl"
#include "studio_asm_assets_ui.inl"
#include "studio_asm_ui.inl"
#include "studio_reactions_ui.inl"
#include "studio_fatality_ui.inl"
#include "studio_bindings_ui.inl"
#include "studio_strike_phases_ui.inl"
#include "studio_sequences_ui.inl"
#include "studio_palette_swatches_ui.inl"
#include "studio_palette_ui.inl"
#include "studio_palette_multi_merge_ui.inl"
#include "studio_palette_merge_ui.inl"
#include "studio_palette_reduction_ui.inl"
#include "studio_palette_usage_ui.inl"
#include "studio_palette_inheritance_ui.inl"
#include "studio_palette_ramps_ui.inl"
#include "studio_import_ui.inl"
#include "studio_legacy_media_ui.inl"
#include "studio_sheet_ui.inl"
#include "studio_digitize_ui.inl"
#include "studio_video_ui.inl"
#include "studio_regions_ui.inl"
};
#include "studio_video_smoke.inl"
#include "studio_digitize_job_smoke.inl"
struct DigitizeSmoke {
    bool finished=false,pending_seed=false;int logical=0;
    int failures=0,count=0,palettes=0;std::vector<std::string> paths;std::vector<unsigned char> original;
    void seed(App &app){app.digitize_add_material();edit_digitize_mask(app.digitizing.session,0,48,32,1,false,1,app.digitizing.error);app.digitize_add_material();edit_digitize_mask(app.digitizing.session,0,80,32,2,false,1,app.digitizing.error);app.digitize_fit();}
    void prepare(App &app,ImGuiIO &io,int actual_frame){if(actual_frame>=580){++failures;finished=true;return;}bool processing=app.digitizing.job||app.digitizing.pending;app.digitize_poll();if(processing)return;if(pending_seed){seed(app);pending_seed=false;return;}int frame=logical++;if(frame>=41)finished=true;auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;auto &u=app.digitizing;auto &s=u.session;auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio digitize UI check failed at frame %d: %s / %s\n",frame,u.error.c_str(),app.error.c_str());}};auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};auto pixel=[&](int x,int y){point(u.preview_origin+ImVec2((x+.5f)*u.preview_zoom,(y+.5f)*u.preview_zoom));};
        if(frame==0){count=art.count();palettes=art.palette_count();original=art.copy(0,0,0,48,64).pixels;check(art.save((fs::u8path(app.prefs)/"digitize-original.IMG").u8string(),app.error));for(int f=0;f<2;++f){std::vector<unsigned char> rgba(128*64*4,255);for(int y=8;y<56;++y)for(int x=32;x<96;++x){auto p=rgba.data()+(y*128+x+f*4)*4;p[0]=x<64?(unsigned char)(80+(y-8)*3):0;p[1]=0;p[2]=x>=64?(unsigned char)(80+(y-8)*3):0;}auto path=(fs::u8path(app.prefs)/("digitize-source-"+std::to_string(f)+".png")).u8string();check(stbi_write_png(path.c_str(),128,64,4,rgba.data(),128*4));paths.push_back(path);}app.request_digitize(paths);io.AddFocusEvent(true);}
        if(frame==3){check(u.open&&s.matte_ready&&!art.dirty());pixel(5,5);}if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==6){check(u.matte_dirty&&!u.valid);point(u.matte_point);}if(frame==7)io.AddMouseButtonEvent(0,true);if(frame==8)io.AddMouseButtonEvent(0,false);
        if(frame==9){check(!u.matte_dirty&&s.matte_ready&&u.view==1);point(u.add_point);}if(frame==10)io.AddMouseButtonEvent(0,true);if(frame==11)io.AddMouseButtonEvent(0,false);
        if(frame==12){check(s.materials.size()==1&&u.view==2);pixel(48,32);}if(frame==13)io.AddMouseButtonEvent(0,true);if(frame==14)io.AddMouseButtonEvent(0,false);
        if(frame==15){check(s.frames[0].mask[32*128+48]==1&&!art.dirty());point(u.add_point);}if(frame==16)io.AddMouseButtonEvent(0,true);if(frame==17)io.AddMouseButtonEvent(0,false);
        if(frame==18){check(s.materials.size()==2);pixel(80,32);}if(frame==19)io.AddMouseButtonEvent(0,true);if(frame==20)io.AddMouseButtonEvent(0,false);
        if(frame==21){check(s.frames[0].mask[32*128+80]==2&&!art.dirty());point(u.fit_point);}if(frame==22)io.AddMouseButtonEvent(0,true);if(frame==23)io.AddMouseButtonEvent(0,false);
        if(frame==24){check(u.valid&&!u.dirty&&u.batch.frames.size()==2&&s.unmapped==0&&u.view==3);pixel(64,63);}if(frame==25)io.AddMouseButtonEvent(1,true);if(frame==26)io.AddMouseButtonEvent(1,false);
        if(frame==27){check(s.custom_anchor&&s.anchor_y==63&&u.dirty&&!art.dirty());point(u.fit_point);}if(frame==28)io.AddMouseButtonEvent(0,true);if(frame==29)io.AddMouseButtonEvent(0,false);
        if(frame==30){check(u.valid&&!u.dirty);point(u.cancel_point);}if(frame==31)io.AddMouseButtonEvent(0,true);if(frame==32)io.AddMouseButtonEvent(0,false);
        if(frame==33){check(!u.open&&!art.dirty()&&art.count()==count);app.request_digitize(paths);pending_seed=true;}
        if(frame==35){check(u.valid&&!u.dirty);point(u.apply_point);}if(frame==36)io.AddMouseButtonEvent(0,true);if(frame==37)io.AddMouseButtonEvent(0,false);
        if(frame==38){check(!u.open&&art.count()==count+2&&art.palette_count()==palettes+1&&art.dirty());auto path=(fs::u8path(app.prefs)/"digitize-result.IMG").u8string();check(art.save(path,app.error));auto loaded=Artwork::open(path,app.error);check(loaded&&loaded->count()==count+2);app.action(Action::Undo);check(art.count()==count&&art.palette_count()==palettes&&art.copy(0,0,0,48,64).pixels==original);app.action(Action::Redo);check(art.count()==count+2);app.action(Action::Undo);app.request_digitize(paths);pending_seed=true;}
    }
};
struct ImportSmoke {
    int failures=0,count=0,palettes=0;std::string file;std::vector<unsigned char> original;bool gif=false;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;auto &u=app.importing;auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio import UI check failed at frame %d: %s / %s\n",frame,u.error.c_str(),app.error.c_str());}};auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};
        if(frame==0){count=art.count();palettes=art.palette_count();original=art.copy(0,0,0,48,64).pixels;check(art.save((fs::u8path(app.prefs)/"import-original.IMG").u8string(),app.error));
            file=(fs::u8path(app.prefs)/(gif?"import-source.gif":"import-source.png")).u8string();
            if(gif){std::vector<unsigned char> b={'G','I','F','8','9','a',2,0,2,0,0x81,0,0,0,0,0,255,0,0,0,0,255,255,255,255};for(int f=0;f<2;++f){int delay=f?600:4;b.insert(b.end(),{0x21,0xf9,4,0,(unsigned char)delay,(unsigned char)(delay>>8),0,0,0x2c,0,0,0,0,2,0,2,0,0,2});std::vector<unsigned char> data;unsigned acc=0;int bits=0;auto code=[&](int c){acc|=c<<bits;bits+=3;while(bits>=8){data.push_back((unsigned char)acc);acc>>=8;bits-=8;}};for(int i=0;i<4;++i){code(4);code(f?2:1);}code(5);if(bits)data.push_back((unsigned char)acc);b.push_back((unsigned char)data.size());b.insert(b.end(),data.begin(),data.end());b.push_back(0);}b.push_back(0x3b);std::ofstream out(fs::u8path(file),std::ios::binary);out.write((char *)b.data(),b.size());}
            else{std::vector<unsigned char> rgba(128*64*4,255);for(int f=0;f<2;++f)for(int y=8;y<56;++y)for(int x=16;x<48;++x){auto p=rgba.data()+(y*128+x+f*64)*4;p[0]=f?0:255;p[1]=0;p[2]=f?255:0;}check(stbi_write_png(file.c_str(),128,64,4,rgba.data(),128*4));}
            app.request_media_import(file);io.AddFocusEvent(true);}
        if(frame==3){check(u.open&&u.valid&&u.batch.frames.size()==2&&!art.dirty());if(!gif)point(u.grid_point);else{u.options.selected={false,true};u.dirty=true;point(u.rebuild_point);}}
        if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==6){if(!gif){check(u.options.grid&&u.dirty);point(u.rebuild_point);}else check(u.valid&&u.batch.frames.size()==1&&u.batch.frames[0].ticks>255);}
        if(frame==7&&!gif)io.AddMouseButtonEvent(0,true);if(frame==8&&!gif)io.AddMouseButtonEvent(0,false);
        if(frame==9){check(u.valid&&!u.dirty&&!art.dirty());u.options.custom_anchor=true;u.source_view=false;u.dirty=true;app.rebuild_import();}
        if(frame==10)point(u.anchor_point);if(frame==11)io.AddMouseButtonEvent(0,true);if(frame==12)io.AddMouseButtonEvent(0,false);
        if(frame==13){check(u.dirty&&!art.dirty());point(u.rebuild_point);}if(frame==14)io.AddMouseButtonEvent(0,true);if(frame==15)io.AddMouseButtonEvent(0,false);
        if(frame==16){check(u.valid&&!u.dirty);point(u.cancel_point);}if(frame==17)io.AddMouseButtonEvent(0,true);if(frame==18)io.AddMouseButtonEvent(0,false);
        if(frame==19){check(!u.open&&!art.dirty()&&art.count()==count);app.request_media_import(file);if(!gif){u.options.grid=true;app.rebuild_import();}}
        if(frame==21)point(u.apply_point);if(frame==22)io.AddMouseButtonEvent(0,true);if(frame==23)io.AddMouseButtonEvent(0,false);
        if(frame==24){check(!u.open&&art.count()==count+2&&art.palette_count()==palettes+1&&art.dirty());auto path=(fs::u8path(app.prefs)/"import-result.IMG").u8string();check(art.save(path,app.error));auto loaded=Artwork::open(path,app.error);check(loaded&&loaded->count()==count+2);app.action(Action::Undo);check(art.count()==count&&art.palette_count()==palettes&&art.copy(0,0,0,48,64).pixels==original);app.action(Action::Redo);check(art.count()==count+2);app.action(Action::Undo);app.request_media_import(file);if(!gif){u.options.grid=true;u.source_view=false;app.rebuild_import();}}
        if(frame==25){auto generation=art.generation();app.load_import_source((fs::u8path(app.prefs)/"missing-source.png").u8string());check(!u.valid&&u.dirty&&u.media.rgba.empty()&&art.generation()==generation&&art.count()==count);}
        if(frame==26){app.load_import_source(file);if(!gif){u.options.grid=true;u.source_view=false;app.rebuild_import();}check(u.valid&&!u.dirty);}
    }
};
struct PaletteRampSmoke {
    int failures=0;std::vector<unsigned short> colors,expected;std::vector<unsigned char> pixels;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;auto &u=app.palettes;auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio palette ramp UI check failed at frame %d: %s / %s\n",frame,u.error.c_str(),u.ramp_error.c_str());}};auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};
        if(frame==0){colors=copy_palette(art,0).words;pixels=art.copy(0,0,0,48,64).pixels;check(art.save((fs::u8path(app.prefs)/"ramp-smoke.IMG").u8string(),app.error));app.open_palettes(*t);io.AddFocusEvent(true);}
        if(frame==3)point(u.swatch_points[8]);if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);if(frame==6){io.AddKeyEvent(ImGuiMod_Shift,true);point(u.swatch_points[11]);}if(frame==7)io.AddMouseButtonEvent(0,true);if(frame==8)io.AddMouseButtonEvent(0,false);
        if(frame==10){io.AddKeyEvent(ImGuiMod_Shift,false);int count=0;for(bool selected:u.selected)count+=selected;check(count==4&&u.selected[8]&&u.selected[11]);point(u.ramps_point);}
        if(frame==11)io.AddMouseButtonEvent(0,true);if(frame==12)io.AddMouseButtonEvent(0,false);
        if(frame==14){check(u.ramp_mode&&u.ramp_valid&&app.palette_pending()&&!art.dirty());point(u.strength_point);}
        if(frame==15)io.AddMouseButtonEvent(0,true);if(frame==16)io.AddMouseButtonEvent(0,false);
        if(frame==18){check(u.ramp_options.strength>0&&u.ramp_options.strength<100&&!art.dirty()&&copy_palette(art,0).words==colors);point(u.cancel_ramp_point);}
        if(frame==19)io.AddMouseButtonEvent(0,true);if(frame==20)io.AddMouseButtonEvent(0,false);
        if(frame==21){check(!u.ramp_mode&&!art.dirty());app.start_palette_ramp(*t);}
        if(frame==23)point(u.gradient_point);if(frame==24)io.AddMouseButtonEvent(0,true);if(frame==25)io.AddMouseButtonEvent(0,false);
        if(frame==26){check(!u.ramp_options.shading);u.ramp_options.stops={{12,0,0},{200,24,12},{255,255,210}};}
        if(frame==27)point(u.reverse_point);if(frame==28)io.AddMouseButtonEvent(0,true);if(frame==29)io.AddMouseButtonEvent(0,false);
        if(frame==30){check(u.ramp_options.stops.front()==GradientColor({255,255,210})&&u.ramp_options.stops.back()==GradientColor({12,0,0}));expected=u.ramp_words;u.ramp_options.hue=70;}
        if(frame==31)u.ramp_options.hue=0;
        if(frame==32){check(u.ramp_words==expected&&!art.dirty());point(u.apply_point);}
        if(frame==33)io.AddMouseButtonEvent(0,true);if(frame==34)io.AddMouseButtonEvent(0,false);
        if(frame==36){check(!u.ramp_mode&&art.dirty()&&copy_palette(art,0).words==expected&&art.copy(0,0,0,48,64).pixels==pixels&&art.palette(0)->numc==16&&art.palette(0)->bitspix==4);for(int i=0;i<16;++i)if(i<8||i>11)check(copy_palette(art,0).words[i]==colors[i]);point(u.undo_point);}
        if(frame==37)io.AddMouseButtonEvent(0,true);if(frame==38)io.AddMouseButtonEvent(0,false);
        if(frame==40){check(!art.dirty()&&copy_palette(art,0).words==colors&&u.selected[8]&&u.selected[11]&&!u.selected[7]);app.start_palette_ramp(*t);u.ramp_options.shading=false;u.ramp_options.stops={{12,0,0},{210,24,12},{255,255,220}};snprintf(u.preset_name,sizeof u.preset_name,"Smoke gradient");app.save_studio_gradient(false);std::vector<GradientPreset> saved;check(read_gradient_presets(app.gradient_preset_path(),saved,u.preset_error));check(u.preset>=0&&saved[u.preset].stops==u.ramp_options.stops);auto count=u.presets.size();app.save_studio_gradient(false);check(u.presets.size()==count);u.presets_loaded=false;app.load_studio_gradients();check(u.presets.size()==count);app.save_studio_gradient(true);check(u.presets.size()+1==count&&!art.dirty());}
        if(frame==42){u.active_stop=1;point(u.ramp_sample_point);}
        if(frame==43)io.AddMouseButtonEvent(0,true);if(frame==44)io.AddMouseButtonEvent(0,false);
        if(frame==46){int c=pixels[32*48+24];unsigned char rgb[3],raw[]={(unsigned char)colors[c],(unsigned char)(colors[c]>>8)};pal_word_to_rgb8(raw,rgb,rgb+1,rgb+2);check(u.ramp_options.stops[1]==GradientColor({rgb[0],rgb[1],rgb[2]})&&!art.dirty());u.ramp_options.stops={{12,0,0},{210,24,12},{255,255,220}};u.ramp_options.hue=0;u.ramp_options.saturation=0;u.ramp_options.lightness=0;}
        if(frame==47)point(ImVec2(0,0));
    }
};
struct PaletteCompactionSmoke {
    int failures=0,expected=0,tail_count=0;std::vector<unsigned char> pixels,rgba;std::vector<uint64_t> ids;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;auto &u=app.palettes;auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio palette compaction UI check failed at frame %d: %s / %s\n",frame,u.error.c_str(),u.usage_error.c_str());}};auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};
        if(frame==0){auto original=copy_palette(art,0).words;std::vector<unsigned short> sparse(64);for(int i=0;i<16;++i)sparse[i<8?i:i+16]=original[i];check(art.add_palette("SPARSE",sparse,app.error)==1);for(int s=0;s<art.count();++s){check(art.assign_palette(s,1,false,app.error));auto im=art.sprite(s);auto p=(unsigned char *)im->data_p;int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x)if(p[y*stride+x]>=8)p[y*stride+x]+=16;ids.push_back(art.sprite_id(s));}pixels=art.copy(0,0,0,48,64).pixels;check(art.save((fs::u8path(app.prefs)/"compact-smoke.IMG").u8string(),app.error));app.open_palettes(*t);io.AddFocusEvent(true);}
        if(frame==3)point(u.usage_point);if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==7){check(u.usage_mode&&u.usage_valid&&u.usage_plan.removed>0&&u.usage_plan.index_changes>0&&!art.dirty());rgba=render_palette_compaction(art,u.usage_plan,0,false);check(rgba==render_palette_compaction(art,u.usage_plan,0,true));point(u.pack_point);}
        if(frame==8)io.AddMouseButtonEvent(0,true);if(frame==9)io.AddMouseButtonEvent(0,false);
        if(frame==10){tail_count=(int)u.usage_plan.words.size();check(u.usage_valid&&tail_count>16&&tail_count<=32&&!u.usage_plan.index_changes&&u.usage_plan.depth==6&&!art.dirty());u.usage_options.ppp=5;point(u.depth_point);}
        if(frame==11)io.AddMouseButtonEvent(0,true);if(frame==12)io.AddMouseButtonEvent(0,false);
        if(frame==14){check(u.usage_plan.depth==5&&u.usage_plan.ppp_changed&&!art.dirty());point(u.apply_point);}
        if(frame==15)io.AddMouseButtonEvent(0,true);if(frame==16)io.AddMouseButtonEvent(0,false);
        if(frame==18){check(!u.usage_mode&&art.palette(1)->numc==tail_count&&art.palette(1)->bitspix==5&&art.dirty()&&art.copy(0,0,0,48,64).pixels==pixels);point(u.undo_point);}
        if(frame==19)io.AddMouseButtonEvent(0,true);if(frame==20)io.AddMouseButtonEvent(0,false);
        if(frame==22){check(art.palette(1)->numc==64&&art.palette(1)->bitspix==6&&!art.dirty());app.start_palette_usage(*t);}
        if(frame==24)point(u.cancel_usage_point);if(frame==25)io.AddMouseButtonEvent(0,true);if(frame==26)io.AddMouseButtonEvent(0,false);
        if(frame==27){check(!u.usage_mode&&!art.dirty()&&art.palette(1)->numc==64);app.start_palette_usage(*t);u.usage_options.recompute_depth=true;}
        if(frame==29){expected=(int)u.usage_plan.words.size();check(expected<=16&&u.usage_plan.index_changes>0);point(u.apply_point);}
        if(frame==30)io.AddMouseButtonEvent(0,true);if(frame==31)io.AddMouseButtonEvent(0,false);
        if(frame==33){check(!u.usage_mode&&art.palette(1)->numc==expected&&art.dirty());PaletteCompactionPlan plan;check(plan_palette_compaction(art,{1,6},plan,app.error)&&render_palette_compaction(art,plan,0,false)==rgba);for(int s=0;s<art.count();++s)check(art.sprite_id(s)==ids[s]);auto path=(fs::u8path(app.prefs)/"compacted-smoke.IMG").u8string();check(art.save(path,app.error));auto loaded=Artwork::open(path,app.error);check(loaded&&loaded->palette(1)->numc==expected);u.command=PaletteCommand::Undo;}
        if(frame==35){check(art.palette(1)->numc==64&&art.copy(0,0,0,48,64).pixels==pixels);check(art.save((fs::u8path(app.prefs)/"compact-review.IMG").u8string(),app.error));app.start_palette_usage(*t);u.usage_options.ppp=5;}
        if(frame==37)check(u.usage_valid&&u.usage_plan.ppp_changed&&!art.dirty());
    }
};
#include "studio_palette_multi_merge_smoke.inl"
struct PaletteMergeSmoke {
    int failures=0;std::vector<unsigned char> pixels;std::vector<unsigned short> words,source_words;std::vector<uint64_t> ids;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;auto &u=app.palettes;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio palette merge UI check failed at frame %d: %s / %s\n",frame,u.error.c_str(),u.merge_error.c_str());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};
        if(frame==0){words=copy_palette(art,0).words;std::array<bool,256> selected{};selected.fill(true);source_words=adjust_palette(words,selected,120,0,0);check(art.add_palette("SOURCE",source_words,app.error)==1);check(art.assign_palette(0,1,false,app.error)&&art.assign_palette(1,1,false,app.error));pixels=art.copy(0,0,0,48,64).pixels;for(int i=0;i<art.count();++i)ids.push_back(art.sprite_id(i));check(art.save((fs::u8path(app.prefs)/"merge-smoke.IMG").u8string(),app.error));app.open_palettes(*t);app.palette_reset(*t,0);io.AddFocusEvent(true);}
        if(frame==3)point(u.merge_point);if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==7){check(u.merge_mode&&u.merge_valid&&u.merge_plan.sprites.size()==2&&u.merge_plan.added>0&&!u.merge_plan.color_changes&&!art.dirty());point(u.grow_point);}
        if(frame==8)io.AddMouseButtonEvent(0,true);if(frame==9)io.AddMouseButtonEvent(0,false);
        if(frame==10){check(u.merge_valid&&u.merge_plan.color_changes&&!u.allow_approximation&&!art.dirty());std::string error;check(!art.merge_palette(u.merge_plan,false,error)&&!art.dirty());point(u.approximate_point);}
        if(frame==11)io.AddMouseButtonEvent(0,true);if(frame==12)io.AddMouseButtonEvent(0,false);
        if(frame==13){check(u.allow_approximation);point(u.apply_point);}
        if(frame==14)io.AddMouseButtonEvent(0,true);if(frame==15)io.AddMouseButtonEvent(0,false);
        if(frame==17){check(!u.merge_mode&&art.dirty()&&art.sprite(0)->palnum==0&&art.palette_count()==2);auto current=art.copy(0,0,0,48,64).pixels;for(size_t i=0;i<current.size();++i)check((current[i]==0)==(pixels[i]==0));point(u.undo_point);}
        if(frame==18)io.AddMouseButtonEvent(0,true);if(frame==19)io.AddMouseButtonEvent(0,false);
        if(frame==21){check(!art.dirty()&&art.sprite(0)->palnum==1&&art.copy(0,0,0,48,64).pixels==pixels);app.start_palette_merge(*t);u.merge_options.remove_source=true;}
        if(frame==23){check(u.merge_valid&&!u.merge_plan.color_changes);point(u.apply_point);}
        if(frame==24)io.AddMouseButtonEvent(0,true);if(frame==25)io.AddMouseButtonEvent(0,false);
        if(frame==27){check(!u.merge_mode&&art.palette_count()==1&&art.sprite(0)->palnum==0);for(int i=0;i<art.count();++i)check(art.sprite_id(i)==ids[i]);u.command=PaletteCommand::Undo;}
        if(frame==28){check(art.palette_count()==2&&art.sprite(0)->palnum==1&&!art.dirty());u.command=PaletteCommand::Redo;}
        if(frame==29){check(art.palette_count()==1&&art.sprite(1)->palnum==0&&art.dirty());auto path=(fs::u8path(app.prefs)/"merged-smoke.IMG").u8string();check(art.save(path,app.error));auto loaded=Artwork::open(path,app.error);check(loaded&&loaded->palette_count()==1&&loaded->sprite(0)->palnum==0&&copy_palette(*loaded,0).words.size()>words.size());u.command=PaletteCommand::Undo;}
        if(frame==31){check(art.palette_count()==2&&art.copy(0,0,0,48,64).pixels==pixels);check(art.save((fs::u8path(app.prefs)/"merge-review.IMG").u8string(),app.error));app.start_palette_merge(*t);u.merge_options.ppp=4;}
        if(frame==33)check(u.merge_mode&&u.merge_valid&&u.merge_plan.ppp_images==8&&!art.dirty()&&copy_palette(art,0).words==words);
    }
};
struct PaletteSmoke {
    int failures=0;uint64_t source=0;std::vector<unsigned short> colors;std::vector<unsigned char> pixels;std::string file;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();if(!t){++failures;return;}auto &u=app.palettes;auto &art=*t->art;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio palette UI check failed at frame %d: %s\n",frame,u.error.c_str());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};
        if(frame==0){source=t->uid;pixels=art.copy(0,0,0,48,64).pixels;colors=copy_palette(art,0).words;file=(fs::u8path(app.prefs)/"palette-smoke-source.IMG").u8string();check(art.save(file,app.error));app.open_palettes(*t);io.AddFocusEvent(true);}
        if(frame==3)point(u.swatch_point);if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==6){int count=0;for(bool v:u.selected)count+=v;check(count==1&&u.selected[8]&&!art.dirty());}
        if(frame==7)point(u.hue_point+ImVec2(45,0));if(frame==8)io.AddMouseButtonEvent(0,true);if(frame==9)point(u.hue_point+ImVec2(75,0));if(frame==10)io.AddMouseButtonEvent(0,false);
        if(frame==11){check(app.palette_pending()&&!art.dirty()&&copy_palette(art,0).words==colors);point(u.reset_point);}
        if(frame==12)io.AddMouseButtonEvent(0,true);if(frame==13)io.AddMouseButtonEvent(0,false);
        if(frame==14){check(!app.palette_pending()&&!art.dirty());u.hue=120;}
        if(frame==15)point(u.apply_point);if(frame==16)io.AddMouseButtonEvent(0,true);if(frame==17)io.AddMouseButtonEvent(0,false);
        if(frame==18){check(art.dirty()&&!app.palette_pending());auto changed=copy_palette(art,0).words;check(changed[8]!=colors[8]&&changed[0]==colors[0]);for(int i=1;i<(int)colors.size();++i)if(i!=8)check(changed[i]==colors[i]);check(art.copy(0,0,0,48,64).pixels==pixels);point(u.undo_point);}
        if(frame==19)io.AddMouseButtonEvent(0,true);if(frame==20)io.AddMouseButtonEvent(0,false);
        if(frame==21){check(!art.dirty()&&copy_palette(art,0).words==colors);point(u.copy_point);}
        if(frame==22)io.AddMouseButtonEvent(0,true);if(frame==23)io.AddMouseButtonEvent(0,false);
        if(frame==24){check(app.palette_clipboard.words==colors);point(u.close_point);}
        if(frame==25)io.AddMouseButtonEvent(0,true);if(frame==26)io.AddMouseButtonEvent(0,false);
        if(frame==27){check(!u.open);app.demo();t=app.tab();app.open_palettes(*t);}
        if(frame==29)point(u.paste_point);if(frame==30)io.AddMouseButtonEvent(0,true);if(frame==31)io.AddMouseButtonEvent(0,false);
        if(frame==32){check(art.palette_count()==2&&u.index==1&&copy_palette(art,1).words==colors&&art.sprite(0)->palnum==0);point(u.assign_point);}
        if(frame==33)io.AddMouseButtonEvent(0,true);if(frame==34)io.AddMouseButtonEvent(0,false);
        if(frame==35){check(art.sprite(0)->palnum==1&&art.copy(0,0,0,48,64).pixels==pixels);auto path=(fs::u8path(app.prefs)/"palette-smoke-copy.IMG").u8string();check(art.save(path,app.error));auto opened=Artwork::open(path,app.error);check(opened&&opened->sprite(0)->palnum==1&&copy_palette(*opened,1).words==colors);u.hue=90;}
        if(frame==36)point(u.close_point);if(frame==37)io.AddMouseButtonEvent(0,true);if(frame==38)io.AddMouseButtonEvent(0,false);
        if(frame==39){check(!u.open&&!art.dirty()&&copy_palette(art,1).words==colors);check(!app.find(source)->art->dirty());app.open_palettes(*t);u.hue=95;}
    }
};
struct EmbeddedSmoke {
    int failures=0;std::vector<unsigned char> pixels;std::string file;int anix=0,aniy=0;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio embedded UI check failed at frame %d: %s\n",frame,app.error.c_str());}};
        auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        if(frame==0){pixels=art.copy(0,0,0,48,64).pixels;anix=(short)art.sprite(0)->anix;aniy=(short)art.sprite(0)->aniy;
            check(art.embedded_add_record(false,"DEMO_WALK",app.error)==0);for(int i=0;i<3;++i)check(art.embedded_append(0,i,app.error));check(art.embedded_add_record(true,"DEMO_SCRIPT",app.error)==1);check(art.embedded_append(1,0,app.error));
            file=(fs::u8path(app.prefs)/"embedded-smoke.IMG").u8string();check(art.save(file,app.error));app.embedded_workspace(*t);}
        if(frame==3){io.AddFocusEvent(true);io.AddMousePosEvent(t->embedded.duplicate_point.x,t->embedded.duplicate_point.y);}
        if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==6){check(t->embedded.records[0].entries.size()==4&&art.dirty());key(ImGuiKey_Z,true);}
        if(frame==7)key(ImGuiKey_Z,false);if(frame==8){check(t->embedded.records[0].entries.size()==3&&!art.dirty());key(ImGuiKey_Y,true);}
        if(frame==9)key(ImGuiKey_Y,false);if(frame==10){check(t->embedded.records[0].entries.size()==4);io.AddKeyEvent(ImGuiKey_Space,true);}
        if(frame==11)io.AddKeyEvent(ImGuiKey_Space,false);
        if(frame==12){check(t->embedded.playing&&t->embedded.tick>0);app.embedded_toggle_play(*t);check(art.copy(0,0,0,48,64).pixels==pixels);}
        if(frame==14){auto v=t->embedded.records[0].entries[0].values;v.ticks=255;v.dx=3;check(art.embedded_edit_entry(0,0,v,app.error));check(art.embedded_rename(0,"EDITED_WALK",app.error));art.commit();check(art.save(file,app.error));}
        if(frame==15)app.embedded_select_record(*t,1);
        if(frame==18)io.AddMousePosEvent(t->embedded.open_point.x,t->embedded.open_point.y);
        if(frame==19)io.AddMouseButtonEvent(0,true);if(frame==20)io.AddMouseButtonEvent(0,false);
        if(frame==21){check(!t->embedded.scripts&&t->embedded.record==0);t->embedded.command=EmbeddedCommand::NewSequence;}
        if(frame==22){check(art.data.seqcnt==2&&art.data.scrcnt==1);app.action(Action::Undo);check(art.data.seqcnt==1);app.action(Action::Redo);check(art.data.seqcnt==2);app.action(Action::Undo);}
        if(frame==24){app.embedded_select_record(*t,0);check(art.save(file,app.error));auto reopened=Artwork::open(file,app.error);std::vector<EmbeddedRecord> records;check(reopened&&read_embedded(*reopened,records,app.error)&&records[0].name=="EDITED_WALK"&&records[0].entries[0].values.ticks==255);t->embedded.command=EmbeddedCommand::Scene;}
        if(frame==25){check(t->project&&t->project->state.lanes[0].frames[0].ticks==255);check(t->project->save((fs::u8path(app.prefs)/"embedded-smoke.ISTUDIO").u8string(),app.scene_source(),app.error));app.embedded_workspace(*t);}
        if(frame==27){check(art.count()==8&&art.copy(0,0,0,48,64).pixels==pixels&&(short)art.sprite(0)->anix==anix&&(short)art.sprite(0)->aniy==aniy&&!art.dirty());app.embedded_seek(*t,2);}
    }
};
#include "studio_hotkeys_smoke.inl"
#include "studio_palette_clipboard_smoke.inl"
#include "studio_palette_swatches_smoke.inl"
#include "studio_palette_effects_smoke.inl"
#include "studio_palette_transparency_smoke.inl"
#include "studio_palette_removal_smoke.inl"
#include "studio_palette_zero_smoke.inl"
#include "studio_palette_move_smoke.inl"
#include "studio_palette_isolation_smoke.inl"
#include "studio_isolation_frames_smoke.inl"
#include "studio_region_remap_smoke.inl"
#include "studio_embedded_metadata_smoke.inl"
#include "studio_opacity_gradient_smoke.inl"
#include "studio_sprite_ramp_smoke.inl"
#include "studio_overlay_frame_smoke.inl"
#include "studio_magnification_smoke.inl"
#include "studio_subframe_swap_smoke.inl"
#include "studio_waterline_smoke.inl"
#include "studio_dummy_body_smoke.inl"
#include "studio_wax_dummy_smoke.inl"
#include "studio_wax_embedded_smoke.inl"
#include "studio_layers_smoke.inl"
#include "studio_layer_move_smoke.inl"
#include "studio_selection_smoke.inl"
#include "studio_regions_smoke.inl"
#include "studio_selection_transform_smoke.inl"
#include "studio_paint_smoke.inl"
#include "studio_clone_smoke.inl"
#include "studio_filters_smoke.inl"
#include "studio_repair_smoke.inl"
#include "studio_batch_smoke.inl"
#include "studio_batch_rename_smoke.inl"
#include "studio_batch_palette_smoke.inl"
#include "studio_batch_delete_smoke.inl"
#include "studio_library_copy_smoke.inl"
#include "studio_library_move_smoke.inl"
#include "studio_family_smoke.inl"
#include "studio_sprite_clipboard_smoke.inl"
#include "studio_library_split_smoke.inl"
#include "studio_library_split_animations_smoke.inl"
#include "studio_animation_copy_smoke.inl"
#include "studio_animation_artwork_smoke.inl"
#include "studio_animation_clipboard_smoke.inl"
#include "studio_animation_damage_smoke.inl"
#include "studio_animation_split_smoke.inl"
#include "studio_palette_cleanup_smoke.inl"
#include "studio_palette_duplicates_smoke.inl"
#include "studio_palette_order_smoke.inl"
#include "studio_palette_clean_copy_smoke.inl"
#include "studio_palette_inheritance_smoke.inl"
#include "studio_palette_reduction_smoke.inl"
#include "studio_ramp_transfer_smoke.inl"
#include "studio_costume_smoke.inl"
#include "studio_costume_borrow_smoke.inl"
#include "studio_costume_presets_smoke.inl"
#include "studio_restore_smoke.inl"
#include "studio_lod_smoke.inl"
#include "studio_export_smoke.inl"
#include "studio_legacy_img_smoke.inl"
#include "studio_legacy_media_smoke.inl"
#include "studio_halfbrite_smoke.inl"
#include "studio_sheet_smoke.inl"
#include "studio_stage_smoke.inl"
#include "studio_anchor_link_smoke.inl"
#include "studio_points_smoke.inl"
#include "studio_tbl_compare_smoke.inl"
#include "studio_strikes_smoke.inl"
#include "studio_asm_smoke.inl"
#include "studio_asm_assets_smoke.inl"
#include "studio_asm_artwork_library_smoke.inl"
#include "studio_asm_artwork_copy_smoke.inl"
#include "studio_asm_artwork_reuse_smoke.inl"
#include "studio_reactions_smoke.inl"
#include "studio_fatality_smoke.inl"
#include "studio_fatality_workspace_smoke.inl"
#include "studio_bindings_smoke.inl"
#include "studio_strike_phases_smoke.inl"
#include "studio_variant_smoke.inl"
#include "studio_welcome_smoke.inl"
#include "studio_tools_smoke.inl"
struct EditingSmoke {
    uint64_t original=0,copy=0;
    int failures=0;
    std::vector<unsigned char> pixels;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio editing UI check failed at frame %d\n",frame);}};
        auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        if(frame==3){original=art.sprite_id(0);pixels=art.copy(0,0,0,48,64).pixels;io.AddFocusEvent(true);key(ImGuiKey_J,true);}
        if(frame==4)key(ImGuiKey_J,false);
        if(frame==5){check(art.count()==9&&t->selected==8);copy=art.sprite_id(8);key(ImGuiKey_Z,true);}
        if(frame==6)key(ImGuiKey_Z,false);
        if(frame==7){check(art.count()==8&&art.selection_id()==original);key(ImGuiKey_Y,true);}
        if(frame==8)key(ImGuiKey_Y,false);
        if(frame==9)check(art.count()==9&&art.selection_id()==copy&&art.sprite_index(copy)==t->selected);
        if(frame==11){app.request_edit(EditDialog::Resize);app.edit_w=96;app.edit_h=128;}
        if(frame==13)io.AddMousePosEvent(app.edit_apply_point.x,app.edit_apply_point.y);
        if(frame==14)io.AddMouseButtonEvent(0,true);
        if(frame==15)io.AddMouseButtonEvent(0,false);
        if(frame==16){auto im=art.sprite(art.sprite_index(copy));check(im&&im->w==96&&im->h==128&&im->anix==48);check(app.edit_dialog==EditDialog::None);}
        if(frame==18)key(ImGuiKey_Z,true);
        if(frame==19)key(ImGuiKey_Z,false);
        if(frame==20){auto im=art.sprite(art.sprite_index(copy));check(im&&im->w==48&&im->h==64);app.action(Action::Undo);check(art.count()==8&&art.selection_id()==original);}
        if(frame==22){check(t->selected_id==original);check(art.copy(0,0,0,48,64).pixels==pixels);app.action(Action::Redo);}
        if(frame==24){check(art.count()==9&&t->selected_id==copy);app.request_edit(EditDialog::Resize);app.edit_w=96;app.edit_h=128;}
    }
};
struct ProjectSmoke {
    uint64_t owner=0,reference=0;int failures=0;ImVec2 start;int x=0,y=0;
    std::string project_path,source_path;std::vector<unsigned char> original;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio project UI check failed at frame %d\n",frame);}};
        auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        if(frame==2){auto t=app.tab();owner=t->uid;auto dir=fs::u8path(app.prefs)/"scene-smoke";fs::create_directories(dir);source_path=(dir/"actor.IMG").u8string();project_path=(dir/"preview.ISTUDIO").u8string();check(t->art->save(source_path,app.error));
            original=t->art->copy(0,0,0,48,64).pixels;app.new_scene(*t);check(t->project&&t->project->dirty());t->scene.smoke=true;}
        auto t=app.find(owner);if(!t||!t->project)return;
        if(frame==3){check(t->project->save(project_path,app.scene_source(),app.error));check(!t->project->dirty()&&!t->art->dirty());}
        if(frame==4){start=ImFloor(t->scene.probe)+ImVec2(1,1);io.AddFocusEvent(true);io.AddMousePosEvent(start.x,start.y);}
        if(frame==5)io.AddMouseButtonEvent(0,true);
        if(frame==6){auto target=ImFloor(start+ImVec2(8*t->scene.probe_zoom,-4*t->scene.probe_zoom));x=(int)std::round((target.x-start.x)/t->scene.probe_zoom);y=(int)std::round((target.y-start.y)/t->scene.probe_zoom);io.AddMousePosEvent(target.x,target.y);}
        if(frame==7)io.AddMouseButtonEvent(0,false);
        if(frame==8){check(t->project->state.lanes[0].x==x&&t->project->state.lanes[0].y==y);check(t->project->dirty()&&!t->art->dirty());key(ImGuiKey_Z,true);}
        if(frame==9)key(ImGuiKey_Z,false);
        if(frame==10){check(!t->project->dirty()&&t->project->state.lanes[0].x==0);key(ImGuiKey_Y,true);}
        if(frame==11)key(ImGuiKey_Y,false);
        if(frame==12){check(t->project->dirty()&&t->project->state.lanes[0].x==x);start=ImFloor(t->scene.probe)+ImVec2(1,1);io.AddMousePosEvent(start.x,start.y);}
        if(frame==13)io.AddMouseButtonEvent(0,true);
        if(frame==14)io.AddMousePosEvent(start.x+5*t->scene.probe_zoom,start.y+3*t->scene.probe_zoom);
        if(frame==15)io.AddKeyEvent(ImGuiKey_Escape,true);
        if(frame==16){io.AddKeyEvent(ImGuiKey_Escape,false);io.AddMouseButtonEvent(0,false);}
        if(frame==17){check(t->project->state.lanes[0].x==x&&t->project->state.lanes[0].y==y);check(t->art->copy(0,0,0,48,64).pixels==original);app.demo();reference=app.tab()->uid;check(app.tab()->art->save((fs::u8path(app.prefs)/"scene-smoke"/"reference.IMG").u8string(),app.error));
            for(int i=0;i<(int)app.tabs.size();++i)if(app.tabs[i]->uid==owner)app.active=i;app.select_uid=owner;t->scene.source_doc=reference;t->scene.source_sprite=app.find(reference)->art->sprite_id(1);t->scene.command=SceneCommand::AddLane;}
        if(frame==18){check(t->project->state.lanes.size()==2);t->project->begin("Timing");t->project->state.lanes[0].frames[0].ticks=7;t->project->commit();check(t->project->save(project_path,app.scene_source(),app.error));
            check(t->project->export_png((fs::u8path(app.prefs)/"scene-smoke"/"export.png").u8string(),0,app.scene_source(),app.error));}
        if(frame==19){app.close_uid=reference;}
        if(frame==20){check(app.close_uid==0&&app.find(reference));check(!app.error.empty());app.error.clear();ImGui::ClosePopupToLevel(0,true);auto backup=fs::u8path(source_path);backup+=".bak";fs::rename(fs::u8path(source_path),backup);app.open_scene(project_path);check(app.pending_project&&app.tabs.size()==2&&t->project&&!app.scene_issues.empty());}
        if(frame==21){auto backup=fs::u8path(source_path);backup+=".bak";fs::rename(backup,fs::u8path(source_path));ImGui::ClosePopupToLevel(0,true);app.stage_scene();check(!app.pending_project&&app.tabs.size()==2);t=app.find(owner);check(t->project->state.lanes.size()==2&&t->project->state.lanes[0].frames[0].ticks==7&&!t->project->dirty());t->scene.smoke=true;}
        if(frame==23){t->project->begin("Quit guard");t->project->state.lanes[0].x=44;t->project->commit();app.action(Action::Quit);}
        if(frame==24){check(app.scene_guard_uid==owner&&app.tabs.size()==2);io.AddMousePosEvent(app.scene_cancel_point.x,app.scene_cancel_point.y);}
        if(frame==25){io.AddMousePosEvent(app.scene_cancel_point.x,app.scene_cancel_point.y);io.AddMouseButtonEvent(0,true);}
        if(frame==26)io.AddMouseButtonEvent(0,false);
        if(frame==27){check(!app.quitting&&!app.scene_guard_uid&&t->project->dirty()&&app.tabs.size()==2);app.action(Action::Undo);check(!t->project->dirty());t->scene.lane=0;t->scene.frame=0;t->scene.tick=0;}
    }
};
struct MotionSmoke {
    int failures=0;std::string file;std::vector<unsigned char> pixels;int anchor_x=0,anchor_y=0;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio motion UI check failed at frame %d\n",frame);}};
        auto t=app.tab();if(!t){++failures;return;}
        auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        if(frame==2){auto dir=fs::u8path(app.prefs)/"motion-smoke";fs::create_directories(dir);check(t->art->save((dir/"actor.IMG").u8string(),app.error));file=(dir/"motion.ISTUDIO").u8string();
            auto im=t->art->sprite(0);anchor_x=im->anix;anchor_y=im->aniy;pixels=t->art->copy(0,0,0,im->w,im->h).pixels;
            app.new_scene(*t);auto &s=t->project->state;s.loop=false;s.onion=false;auto &lane=s.lanes[0];lane.frames.resize(2);lane.loop=false;lane.hold_end=false;
            auto &first=lane.frames[0];first.ticks=2;first.visible_from=2;first.visible_until=8;first.vx=3;first.cap_x=6;first.dual=true;first.dual_x=-70;
            auto &second=lane.frames[1];second.ticks=2;second.visible_from=4;second.visible_until=10;second.vx=-2;second.x=80;second.z=1;t->scene.motion_smoke=true;}
        if(!t->project)return;
        if(frame==3)check(t->project->save(file,app.scene_source(),app.error));
        if(frame==4){t->scene.tick=4;t->scene.frame=0;io.AddFocusEvent(true);}
        if(frame==5)io.AddMousePosEvent(t->scene.show_here_point.x,t->scene.show_here_point.y);
        if(frame==6)io.AddMouseButtonEvent(0,true);
        if(frame==7)io.AddMouseButtonEvent(0,false);
        if(frame==8){check(t->project->state.lanes[0].frames[0].visible_from==4&&t->project->dirty());key(ImGuiKey_Z,true);}
        if(frame==9)key(ImGuiKey_Z,false);
        if(frame==10){check(t->project->state.lanes[0].frames[0].visible_from==2&&!t->project->dirty());key(ImGuiKey_Y,true);}
        if(frame==11)key(ImGuiKey_Y,false);
        if(frame==12){check(t->project->state.lanes[0].frames[0].visible_from==4);check(t->project->save(file,app.scene_source(),app.error));}
        if(frame==14){app.open_scene(file);check(!app.pending_project);t=app.tab();check(t&&t->project&&t->project->state.lanes[0].frames[0].dual&&t->project->state.lanes[0].frames[0].cap_x==6);t->scene.motion_smoke=true;
            t->scene.source_sprite=t->art->sprite_id(2);t->scene.command=SceneCommand::ReplaceFrame;app.apply_scene_command(*t);auto &f=t->project->state.lanes[0].frames[0];check(f.visible_from==4&&f.visible_until==8&&f.cap_x==6&&f.dual&&f.ticks==2&&f.pieces[0].runtime_id==t->art->sprite_id(2));app.action(Action::Undo);check(!t->project->dirty());}
        if(frame==15){t->project->begin("Fast preview");t->project->state.tick_hz=240;t->project->commit();t->scene.tick=0;t->scene.playing=true;}
        if(frame==22){check(!t->scene.playing&&t->scene.tick==10);check(evaluate_scene(t->project->state,(int)t->scene.tick).empty());
            auto im=t->art->sprite(0);check(!t->art->dirty()&&im->anix==anchor_x&&im->aniy==anchor_y&&t->art->copy(0,0,0,im->w,im->h).pixels==pixels);t->scene.tick=6;t->scene.frame=0;}
    }
};
struct TriggerSmoke {
    int failures=0;uint64_t source_id=0,hit_id=0;std::string file;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio trigger UI check failed at frame %d\n",frame);}};
        auto t=app.tab();if(!t){++failures;return;}
        auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        if(frame==2){auto dir=fs::u8path(app.prefs)/"trigger-smoke";fs::create_directories(dir);check(t->art->save((dir/"actor.IMG").u8string(),app.error));file=(dir/"effect.ISTUDIO").u8string();
            app.new_scene(*t);auto &s=t->project->state;s.loop=false;s.onion=false;s.lanes[0].frames.resize(2);s.lanes[0].name="Attack";s.lanes[0].x=-55;s.lanes[0].loop=false;source_id=s.lanes[0].id;hit_id=s.lanes[0].frames[1].id;
            t->scene.source_doc=t->uid;t->scene.source_sprite=t->art->sprite_id(2);t->scene.command=SceneCommand::AddLane;app.apply_scene_command(*t);
            auto &effect=s.lanes[1];effect.name="Hit effect";effect.delay=1;effect.x=65;effect.loop=false;effect.hold_end=false;effect.frames[0].ticks=3;
            t->scene.trigger_source_lane=source_id;t->scene.trigger_source_frame=hit_id;t->scene.trigger_smoke=true;}
        if(!t->project)return;auto &s=t->project->state;
        if(frame==3)check(t->project->save(file,app.scene_source(),app.error));
        if(frame==4){io.AddFocusEvent(true);io.AddMousePosEvent(t->scene.follow_point.x,t->scene.follow_point.y);}
        if(frame==5)io.AddMouseButtonEvent(0,true);
        if(frame==6)io.AddMouseButtonEvent(0,false);
        if(frame==7){check(s.lanes[1].trigger_frame==hit_id&&scene_lane_start(s,1)==5&&t->project->dirty()&&!t->art->dirty());key(ImGuiKey_Z,true);}
        if(frame==8)key(ImGuiKey_Z,false);
        if(frame==9){check(!s.lanes[1].trigger_lane&&!t->project->dirty());key(ImGuiKey_Y,true);}
        if(frame==10)key(ImGuiKey_Y,false);
        if(frame==11)check(scene_lane_start(s,1)==5&&s.lanes[1].trigger_frame==hit_id);
        if(frame==12){check(t->project->save(file,app.scene_source(),app.error));t->project->begin("Retime windup");s.lanes[0].frames[0].ticks=7;t->project->commit();check(scene_lane_start(s,1)==8);}
        if(frame==13){app.action(Action::Undo);check(scene_lane_start(s,1)==5&&!t->project->dirty());app.action(Action::Redo);check(scene_lane_start(s,1)==8);}
        if(frame==14){t->scene.lane=0;t->scene.frame=1;t->scene.command=SceneCommand::MoveLeft;}
        if(frame==15){check(s.lanes[0].frames[0].id==hit_id&&scene_lane_start(s,1)==1);app.action(Action::Undo);check(scene_lane_start(s,1)==8);app.action(Action::Redo);check(scene_lane_start(s,1)==1);}
        if(frame==16){check(t->project->save(file,app.scene_source(),app.error));t->scene.lane=1;t->scene.frame=0;}
        if(frame==18)io.AddMousePosEvent(t->scene.detach_point.x,t->scene.detach_point.y);
        if(frame==19)io.AddMouseButtonEvent(0,true);
        if(frame==20)io.AddMouseButtonEvent(0,false);
        if(frame==21){check(!s.lanes[1].trigger_lane&&s.lanes[1].delay==1&&scene_lane_start(s,1)==1);app.action(Action::Undo);check(s.lanes[1].trigger_frame==hit_id&&!t->project->dirty());}
        if(frame==22){t->scene.lane=0;t->scene.frame=0;t->scene.command=SceneCommand::DeleteFrame;}
        if(frame==23){check(s.lanes[0].frames.size()==2&&s.lanes[1].trigger_frame==hit_id&&!app.error.empty());app.error.clear();t->scene.command=SceneCommand::DuplicateFrame;}
        if(frame==24){check(s.lanes[0].frames.size()==3&&s.lanes[0].frames[1].id!=hit_id&&scene_lane_start(s,1)==1);app.action(Action::Undo);check(s.lanes[0].frames.size()==2&&!t->project->dirty());}
        if(frame==25){app.open_scene(file);check(!app.pending_project);t=app.tab();check(t->project->state.lanes[1].trigger_frame==hit_id&&scene_lane_start(t->project->state,1)==1&&!t->project->dirty()&&!t->art->dirty());t->scene.lane=1;t->scene.frame=0;t->scene.tick=2;t->scene.trigger_smoke=true;}
    }
};
}

static int studio_run(int argc,char **argv,studio::StartupLog &startup,bool quiet,bool software){
    startup.stage("Initialize SDL video");if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)!=0){startup.failure(SDL_GetError(),quiet);return 1;}
    bool layer_move_preview=argc>=3&&std::string(argv[1])=="--smoke-layer-move-preview";
    bool layer_move_smoke=argc>=3&&(std::string(argv[1])=="--smoke-layer-move"||layer_move_preview);
    bool layers_smoke=argc>=3&&std::string(argv[1])=="--smoke-layers";
    bool effects_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-effects";
    bool swatches_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-swatches";
    bool palette_clip_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-clipboard";
    bool hotkeys_smoke=argc>=3&&std::string(argv[1])=="--smoke-hotkeys";
    bool alignment_smoke=argc>=3&&std::string(argv[1])=="--smoke-alignment";
    bool gesture_smoke=argc>=3&&std::string(argv[1])=="--smoke-gesture";
    bool editing_smoke=argc>=3&&std::string(argv[1])=="--smoke-editing";
    bool selection_smoke=argc>=3&&std::string(argv[1])=="--smoke-selection";
    bool region_smoke=argc>=3&&std::string(argv[1])=="--smoke-regions";
    bool floating_smoke=argc>=3&&std::string(argv[1])=="--smoke-floating";
    bool tools_smoke=argc>=3&&std::string(argv[1])=="--smoke-tools";
    bool welcome_smoke=argc>=3&&std::string(argv[1]).find("--smoke-welcome")==0;
    bool variant_smoke=argc>=3&&std::string(argv[1])=="--smoke-variant";
    bool phases_smoke=argc>=3&&std::string(argv[1]).find("--smoke-phases")==0;
    bool bindings_smoke=argc>=3&&std::string(argv[1])=="--smoke-bindings";
    bool fatality_linked_smoke=argc>=3&&std::string(argv[1])=="--smoke-fatality-linked";
    bool fatality_workspace_smoke=fatality_linked_smoke||argc>=3&&std::string(argv[1])=="--smoke-fatality-workspace";
    bool fatality_smoke=argc>=3&&std::string(argv[1]).find("--smoke-fatality")==0;
    bool reactions_smoke=argc>=3&&std::string(argv[1]).find("--smoke-reactions")==0;
    bool asm_artwork_reuse_smoke=argc>=3&&std::string(argv[1]).find("--smoke-asm-reuse")==0;
    bool asm_artwork_reuse_review_smoke=argc>=3&&std::string(argv[1])=="--smoke-asm-reuse-review";
    bool asm_artwork_reuse_all_smoke=argc>=3&&std::string(argv[1])=="--smoke-asm-reuse-all";
    bool asm_artwork_copy_smoke=argc>=3&&std::string(argv[1]).find("--smoke-asm-copy")==0;
    bool asm_artwork_copy_review_smoke=argc>=3&&std::string(argv[1])=="--smoke-asm-copy-review";
    bool asm_library_smoke=argc>=3&&std::string(argv[1]).find("--smoke-asm-library")==0;
    bool asm_library_review_smoke=argc>=3&&std::string(argv[1])=="--smoke-asm-library-review";
    bool asm_assets_smoke=argc>=3&&std::string(argv[1])=="--smoke-asm-assets";
    bool asm_smoke=fatality_smoke||reactions_smoke||(argc>=3&&std::string(argv[1]).find("--smoke-asm")==0);
    bool strikes_smoke=argc>=3&&std::string(argv[1])=="--smoke-strikes";
    bool table_compare_smoke=argc>=3&&std::string(argv[1])=="--smoke-tbl-compare";
    bool points_smoke=argc>=3&&std::string(argv[1])=="--smoke-points";
    bool anchor_link_smoke=argc>=3&&std::string(argv[1])=="--smoke-anchor-link";
    bool stage_smoke=argc>=3&&std::string(argv[1])=="--smoke-stage";
    bool sheet_smoke=argc>=3&&std::string(argv[1])=="--smoke-sheet";
    bool iff_dest_smoke=argc>=3&&std::string(argv[1])=="--smoke-iff-dest";
    bool halfbrite_smoke=iff_dest_smoke||(argc>=3&&std::string(argv[1])=="--smoke-halfbrite");
    bool legacy_media_smoke=argc>=3&&std::string(argv[1])=="--smoke-legacy-media";
    bool legacy_img_smoke=argc>=3&&std::string(argv[1])=="--smoke-legacy-img";
    bool export_smoke=argc>=3&&std::string(argv[1])=="--smoke-export";
    bool lod_smoke=argc>=3&&std::string(argv[1])=="--smoke-lod";
    bool restore_smoke=argc>=3&&std::string(argv[1])=="--smoke-restore";
    bool region_remap_smoke=argc>=3&&std::string(argv[1])=="--smoke-region-remap";
    bool isolation_frames_smoke=argc>=3&&std::string(argv[1])=="--smoke-isolation-frames";
    bool isolation_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-isolation";
    bool palette_move_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-move";
    bool zero_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-zero";
    bool removal_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-remove";
    bool transparency_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-transparency";
    bool costume_presets_smoke=argc>=3&&std::string(argv[1])=="--smoke-costume-presets";
    bool costume_borrow_smoke=argc>=3&&std::string(argv[1])=="--smoke-costume-borrow";
    bool costume_smoke=argc>=3&&std::string(argv[1])=="--smoke-costume";
    bool ramp_transfer_smoke=argc>=3&&std::string(argv[1])=="--smoke-ramp-transfer";
    bool reduce_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-reduce";
    bool inherit_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-inherit";
    bool clean_copy_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-clean-copy";
    bool order_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-order";
    bool duplicates_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-duplicates";
    bool cleanup_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-cleanup";
    bool animation_damage_smoke=argc>=3&&std::string(argv[1])=="--smoke-animation-damage";
    bool animation_split_smoke=argc>=3&&std::string(argv[1])=="--smoke-animation-split";
    bool animation_clipboard_smoke=argc>=3&&std::string(argv[1])=="--smoke-animation-clipboard";
    bool animation_artwork_smoke=argc>=3&&std::string(argv[1])=="--smoke-animation-artwork";
    bool animation_copy_smoke=animation_damage_smoke||animation_split_smoke||animation_clipboard_smoke||animation_artwork_smoke||(argc>=3&&std::string(argv[1])=="--smoke-animation-copy");
    bool split_animations_smoke=argc>=3&&std::string(argv[1])=="--smoke-library-split-animations";
    bool split_smoke=split_animations_smoke||(argc>=3&&std::string(argv[1])=="--smoke-library-split");
    bool sprite_clip_smoke=argc>=3&&std::string(argv[1])=="--smoke-sprite-clipboard";
    bool family_smoke=argc>=3&&std::string(argv[1])=="--smoke-family";
    bool library_move_smoke=argc>=3&&std::string(argv[1])=="--smoke-library-move";
    bool library_copy_smoke=library_move_smoke||(argc>=3&&std::string(argv[1])=="--smoke-library-copy");
    bool batch_delete_smoke=argc>=3&&std::string(argv[1])=="--smoke-batch-delete";
    bool batch_palette_smoke=argc>=3&&std::string(argv[1])=="--smoke-batch-palette";
    bool batch_rename_smoke=argc>=3&&std::string(argv[1])=="--smoke-batch-rename";
    bool batch_smoke=argc>=3&&std::string(argv[1])=="--smoke-batch";
    bool repair_smoke=argc>=3&&std::string(argv[1])=="--smoke-repair";
    bool filters_smoke=argc>=3&&std::string(argv[1])=="--smoke-filters";
    bool clone_smoke=argc>=3&&std::string(argv[1])=="--smoke-clone";
    bool paint_smoke=argc>=3&&std::string(argv[1])=="--smoke-paint";
    bool project_smoke=argc>=3&&std::string(argv[1])=="--smoke-project";
    bool motion_smoke=argc>=3&&std::string(argv[1])=="--smoke-motion";
    bool trigger_smoke=argc>=3&&std::string(argv[1])=="--smoke-trigger";
    bool wax_embedded_smoke=argc>=3&&std::string(argv[1])=="--smoke-wax-embedded";
    bool wax_dummy_smoke=argc>=3&&std::string(argv[1])=="--smoke-wax-dummy";
    bool dummy_body_smoke=argc>=3&&std::string(argv[1])=="--smoke-dummy-body";
    bool waterline_smoke=argc>=3&&std::string(argv[1])=="--smoke-waterline";
    bool subframe_swap_smoke=argc>=3&&std::string(argv[1])=="--smoke-subframe-swap";
    bool magnification_smoke=argc>=3&&std::string(argv[1])=="--smoke-magnification";
    bool overlay_frame_smoke=argc>=3&&std::string(argv[1])=="--smoke-overlay-frame";
    bool sprite_ramp_smoke=argc>=3&&std::string(argv[1])=="--smoke-sprite-ramp";
    bool opacity_smoke=argc>=3&&std::string(argv[1])=="--smoke-opacity-gradient";
    bool metadata_smoke=argc>=3&&std::string(argv[1])=="--smoke-embedded-metadata";
    bool embedded_smoke=argc>=3&&std::string(argv[1])=="--smoke-embedded";
    bool palette_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette";
    bool marked_merge_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-marked";
    bool merge_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-merge";
    bool compact_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-compact";
    bool job_smoke=argc>=3&&(std::string(argv[1])=="--smoke-digitize-job"||std::string(argv[1])=="--smoke-digitize-progress");
    bool video_smoke=argc>=3&&(std::string(argv[1])=="--smoke-video"||std::string(argv[1])=="--smoke-video-result");
    bool digitize_smoke=argc>=3&&std::string(argv[1])=="--smoke-digitize";
    bool import_smoke=argc>=3&&(std::string(argv[1])=="--smoke-import"||std::string(argv[1])=="--smoke-import-gif");
    bool ramp_smoke=argc>=3&&std::string(argv[1])=="--smoke-palette-ramp";
    bool smoke=argc>=3&&(std::string(argv[1])=="--smoke"||layer_move_smoke||layers_smoke||effects_smoke||swatches_smoke||palette_clip_smoke||hotkeys_smoke||alignment_smoke||gesture_smoke||editing_smoke||selection_smoke||region_smoke||floating_smoke||paint_smoke||clone_smoke||filters_smoke||repair_smoke||batch_smoke||batch_rename_smoke||batch_palette_smoke||batch_delete_smoke||library_copy_smoke||family_smoke||sprite_clip_smoke||split_smoke||animation_copy_smoke||region_remap_smoke||isolation_frames_smoke||isolation_smoke||palette_move_smoke||zero_smoke||removal_smoke||transparency_smoke||costume_presets_smoke||costume_borrow_smoke||costume_smoke||ramp_transfer_smoke||reduce_smoke||inherit_smoke||clean_copy_smoke||order_smoke||duplicates_smoke||cleanup_smoke||export_smoke||legacy_img_smoke||legacy_media_smoke||halfbrite_smoke||sheet_smoke||stage_smoke||anchor_link_smoke||points_smoke||table_compare_smoke||strikes_smoke||asm_smoke||bindings_smoke||phases_smoke||lod_smoke||restore_smoke||variant_smoke||welcome_smoke||tools_smoke||project_smoke||motion_smoke||trigger_smoke||subframe_swap_smoke||waterline_smoke||dummy_body_smoke||wax_dummy_smoke||wax_embedded_smoke||magnification_smoke||overlay_frame_smoke||sprite_ramp_smoke||opacity_smoke||metadata_smoke||embedded_smoke||palette_smoke||marked_merge_smoke||merge_smoke||compact_smoke||ramp_smoke||import_smoke||digitize_smoke||video_smoke||job_smoke);
    auto app_owner=std::make_unique<studio::App>();auto &app=*app_owner;
    app.gesture_smoke=gesture_smoke;
    startup.stage("Create editor window");
    app.window=SDL_CreateWindow("Imgtool Studio - artwork editor",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,1440,900,SDL_WINDOW_RESIZABLE|(smoke?SDL_WINDOW_HIDDEN:SDL_WINDOW_SHOWN));
    if(!app.window){startup.failure(SDL_GetError(),quiet);SDL_Quit();return 1;}SDL_SetWindowMinimumSize(app.window,1000,680);
    startup.stage(software||smoke?"Create software renderer":"Create accelerated renderer");
    app.renderer=SDL_CreateRenderer(app.window,-1,(software||smoke)?SDL_RENDERER_SOFTWARE:SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if(!app.renderer)app.renderer=SDL_CreateRenderer(app.window,-1,SDL_RENDERER_SOFTWARE);
    if(!app.renderer){startup.failure(SDL_GetError(),quiet);SDL_DestroyWindow(app.window);SDL_Quit();return 1;}
    SDL_RendererInfo renderer_info{};if(SDL_GetRendererInfo(app.renderer,&renderer_info)==0)startup.write(std::string("Renderer: ")+(renderer_info.name?renderer_info.name:"unknown"));
    startup.stage("Load UI fonts and settings");
    IMGUI_CHECKVERSION();ImGui::CreateContext();auto &io=ImGui::GetIO();io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    char *pref=SDL_GetPrefPath("midway-imgtool","studio");app.prefs=pref?pref:".";SDL_free(pref);
    if(smoke)app.prefs=std::filesystem::absolute(std::filesystem::u8path(argv[2])).parent_path().u8string();
    app.ini=(std::filesystem::u8path(app.prefs)/"studio-layout.ini").u8string();io.IniFilename=smoke?nullptr:app.ini.c_str();
    {std::ifstream f(std::filesystem::u8path(app.prefs)/"recent.txt");std::string line;while(std::getline(f,line)&&app.recent.size()<10)if(!line.empty())app.recent.push_back(line);}
    studio::apply_theme();
    const char *fonts[]={"C:/Windows/Fonts/segoeui.ttf","/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf","/System/Library/Fonts/Supplemental/Arial.ttf"};
    const char *font_path=nullptr;for(auto path:fonts)if(std::filesystem::exists(path)){if(io.Fonts->AddFontFromFileTTF(path,16)){font_path=path;break;}}if(!font_path)io.Fonts->AddFontDefault();
    // Keep the primary UI face; add common CJK filename glyphs when available.
    const char *fallbacks[]={"C:/Windows/Fonts/msyh.ttc","/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc","/System/Library/Fonts/PingFang.ttc"};
    for(auto path:fallbacks)if(std::filesystem::exists(path)){ImFontConfig config;config.MergeMode=true;io.Fonts->AddFontFromFileTTF(path,16,&config,io.Fonts->GetGlyphRangesChineseSimplifiedCommon());break;}
    // Bake home-screen headings at their displayed sizes; scaling body text blurs them.
    if(font_path){app.home_title=io.Fonts->AddFontFromFileTTF(font_path,32);app.home_section=io.Fonts->AddFontFromFileTTF(font_path,20);}
    else {ImFontConfig title;title.SizePixels=32;app.home_title=io.Fonts->AddFontDefault(&title);ImFontConfig section;section.SizePixels=20;app.home_section=io.Fonts->AddFontDefault(&section);}
    char *base_path=SDL_GetBasePath();auto icon_path=std::filesystem::u8path(base_path?base_path:"")/"assets"/"MaterialSymbolsSharp-Regular.ttf";SDL_free(base_path);
    if(std::filesystem::exists(icon_path)){static ImVector<ImWchar> icon_ranges;ImFontGlyphRangesBuilder ranges;for(auto &spec:studio::tool_specs)ranges.AddText(spec.glyph);ranges.BuildRanges(&icon_ranges);app.tool_font=io.Fonts->AddFontFromFileTTF(icon_path.u8string().c_str(),22,nullptr,icon_ranges.Data);}
    startup.stage("Initialize UI renderer");
    ImGui_ImplSDL2_InitForSDLRenderer(app.window,app.renderer);ImGui_ImplSDLRenderer2_Init(app.renderer);
    startup.stage("Open requested artwork");
    if(smoke){
        if(welcome_smoke){}else if(!gesture_smoke&&!editing_smoke&&!selection_smoke&&!region_smoke&&!floating_smoke&&!paint_smoke&&!clone_smoke&&!filters_smoke&&!repair_smoke&&!batch_smoke&&!batch_rename_smoke&&!batch_palette_smoke&&!batch_delete_smoke&&!library_copy_smoke&&!family_smoke&&!sprite_clip_smoke&&!split_smoke&&!animation_copy_smoke&&!cleanup_smoke&&!export_smoke&&!legacy_img_smoke&&!legacy_media_smoke&&!sheet_smoke&&!stage_smoke&&!anchor_link_smoke&&!points_smoke&&!table_compare_smoke&&!strikes_smoke&&!asm_smoke&&!bindings_smoke&&!phases_smoke&&!lod_smoke&&!restore_smoke&&!variant_smoke&&!tools_smoke&&!project_smoke&&!motion_smoke&&!trigger_smoke&&!embedded_smoke&&!palette_smoke&&!merge_smoke&&!compact_smoke&&!ramp_smoke&&!import_smoke&&!digitize_smoke&&!video_smoke&&!job_smoke&&argc>3&&std::string(argv[3])!="--small")app.open(argv[3]);else app.demo();
        for(int i=3;i<argc;++i)if(std::string(argv[i])=="small"||std::string(argv[i])=="--small")SDL_SetWindowSize(app.window,1000,680);
        if((alignment_smoke||gesture_smoke)&&app.tab()){auto &t=*app.tab();app.workspace(t,true);if(alignment_smoke)t.alignment.facing=2;}
    }
    else {for(int i=1;i<argc;++i)if(std::string(argv[i]).rfind("--",0)!=0){app.open(argv[i]);break;}for(int i=1;i<argc;++i)if(app.tab()){if(std::string(argv[i])=="--alignment")app.workspace(*app.tab(),true);if(std::string(argv[i])=="--project")app.new_scene(*app.tab());if(std::string(argv[i])=="--embedded")app.embedded_workspace(*app.tab());}}
    int frames=0,exit_code=0;
    ImVec2 gesture_start;int original_x=0,original_y=0,expected_x=0,expected_y=0;
    std::vector<unsigned char> original_pixels;
    studio::LayerMoveSmoke layer_move_check;layer_move_check.preview=layer_move_preview;
    studio::CostumeSmoke costume_check;
    studio::CostumeBorrowSmoke costume_borrow_check;
    studio::CostumePresetsSmoke costume_presets_check;
    studio::RampTransferSmoke ramp_transfer_check;
    studio::PaletteReductionSmoke reduce_check;
    studio::PaletteInheritanceSmoke inherit_check;
    studio::PaletteCleanCopySmoke clean_copy_check;
    studio::PaletteOrderSmoke order_check;
    studio::PaletteDuplicatesSmoke duplicates_check;
    studio::PaletteEffectsSmoke effects_check;
    studio::PaletteTransparencySmoke transparency_check;
    studio::PaletteRemovalSmoke removal_check;
    studio::PaletteZeroSmoke zero_check;
    studio::PaletteMoveSmoke palette_move_check;
    studio::PaletteIsolationSmoke isolation_check;
    studio::IsolationFramesSmoke isolation_frames_check;
    studio::RegionRemapSmoke region_remap_check;
    studio::PaletteSwatchesSmoke swatches_check;
    studio::PaletteClipboardSmoke palette_clip_check;
    studio::LayersSmoke layers_check;
    studio::EditingSmoke editing_check;
    studio::SelectionSmoke selection_check;
    studio::RegionSmoke region_check;
    studio::FloatingSmoke floating_check;
    studio::PaintSmoke paint_check;
    studio::CloneSmoke clone_check;
    studio::FiltersSmoke filters_check;
    studio::RepairSmoke repair_check;
    studio::BatchSmoke batch_check;studio::BatchRenameSmoke batch_rename_check;studio::BatchPaletteSmoke batch_palette_check;studio::BatchDeleteSmoke batch_delete_check;studio::LibraryCopySmoke library_copy_check;studio::LibraryMoveSmoke library_move_check;studio::FamilySmoke family_check;studio::SpriteClipboardSmoke sprite_clip_check;studio::LibrarySplitSmoke split_check;studio::LibrarySplitAnimationsSmoke split_animations_check;studio::AnimationDamageSmoke animation_damage_check;studio::AnimationCopySmoke animation_copy_check;studio::AnimationArtworkSmoke animation_artwork_check;studio::AnimationClipboardSmoke animation_clipboard_check;studio::AnimationSplitSmoke animation_split_check;studio::PaletteCleanupSmoke cleanup_check;
    studio::RestoreSmoke restore_check;
    studio::LodSmoke lod_check;
    studio::ExportSmoke export_check;
    studio::LegacyImgSmoke legacy_img_check;
    studio::LegacyMediaSmoke legacy_media_check;
    studio::HalfbriteSmoke halfbrite_check;halfbrite_check.destination=iff_dest_smoke;
    studio::SheetSmoke sheet_check;
    studio::StageSmoke stage_check;
    studio::AnchorLinkSmoke anchor_link_check;
    studio::PointsSmoke points_check;
    studio::TableCompareSmoke table_compare_check;
    studio::StrikePhasesSmoke phases_check;phases_check.edit=argc>1&&std::string(argv[1])=="--smoke-phases-edit";phases_check.real=argc>1&&std::string(argv[1])=="--smoke-phases-real";
    studio::BindingsSmoke bindings_check;
    studio::StrikesSmoke strikes_check;
    studio::HotkeysSmoke hotkeys_check;
    studio::FatalityWorkspaceSmoke fatality_workspace_check;
    studio::AsmAssetsSmoke asm_assets_check;
    studio::AsmArtworkLibrarySmoke asm_library_check;
    studio::AsmArtworkCopySmoke asm_artwork_copy_check;
    studio::AsmArtworkReuseSmoke asm_artwork_reuse_check;
    studio::FatalitySmoke fatality_check;fatality_check.real=argc>1&&std::string(argv[1])=="--smoke-fatality-real";
    studio::ReactionsSmoke reactions_check;reactions_check.real=argc>1&&std::string(argv[1])=="--smoke-reactions-real";
    studio::AsmSmoke asm_check;asm_check.real=argc>1&&std::string(argv[1])=="--smoke-asm-real";
    studio::VariantSmoke variant_check;
    studio::ToolsSmoke tools_check;
    studio::WelcomeSmoke welcome_check;welcome_check.kind=argc>1&&std::string(argv[1])=="--smoke-welcome-empty"?1:argc>1&&std::string(argv[1])=="--smoke-welcome-many"?2:argc>1&&std::string(argv[1])=="--smoke-welcome-check"?3:0;
    studio::ProjectSmoke project_check;
    studio::MotionSmoke motion_check;
    studio::TriggerSmoke trigger_check;
    studio::EmbeddedSmoke embedded_check;
    studio::EmbeddedMetadataSmoke metadata_check;
    studio::OpacityGradientSmoke opacity_check;
    studio::SpriteRampSmoke sprite_ramp_check;
    studio::OverlayFrameSmoke overlay_frame_check;
    studio::MagnificationSmoke magnification_check;
    studio::SubframeSwapSmoke subframe_swap_check;
    studio::WaterlineSmoke waterline_check;
    studio::DummyBodySmoke dummy_body_check;
    studio::WaxDummySmoke wax_dummy_check;studio::WaxEmbeddedSmoke wax_embedded_check;
    studio::PaletteSmoke palette_check;
    studio::PaletteMultiMergeSmoke marked_merge_check;
    studio::PaletteMergeSmoke merge_check;
    studio::PaletteCompactionSmoke compact_check;
    studio::DigitizeJobSmoke job_check;job_check.preview=argc>1&&std::string(argv[1])=="--smoke-digitize-progress";studio::VideoSmoke video_check;video_check.result=argc>1&&std::string(argv[1])=="--smoke-video-result";studio::DigitizeSmoke digitize_check;studio::PaletteRampSmoke ramp_check;studio::ImportSmoke import_check;import_check.gif=std::string(argc>1?argv[1]:"")=="--smoke-import-gif";
    startup.stage("Enter editor loop");bool first_frame=true;
    while(app.running){
        SDL_Event e;auto event=[&](){ImGui_ImplSDL2_ProcessEvent(&e);if(e.type==SDL_QUIT){app.finish();app.action(studio::Action::Quit);}if(e.type==SDL_DROPFILE){if(!app.fatality.open&&!app.fatality.saving&&!app.fatality.guard&&!app.strike_phases.open&&!app.binding_ui.open&&!app.binding_ui.saving&&!app.binding_ui.guard&&!app.asm_ui.open&&!app.strikes.open&&!app.strikes.guard&&!app.strikes.saving&&!app.table_compare.open&&!app.points.open&&!app.anchor_link.open&&!app.stage_dialog_open&&!app.sheet.open&&!app.legacy_media.open&&!app.exporting.open&&!app.lod.open&&!app.restoring.open&&!app.batch.open&&!app.renaming.open&&!app.costume.open&&!app.ramp_transfer.open&&!app.batch_palette.open&&!app.deleting.open&&!app.library_copy.open&&!app.animation_copy.open&&!app.cleanup.open&&!app.regions.open&&!app.video.open&&!app.digitizing.open&&!app.importing.open&&!app.palettes.open&&!app.close_uid&&!app.help&&app.error.empty()&&!app.path_dialog&&app.edit_dialog==studio::EditDialog::None)app.open(e.drop.file);SDL_free(e.drop.file);}};
        if(SDL_WaitEventTimeout(&e,16)){event();while(SDL_PollEvent(&e))event();}
        ImGui_ImplSDLRenderer2_NewFrame();ImGui_ImplSDL2_NewFrame();
        if(editing_smoke)editing_check.prepare(app,io,frames);
        if(selection_smoke)selection_check.prepare(app,io,frames);
        if(region_smoke)region_check.prepare(app,io,frames);
        if(floating_smoke)floating_check.prepare(app,io,frames);
        if(paint_smoke)paint_check.prepare(app,io,frames);
        if(clone_smoke)clone_check.prepare(app,io,frames);
        if(filters_smoke)filters_check.prepare(app,io,frames);
        if(repair_smoke)repair_check.prepare(app,io,frames);
        if(batch_smoke)batch_check.prepare(app,io,frames);
        if(batch_rename_smoke)batch_rename_check.prepare(app,io,frames);
        if(batch_palette_smoke)batch_palette_check.prepare(app,io,frames);
        if(batch_delete_smoke)batch_delete_check.prepare(app,io,frames);
        if(library_move_smoke)library_move_check.prepare(app,io,frames);else if(library_copy_smoke)library_copy_check.prepare(app,io,frames);
        if(family_smoke)family_check.prepare(app,io,frames);
        if(sprite_clip_smoke)sprite_clip_check.prepare(app,io,frames);
        if(split_animations_smoke)split_animations_check.prepare(app,io,frames);else if(split_smoke)split_check.prepare(app,io,frames);
        if(animation_damage_smoke)animation_damage_check.prepare(app,io,frames);else if(animation_split_smoke)animation_split_check.prepare(app,io,frames);else if(animation_clipboard_smoke)animation_clipboard_check.prepare(app,io,frames);else if(animation_artwork_smoke)animation_artwork_check.prepare(app,io,frames);else if(animation_copy_smoke)animation_copy_check.prepare(app,io,frames);
        if(costume_smoke)costume_check.prepare(app,io,frames);
        if(costume_borrow_smoke)costume_borrow_check.prepare(app,io,frames);
        if(costume_presets_smoke)costume_presets_check.prepare(app,io,frames);
        if(ramp_transfer_smoke)ramp_transfer_check.prepare(app,io,frames);
        if(reduce_smoke)reduce_check.prepare(app,io,frames);
        if(inherit_smoke)inherit_check.prepare(app,io,frames);
        if(clean_copy_smoke)clean_copy_check.prepare(app,io,frames);
        if(order_smoke)order_check.prepare(app,io,frames);
        if(duplicates_smoke)duplicates_check.prepare(app,io,frames);
        if(cleanup_smoke)cleanup_check.prepare(app,io,frames);
        if(restore_smoke)restore_check.prepare(app,io,frames);
        if(lod_smoke)lod_check.prepare(app,io,frames);
        if(strikes_smoke)strikes_check.prepare(app,io,frames);
        if(layer_move_smoke)layer_move_check.prepare(app,io,frames);
        if(effects_smoke)effects_check.prepare(app,io,frames);
        if(transparency_smoke)transparency_check.prepare(app,io,frames);
        if(removal_smoke)removal_check.prepare(app,io,frames);
        if(zero_smoke)zero_check.prepare(app,io,frames);
        if(palette_move_smoke)palette_move_check.prepare(app,io,frames);
        if(isolation_smoke)isolation_check.prepare(app,io,frames);
        if(isolation_frames_smoke)isolation_frames_check.prepare(app,io,frames);
        if(region_remap_smoke)region_remap_check.prepare(app,io,frames);
        if(swatches_smoke)swatches_check.prepare(app,io,frames);
        if(palette_clip_smoke)palette_clip_check.prepare(app,io,frames);
        if(layers_smoke)layers_check.prepare(app,io,frames);
        if(hotkeys_smoke)hotkeys_check.prepare(app,io,frames);
        if(fatality_workspace_smoke)fatality_workspace_check.prepare(app,io,frames);else if(fatality_smoke)fatality_check.prepare(app,io,frames);else if(reactions_smoke)reactions_check.prepare(app,io,frames);else if(asm_artwork_reuse_smoke)asm_artwork_reuse_check.prepare(app,io,frames);else if(asm_artwork_copy_smoke)asm_artwork_copy_check.prepare(app,io,frames);else if(asm_library_smoke)asm_library_check.prepare(app,io,frames);else if(asm_assets_smoke)asm_assets_check.prepare(app,io,frames);else if(asm_smoke)asm_check.prepare(app,io,frames);
        if(bindings_smoke)bindings_check.prepare(app,io,frames);
        if(phases_smoke)phases_check.prepare(app,io,frames);
        if(table_compare_smoke)table_compare_check.prepare(app,io,frames);
        if(points_smoke)points_check.prepare(app,io,frames);
        if(anchor_link_smoke)anchor_link_check.prepare(app,io,frames);
        if(stage_smoke)stage_check.prepare(app,io,frames);
        if(sheet_smoke)sheet_check.prepare(app,io,frames);
        if(legacy_media_smoke)legacy_media_check.prepare(app,io,frames);
        if(halfbrite_smoke)halfbrite_check.prepare(app,io,frames);
        if(legacy_img_smoke)legacy_img_check.prepare(app,io,frames);
        if(export_smoke)export_check.prepare(app,io,frames);
        if(variant_smoke)variant_check.prepare(app,io,frames);
        if(welcome_smoke)welcome_check.prepare(app,io,frames);
        if(tools_smoke)tools_check.prepare(app,io,frames);
        if(project_smoke)project_check.prepare(app,io,frames);
        if(motion_smoke)motion_check.prepare(app,io,frames);
        if(trigger_smoke)trigger_check.prepare(app,io,frames);
        if(embedded_smoke)embedded_check.prepare(app,io,frames);
        if(metadata_smoke)metadata_check.prepare(app,io,frames);
        if(opacity_smoke)opacity_check.prepare(app,io,frames);
        if(sprite_ramp_smoke)sprite_ramp_check.prepare(app,io,frames);
        if(overlay_frame_smoke)overlay_frame_check.prepare(app,io,frames);
        if(magnification_smoke)magnification_check.prepare(app,io,frames);
        if(subframe_swap_smoke)subframe_swap_check.prepare(app,io,frames);
        if(waterline_smoke)waterline_check.prepare(app,io,frames);
        if(dummy_body_smoke)dummy_body_check.prepare(app,io,frames);
        if(wax_dummy_smoke)wax_dummy_check.prepare(app,io,frames);
        if(wax_embedded_smoke)wax_embedded_check.prepare(app,io,frames);
        if(palette_smoke)palette_check.prepare(app,io,frames);
        if(marked_merge_smoke)marked_merge_check.prepare(app,io,frames);
        if(merge_smoke)merge_check.prepare(app,io,frames);
        if(compact_smoke)compact_check.prepare(app,io,frames);
        if(ramp_smoke)ramp_check.prepare(app,io,frames);if(import_smoke)import_check.prepare(app,io,frames);if(digitize_smoke)digitize_check.prepare(app,io,frames);if(video_smoke)video_check.prepare(app,io,frames);if(job_smoke)job_check.prepare(app,io,frames);
        if(gesture_smoke&&app.tab()){
            auto &t=*app.tab();auto im=t.art->sprite(t.selected);
            auto check=[&](bool value){if(!value){exit_code=3;fprintf(stderr,"Studio gesture check failed at frame %d: anchor %d,%d; expected start %d,%d; dirty=%d; dragging=%d\n",frames,(short)im->anix,(short)im->aniy,original_x,original_y,t.art->dirty(),t.alignment.dragging);}};
            if(frames==3){gesture_start=ImFloor(app.gesture_point);original_x=(short)im->anix;original_y=(short)im->aniy;
                original_pixels=t.art->copy(t.selected,0,0,im->w,im->h).pixels;io.AddFocusEvent(true);io.AddMousePosEvent(gesture_start.x,gesture_start.y);}
            if(frames==4)io.AddMouseButtonEvent(0,true);
            if(frames==5){auto target=ImFloor(gesture_start+ImVec2(3*app.gesture_zoom,-2*app.gesture_zoom));
                expected_x=original_x-(int)std::round((target.x-gesture_start.x)/app.gesture_zoom);
                expected_y=original_y-(int)std::round((target.y-gesture_start.y)/app.gesture_zoom);
                io.AddMousePosEvent(target.x,target.y);}
            if(frames==6)io.AddMouseButtonEvent(0,false);
            if(frames==7){check((short)im->anix==expected_x&&(short)im->aniy==expected_y&&t.art->dirty());
                check(t.art->copy(t.selected,0,0,im->w,im->h).pixels==original_pixels);
                io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_Z,true);}
            if(frames==8){io.AddKeyEvent(ImGuiKey_Z,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);}
            if(frames==9){check(!t.art->dirty()&&(short)im->anix==original_x&&(short)im->aniy==original_y);
                io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_Y,true);}
            if(frames==10){io.AddKeyEvent(ImGuiKey_Y,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);}
            if(frames==11){check(t.art->dirty()&&(short)im->anix==expected_x);
                gesture_start=ImFloor(app.gesture_point);io.AddMousePosEvent(gesture_start.x,gesture_start.y);}
            if(frames==12)io.AddMouseButtonEvent(0,true);
            if(frames==13)io.AddMousePosEvent(gesture_start.x+5*app.gesture_zoom,gesture_start.y+4*app.gesture_zoom);
            if(frames==14)io.AddKeyEvent(ImGuiKey_Escape,true);
            if(frames==15){io.AddKeyEvent(ImGuiKey_Escape,false);io.AddMouseButtonEvent(0,false);}
            if(frames==16){check((short)im->anix==expected_x&&(short)im->aniy==expected_y);app.action(studio::Action::Undo);check(!t.art->dirty());}
        }
        ImGui::NewFrame();app.frame();ImGui::Render();
        SDL_SetRenderDrawColor(app.renderer,15,18,23,255);SDL_RenderClear(app.renderer);ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(),app.renderer);
        if(smoke&&((++frames==((video_smoke||digitize_smoke||job_smoke)?600:wax_embedded_smoke?56:wax_dummy_smoke?54:dummy_body_smoke?232:waterline_smoke?278:subframe_swap_smoke?232:magnification_smoke?152:overlay_frame_smoke?236:sprite_ramp_smoke?394:opacity_smoke?320:metadata_smoke?318:region_remap_smoke?246:isolation_frames_smoke?270:isolation_smoke?282:palette_move_smoke?268:zero_smoke?320:removal_smoke?250:transparency_smoke?240:costume_presets_smoke?360:costume_borrow_smoke?316:costume_smoke?234:ramp_transfer_smoke?182:reduce_smoke?186:inherit_smoke?148:clean_copy_smoke?138:order_smoke?158:duplicates_smoke?108:effects_smoke?176:swatches_smoke?228:palette_clip_smoke?160:layer_move_smoke?(layer_move_preview?24:284):layers_smoke?120:marked_merge_smoke?144:hotkeys_smoke?368:phases_smoke?(phases_check.edit?104:74):bindings_smoke?100:fatality_linked_smoke?188:fatality_smoke?(fatality_check.real?12:230):reactions_smoke?(reactions_check.real?18:140):asm_artwork_reuse_smoke?(asm_artwork_reuse_review_smoke?54:asm_artwork_reuse_all_smoke?105:126):asm_artwork_copy_smoke?(asm_artwork_copy_review_smoke?81:120):asm_library_smoke?(asm_library_review_smoke?38:110):asm_assets_smoke?164:asm_smoke?52:strikes_smoke?97:table_compare_smoke?31:points_smoke?54:anchor_link_smoke?60:stage_smoke?43:sheet_smoke?68:iff_dest_smoke?120:halfbrite_smoke?96:legacy_media_smoke?60:legacy_img_smoke?30:export_smoke?99:lod_smoke?70:restore_smoke?98:animation_damage_smoke?84:animation_split_smoke?130:animation_clipboard_smoke?152:animation_artwork_smoke?140:animation_copy_smoke?244:split_animations_smoke?140:split_smoke?114:sprite_clip_smoke?144:cleanup_smoke?92:family_smoke?74:library_move_smoke?160:library_copy_smoke?110:batch_delete_smoke?112:batch_palette_smoke?118:batch_rename_smoke?92:batch_smoke?66:repair_smoke?63:filters_smoke?43:tools_smoke?50:welcome_smoke?26:variant_smoke?56:clone_smoke?50:paint_smoke?59:floating_smoke?53:region_smoke?38:selection_smoke?58:import_smoke?28:ramp_smoke?49:compact_smoke?40:merge_smoke?36:palette_smoke?43:project_smoke||motion_smoke||trigger_smoke||embedded_smoke?30:editing_smoke?28:gesture_smoke?20:5))||(video_smoke&&video_check.finished)||(digitize_smoke&&digitize_check.finished)||(job_smoke&&job_check.finished))){int w,h;SDL_GetRendererOutputSize(app.renderer,&w,&h);std::vector<unsigned char> pixels((size_t)w*h*4);
            if(SDL_RenderReadPixels(app.renderer,nullptr,SDL_PIXELFORMAT_RGBA32,pixels.data(),w*4)!=0||!stbi_write_png(argv[2],w,h,4,pixels.data(),w*4))exit_code=1;
            if(costume_check.failures)exit_code=64;
            if(costume_borrow_check.failures)exit_code=65;
            if(costume_presets_check.failures)exit_code=66;
            if(transparency_check.failures)exit_code=67;
            if(removal_check.failures)exit_code=68;
            if(zero_check.failures)exit_code=69;
            if(palette_move_check.failures)exit_code=70;
            if(isolation_check.failures)exit_code=71;
            if(isolation_frames_check.failures)exit_code=72;
            if(region_remap_check.failures)exit_code=73;
            if(metadata_check.failures)exit_code=74;
            if(opacity_check.failures)exit_code=75;
            if(sprite_ramp_check.failures)exit_code=76;
            if(overlay_frame_check.failures)exit_code=77;
            if(magnification_check.failures)exit_code=78;
            if(subframe_swap_check.failures)exit_code=79;
            if(waterline_check.failures)exit_code=80;
            if(dummy_body_check.failures)exit_code=81;
            if(wax_dummy_check.failures)exit_code=83;
            if(wax_embedded_check.failures)exit_code=84;
            if(ramp_transfer_check.failures)exit_code=63;
            if(reduce_check.failures)exit_code=62;
            if(inherit_check.failures)exit_code=61;
            if(clean_copy_check.failures)exit_code=60;
            if(order_check.failures)exit_code=59;
            if(duplicates_check.failures)exit_code=58;
            if(effects_check.failures)exit_code=57;
            if(swatches_check.failures)exit_code=56;
            if(palette_clip_check.failures)exit_code=55;
            if(layer_move_check.failures)exit_code=54;
            if(layers_check.failures)exit_code=53;
            if(marked_merge_check.failures)exit_code=52;
            if(hotkeys_check.failures)exit_code=51;
            if(selection_check.failures)exit_code=17;
            if(region_check.failures)exit_code=18;
            if(floating_check.failures)exit_code=19;
            if(paint_check.failures)exit_code=20;
            if(clone_check.failures)exit_code=21;
            if(variant_check.failures)exit_code=22;
            if(welcome_check.failures)exit_code=23;
            if(tools_check.failures)exit_code=24;
            if(filters_check.failures)exit_code=25;
            if(repair_check.failures)exit_code=26;
            if(batch_check.failures)exit_code=27;
            if(family_check.failures)exit_code=46;
            if(sprite_clip_check.failures)exit_code=48;
            if(split_check.failures||split_animations_check.failures)exit_code=49;
            if(animation_damage_check.failures||animation_copy_check.failures||animation_artwork_check.failures||animation_clipboard_check.failures||animation_split_check.failures)exit_code=50;
            if(cleanup_check.failures)exit_code=47;
            if(batch_rename_check.failures)exit_code=42;
            if(batch_palette_check.failures)exit_code=43;
            if(batch_delete_check.failures)exit_code=44;
            if(library_copy_check.failures||library_move_check.failures)exit_code=45;
            if(restore_check.failures)exit_code=28;
            if(lod_check.failures)exit_code=29;
            if(export_check.failures)exit_code=30;
            if(legacy_img_check.failures)exit_code=31;
            if(legacy_media_check.failures)exit_code=32;
            if(halfbrite_check.failures)exit_code=82;
            if(sheet_check.failures)exit_code=33;
            if(stage_check.failures)exit_code=34;
            if(anchor_link_check.failures)exit_code=35;
            if(points_check.failures)exit_code=36;
            if(table_compare_check.failures)exit_code=37;
            if(strikes_check.failures)exit_code=38;
            if(asm_artwork_reuse_check.failures||asm_artwork_copy_check.failures||asm_library_check.failures||asm_assets_check.failures||asm_check.failures||reactions_check.failures||fatality_check.failures||fatality_workspace_check.failures)exit_code=39;
            if(bindings_check.failures)exit_code=40;
            if(phases_check.failures)exit_code=41;
            if(!app.error.empty())exit_code=2;if(editing_check.failures)exit_code=4;if(project_check.failures)exit_code=5;if(motion_check.failures)exit_code=6;if(trigger_check.failures)exit_code=7;if(embedded_check.failures)exit_code=8;if(palette_check.failures)exit_code=9;if(merge_check.failures)exit_code=10;if(compact_check.failures)exit_code=11;if(ramp_check.failures)exit_code=12;if(import_check.failures)exit_code=13;if(digitize_check.failures)exit_code=14;if(video_check.failures)exit_code=15;if(job_check.failures)exit_code=16;app.running=false;
        }
        SDL_RenderPresent(app.renderer);
        if(first_frame){startup.stage("Editor rendered successfully");first_frame=false;}
    }
    app.dummy_body.plan={};app.waterline.plan={};app.subframe_swap.plan={};app.clear_overlay_frame_textures();app.clear_sprite_ramp_textures();app.clear_opacity_gradient_textures();app.clear_region_remap_textures();app.clear_isolation_frames_textures();app.clear_palette_isolation_textures();app.clear_palette_move_textures();app.clear_palette_zero_textures();app.clear_palette_removal_textures();app.clear_palette_transparency_textures();app.clear_sheet_preview();app.clear_indexed_preview();app.stop_video();app.stop_digitize();app.clear_import_textures();app.clear_region_preview();app.clear_layer_drag();app.clear_floating();app.clear_paint_line();app.clear_variant();app.clear_batch_textures();app.clear_costume_textures();app.clear_ramp_transfer_textures();app.clear_batch_palette_textures();app.clear_batch_delete_preview();app.clear_library_copy_preview();app.clear_restore_textures();app.clear_lod_preview();app.lod.review={};app.asm_ui.assets=studio::AsmAssetsUi{};app.tabs.clear();if(app.paste_texture)SDL_DestroyTexture(app.paste_texture);if(app.palettes.preview)SDL_DestroyTexture(app.palettes.preview);if(app.palettes.original)SDL_DestroyTexture(app.palettes.original);ImGui_ImplSDLRenderer2_Shutdown();ImGui_ImplSDL2_Shutdown();ImGui::DestroyContext();SDL_DestroyRenderer(app.renderer);SDL_DestroyWindow(app.window);SDL_Quit();startup.stage("Closed normally");return exit_code;
}

int main(int argc,char **argv){
    SDL_SetMainReady();bool quiet=false,software=false;std::string log_folder;
    for(int i=1;i<argc;++i){auto arg=std::string(argv[i]);if(arg=="--software-renderer")software=true;if(arg.rfind("--smoke",0)==0)quiet=true;}
    if(quiet&&argc>=3){std::error_code ec;log_folder=std::filesystem::absolute(std::filesystem::u8path(argv[2]),ec).parent_path().u8string();}
    else{char *pref=SDL_GetPrefPath("midway-imgtool","studio");log_folder=pref?pref:".";SDL_free(pref);}
    studio::StartupLog startup(log_folder);startup.stage("Starting Imgtool Studio");
    char *base=SDL_GetBasePath();startup.write(std::string("Build folder: ")+(base?base:"unknown"));SDL_free(base);
#ifdef _WIN32
    studio::startup_log=&startup;auto previous=SetUnhandledExceptionFilter(studio::startup_exception);
#endif
    int result=1;try{result=studio_run(argc,argv,startup,quiet,software);}catch(const std::exception &e){startup.failure(e.what(),quiet);}catch(...){startup.failure("Unexpected error while starting Studio.",quiet);}
#ifdef _WIN32
    SetUnhandledExceptionFilter(previous);studio::startup_log=nullptr;
#endif
    return result;
}
