struct LibraryMarksSmoke {
    int failures=0;uint64_t owner=0,generation=0;std::string file,bytes;std::vector<uint64_t> ids;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto read=[&](const std::string &path){std::ifstream f(fs::u8path(path),std::ios::binary);return std::string{std::istreambuf_iterator<char>(f),{}};};
        auto check=[&](bool ok,const char *label){if(!ok){++failures;fprintf(stderr,"Library marks UI failure %d (%s): %s\n",frame,label,app.error.c_str());}};
        auto click=[&](int at,ImVec2 p,ImGuiKeyChord mod=0){if(frame==at-1){io.AddMousePosEvent(p.x,p.y);if(mod)io.AddKeyEvent((ImGuiKey)mod,true);}if(frame==at)io.AddMouseButtonEvent(0,true);if(frame==at+1)io.AddMouseButtonEvent(0,false);if(frame==at+2&&mod)io.AddKeyEvent((ImGuiKey)mod,false);};
        if(frame==0){auto t=app.tab();if(!t){check(false,"source tab");return;}io.AddFocusEvent(true);owner=t->uid;file=(fs::u8path(app.prefs)/"mark-source.IMG").u8string();check(t->art->save(file,app.error),"save fixture");bytes=read(file);for(int i=0;i<t->art->count();++i)ids.push_back(t->art->sprite_id(i));t->art->color(0,8,0x7c00);t->art->commit();t->art->undo();generation=t->art->generation();check(t->art->can_redo()&&!t->art->dirty(),"existing Redo");}
        auto t=app.find(owner);if(!t){check(false,"source available");return;}auto &a=*t->art;
        auto marked=[&](std::initializer_list<int> picks){if(app.mark_count(*t)!=(int)picks.size())return false;for(int i:picks)if(!app.marked(*t,ids[i]))return false;return true;};
        click(6,t->select_frames_point);if(frame==9)check(t->select_frames,"Select frames enables row marking");click(10,t->row_points[0]);if(frame==13)check(marked({0})&&t->selected==0,"row click checks first frame");
        click(14,t->row_points[1]);if(frame==17)check(marked({0,1})&&t->selected==1,"second row checks without clearing first");click(18,t->row_points[0]);if(frame==21)check(marked({1}),"row click unchecks");
        click(22,t->select_frames_point);if(frame==25)check(!t->select_frames&&marked({1}),"Done keeps checked frames");click(26,t->row_points[2]);if(frame==29)check(marked({1})&&t->selected==2,"ordinary browsing preserves marks");
        click(30,t->row_points[0],ImGuiMod_Ctrl);if(frame==33)check(marked({0,1})&&t->mark_anchor==ids[0],"Ctrl-click toggles mark");click(34,t->row_points[2],ImGuiMod_Shift);
        if(frame==37){check(marked({0,1,2}),"Shift-click checks range");t->selection=rectangle_selection(48,64,4,4,2,2);}click(38,t->mark_points[2],ImGuiMod_Shift);
        if(frame==41){check(marked({})&&t->selected==2&&t->selection.count()==4,"Shift-checkbox unchecks range without changing pixels/focus");snprintf(t->filter.InputBuf,sizeof t->filter.InputBuf,"SENTINEL1,SENTINEL3");t->filter.Build();}
        click(46,t->row_points[0],ImGuiMod_Ctrl);click(50,t->row_points[2],ImGuiMod_Shift);
        if(frame==54){check(marked({0,2})&&!app.marked(*t,ids[1]),"filtered range excludes hidden frames");app.open_png_batch(*t);}
        if(frame==58){check(app.png_batch.open&&app.png_batch.valid&&app.png_batch.scope==0&&app.png_batch.plan.included==2,"PNG export uses checked frames");}click(60,app.png_batch.cancel_point);
        if(frame==64){check(!app.png_batch.open&&marked({0,2}),"Cancel export preserves marks");t->filter.Clear();}click(66,t->select_frames_point);
        if(frame==70){check(t->select_frames,"selection mode restored");app.demo();}auto active=app.tab();if(active&&active->uid!=owner){if(frame==73)check(!active->select_frames&&!app.mark_count(*active),"other library starts independently");click(74,active->select_frames_point);click(78,active->row_points[0]);if(frame==81)check(active->select_frames&&app.mark_count(*active)==1,"other library marks independent");}
        if(frame==82){for(int i=0;i<(int)app.tabs.size();++i)if(app.tabs[i]->uid==owner)app.active=i;app.select_uid=owner;}if(frame==86)check(t->select_frames&&marked({0,2}),"return to original library keeps selection mode and marks");
        click(90,t->row_points[2]);if(frame==93)check(marked({0}),"row unchecks after switching tabs");click(94,t->select_frames_point);click(98,t->row_points[1]);
        if(frame==102)check(!t->select_frames&&marked({0})&&t->selected==1&&!a.dirty()&&a.generation()==generation&&a.can_redo()&&!a.can_undo()&&read(file)==bytes,"marks preserve IMG bytes, dirty state and Undo/Redo");
        click(104,app.tools_ui.points[(int)Tool::Select]);if(frame==108)check(app.tool==Tool::Select&&marked({0}),"pixel Select stays separate");click(110,t->select_frames_point);click(114,t->row_points[2]);
        if(frame==118){check(t->select_frames&&marked({0,2})&&!a.dirty()&&a.can_redo(),"final checked rows");io.AddMousePosEvent(5,5);}
    }
};
