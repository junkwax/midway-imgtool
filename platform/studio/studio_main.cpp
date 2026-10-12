#define IMGUI_DEFINE_MATH_OPERATORS
#include "studio_document.h"
#include "studio_theme.h"
#include "studio_animation.h"
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
#include <filesystem>
#include <fstream>
#include <unordered_map>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <SDL_syswm.h>
#endif

namespace studio {
namespace fs=std::filesystem;
enum class Tool { Select, Pencil, Erase, Fill, Pick, Pan };
enum class Action { Open, Save, SaveAs, Undo, Redo, Copy, Paste, Clear, Fit, ZoomIn, ZoomOut, Demo, Close, Quit,
    NewLibrary, NewSprite, Rename, Duplicate, Delete, MoveUp, MoveDown, Resize, CanvasSize, Trim, Transform, ImportPng, ExportPng };
enum class EditDialog { None, NewSprite, Rename, Delete, Resize, CanvasSize, Transform, ImportPng, ExportPng };
struct Selection { bool valid=false; int x=0,y=0,w=0,h=0; };
struct Texture { SDL_Texture *value=nullptr; uint64_t revision=0; };
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
struct Tab {
    std::unique_ptr<Artwork> art;
    uint64_t uid=0;
    uint64_t selected_id=0,seen_structure=0;
    int selected=0,color=8,radius=1;
    float zoom=0;
    ImVec2 pan{0,0};
    bool flip=false,grid=false,anchors=true,reveal_selection=false;
    bool match_palette=true;
    int view_w=0,view_h=0;
    MirrorConvention convention=MirrorConvention_Ani2;
    Selection selection;
    ImGuiTextFilter filter;
    std::unordered_map<int,Texture> textures;
    uint64_t check_revision=0;
    int ppp=6, checked_ppp=-1;
    L2Report report;
    AlignmentView alignment;
    ~Tab(){for(auto &v:textures)if(v.second.value)SDL_DestroyTexture(v.second.value);}
    std::string untitled="Sample library";
    std::string name() const {return art->path().empty()?untitled:fs::u8path(art->path()).filename().u8string();}
};

static constexpr ImGuiWindowFlags panel_flags=ImGuiWindowFlags_NoSavedSettings;
static std::string native_file(SDL_Window *window,bool save,const std::string &initial,bool png=false) {
#ifdef _WIN32
    wchar_t name[32768]={}; auto value=fs::u8path(initial).wstring();
    if(save && value.size()<32768)std::copy(value.begin(),value.end(),name);
    OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.lpstrFile=name;ofn.nMaxFile=32768;
    ofn.lpstrFilter=png?L"PNG image\0*.png;*.PNG\0":L"Midway IMG library\0*.IMG;*.img\0All files\0*.*\0";
    ofn.lpstrDefExt=png?L"png":L"img";ofn.lpstrTitle=png?(save?L"Export sprite PNG - Imgtool Studio":L"Import sprite PNG - Imgtool Studio"):(save?L"Save IMG library - Imgtool Studio":L"Open IMG library - Imgtool Studio");
    ofn.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    SDL_SysWMinfo wm{};SDL_VERSION(&wm.version);
    if(SDL_GetWindowWMInfo(window,&wm))ofn.hwndOwner=wm.info.win.window;
    if(save?GetSaveFileNameW(&ofn):GetOpenFileNameW(&ofn))return fs::path(name).u8string();
#else
    (void)window;(void)save;(void)initial;(void)png;
#endif
    return {};
}

class App {
public:
    SDL_Window *window=nullptr;
    SDL_Renderer *renderer=nullptr;
    std::vector<std::unique_ptr<Tab>> tabs;
    int active=-1;
    uint64_t next_uid=1,close_uid=0,remove_uid=0,select_uid=0;
    Tool tool=Tool::Select;
    Clipboard clipboard;
    SDL_Texture *paste_texture=nullptr;
    uint64_t paste_generation=0,paste_uid=0;
    int paste_sprite=-1;
    bool running=true,quitting=false,help=false,paste_mode=false,stroke=false,select_drag=false,anchor_drag=false;
    ImVec2 anchor_drag_mouse;
    int anchor_start_x=0,anchor_start_y=0;
    int start_x=0,start_y=0,last_x=0,last_y=0;
    std::string error,status="Open an IMG library or explore the sample.",prefs,ini;
    std::vector<std::string> recent;
    bool path_dialog=false,path_save=false;
    uint64_t path_uid=0;
    char path_input[2048]={};
    // Hidden rendering checks use the evaluated placement to drive real gestures.
    bool gesture_smoke=false;
    ImVec2 gesture_point;
    float gesture_zoom=1;
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
    ImVec2 edit_apply_point;

    Tab *tab(){return active>=0 && active<(int)tabs.size()?tabs[active].get():nullptr;}
    Tab *find(uint64_t uid){for(auto &t:tabs)if(t->uid==uid)return t.get();return nullptr;}
    void finish(){if(tab()){tab()->art->commit();tab()->alignment.dragging=false;}stroke=false;select_drag=false;anchor_drag=false;}
    void recent_add(const std::string &path){
        recent.erase(std::remove(recent.begin(),recent.end(),path),recent.end());recent.insert(recent.begin(),path);
        if(recent.size()>10)recent.resize(10);
        std::ofstream f(fs::u8path(prefs)/"recent.txt");for(auto &p:recent)f<<p<<'\n';
    }
    void open(const std::string &path){
        finish();paste_mode=false;
        std::error_code ec;auto absolute=fs::absolute(fs::u8path(path),ec).lexically_normal();
        if(ec){error=ec.message();return;}
        for(int i=0;i<(int)tabs.size();++i)if(!tabs[i]->art->path().empty() && fs::equivalent(absolute,fs::u8path(tabs[i]->art->path()),ec)){active=i;select_uid=tabs[i]->uid;return;}
        auto doc=Artwork::open(absolute.u8string(),error);if(!doc)return;
        auto t=std::make_unique<Tab>();t->uid=next_uid++;t->art=std::move(doc);t->color=1;t->selected_id=t->art->sprite_id(0);
        t->alignment.initialize(*t->art);sync_alignment(*t);
        tabs.push_back(std::move(t));active=(int)tabs.size()-1;select_uid=tabs.back()->uid;recent_add(absolute.u8string());status="Opened "+absolute.filename().u8string();
    }
    void demo(){finish();paste_mode=false;auto t=std::make_unique<Tab>();t->uid=next_uid++;t->art=Artwork::demo();t->selected_id=t->art->sprite_id(0);t->alignment.initialize(*t->art);sync_alignment(*t);tabs.push_back(std::move(t));active=(int)tabs.size()-1;select_uid=tabs.back()->uid;status="Sample artwork. Save As to create your own IMG.";}
    void synchronize(Tab &t){
        bool structural=t.seen_structure!=t.art->structure_generation();
        if(!structural&&t.selected_id==t.art->selection_id())return;
        int found=t.art->sprite_index(t.art->selection_id());t.selected=found>=0?found:std::clamp(t.selected,0,t.art->count()-1);t.selected_id=t.art->sprite_id(t.selected);t.art->select_sprite(t.selected_id);
        t.alignment.player.playing=false;if(structural)t.alignment.initialize(*t.art);sync_alignment(t);
        if(structural){for(auto &cache:t.textures)if(cache.second.value)SDL_DestroyTexture(cache.second.value);t.textures.clear();}
        auto im=t.art->sprite(t.selected);if(t.view_w!=im->w||t.view_h!=im->h)t.zoom=0;
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
        auto t=tab();if(a==Action::Open||a==Action::Demo||a==Action::Quit||a==Action::NewLibrary)return true;
        if(!t)return false;
        if(a==Action::Undo)return t->art->can_undo();if(a==Action::Redo)return t->art->can_redo();
        if(a==Action::Paste)return !clipboard.pixels.empty()&&!t->flip&&!t->alignment.enabled;
        if(a==Action::Clear)return t->selection.valid&&!t->flip&&!t->alignment.enabled;
        if(a==Action::MoveUp)return t->selected>0;
        if(a==Action::MoveDown)return t->selected+1<t->art->count();
        if(a==Action::Delete)return t->art->count()>1;
        return true;
    }
    void action(Action a){
        if(!enabled(a))return;finish();auto t=tab();
        if(t&&a!=Action::Fit&&a!=Action::ZoomIn&&a!=Action::ZoomOut)t->alignment.player.playing=false;
        switch(a){
        case Action::Open:choose_open();break;
        case Action::Demo:demo();break;
        case Action::NewLibrary:new_library();break;
        case Action::NewSprite:request_edit(EditDialog::NewSprite);break;
        case Action::Rename:request_edit(EditDialog::Rename);break;
        case Action::Delete:request_edit(EditDialog::Delete);break;
        case Action::Resize:request_edit(EditDialog::Resize);break;
        case Action::CanvasSize:request_edit(EditDialog::CanvasSize);break;
        case Action::Transform:request_edit(EditDialog::Transform);break;
        case Action::ImportPng:request_edit(EditDialog::ImportPng);break;
        case Action::ExportPng:request_edit(EditDialog::ExportPng);break;
        case Action::Duplicate:{int added=t->art->duplicate_sprite(t->selected,error);if(added>=0){t->selected=added;t->selected_id=t->art->sprite_id(added);t->art->select_sprite(t->selected_id);}break;}
        case Action::Trim:t->art->trim_sprite(t->selected,error);break;
        case Action::MoveUp:case Action::MoveDown:t->art->move_sprite(t->selected,t->selected+(a==Action::MoveUp?-1:1),error);break;
        case Action::Save:save(*t,false);break;
        case Action::SaveAs:save(*t,true);break;
        case Action::Undo:t->art->undo();break;
        case Action::Redo:t->art->redo();break;
        case Action::Copy:{auto im=t->art->sprite(t->selected);auto r=t->selection;
            if(paste_texture){SDL_DestroyTexture(paste_texture);paste_texture=nullptr;}
            clipboard=t->art->copy(t->selected,r.valid?r.x:0,r.valid?r.y:0,r.valid?r.w:im->w,r.valid?r.h:im->h);
            status="Copied pixels and palette. Paste, then click the canvas to place.";break;}
        case Action::Paste:paste_mode=true;status="Click to place. Escape cancels. Colors map to the destination palette.";break;
        case Action::Clear:{auto r=t->selection;t->art->clear(t->selected,r.x,r.y,r.w,r.h);break;}
        case Action::Fit:if(t->alignment.enabled){t->alignment.zoom=0;t->alignment.pan=ImVec2();}else{t->zoom=0;t->pan=ImVec2();}break;
        case Action::ZoomIn:case Action::ZoomOut:{float &zoom=t->alignment.enabled?t->alignment.zoom:t->zoom;
            zoom=std::clamp((zoom>0?zoom:1)*(a==Action::ZoomIn?1.25f:.8f),t->alignment.enabled?.25f:1.f,64.f);break;}
        case Action::Close:close_uid=t->uid;break;
        case Action::Quit:edit_dialog=EditDialog::None;help=false;path_dialog=false;quitting=true;if(t)close_uid=t->uid;else running=false;break;
        }
    }
    void item(const char *label,const char *shortcut,Action a){if(ImGui::MenuItem(label,shortcut,false,enabled(a)))action(a);}
    void button(const char *label,Action a){ImGui::BeginDisabled(!enabled(a));if(ImGui::Button(label))action(a);ImGui::EndDisabled();}
    void sync_alignment(Tab &t){
        auto &v=t.alignment;int sequence,frame;
        if(locate_preview_sprite(v.sequences,t.selected,sequence,frame)){
            v.sequence=sequence;v.player.seek(frame,(int)v.sequences[sequence].frames.size());
            auto &item=v.sequences[sequence].frames[frame];
            if(v.enabled)v.multipart=std::find(item.pieces.begin(),item.pieces.end(),t.selected)!=item.pieces.end();
        }
    }
    void select(int index){finish();if(auto t=tab()){t->selected=index;t->selected_id=t->art->sprite_id(index);t->art->select_sprite(t->selected_id);t->reveal_selection=true;t->selection={};paste_mode=false;t->alignment.player.playing=false;sync_alignment(*t);auto im=t->art->sprite(index);auto p=t->art->palette(im->palnum);t->color=std::min(t->color,(int)p->numc-1);}}
    void select_frame(Tab &t,int index,bool pause=true){
        auto &v=t.alignment;finish();
        if(pause)v.player.playing=false;
        v.player.seek(index,(int)v.sequences[v.sequence].frames.size());
        if(auto frame=v.frame()){auto pieces=preview_pieces(*frame,v.multipart);t.selected=pieces.front();t.selected_id=t.art->sprite_id(t.selected);t.art->select_sprite(t.selected_id);t.reveal_selection=true;t.selection={};}
    }
    void workspace(Tab &t,bool alignment){
        finish();paste_mode=false;t.alignment.player.playing=false;t.alignment.enabled=alignment;
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
        std::vector<unsigned char> rgba((size_t)im->w*im->h*4);int stride=(im->w+3)&~3;
        for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){int ci=((unsigned char *)im->data_p)[y*stride+x];auto q=&rgba[((size_t)y*im->w+x)*4];
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
        for(size_t i=0;i<clipboard.pixels.size();++i){auto c=clipboard.pixels[i];auto out=rgba.data()+i*4;pal_word_to_rgb8((unsigned char *)p->data_p+map[c]*2,out,out+1,out+2);out[3]=c?190:0;}
        SDL_SetTextureBlendMode(paste_texture,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(paste_texture,SDL_ScaleModeNearest);
        SDL_UpdateTexture(paste_texture,nullptr,rgba.data(),clipboard.w*4);
        paste_generation=t.art->generation();paste_uid=t.uid;paste_sprite=t.selected;return paste_texture;
    }
    void menu(){
        if(!ImGui::BeginMenuBar())return;
        ImGui::TextColored(accent_surface(1),"imgtool");ImGui::SameLine();ImGui::TextDisabled("STUDIO");ImGui::Separator();
        if(ImGui::BeginMenu("File")){item("New IMG library","Ctrl+N",Action::NewLibrary);item("Open IMG...","Ctrl+O",Action::Open);
            if(ImGui::BeginMenu("Open recent",!recent.empty())){auto list=recent;for(auto &p:list)if(ImGui::MenuItem(p.c_str()))open(p);ImGui::EndMenu();}
            item("Save","Ctrl+S",Action::Save);item("Save As...","Ctrl+Shift+S",Action::SaveAs);ImGui::Separator();
            item("Import PNG sprite...",nullptr,Action::ImportPng);item("Export selected PNG...",nullptr,Action::ExportPng);ImGui::Separator();
            item("Explore sample",nullptr,Action::Demo);item("Close library","Ctrl+W",Action::Close);item("Quit",nullptr,Action::Quit);ImGui::EndMenu();}
        if(ImGui::BeginMenu("Edit")){item("Undo","Ctrl+Z",Action::Undo);item("Redo","Ctrl+Y",Action::Redo);ImGui::Separator();item("Copy pixels","Ctrl+C",Action::Copy);item("Paste pixels","Ctrl+V",Action::Paste);item("Clear selection","Delete",Action::Clear);ImGui::EndMenu();}
        if(ImGui::BeginMenu("Sprite",tab()!=nullptr)){
            item("New sprite...",nullptr,Action::NewSprite);item("Rename...","F2",Action::Rename);item("Duplicate","Ctrl+J",Action::Duplicate);item("Delete sprite...",nullptr,Action::Delete);
            ImGui::Separator();item("Move up",nullptr,Action::MoveUp);item("Move down",nullptr,Action::MoveDown);ImGui::Separator();
            item("Resize artwork...",nullptr,Action::Resize);item("Canvas size...",nullptr,Action::CanvasSize);item("Trim transparent bounds",nullptr,Action::Trim);item("Rotate / flip...",nullptr,Action::Transform);ImGui::EndMenu();
        }
        if(ImGui::BeginMenu("View")){item("Fit artwork","Ctrl+0",Action::Fit);item("Zoom in","Ctrl++",Action::ZoomIn);item("Zoom out","Ctrl+-",Action::ZoomOut);if(tab()){ImGui::MenuItem("Pixel grid",nullptr,&tab()->grid);ImGui::MenuItem("Animation anchor",nullptr,&tab()->anchors);}ImGui::EndMenu();}
        if(ImGui::BeginMenu("Help")){if(ImGui::MenuItem("Using Studio"))help=true;ImGui::EndMenu();}
        ImGui::EndMenuBar();
    }
    void shortcuts(){
        if(ImGui::GetIO().WantTextInput || ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel))return;
        const auto route=ImGuiInputFlags_RouteGlobal;
        struct Key {ImGuiKeyChord key;Action action;};
        const Key keys[]={{ImGuiMod_Ctrl|ImGuiKey_N,Action::NewLibrary},{ImGuiMod_Ctrl|ImGuiKey_J,Action::Duplicate},{ImGuiKey_F2,Action::Rename},{ImGuiMod_Ctrl|ImGuiKey_O,Action::Open},{ImGuiMod_Ctrl|ImGuiKey_S,Action::Save},{ImGuiMod_Ctrl|ImGuiMod_Shift|ImGuiKey_S,Action::SaveAs},
            {ImGuiMod_Ctrl|ImGuiKey_Z,Action::Undo},{ImGuiMod_Ctrl|ImGuiKey_Y,Action::Redo},{ImGuiMod_Ctrl|ImGuiKey_C,Action::Copy},{ImGuiMod_Ctrl|ImGuiKey_V,Action::Paste},
            {ImGuiMod_Ctrl|ImGuiKey_W,Action::Close},{ImGuiMod_Ctrl|ImGuiKey_0,Action::Fit},{ImGuiMod_Ctrl|ImGuiKey_Equal,Action::ZoomIn},{ImGuiMod_Ctrl|ImGuiKey_Minus,Action::ZoomOut},{ImGuiKey_Delete,Action::Clear}};
        for(auto &k:keys)if(ImGui::Shortcut(k.key,route))action(k.action);
        auto t=tab();if(!t)return;
        if(ImGui::Shortcut(ImGuiKey_Escape,route)){t->art->cancel();t->alignment.dragging=false;stroke=false;select_drag=false;anchor_drag=false;paste_mode=false;t->selection={};}
        if(ImGui::Shortcut(ImGuiKey_Tab,route))workspace(*t,!t->alignment.enabled);
        if(t->alignment.enabled){
            auto &v=t->alignment;
            if(ImGui::Shortcut(ImGuiKey_Space,route))toggle_playback(*t);
            if(ImGui::Shortcut(ImGuiKey_LeftArrow,route)||ImGui::Shortcut(ImGuiKey_UpArrow,route))select_frame(*t,v.player.frame? v.player.frame-1:(int)v.sequences[v.sequence].frames.size()-1);
            if(ImGui::Shortcut(ImGuiKey_RightArrow,route)||ImGui::Shortcut(ImGuiKey_DownArrow,route))select_frame(*t,(v.player.frame+1)%(int)v.sequences[v.sequence].frames.size());
            return;
        }
        if(ImGui::Shortcut(ImGuiKey_UpArrow,route))select((t->selected+t->art->count()-1)%t->art->count());
        if(ImGui::Shortcut(ImGuiKey_DownArrow,route))select((t->selected+1)%t->art->count());
        const ImGuiKey tool_keys[]={ImGuiKey_R,ImGuiKey_P,ImGuiKey_E,ImGuiKey_G,ImGuiKey_I,ImGuiKey_H};
        for(int i=0;i<6;++i)if(ImGui::Shortcut(tool_keys[i],route)){finish();tool=(Tool)i;paste_mode=false;}
    }
    void library(Tab &t){
        ImGui::TextUnformatted("SPRITES");ImGui::SameLine();ImGui::TextDisabled("%d",t.art->count());
        ImGui::SetNextItemWidth(-1);if(ImGui::InputTextWithHint("##search","Search sprites...",t.filter.InputBuf,sizeof t.filter.InputBuf))t.filter.Build();
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Search sprite names. Commas include alternatives; -name excludes.");
        ImGui::TextDisabled("Select to edit  /  Up & Down to browse");ImGui::Separator();
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
    void tool_shelf(Tab &t){
        const char *names[]={"Select","Pencil","Erase","Fill","Pick","Pan"};
        const char *tips[]={"R: drag a rectangle to select pixels","P: draw with the selected palette index","E: paint transparency","G: replace connected pixels of the same index","I: pick a palette index from the sprite","H: pan; middle-drag works with any tool"};
        // Wrap buttons when the canvas column is narrow.
        float right=ImGui::GetCursorScreenPos().x+ImGui::GetContentRegionAvail().x;
        for(int i=0;i<6;++i){if(i && ImGui::GetItemRectMax().x+ImGui::GetStyle().ItemSpacing.x+ImGui::CalcTextSize(names[i]).x+ImGui::GetStyle().FramePadding.x*2<right)ImGui::SameLine();
            bool selected_tool=tool==(Tool)i;
            if(selected_tool)ImGui::PushStyleColor(ImGuiCol_Button,accent_surface(.50f));
            if(ImGui::Button(names[i])){finish();tool=(Tool)i;paste_mode=false;}
            if(selected_tool)ImGui::PopStyleColor();
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",tips[i]);
        }
        if(ImGui::GetItemRectMax().x+65<right)ImGui::SameLine();
        if(ImGui::Button("Fit"))action(Action::Fit);
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Fit artwork to the canvas (Ctrl+0)");
        if(paste_mode)ImGui::TextColored(accent_surface(1),"PASTE  |  click to place, Esc to cancel");
        else if(t.flip)ImGui::TextDisabled("Flipped preview  |  turn off to edit pixels");
        else ImGui::TextDisabled("%s  |  wheel zooms, middle-drag pans",names[(int)tool]);
    }
    void canvas(Tab &t,float height){
        ImGui::BeginChild("canvas",ImVec2(0,height),ImGuiChildFlags_Border,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        auto im=t.art->sprite(t.selected);auto io=ImGui::GetIO();auto pos=ImGui::GetCursorScreenPos();auto available=ImGui::GetContentRegionAvail();
        t.view_w=im->w;t.view_h=im->h;
        available.x=std::max(10.f,available.x);available.y=std::max(10.f,available.y);
        ImGui::InvisibleButton("##surface",available,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle|ImGuiButtonFlags_MouseButtonRight);
        bool hovered=ImGui::IsItemHovered(),held=ImGui::IsItemActive();
        float fit=std::max(1.f,std::floor(std::min((available.x-60)/im->w,(available.y-60)/im->h)));
        float z=t.zoom>0?t.zoom:std::min(16.f,fit);
        if(hovered && io.MouseWheel!=0){t.zoom=std::clamp(z*(io.MouseWheel>0?1.25f:.8f),1.f,64.f);z=t.zoom;}
        if(hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))t.pan+=io.MouseDelta;
        if(held && tool==Tool::Pan && ImGui::IsMouseDragging(ImGuiMouseButton_Left))t.pan+=io.MouseDelta;
        ImVec2 size(im->w*z,im->h*z),origin=pos+(available-size)*.5f+t.pan;
        ImVec2 anchor=origin+ImVec2((short)im->anix*z,(short)im->aniy*z);
        if(t.flip)origin.x=anchor.x-anipoint_effective((short)im->anix,im->w,true,t.convention)*z;
        auto dl=ImGui::GetWindowDrawList();dl->PushClipRect(pos,pos+available,true);
        dl->AddRectFilled(pos,pos+available,IM_COL32(18,21,27,255));
        ImVec2 ca=ImMax(pos,origin),cb=ImMin(pos+available,origin+size);
        for(float yy=ca.y;yy<cb.y;yy+=12)for(float xx=ca.x;xx<cb.x;xx+=12){int cellx=(int)std::floor((xx-origin.x)/12),celly=(int)std::floor((yy-origin.y)/12);dl->AddRectFilled(ImVec2(xx,yy),ImMin(ImVec2(xx+12,yy+12),cb),((cellx+celly)&1)?IM_COL32(41,44,51,255):IM_COL32(33,36,43,255));}
        if(auto tex=texture(t,t.selected))dl->AddImage((ImTextureID)(intptr_t)tex,origin,origin+size,ImVec2(t.flip?1.f:0.f,0),ImVec2(t.flip?0.f:1.f,1));
        dl->AddRect(origin,origin+size,IM_COL32(90,96,106,255));
        if(t.grid && z>=6){for(int x=0;x<=im->w;++x)if(origin.x+x*z>=pos.x&&origin.x+x*z<=pos.x+available.x)dl->AddLine(origin+ImVec2(x*z,0),origin+ImVec2(x*z,size.y),IM_COL32(10,12,16,90));for(int y=0;y<=im->h;++y)if(origin.y+y*z>=pos.y&&origin.y+y*z<=pos.y+available.y)dl->AddLine(origin+ImVec2(0,y*z),origin+ImVec2(size.x,y*z),IM_COL32(10,12,16,90));}
        if(t.anchors){dl->AddLine(anchor-ImVec2(12,0),anchor+ImVec2(12,0),IM_COL32(239,193,94,255),1.5f);dl->AddLine(anchor-ImVec2(0,12),anchor+ImVec2(0,12),IM_COL32(239,193,94,255),1.5f);dl->AddCircle(anchor,4,IM_COL32(239,193,94,255));}
        int x=(int)std::floor((io.MousePos.x-origin.x)/z),y=(int)std::floor((io.MousePos.y-origin.y)/z);
        bool inside=x>=0&&y>=0&&x<im->w&&y<im->h;
        bool on_anchor=tool==Tool::Select&&t.anchors&&!t.flip&&std::abs(io.MousePos.x-anchor.x)<12&&std::abs(io.MousePos.y-anchor.y)<12;
        if(hovered&&on_anchor&&!paste_mode&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
            finish();anchor_drag=true;anchor_drag_mouse=io.MousePos;anchor_start_x=(short)im->anix;anchor_start_y=(short)im->aniy;t.art->begin("Move animation anchor");
        }
        if(anchor_drag&&ImGui::IsMouseDown(ImGuiMouseButton_Left))t.art->anchor(t.selected,
            anchor_start_x+(int)std::round((io.MousePos.x-anchor_drag_mouse.x)/z),anchor_start_y+(int)std::round((io.MousePos.y-anchor_drag_mouse.y)/z));
        if(hovered&&on_anchor)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if(hovered && inside && ImGui::IsMouseClicked(ImGuiMouseButton_Right))t.color=((unsigned char *)im->data_p)[y*((im->w+3)&~3)+(t.flip?im->w-1-x:x)];
        if(hovered && inside && !t.flip && !anchor_drag && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
            if(paste_mode){t.art->paste(t.selected,clipboard,x,y);paste_mode=false;}
            else if(tool==Tool::Pick)t.color=((unsigned char *)im->data_p)[y*((im->w+3)&~3)+x];
            else if(tool==Tool::Fill)t.art->fill(t.selected,x,y,(unsigned char)t.color);
            else if(tool==Tool::Select){select_drag=true;start_x=x;start_y=y;t.selection={true,x,y,1,1};}
            else if(tool==Tool::Pencil||tool==Tool::Erase){stroke=true;last_x=x;last_y=y;t.art->begin(tool==Tool::Erase?"Erase pixels":"Paint pixels");}
        }
        if(stroke && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !t.flip){
            // Interpolate within the image; cap out-of-canvas coordinates so a
            // fast drag cannot turn into an unbounded loop.
            int nx=std::clamp(x,-32,(int)im->w+32),ny=std::clamp(y,-32,(int)im->h+32);
            int steps=std::max(std::abs(nx-last_x),std::abs(ny-last_y));
            for(int n=0;n<=steps;++n){float f=steps?(float)n/steps:0;t.art->paint(t.selected,(int)std::round(last_x+(nx-last_x)*f),(int)std::round(last_y+(ny-last_y)*f),(unsigned char)(tool==Tool::Erase?0:t.color),t.radius);}
            last_x=nx;last_y=ny;
        }
        if(select_drag && ImGui::IsMouseDown(ImGuiMouseButton_Left)){int xx=std::clamp(x,0,(int)im->w-1),yy=std::clamp(y,0,(int)im->h-1);t.selection={true,std::min(start_x,xx),std::min(start_y,yy),std::abs(start_x-xx)+1,std::abs(start_y-yy)+1};}
        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)){if(stroke||anchor_drag)t.art->commit();stroke=false;select_drag=false;anchor_drag=false;}
        if(t.selection.valid&&!t.flip){auto r=t.selection;dl->AddRect(origin+ImVec2(r.x*z,r.y*z),origin+ImVec2((r.x+r.w)*z,(r.y+r.h)*z),IM_COL32(236,218,162,255),0,0,1.5f);}
        if(paste_mode&&hovered&&!t.flip){ImVec2 a=origin+ImVec2(x*z,y*z),b=a+ImVec2(clipboard.w*z,clipboard.h*z);
            dl->PushClipRect(ImMax(pos,origin),ImMin(pos+available,origin+size),true);
            if(auto tex=paste_preview(t))dl->AddImage((ImTextureID)(intptr_t)tex,a,b);
            dl->PopClipRect();dl->AddRect(a,b,ImGui::GetColorU32(ImGuiCol_CheckMark),0,0,2);}
        if(hovered&&inside&&!t.flip&&(tool==Tool::Pencil||tool==Tool::Erase)){auto a=origin+ImVec2(x*z,y*z);dl->AddRect(a,a+ImVec2(z,z),IM_COL32(255,255,255,230));}
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
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Preview duration for this frame. Not written to IMG; scene timing persistence will arrive with WAX support.");
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
        ImGui::BeginChild("swatches",ImVec2(0,0),ImGuiChildFlags_None);
        int columns=std::max(1,(int)(ImGui::GetContentRegionAvail().x/27));
        for(int i=0;i<p->numc;++i){if(i%columns)ImGui::SameLine(0,3);ImGui::PushID(i);
            unsigned char r,g,b;pal_word_to_rgb8((unsigned char *)p->data_p+i*2,&r,&g,&b);
            ImVec2 a=ImGui::GetCursorScreenPos();if(ImGui::ColorButton("##swatch",ImVec4(r/255.f,g/255.f,b/255.f,i?1.f:0.f),ImGuiColorEditFlags_NoTooltip,ImVec2(24,24))){t.art->commit();t.color=i;}
            if(t.color==i)ImGui::GetWindowDrawList()->AddRect(a-ImVec2(1,1),a+ImVec2(25,25),IM_COL32(255,235,210,255),2,0,2);
            if(ImGui::IsItemHovered())ImGui::SetTooltip(i?"Index %d  |  RGB %d, %d, %d":"Index 0  |  Transparent",i,r,g,b);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    void inspector(Tab &t){
        auto im=t.art->sprite(t.selected);auto p=t.art->palette(im->palnum);
        ImGui::TextUnformatted("SPRITE INSPECTOR");ImGui::Separator();ImGui::TextUnformatted(im->n_s);
        ImGui::TextDisabled("%d x %d pixels  |  %d of %d",im->w,im->h,t.selected+1,t.art->count());
        if(!t.alignment.enabled){
            ImGui::Spacing();if(ImGui::Button("Rename"))request_edit(EditDialog::Rename);ImGui::SameLine();if(ImGui::Button("Resize"))request_edit(EditDialog::Resize);ImGui::SameLine();if(ImGui::Button("More..."))ImGui::OpenPopup("sprite-more");
            if(ImGui::BeginPopup("sprite-more")){
                auto queue=[&](const char *label,Action a){if(ImGui::MenuItem(label,nullptr,false,enabled(a))){deferred_action=a;deferred_sprite=t.selected_id;have_deferred=true;}};
                queue("New sprite...",Action::NewSprite);queue("Duplicate",Action::Duplicate);queue("Canvas size...",Action::CanvasSize);queue("Trim bounds",Action::Trim);queue("Rotate / flip...",Action::Transform);queue("Export PNG...",Action::ExportPng);queue("Delete...",Action::Delete);ImGui::EndPopup();}
        }
        ImGui::Spacing();ImGui::SeparatorText("Animation anchor");
        int xy[]={(short)im->anix,(short)im->aniy};ImGui::SetNextItemWidth(-1);
        if(ImGui::InputInt2("##anchor",xy)){t.alignment.player.playing=false;t.art->anchor(t.selected,xy[0],xy[1]);}if(ImGui::IsItemDeactivatedAfterEdit())t.art->commit();
        ImGui::TextDisabled("X / Y  |  this sprite only");
        if(!t.alignment.enabled){ImGui::Checkbox("Show anchor",&t.anchors);ImGui::Checkbox("Flipped preview",&t.flip);}
        const char *conventions[]={"Multipart (ani2)","Single part (ganiof)"};int conv=(int)t.convention;ImGui::SetNextItemWidth(-1);
        if(ImGui::Combo("##mirror",&conv,conventions,2))t.convention=(MirrorConvention)conv;
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Preview uses the selected engine's anchor convention. This does not change the IMG.");
        if(t.alignment.enabled){alignment_inspector(t);return;}
        ImGui::Spacing();ImGui::SeparatorText("Active tool");
        if(tool==Tool::Pencil||tool==Tool::Erase){ImGui::SetNextItemWidth(-1);ImGui::SliderInt("##radius",&t.radius,1,16,"Radius: %d px");}
        else if(tool==Tool::Select)ImGui::TextWrapped("Drag a rectangle. Copy with Ctrl+C; Delete clears its pixels.");
        else if(tool==Tool::Fill)ImGui::TextWrapped("Fills connected pixels of the same palette index.");
        else if(tool==Tool::Pick)ImGui::TextWrapped("Click to pick a color. Right-click also picks with any tool.");
        else ImGui::TextWrapped("Drag to pan. The mouse wheel zooms.");
        ImGui::Checkbox("Pixel grid",&t.grid);
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
        ImGui::SeparatorText("Shared palette color");
        t.color=std::clamp(t.color,0,(int)p->numc-1);
        int usage=0;for(int i=0;i<t.art->count();++i)if(t.art->sprite(i)->palnum==im->palnum)++usage;
        ImGui::Text("Index %d",t.color);ImGui::TextDisabled("Used by %d sprites",usage);
        unsigned short word=palette_word_at((unsigned char *)p->data_p,t.color);int rgb[]={(word>>10)&31,(word>>5)&31,word&31};
        ImGui::BeginDisabled(t.color==0);const char *labels[]={"R","G","B"};
        for(int i=0;i<3;++i){ImGui::SetNextItemWidth(-30);if(ImGui::SliderInt(labels[i],&rgb[i],0,31)){t.art->color(im->palnum,t.color,(unsigned short)((rgb[0]<<10)|(rgb[1]<<5)|rgb[2]));}if(ImGui::IsItemDeactivatedAfterEdit())t.art->commit();}
        ImGui::EndDisabled();
        if(t.color==0)ImGui::TextDisabled("Index 0 stays transparent.");else ImGui::TextWrapped("Color changes affect every sprite using this palette. Undo restores the whole change.");
        ImGui::Spacing();
        if(ImGui::CollapsingHeader("Packing checks")){
            ImGui::SetNextItemWidth(-1);ImGui::SliderInt("##ppp",&t.ppp,0,8,"PPP: %d (0 = auto)");
            if(!stroke&&(t.check_revision!=t.art->generation()||t.checked_ppp!=t.ppp)){t.report=t.art->check(t.ppp);t.check_revision=t.art->generation();t.checked_ppp=t.ppp;}
            ImGui::TextWrapped("%d packing changes / %d warnings",t.report.break_count,t.report.warn_count);
            ImGui::TextDisabled("Compared with the loaded IMG");
            for(auto &issue:t.report.issues){ImGui::PushID(&issue);if(ImGui::Selectable(issue.img_name.c_str()))select(issue.img_idx);ImGui::TextWrapped("%s",issue.message.c_str());ImGui::PopID();}
        }
    }
    void welcome(){
        ImGui::SetCursorPosY(ImGui::GetCursorPosY()+50);
        float width=std::min(650.f,ImGui::GetContentRegionAvail().x-32);ImGui::SetCursorPosX((ImGui::GetWindowWidth()-width)*.5f);
        ImGui::BeginChild("welcome",ImVec2(width,0));
        ImGui::TextColored(accent_surface(1),"IMGTOOL STUDIO");ImGui::Spacing();
        ImGui::SetWindowFontScale(1.75f);ImGui::TextUnformatted("Make every pixel count.");ImGui::SetWindowFontScale(1);
        ImGui::Spacing();ImGui::TextWrapped("Open a Midway IMG library to browse sprites, edit indexed colors, and check animation anchors.");ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button,accent_surface(.5f));if(ImGui::Button("Open IMG library...",ImVec2(210,42)))choose_open();ImGui::PopStyleColor();
        ImGui::SameLine();if(ImGui::Button("Explore sample",ImVec2(170,42)))demo();
        ImGui::Spacing();ImGui::Spacing();ImGui::SeparatorText("Recent libraries");
        if(recent.empty())ImGui::TextDisabled("Your opened libraries will appear here.");
        auto list=recent;for(auto &p:list){if(ImGui::Selectable(fs::u8path(p).filename().u8string().c_str()))open(p);if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",p.c_str());}
        ImGui::EndChild();
    }
    void editing_dialog(){
        if(edit_dialog!=EditDialog::None)ImGui::OpenPopup("Edit sprite");
        ImGui::SetNextWindowSizeConstraints(ImVec2(480,0),ImVec2(620,900));
        if(!ImGui::BeginPopupModal("Edit sprite",nullptr,ImGuiWindowFlags_AlwaysAutoResize))return;
        auto target=find(edit_doc);int index=target?target->art->sprite_index(edit_sprite):-1;
        auto im=target?target->art->sprite(index):nullptr;
        const char *titles[]={"","New sprite","Rename sprite","Delete sprite","Resize artwork","Canvas size","Rotate / flip artwork","Import PNG sprite","Export PNG sprite"};
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
        ImGui::SameLine();if(ImGui::Button("Cancel",ImVec2(110,0))){edit_dialog=EditDialog::None;edit_error.clear();ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
    void dialogs(){
        editing_dialog();
        if(!error.empty()&&!close_uid)ImGui::OpenPopup("Could not complete action");
        if(ImGui::BeginPopupModal("Could not complete action",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+460);ImGui::TextUnformatted(error.c_str());ImGui::PopTextWrapPos();if(ImGui::Button("OK",ImVec2(100,0))){error.clear();ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
        if(help)ImGui::OpenPopup("Using Imgtool Studio");
        if(ImGui::BeginPopupModal("Using Imgtool Studio",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            ImGui::TextUnformatted("ARTWORK & ALIGNMENT PREVIEW");ImGui::Separator();
            ImGui::TextUnformatted("Open / Save: Ctrl+O / Ctrl+S\nUndo / Redo: Ctrl+Z / Ctrl+Y\nCopy / Paste pixels: Ctrl+C / Ctrl+V\nSelect / Pencil / Erase / Fill / Pick / Pan: R / P / E / G / I / H\nBrowse sprites: Up / Down   |   Fit: Ctrl+0\nZoom: mouse wheel   |   Pan: middle-drag\nCancel paste or selection: Escape");
            ImGui::Spacing();ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+490);
            ImGui::TextUnformatted("This first Studio build edits existing sprite pixels, shared palettes and primary animation anchors. Each library keeps its own undo history. Paste maps colors to the destination palette and leaves transparent pixels untouched.");
            ImGui::Spacing();ImGui::TextUnformatted("File > New IMG library (Ctrl+N), Import PNG sprite and Export selected PNG cover the basic file workflow. Sprite commands manage names (F2), copies (Ctrl+J), order and geometry. Right-click a library row for the same actions. The inspector assigns palettes or makes one private before recoloring. Structural edits and geometry changes undo with their metadata and selection.");
            ImGui::Spacing();ImGui::TextUnformatted("Animation & alignment (Tab): browse name-based frame groups, reconstruct subframes, scrub the timeline and press Space to play/pause. Arrows step frames. World View holds the anchor target fixed; drag a piece to edit its IMG anchor, or enable Drag the whole frame. Escape cancels the gesture. In Artwork, use Select to drag the yellow anchor handle.");
            ImGui::Spacing();ImGui::TextUnformatted("Onion skin, both facing conventions, a floor guide and reference sprites help compare placement. Preview timing and playfield/reference settings stay in this session; they are not written to IMG. WAX projects and embedded/ASM sequence editing are still pending.");
            ImGui::Spacing();ImGui::TextUnformatted("GIF/video/sprite-sheet and legacy imports, advanced palette workflows, batch operations and game-source editing remain in classic imgtool.");
            ImGui::PopTextWrapPos();if(ImGui::Button("Done",ImVec2(100,0))){help=false;ImGui::CloseCurrentPopup();}ImGui::EndPopup();
        }
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
        if(close_uid&&!path_dialog){auto t=find(close_uid);if(!t){close_uid=0;}else if(!t->art->dirty()){remove_uid=close_uid;close_uid=0;}else ImGui::OpenPopup("Save changes?");}
        if(ImGui::BeginPopupModal("Save changes?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            auto t=find(close_uid);ImGui::Text("Save changes to %s?",t?t->name().c_str():"this library");
            if(!error.empty()){ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+420);ImGui::TextColored(accent_surface(1),"%s",error.c_str());ImGui::PopTextWrapPos();}
            if(ImGui::Button("Save",ImVec2(100,0))&&t){if(save(*t,false)){remove_uid=close_uid;close_uid=0;ImGui::CloseCurrentPopup();}else if(path_dialog)ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button("Discard",ImVec2(100,0))){remove_uid=close_uid;close_uid=0;error.clear();ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button("Cancel",ImVec2(100,0))){close_uid=0;quitting=false;error.clear();ImGui::CloseCurrentPopup();}ImGui::EndPopup();
        }
    }
    void frame(){
        for(auto &t:tabs)synchronize(*t);
        shortcuts();
        for(auto &t:tabs)synchronize(*t);
        if(auto t=tab())if(t->alignment.enabled&&!t->alignment.dragging&&!close_uid&&!help&&error.empty()&&!path_dialog&&edit_dialog==EditDialog::None){
            auto &v=t->alignment;int before=v.player.frame;
            v.player.advance(ImGui::GetIO().DeltaTime,v.holds[v.sequence],v.tick_hz);
            if(v.player.frame!=before){auto pieces=preview_pieces(*v.frame(),v.multipart);t->selected=pieces.front();t->selected_id=t->art->sprite_id(t->selected);t->art->select_sprite(t->selected_id);t->reveal_selection=true;t->selection={};}
        }
        ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Studio",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_MenuBar|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        menu();
        if(tabs.empty())welcome();
        else {
            button("Open...",Action::Open);ImGui::SameLine();button("Save",Action::Save);ImGui::SameLine();button("Undo",Action::Undo);ImGui::SameLine();button("Redo",Action::Redo);
            if(auto t=tab()){
                ImGui::SameLine();ImGui::TextDisabled("|");ImGui::SameLine();
                bool artwork=!t->alignment.enabled;
                if(artwork)ImGui::PushStyleColor(ImGuiCol_Button,accent_surface(.45f));
                if(ImGui::Button("Artwork"))workspace(*t,false);
                if(artwork)ImGui::PopStyleColor();ImGui::SameLine();
                if(!artwork)ImGui::PushStyleColor(ImGuiCol_Button,accent_surface(.45f));
                if(ImGui::Button("Animation & alignment"))workspace(*t,true);
                if(!artwork)ImGui::PopStyleColor();
            }
            if(ImGui::BeginTabBar("libraries",ImGuiTabBarFlags_FittingPolicyScroll)){
                for(int i=0;i<(int)tabs.size();++i){auto &t=*tabs[i];std::string label=t.name()+"###library"+std::to_string(t.uid);bool opened=true;
                    ImGuiTabItemFlags flags=t.art->dirty()?ImGuiTabItemFlags_UnsavedDocument:0;
                    // Selected tab is driven by explicit open/navigation, not a changing filename.
                    if(select_uid==t.uid)flags|=ImGuiTabItemFlags_SetSelected;
                    if(ImGui::BeginTabItem(label.c_str(),&opened,flags)){if(select_uid==t.uid)select_uid=0;if(active!=i){if(tab())tab()->alignment.player.playing=false;finish();active=i;paste_mode=false;}ImGui::EndTabItem();}
                    if(!opened){finish();close_uid=t.uid;}
                }ImGui::EndTabBar();
            }
            if(auto t=tab()){
                synchronize(*t);
                ImGui::PushID((int)t->uid);
                float height=std::max(250.f,ImGui::GetContentRegionAvail().y-31);
                if(ImGui::BeginTable("workspace",3,ImGuiTableFlags_Resizable|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp,ImVec2(0,height))){
                    ImGui::TableSetupColumn("Library",ImGuiTableColumnFlags_WidthFixed,245);
                    ImGui::TableSetupColumn("Canvas",ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Inspector",ImGuiTableColumnFlags_WidthFixed,270);ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);ImGui::BeginChild("library",ImVec2(0,height),ImGuiChildFlags_None);library(*t);ImGui::EndChild();
                    ImGui::TableSetColumnIndex(1);ImGui::BeginChild("editor",ImVec2(0,height),ImGuiChildFlags_None);
                    if(t->alignment.enabled){
                        alignment_controls(*t);float canvas_h=std::max(120.f,ImGui::GetContentRegionAvail().y-215);
                        alignment_canvas(*t,canvas_h);animation_timeline(*t,ImGui::GetContentRegionAvail().y);
                    }else{
                        tool_shelf(*t);float canvas_h=std::max(120.f,ImGui::GetContentRegionAvail().y-145);
                        canvas(*t,canvas_h);ImGui::BeginChild("palette",ImVec2(0,0),ImGuiChildFlags_Border);palette_strip(*t);ImGui::EndChild();
                    }ImGui::EndChild();
                    ImGui::TableSetColumnIndex(2);ImGui::BeginChild("inspector",ImVec2(0,height),ImGuiChildFlags_None);inspector(*t);ImGui::EndChild();ImGui::EndTable();
                }
                ImGui::Separator();ImGui::TextDisabled("%s",status.c_str());
                ImGui::PopID();
            }
        }
        dialogs();ImGui::End();
        if(have_deferred){have_deferred=false;if(auto t=tab()){int index=t->art->sprite_index(deferred_sprite);if(index>=0){select(index);action(deferred_action);}}}
        if(remove_uid){for(auto it=tabs.begin();it!=tabs.end();++it)if((*it)->uid==remove_uid){int erased=(int)(it-tabs.begin());tabs.erase(it);if(active>=erased)--active;active=std::clamp(active,0,std::max(0,(int)tabs.size()-1));break;}remove_uid=0;paste_mode=false;}
        if(quitting&&!close_uid){if(tabs.empty())running=false;else close_uid=tabs.back()->uid;}
    }
};
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
}

int main(int argc,char **argv){
    SDL_SetMainReady();if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)!=0)return 1;
    bool alignment_smoke=argc>=3&&std::string(argv[1])=="--smoke-alignment";
    bool gesture_smoke=argc>=3&&std::string(argv[1])=="--smoke-gesture";
    bool editing_smoke=argc>=3&&std::string(argv[1])=="--smoke-editing";
    bool smoke=argc>=3&&(std::string(argv[1])=="--smoke"||alignment_smoke||gesture_smoke||editing_smoke);
    studio::App app;
    app.gesture_smoke=gesture_smoke;
    app.window=SDL_CreateWindow("Imgtool Studio - artwork editor",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,1440,900,SDL_WINDOW_RESIZABLE|(smoke?SDL_WINDOW_HIDDEN:SDL_WINDOW_SHOWN));
    if(!app.window){SDL_Quit();return 1;}SDL_SetWindowMinimumSize(app.window,1000,680);
    app.renderer=SDL_CreateRenderer(app.window,-1,smoke?SDL_RENDERER_SOFTWARE:SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if(!app.renderer)app.renderer=SDL_CreateRenderer(app.window,-1,SDL_RENDERER_SOFTWARE);
    if(!app.renderer){SDL_DestroyWindow(app.window);SDL_Quit();return 1;}
    IMGUI_CHECKVERSION();ImGui::CreateContext();auto &io=ImGui::GetIO();io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    char *pref=SDL_GetPrefPath("midway-imgtool","studio");app.prefs=pref?pref:".";SDL_free(pref);
    if(smoke)app.prefs=std::filesystem::absolute(std::filesystem::u8path(argv[2])).parent_path().u8string();
    app.ini=(std::filesystem::u8path(app.prefs)/"studio-layout.ini").u8string();io.IniFilename=smoke?nullptr:app.ini.c_str();
    {std::ifstream f(std::filesystem::u8path(app.prefs)/"recent.txt");std::string line;while(std::getline(f,line)&&app.recent.size()<10)if(!line.empty())app.recent.push_back(line);}
    studio::apply_theme();
    const char *fonts[]={"C:/Windows/Fonts/segoeui.ttf","/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf","/System/Library/Fonts/Supplemental/Arial.ttf"};
    bool font=false;for(auto path:fonts)if(std::filesystem::exists(path)){font=io.Fonts->AddFontFromFileTTF(path,16)!=nullptr;if(font)break;}if(!font)io.Fonts->AddFontDefault();
    ImGui_ImplSDL2_InitForSDLRenderer(app.window,app.renderer);ImGui_ImplSDLRenderer2_Init(app.renderer);
    if(smoke){
        if(!gesture_smoke&&!editing_smoke&&argc>3&&std::string(argv[3])!="--small")app.open(argv[3]);else app.demo();
        for(int i=3;i<argc;++i)if(std::string(argv[i])=="small"||std::string(argv[i])=="--small")SDL_SetWindowSize(app.window,1000,680);
        if((alignment_smoke||gesture_smoke)&&app.tab()){auto &t=*app.tab();app.workspace(t,true);if(alignment_smoke)t.alignment.facing=2;}
    }
    else if(argc>1){app.open(argv[1]);for(int i=2;i<argc;++i)if(std::string(argv[i])=="--alignment"&&app.tab())app.workspace(*app.tab(),true);}
    int frames=0,exit_code=0;
    ImVec2 gesture_start;int original_x=0,original_y=0,expected_x=0,expected_y=0;
    std::vector<unsigned char> original_pixels;
    studio::EditingSmoke editing_check;
    while(app.running){
        SDL_Event e;auto event=[&](){ImGui_ImplSDL2_ProcessEvent(&e);if(e.type==SDL_QUIT){app.finish();app.action(studio::Action::Quit);}if(e.type==SDL_DROPFILE){if(!app.close_uid&&!app.help&&app.error.empty()&&!app.path_dialog&&app.edit_dialog==studio::EditDialog::None)app.open(e.drop.file);SDL_free(e.drop.file);}};
        if(SDL_WaitEventTimeout(&e,16)){event();while(SDL_PollEvent(&e))event();}
        ImGui_ImplSDLRenderer2_NewFrame();ImGui_ImplSDL2_NewFrame();
        if(editing_smoke)editing_check.prepare(app,io,frames);
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
        if(smoke&&++frames==(editing_smoke?28:gesture_smoke?20:5)){int w,h;SDL_GetRendererOutputSize(app.renderer,&w,&h);std::vector<unsigned char> pixels((size_t)w*h*4);
            if(SDL_RenderReadPixels(app.renderer,nullptr,SDL_PIXELFORMAT_RGBA32,pixels.data(),w*4)!=0||!stbi_write_png(argv[2],w,h,4,pixels.data(),w*4))exit_code=1;
            if(!app.error.empty())exit_code=2;if(editing_check.failures)exit_code=4;app.running=false;
        }
        SDL_RenderPresent(app.renderer);
    }
    app.tabs.clear();if(app.paste_texture)SDL_DestroyTexture(app.paste_texture);ImGui_ImplSDLRenderer2_Shutdown();ImGui_ImplSDL2_Shutdown();ImGui::DestroyContext();SDL_DestroyRenderer(app.renderer);SDL_DestroyWindow(app.window);SDL_Quit();return exit_code;
}
