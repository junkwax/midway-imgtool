struct PaletteSwatchesSmoke {
    int failures=0;uint64_t generation=0;
    std::vector<unsigned short> original,source;std::vector<unsigned char> pixels;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto &u=app.palettes;auto t=app.tab();
        auto check=[&](bool ok,const char *label){if(!ok){++failures;fprintf(stderr,"Swatch UI failure %d (%s): %s / %s\n",frame,label,u.error.c_str(),u.swatch_error.c_str());}};
        auto click=[&](int at,ImVec2 p,int button=0,bool ctrl=false){if(frame==at-1)io.AddMousePosEvent(p.x,p.y);if(frame==at){if(ctrl)io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddMouseButtonEvent(button,true);}if(frame==at+1){io.AddMouseButtonEvent(button,false);if(ctrl)io.AddKeyEvent(ImGuiMod_Ctrl,false);}};
        auto key=[&](int at,ImGuiKey k,bool shift=false){if(frame==at||frame==at+1){bool down=frame==at;io.AddKeyEvent(ImGuiMod_Ctrl,down);if(shift)io.AddKeyEvent(ImGuiMod_Shift,down);io.AddKeyEvent(k,down);}};
        if(frame==0){original=copy_palette(*t->art,0).words;source=original;source[2]=0xffff;source[8]=0x4321;source[15]=0x1357;
            check(t->art->add_palette("COSTUME",source,app.error)==1,"fixture palette");t->ppp=4;
            check(t->art->save((fs::u8path(app.prefs)/"swatch-source.IMG").u8string(),app.error),"save fixture");
            generation=t->art->generation();app.palette_clipboard=copy_palette(*t->art,0);app.clipboard=t->art->copy(0,0,0,48,64);pixels=app.clipboard.pixels;
            app.open_palettes(*t);app.palette_reset(*t,1);io.AddFocusEvent(true);io.AddMousePosEvent(5,5);}
        click(4,u.swatch_points[2]);click(8,u.swatch_points[8],0,true);click(12,u.swatch_points[15],0,true);click(16,u.swatch_points[8],1);click(20,u.copy_swatches_point);
        if(frame==24)check(app.swatch_clipboard.slots.size()==3&&app.swatch_clipboard.slots[0].index==2&&app.swatch_clipboard.slots[1].index==8&&app.swatch_clipboard.slots[2].index==15&&t->art->generation()==generation&&!t->art->dirty(),"selected copy owns words and indices");
        click(26,u.palette_row_points[0]);click(30,u.swatch_points[2],1);click(34,u.paste_swatches_point);
        if(frame==38)check(u.swatch_mode&&u.swatch_valid&&u.swatch_plan.changed==3&&!u.swatch_plan.growth&&u.swatch_plan.primary==8&&copy_palette(*t->art,0).words==original,"original-index review is read only");
        key(40,ImGuiKey_V);key(44,ImGuiKey_Z);
        if(frame==48)check(u.swatch_mode&&t->art->palette_count()==2&&!t->art->dirty()&&t->art->generation()==generation,"review blocks palette shortcuts");click(52,u.cancel_swatches_point);
        if(frame==56)check(!u.swatch_mode&&copy_palette(*t->art,0).words==original&&app.swatch_clipboard.slots.size()==3,"cancel retains source and clipboard");
        click(58,u.swatches_menu_point);click(62,u.paste_swatches_point);click(68,u.apply_point);
        if(frame==72)check(!u.swatch_mode&&copy_palette(*t->art,0).words==source&&t->art->dirty()&&t->art->copy(0,0,0,48,64).pixels==pixels,"Apply recolors without remapping pixels");key(72,ImGuiKey_Z);
        if(frame==76)check(copy_palette(*t->art,0).words==original&&!t->art->dirty(),"one Undo restores colors");key(76,ImGuiKey_Z,true);
        if(frame==80)check(copy_palette(*t->art,0).words==source&&t->art->dirty(),"Redo restores colors");key(80,ImGuiKey_Z);
        click(88,u.swatches_menu_point);click(92,u.append_swatches_point);
        if(frame==96)check(u.swatch_mode&&u.swatch_valid&&u.swatch_plan.growth==3&&u.swatch_plan.words.size()==19&&u.swatch_plan.ppp_images==8&&u.swatch_plan.depth==5,"append review shows growth and PPP");
        click(100,u.apply_point);
        if(frame==104)check(t->art->palette(0)->numc==19&&t->art->palette(0)->bitspix==5&&copy_palette(*t->art,0).words[16]==source[2],"append applies every copied color");key(104,ImGuiKey_Z);
        if(frame==108)check(t->art->palette(0)->numc==16&&!t->art->dirty(),"Undo append");click(110,u.swatch_points[0],1);click(114,u.copy_one_point);
        if(frame==118)check(app.swatch_clipboard.slots.size()==1&&app.swatch_clipboard.slots[0].index==0,"copy zero color");
        click(120,u.swatch_points[3],1);click(124,u.paste_one_point);
        if(frame==128)check(u.swatch_mode&&u.swatch_valid&&u.swatch_plan.mode==SwatchPaste::AtSlot&&u.swatch_plan.words[3]==original[0]&&u.swatch_plan.words[0]==original[0],"zero color can become opaque");
        click(132,u.apply_point);
        if(frame==136)check(!u.swatch_mode&&copy_palette(*t->art,0).words[3]==original[0]&&t->art->palette(0)->numc==16,"one-slot paste");key(136,ImGuiKey_Z);
        click(142,u.swatch_points[0],1);click(146,u.paste_swatches_point);
        if(frame==150)check(u.swatch_mode&&!u.swatch_valid&&!u.swatch_error.empty(),"index zero original paste refused");click(152,u.apply_point);
        if(frame==156)check(!t->art->dirty()&&copy_palette(*t->art,0).words==original,"invalid Apply cannot edit transparency");click(158,u.cancel_swatches_point);
        if(frame==164)app.start_swatch_review(*t,SwatchPaste::Append,1);
        if(frame==168){t->art->color(0,2,0x2222);t->art->commit();}
        if(frame==172)check(u.swatch_mode&&!u.swatch_valid&&!u.swatch_error.empty(),"changed source invalidates review");click(176,u.cancel_swatches_point);
        if(frame==180){t->art->undo();app.palette_reset(*t,0);u.hue=45;}
        click(184,u.swatches_menu_point);click(190,u.swatch_points[8],1);
        if(frame==196){check(!u.swatch_mode&&app.swatch_clipboard.slots.size()==1&&app.palette_pending(),"pending edit blocks color clipboard");u.hue=0;}
        click(200,u.swatches_menu_point);click(204,u.clear_swatches_point);
        if(frame==208)check(app.swatch_clipboard.slots.empty()&&!t->art->dirty(),"clear changes no artwork");
        if(frame==212){check(app.palette_clipboard.words==original&&app.clipboard.pixels==pixels&&!app.sprite_clipboard&&!app.animation_clipboard,"other clipboards retained");
            u.selected.fill(false);u.selected[2]=u.selected[8]=u.selected[15]=true;app.palette_reset(*t,1,true);app.copy_swatch_colors(*t,2,false);app.palette_reset(*t,0);app.start_swatch_review(*t,SwatchPaste::Append,1);}
        if(frame==216){check(u.swatch_valid&&t->art->apply_palette_swatches(u.swatch_plan,u.error),"save fixture paste");u.swatch_mode=false;app.palette_reset(*t,0);auto path=(fs::u8path(app.prefs)/"swatch-result.IMG").u8string();check(t->art->save(path,app.error),"save pasted colors");auto disk=Artwork::open(path,app.error);check(disk&&disk->disk_version()==0x634&&disk->palette(0)->numc==19&&disk->palette(0)->bitspix==5&&disk->copy(0,0,0,48,64).pixels==pixels,"same IMG format reopen");}
        if(frame==220){t->art->undo();app.palette_reset(*t,0);app.start_swatch_review(*t,SwatchPaste::Append,1);io.AddMousePosEvent(5,5);}
        if(frame==224)check(u.open&&u.swatch_mode&&u.swatch_valid&&u.error.empty()&&app.error.empty(),"final review health");
    }
};
