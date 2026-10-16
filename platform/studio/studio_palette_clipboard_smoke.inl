struct PaletteClipboardSmoke {
    int failures=0;uint64_t source=0,target=0;std::vector<unsigned char> pixel_guard;
    std::vector<unsigned short> first,second,captured_second;uint64_t generation=0;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto &u=app.palettes;auto t=app.tab();
        auto check=[&](bool ok,const char *label){if(!ok){++failures;fprintf(stderr,"Palette clipboard UI failure %d (%s): %s / %s\n",frame,label,u.error.c_str(),app.error.c_str());}};
        auto key=[&](int at,ImGuiKey k,ImGuiKeyChord mods=0){if(frame==at||frame==at+1){bool down=frame==at;io.AddKeyEvent(ImGuiMod_Ctrl,down&&(mods&ImGuiMod_Ctrl));io.AddKeyEvent(ImGuiMod_Shift,down&&(mods&ImGuiMod_Shift));io.AddKeyEvent(k,down);}};
        auto click=[&](int at,ImVec2 point,int button=0){if(frame==at-1)io.AddMousePosEvent(point.x,point.y);if(frame==at)io.AddMouseButtonEvent(button,true);if(frame==at+1)io.AddMouseButtonEvent(button,false);};
        auto count=[&](){return app.palette_clipboard_count();};
        if(frame==0){source=t->uid;first=copy_palette(*t->art,0).words;second=first;second[8]=0x7fff;check(t->art->add_palette("COSTUME",second,app.error)==1&&t->art->add_palette("UNUSED",first,app.error)==2,"source palettes");auto pal=t->art->palette(1);pal->flags=0x81;pal->bitspix=5;pal->file_spare=0x7931;memcpy(pal->file_name_raw,pal->n_s,10);pal->file_name_raw[9]=0x76;
            check(t->art->save((fs::u8path(app.prefs)/"palette-bank-source.IMG").u8string(),app.error),"source save");generation=t->art->generation();app.clipboard=t->art->copy(0,0,0,48,64);pixel_guard=app.clipboard.pixels;app.open_palettes(*t);io.AddFocusEvent(true);io.AddMousePosEvent(5,5);}
        key(4,ImGuiKey_Space);key(8,ImGuiKey_DownArrow);key(12,ImGuiKey_Space);key(16,ImGuiKey_UpArrow);
        if(frame==20){snprintf(u.filter.InputBuf,sizeof u.filter.InputBuf,"UNUSED");u.filter.Build();}key(20,ImGuiKey_C,ImGuiMod_Ctrl);
        if(frame==24){check(count()==2&&app.palette_clipboard.entries.size()==2&&app.palette_clipboard.entries[1].words==second&&t->palette_marks.size()==2&&t->art->generation()==generation&&!t->art->dirty(),"marked copy ignores search and changes no source data");u.hue=90;}
        key(24,ImGuiKey_C,ImGuiMod_Ctrl);key(28,ImGuiKey_V,ImGuiMod_Ctrl);
        if(frame==32){check(count()==2&&t->art->palette_count()==3&&!t->art->dirty()&&app.palette_pending(),"pending preview blocks copy and paste");u.hue=0;t->art->color(1,1,0x4567);t->art->commit();check(app.palette_clipboard.entries[1].words==second,"source edit leaves clipboard frozen");}
        click(37,u.copy_point,1);click(41,u.copy_selected_point);
        if(frame==44)check(count()==1&&app.palette_clipboard.words==first&&t->palette_marks.size()==2,"right-click selected only overrides marks");key(44,ImGuiKey_C,ImGuiMod_Ctrl);
        if(frame==48){check(count()==2&&app.palette_clipboard.entries[1].words[1]==0x4567,"batch capture replaces clipboard");captured_second=app.palette_clipboard.entries[1].words;t->art->undo();}
        click(52,u.close_point);if(frame==56){check(!u.open&&!t->art->dirty(),"source clean before close");app.action(Action::Close);}
        if(frame==60){check(!app.find(source)&&count()==2&&app.palette_clipboard.entries[1].words==captured_second,"clipboard survives source destruction");app.demo();t=app.tab();target=t->uid;app.open_palettes(*t);u.filter.Clear();}
        key(64,ImGuiKey_V,ImGuiMod_Ctrl);
        if(frame==68){check(t->art->palette_count()==3&&u.index==1&&palette_name(*t->art->palette(1))=="STUDIOP_2"&&palette_name(*t->art->palette(2))=="COSTUME"&&t->art->palette(2)->flags==0x81&&t->art->palette(2)->bitspix==5&&t->art->palette(2)->file_name_raw[9]==0x76&&copy_palette(*t->art,2).words==captured_second&&t->art->sprite(0)->palnum==0,"batch paste appends metadata and selects first");}
        key(72,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==76)check(t->art->palette_count()==1&&!t->art->dirty()&&count()==2,"one Undo removes whole batch");key(76,ImGuiKey_Z,ImGuiMod_Ctrl|ImGuiMod_Shift);
        if(frame==80)check(t->art->palette_count()==3&&t->art->dirty(),"Redo restores batch");key(80,ImGuiKey_V,ImGuiMod_Ctrl);
        if(frame==84)check(t->art->palette_count()==5&&u.index==3&&palette_name(*t->art->palette(4))=="COSTUME_2","repeated paste resolves names");key(84,ImGuiKey_Z,ImGuiMod_Ctrl);
        if(frame==90)u.hue=100;key(92,ImGuiKey_C,ImGuiMod_Ctrl);key(96,ImGuiKey_V,ImGuiMod_Ctrl);
        if(frame==100)check(count()==2&&t->art->palette_count()==3&&app.palette_pending(),"destination preview protects clipboard and library");click(102,u.reset_point);
        if(frame==106)check(!app.palette_pending(),"reset preview");click(108,u.clear_clipboard_point);
        if(frame==112)check(!count()&&t->art->palette_count()==3,"Clear releases clipboard without document change");key(112,ImGuiKey_V,ImGuiMod_Ctrl);
        if(frame==116)check(t->art->palette_count()==3&&!count(),"empty clipboard cannot paste");key(120,ImGuiKey_C,ImGuiMod_Ctrl);
        if(frame==124)check(count()==1&&app.palette_clipboard.name==palette_name(*t->art->palette(u.index))&&app.palette_clipboard.words==copy_palette(*t->art,u.index).words,"no marks copies selected palette");
        if(frame==126){check(app.clipboard.pixels==pixel_guard&&!app.sprite_clipboard&&!app.animation_clipboard,"unrelated clipboards retained");auto file=(fs::u8path(app.prefs)/"palette-bank-result.IMG").u8string();check(t->art->save(file,app.error),"destination save");auto disk=Artwork::open(file,app.error);check(disk&&disk->disk_version()==0x634&&disk->palette_count()==3&&disk->palette(2)->file_spare==0x7931&&copy_palette(*disk,2).words==captured_second,"same-format reopen");}
        if(frame==130)app.palette_reset(*t,1);key(132,ImGuiKey_Space);key(136,ImGuiKey_DownArrow);key(140,ImGuiKey_Space);key(144,ImGuiKey_C,ImGuiMod_Ctrl);
        if(frame==148)check(count()==2&&t->palette_marks.size()==2&&!t->art->dirty()&&app.clipboard.pixels==pixel_guard,"final owned clipboard");
        if(frame==152){app.palette_reset(*t,1);io.AddMousePosEvent(5,5);}
        if(frame==156)check(u.open&&count()==2&&app.error.empty()&&u.error.empty(),"final health");
    }
};
