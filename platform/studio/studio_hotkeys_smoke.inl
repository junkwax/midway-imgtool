// Actual ImGui input routing: shortcuts, modal isolation and read-only Help.
struct HotkeysSmoke {
    int failures=0,count=0,palette_count=0;uint64_t uid=0,other=0;unsigned version=0;
    std::vector<unsigned char> pixels;std::vector<unsigned short> palette_words;int order=0,paste_start=0;ImVec2 pan_start,pan_mouse;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.find(uid);
        auto check=[&](bool ok,const char *label){if(!ok){++failures;fprintf(stderr,"Hotkey UI failure at %d (%s): %s / %s\n",frame,label,app.error.c_str(),app.status.c_str());}};
        auto key=[&](int at,ImGuiKey k,ImGuiKeyChord mods=0){if(frame==at||frame==at+1){bool down=frame==at;io.AddKeyEvent(ImGuiMod_Ctrl,down&&(mods&ImGuiMod_Ctrl));io.AddKeyEvent(ImGuiMod_Shift,down&&(mods&ImGuiMod_Shift));io.AddKeyEvent(ImGuiMod_Alt,down&&(mods&ImGuiMod_Alt));io.AddKeyEvent(k,down);}};
        auto click=[&](int at,ImVec2 pos){if(frame==at-1)io.AddMousePosEvent(pos.x,pos.y);if(frame==at)io.AddMouseButtonEvent(0,true);if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto clean=[&](){return t&&!t->art->dirty()&&t->art->count()==count&&t->art->palette_count()==palette_count&&t->art->disk_version()==version&&t->art->copy(0,0,0,48,64).pixels==pixels;};
        if(frame==0){t=app.tab();uid=t->uid;count=t->art->count();palette_count=t->art->palette_count();version=t->art->disk_version();pixels=t->art->copy(0,0,0,48,64).pixels;check(t->art->save((fs::u8path(app.prefs)/"hotkeys-source.IMG").u8string(),app.error),"save fixture");io.AddFocusEvent(true);io.AddMousePosEvent(5,5);}
        key(4,ImGuiKey_Space);if(frame==8)check(app.mark_count(*t)==1&&clean(),"Space marks");
        key(8,ImGuiKey_M,ImGuiMod_Shift);if(frame==12)check(app.mark_count(*t)==count&&clean(),"Shift M marks all");
        key(12,ImGuiKey_M);if(frame==16)check(!app.mark_count(*t)&&clean(),"M clears marks");
        key(16,ImGuiKey_V);if(frame==20)check(app.tool==Tool::Variant,"V variant");
        key(20,ImGuiKey_V,ImGuiMod_Shift);if(frame==24)check(app.tool==Tool::Move,"Shift V move");
        key(24,ImGuiKey_E);if(frame==28)check(app.tool==Tool::SmartErase,"E smart erase");
        key(28,ImGuiKey_E,ImGuiMod_Shift);if(frame==32)check(app.tool==Tool::Erase,"Shift E erase");
        key(32,ImGuiKey_K);if(frame==36)check(t->alignment.enabled&&t->alignment.player.playing,"K playback");
        key(36,ImGuiKey_K);if(frame==40)check(!t->alignment.player.playing,"K pause");
        key(40,ImGuiKey_Tab);if(frame==44){check(!t->alignment.enabled,"Tab artwork");app.select(0);}
        key(44,ImGuiKey_F9);if(frame==48)check(app.diagnostics,"F9 diagnostics");key(48,ImGuiKey_F9);if(frame==52)check(!app.diagnostics,"F9 closes");
        key(52,ImGuiKey_C,ImGuiMod_Ctrl|ImGuiMod_Shift);if(frame==56)check(app.regions.open&&!app.regions.cut&&clean(),"copy new review");key(56,ImGuiKey_Escape);
        key(60,ImGuiKey_X,ImGuiMod_Ctrl|ImGuiMod_Shift);if(frame==64)check(app.regions.open&&app.regions.cut&&clean(),"cut new review");key(64,ImGuiKey_Escape);
        key(68,ImGuiKey_C,ImGuiMod_Ctrl);if(frame==72)check(app.clipboard.pixels==pixels,"copy pixels");
        key(72,ImGuiKey_V,ImGuiMod_Ctrl|ImGuiMod_Shift);if(frame==76)check(t->art->count()==count+1&&t->art->copy(count,0,0,48,64).pixels==pixels&&t->art->palette_count()==palette_count&&t->art->disk_version()==version,"paste new one undo");
        key(76,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==80)check(clean(),"paste new undo");key(80,ImGuiKey_Z,ImGuiMod_Ctrl|ImGuiMod_Shift);if(frame==84)check(t->art->count()==count+1,"shift Z redo");key(84,ImGuiKey_Z,ImGuiMod_Ctrl);
        key(88,ImGuiKey_P,ImGuiMod_Ctrl);if(frame==92)check(app.points.open&&clean(),"point review");key(92,ImGuiKey_Escape);
        key(96,ImGuiKey_R,ImGuiMod_Ctrl);if(frame==100)check(app.edit_dialog==EditDialog::Rename&&clean(),"Ctrl R rename");click(101,app.edit_cancel_point);
        if(frame==105){app.select(1);order=1;}key(108,ImGuiKey_PageUp,ImGuiMod_Alt);if(frame==112)check(t->art->sprite_index(t->selected_id)==0&&t->art->dirty(),"move up");key(112,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==116)check(clean(),"move undo");
        if(frame==117){app.select(0);app.action(Action::SelectOpaque);app.begin_floating(*t);}key(120,ImGuiKey_H);if(frame==124)check(app.floating.active&&app.floating.stage.flip_h&&!app.help,"floating H flip, no Help");key(124,ImGuiKey_V);if(frame==128)check(app.floating.stage.flip_v,"floating V flip");key(128,ImGuiKey_E,ImGuiMod_Ctrl);if(frame==132)check(!app.floating.active&&t->art->dirty(),"Ctrl E merge");key(132,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==136)check(clean(),"transform undo");
        if(frame==137){app.tool=Tool::Pencil;t->radius=3;}key(140,ImGuiKey_LeftBracket);if(frame==144)check(t->radius==2&&!app.batch_palette.open,"brush bracket");
        if(frame==145)app.tool=Tool::Select;key(148,ImGuiKey_RightBracket);if(frame==152)check(app.batch_palette.open&&app.batch_palette.scope==3&&app.batch_palette.plan.rows.size()==1&&app.batch_palette.plan.rows[0].sprite==t->selected_id&&clean(),"selected palette assignment");key(152,ImGuiKey_Escape);
        key(156,ImGuiKey_Semicolon);if(frame==160)check(app.batch.open&&app.batch.scope==0&&app.batch.options.kind==BatchGeometryKind::Resize&&app.batch.options.quality&&clean(),"reduce review");key(160,ImGuiKey_Escape);
        if(frame==164)app.action(Action::ManagePalettes);key(168,ImGuiKey_C,ImGuiMod_Ctrl);if(frame==172){check(!app.palette_clipboard.words.empty()&&clean(),"palette Ctrl C");palette_words=app.palette_clipboard.words;}
        key(172,ImGuiKey_V,ImGuiMod_Ctrl);if(frame==176)check(t->art->palette_count()==palette_count+1&&copy_palette(*t->art,app.palettes.index).words==palette_words,"palette Ctrl V");key(176,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==180)check(clean(),"palette undo");
        key(180,ImGuiKey_Z,ImGuiMod_Ctrl|ImGuiMod_Shift);if(frame==184)check(t->art->palette_count()==palette_count+1,"palette redo");key(184,ImGuiKey_Z,ImGuiMod_Ctrl);click(190,app.palettes.close_point);
        if(frame==194){check(!app.palettes.open&&clean(),"palette closes");app.new_library();other=app.tab()->uid;}
        key(198,ImGuiKey_Tab,ImGuiMod_Ctrl);if(frame==202)check(app.tab()->uid==uid,"next library");key(202,ImGuiKey_Tab,ImGuiMod_Ctrl|ImGuiMod_Shift);if(frame==206)check(app.tab()->uid==other,"previous library");
        if(frame==207){app.active=0;app.select_uid=uid;app.select(0);t->selection={};}key(210,ImGuiKey_Delete);if(frame==214)check(app.edit_dialog==EditDialog::Delete&&clean(),"Delete reviews sprite without selection");click(215,app.edit_cancel_point);
        if(frame==219){app.select(0);t->selection=rectangle_selection(48,64,16,8,4,6);app.clipboard=t->art->copy_selection(0,t->selection);}
        key(220,ImGuiKey_V,ImGuiMod_Ctrl);key(224,ImGuiKey_T,ImGuiMod_Ctrl);if(frame==228)check(app.floating.active&&app.floating.stage.clipboard&&clean(),"Ctrl T opens paste transform");
        key(228,ImGuiKey_H);if(frame==232)check(app.floating.stage.flip_h&&!app.help&&clean(),"paste H flips");key(232,ImGuiKey_V);if(frame==236){check(app.floating.stage.flip_v,"paste V flips");paste_start=app.floating.stage.x;}
        key(236,ImGuiKey_RightArrow,ImGuiMod_Shift);if(frame==240)check(app.floating.stage.x==std::min(paste_start+10,app.floating.stage.canvas_w-app.floating.stage.w),"paste Shift arrow");key(240,ImGuiKey_E,ImGuiMod_Ctrl);if(frame==244)check(!app.floating.active&&t->art->dirty(),"paste Ctrl E applies");key(244,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==248)check(clean(),"paste transform single undo");
        key(248,ImGuiKey_V,ImGuiMod_Ctrl);key(252,ImGuiKey_H);if(frame==256)check(app.floating.active&&app.floating.stage.clipboard&&app.floating.stage.flip_h&&clean(),"paste direct H starts preview");key(256,ImGuiKey_J,ImGuiMod_Ctrl);if(frame==260)check(app.floating.active&&app.floating.stage.copy&&t->art->dirty(),"paste Ctrl J stamps and retains preview");key(260,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==264)check(!app.floating.active&&t->art->dirty(),"first Undo cancels duplicate preview");key(264,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==268)check(clean(),"second Undo removes stamp");
        key(272,ImGuiKey_V,ImGuiMod_Ctrl);key(276,ImGuiKey_Enter);if(frame==280)check(!app.floating.active&&!app.paste_mode&&t->art->dirty(),"Enter places paste");key(280,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==284)check(clean(),"Enter paste single Undo");
        if(frame==285){app.select(0);t->selection={};app.tool=Tool::Select;}key(288,ImGuiKey_RightArrow,ImGuiMod_Ctrl);if(frame==292)check(t->alignment.player.frame==1&&t->selected==0&&clean(),"Ctrl Right reorders temporary preview");key(292,ImGuiKey_LeftArrow,ImGuiMod_Ctrl);if(frame==296)check(t->alignment.player.frame==0&&clean(),"Ctrl Left restores preview order");
        if(frame==299){pan_start=t->pan;pan_mouse=t->canvas_origin+ImVec2(20*t->canvas_zoom,20*t->canvas_zoom);app.tool=Tool::Pencil;io.AddMousePosEvent(pan_mouse.x,pan_mouse.y);}
        if(frame==300)io.AddKeyEvent(ImGuiKey_Space,true);if(frame==301)io.AddMouseButtonEvent(0,true);if(frame==302)io.AddMousePosEvent(pan_mouse.x+20,pan_mouse.y+12);if(frame==303)io.AddMouseButtonEvent(0,false);if(frame==304)io.AddKeyEvent(ImGuiKey_Space,false);
        if(frame==308){check(t->pan.x>pan_start.x&&t->pan.y>pan_start.y&&!app.mark_count(*t)&&clean(),"Space drag pans without painting or marking");app.tool=Tool::Select;}
        key(320,ImGuiKey_H);if(frame==324)check(app.help&&clean(),"H help");
        key(324,ImGuiKey_J,ImGuiMod_Ctrl);key(328,ImGuiKey_Space);key(332,ImGuiKey_Delete);if(frame==336)check(app.help&&clean()&&!app.mark_count(*t),"help blocks edits");
        click(338,app.help_search_point);if(frame==341)io.AddInputCharactersUTF8("DMA2");if(frame==345)check(app.help_search.IsActive()&&app.help_topic>=0&&std::string(help_topics[app.help_topic].title)=="DMA2 hardware reference"&&clean(),"search full Help");
        key(346,ImGuiKey_V);if(frame==350)check(app.tool==Tool::Select&&clean(),"typing cannot switch tools");key(350,ImGuiKey_Escape);
        if(frame==354){check(!app.help&&clean(),"Escape closes Help");app.help_search.Clear();app.help_topic=1;}
        key(358,ImGuiKey_F1);if(frame==362)check(app.help&&app.help_topic==1&&app.keyboard_reference().find("Ctrl+Shift+V")!=std::string::npos&&clean(),"F1 keyboard guide");
        if(frame==363)io.AddMousePosEvent(5,5);
        if(frame==364){check(t->art->save((fs::u8path(app.prefs)/"hotkeys-result.IMG").u8string(),app.error),"roundtrip save");std::ifstream a(fs::u8path(app.prefs)/"hotkeys-source.IMG",std::ios::binary),b(fs::u8path(app.prefs)/"hotkeys-result.IMG",std::ios::binary);std::string x((std::istreambuf_iterator<char>(a)),{}),y((std::istreambuf_iterator<char>(b)),{});check(x==y,"exact IMG bytes after review and undo");}
    }
};
