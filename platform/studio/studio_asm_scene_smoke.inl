struct AsmSceneSmoke {
    int failures=0;uint64_t owner=0,other=0,generation=0,lane_id=0,frame_id=0;
    std::string img,other_img,asm_path,native,original,other_original,asm_original;SceneProject *baseline=nullptr;
    void prepare(App &app,ImGuiIO &io,int frame){auto &u=app.asm_ui;auto &v=u.scene_review;
        auto check=[&](bool ok,const char *label){if(!ok){++failures;fprintf(stderr,"ASM scene UI failure %d (%s): %s / %s / %s\n",frame,label,app.error.c_str(),u.error.c_str(),v.error.c_str());}};
        auto activate=[&](uint64_t uid){for(int i=0;i<(int)app.tabs.size();++i)if(app.tabs[i]->uid==uid){app.active=i;app.select_uid=uid;}};
        auto read=[](const std::string &p){std::ifstream f(fs::u8path(p),std::ios::binary);return std::string{std::istreambuf_iterator<char>(f),{}};};
        auto click=[&](int at,ImVec2 p){if(frame==at-1)io.AddMousePosEvent(p.x,p.y);if(frame==at)io.AddMouseButtonEvent(0,true);if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        if(frame==0){auto t=app.tab();if(!t){++failures;return;}owner=t->uid;io.AddFocusEvent(true);img=(fs::u8path(app.prefs)/"scene-source.IMG").u8string();other_img=(fs::u8path(app.prefs)/"scene-other.IMG").u8string();asm_path=(fs::u8path(app.prefs)/"scene.ASM").u8string();native=(fs::u8path(app.prefs)/"from-asm.ISTUDIO").u8string();
            check(t->art->save(img,app.error),"save source");original=read(img);app.demo();auto second=app.tab();other=second->uid;check(second->art->rename_sprite(0,"REMOTE1",app.error)&&second->art->save(other_img,app.error),"save remote piece");other_original=read(other_img);activate(owner);t=app.tab();app.new_scene(*t);check(t->project&&t->project->save((fs::u8path(app.prefs)/"baseline.ISTUDIO").u8string(),app.scene_source(),app.error),"existing project");baseline=t->project.get();
            asm_original="demo_anitab\n\t.long a_demo\na_demo\n\t.long GROUP,ani_adjustxy\n\t.word 5,-3\n\t.long ani_flip,SENTINEL2,ani_flip_v,GROUP,0\nGROUP\n\t.long SENTINEL1,REMOTE1,0\n";{std::ofstream f(fs::u8path(asm_path),std::ios::binary);f<<asm_original;}check(app.load_asm(asm_path),"load ASM");app.show_asm();u.facing=true;u.origin_y=210;}
        auto t=app.find(owner);if(!t){++failures;return;}
        click(4,v.show_point);if(frame==8)check(v.open&&v.valid&&v.snapshot&&v.snapshot->scene().assets.size()==2&&v.snapshot->scene().lanes[0].frames.size()==3&&v.snapshot->scene().lanes[0].frames[0].pieces.size()==2,"complete cross-library review");click(9,v.cancel_point);
        if(frame==13)check(!v.open&&u.open&&t->project.get()==baseline&&!t->project->dirty(),"Cancel preserves project");click(14,v.show_point);
        if(frame==18){check(v.valid,"reopen review");t->art->anchor(0,1,2);t->art->commit();}click(19,v.apply_point);if(frame==23)check(v.open&&!v.error.empty()&&t->project.get()==baseline,"stale artwork blocks create");click(24,v.cancel_point);
        if(frame==28){t->art->undo();generation=t->art->generation();check(!t->art->dirty()&&read(img)==original,"source Undo");}click(29,v.show_point);click(34,v.hold_point);
        if(frame==37){io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);}if(frame==38){io.AddKeyEvent(ImGuiMod_Ctrl,false);io.AddKeyEvent(ImGuiKey_A,false);io.AddInputCharactersUTF8("6");}if(frame==39)io.AddKeyEvent(ImGuiKey_Enter,true);if(frame==40)io.AddKeyEvent(ImGuiKey_Enter,false);
        click(42,v.apply_point);if(frame==45)check(v.open&&v.dirty&&v.options.hold==6&&t->project.get()==baseline,"changed settings need review");click(46,v.build_point);
        if(frame==50)check(v.valid&&!v.dirty&&v.snapshot->scene().lanes[0].frames[0].ticks==6,"rebuilt hold");
        if(frame==51){t->project->begin("Existing edit");t->project->state.lanes[0].x=3;t->project->commit();}click(52,v.apply_point);
        if(frame==56)check(!u.open&&app.pending_project&&app.scene_guard_uid==owner&&t->project.get()==baseline&&t->project->dirty(),"existing unsaved project guarded");click(58,app.scene_cancel_point);
        if(frame==62){check(!app.pending_project&&!app.scene_guard_uid&&t->project.get()==baseline,"guard Cancel");app.action(Action::Undo);check(!t->project->dirty(),"restore existing project");app.show_asm();u.hold=6;}
        click(66,v.show_point);if(frame==70)check(v.valid&&v.snapshot,"final review");click(72,v.apply_point);
        if(frame==76){check(!u.open&&!v.open&&!app.pending_project&&t->project.get()!=baseline&&t->project->dirty()&&!t->project->can_undo()&&t->project->state.assets.size()==2&&t->project->state.lanes[0].frames[0].pieces.size()==2,"create editable scene");lane_id=t->project->state.lanes[0].id;frame_id=t->project->state.lanes[0].frames[0].id;}
        if(frame==78)check(t->project->save(native,app.scene_source(),app.error)&&t->project->export_png((fs::u8path(app.prefs)/"scene.png").u8string(),12,app.scene_source(),app.error),"save and export");if(frame==80)app.open_scene(native);
        if(frame==84)check(t->project&&!t->project->dirty()&&t->project->state.lanes[0].id==lane_id&&t->project->state.lanes[0].frames[0].id==frame_id&&t->project->state.lanes[0].frames[0].pieces.size()==2,"native reopen");
        if(frame==86){auto partial=AsmSource::parse("a_bad\n\t.long ani_nosleep,SENTINEL1,0\n",app.error);check(partial!=nullptr,"no-sleep fixture");app.install_asm(std::move(partial));app.show_asm();}click(90,v.show_point);if(frame==94)check(v.open&&!v.valid&&v.error.find("No-sleep")!=std::string::npos,"no-sleep timing blocks");click(95,v.apply_point);if(frame==99)check(v.open&&!t->project->dirty()&&t->project->state.lanes[0].id==lane_id,"blocked Apply preserves project");click(100,v.cancel_point);
        if(frame==104){check(app.load_asm(asm_path),"restore source");app.show_asm();u.hold=6;u.facing=true;u.origin_y=210;}click(106,v.show_point);
        if(frame==110){check(v.valid&&v.snapshot&&read(img)==original&&read(other_img)==other_original&&read(asm_path)==asm_original&&t->art->generation()==generation&&!t->art->dirty()&&!app.find(other)->art->dirty(),"sources and formats unchanged");v.tick=12;io.AddMousePosEvent(5,5);}
    }
};
