struct LayerMoveSmoke {
    bool preview=false;int failures=0;uint64_t uid=0,generation=0;
    std::vector<unsigned char> base;ImVec2 mouse,pan;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.find(uid);auto check=[&](bool ok,const char *label){if(!ok){++failures;fprintf(stderr,"Overlay move failure at %d (%s): %s / %s\n",frame,label,app.error.c_str(),app.status.c_str());}};
        auto key=[&](int at,ImGuiKey k,ImGuiKeyChord mods=0){if(frame==at||frame==at+1){bool down=frame==at;io.AddKeyEvent(ImGuiMod_Ctrl,down&&(mods&ImGuiMod_Ctrl));io.AddKeyEvent(ImGuiMod_Shift,down&&(mods&ImGuiMod_Shift));io.AddKeyEvent(k,down);}};
        auto click=[&](int at,ImVec2 pos){if(frame==at-1)io.AddMousePosEvent(pos.x,pos.y);if(frame==at)io.AddMouseButtonEvent(0,true);if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto pixel=[&](float x,float y){mouse=t->canvas_origin+ImVec2(x*t->canvas_zoom,y*t->canvas_zoom);io.AddMousePosEvent(mouse.x,mouse.y);};
        auto move=[&](int x,int y){io.AddMousePosEvent(mouse.x+x*t->canvas_zoom,mouse.y+y*t->canvas_zoom);};
        auto layer=[&](){return img_layer(t->art->sprite(0));};
        auto at=[&](int x,int y){auto l=layer();return l&&l->x==x&&l->y==y;};
        auto clean=[&](){auto im=t->art->sprite(0);return !t->art->dirty()&&t->art->copy(0,0,0,48,64).pixels==base&&im->anix==24&&im->aniy==60;};
        if(frame==0){t=app.tab();uid=t->uid;app.select(0);base=t->art->copy(0,0,0,48,64).pixels;app.clipboard=t->art->copy(0,16,8,12,10);t->selection=rectangle_selection(48,64,12,12,6,6);io.AddFocusEvent(true);io.AddMousePosEvent(5,5);}
        key(4,ImGuiKey_V,ImGuiMod_Ctrl);if(frame==6){app.paste_x=10;app.paste_y=12;}key(8,ImGuiKey_L);
        if(frame==12){check(at(10,12)&&t->move_overlay&&app.tool==Tool::Move,"paste selects overlay target");check(t->art->save((fs::u8path(app.prefs)/"move-source.IMG").u8string(),app.error),"save fixture");generation=t->art->generation();}
        if(frame==14)pixel(12.5f,14.5f);if(frame==15)io.AddMouseButtonEvent(0,true);if(frame==16)move(5,-3);
        if(frame==18)check(app.layer_drag.active&&app.layer_drag.x==15&&app.layer_drag.y==9&&at(10,12)&&clean()&&t->art->generation()==generation&&!app.floating.active,"read-only drag preview");
        if(preview&&frame>=20)return;
        if(frame==20)io.AddMouseButtonEvent(0,false);
        if(frame==24)check(!app.layer_drag.active&&at(15,9)&&t->art->dirty()&&t->art->copy(0,0,0,48,64).pixels==base,"release moves overlay only");key(24,ImGuiKey_Z,ImGuiMod_Ctrl);
        if(frame==28)check(at(10,12)&&clean(),"one Undo restores complete drag");key(28,ImGuiKey_Y,ImGuiMod_Ctrl);
        if(frame==32)check(at(15,9)&&t->art->dirty(),"Redo");key(32,ImGuiKey_Z,ImGuiMod_Ctrl);
        if(frame==36)check(at(10,12)&&clean(),"reset drag");
        if(frame==40)pixel(12.5f,14.5f);if(frame==41)io.AddMouseButtonEvent(0,true);if(frame==42)move(7,6);key(44,ImGuiKey_Escape);if(frame==46)io.AddMouseButtonEvent(0,false);
        if(frame==48)check(!app.layer_drag.active&&at(10,12)&&clean()&&t->art->can_redo(),"Escape cancels without consuming history");
        if(frame==52)pixel(12.5f,14.5f);if(frame==53)io.AddMouseButtonEvent(0,true);if(frame==54)move(-4,5);key(56,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==58)io.AddMouseButtonEvent(0,false);
        if(frame==60)check(!app.layer_drag.active&&at(10,12)&&clean()&&t->art->can_redo(),"Undo cancels preview only");
        if(frame==64)pixel(40.5f,50.5f);if(frame==65)io.AddMouseButtonEvent(0,true);if(frame==66)move(-3,-3);if(frame==68)io.AddMouseButtonEvent(0,false);
        if(frame==70)check(at(10,12)&&clean()&&!app.floating.active,"outside bounds never moves base selection");
        if(frame==72)pixel(12.5f,14.5f);if(frame==73)io.AddMouseButtonEvent(0,true);if(frame==74){io.AddKeyEvent(ImGuiMod_Shift,true);move(6,2);}
        if(frame==76)check(app.layer_drag.active&&app.layer_drag.x==16&&app.layer_drag.y==12&&clean(),"Shift locks dominant axis");
        if(frame==78)io.AddMouseButtonEvent(0,false);if(frame==80)io.AddKeyEvent(ImGuiMod_Shift,false);
        if(frame==82)check(at(16,12)&&t->art->dirty(),"axis-lock release");key(84,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==88)check(at(10,12)&&clean(),"axis-lock Undo");
        click(90,app.layer_base_point);if(frame==94){check(!t->move_overlay&&app.tool==Tool::Move,"base target choice");t->selection=rectangle_selection(48,64,12,12,6,6);}
        if(frame==96)pixel(13.5f,13.5f);if(frame==97)io.AddMouseButtonEvent(0,true);if(frame==98)move(2,1);if(frame==100)io.AddMouseButtonEvent(0,false);
        if(frame==104)check(app.floating.active&&!app.layer_drag.active&&at(10,12)&&clean(),"base transform retains stationary layer");key(104,ImGuiKey_Enter);
        if(frame==108)check(!app.floating.active&&at(10,12)&&t->art->dirty(),"apply base transform");key(108,ImGuiKey_Z,ImGuiMod_Ctrl);
        if(frame==112)check(at(10,12)&&clean(),"base transform Undo");click(114,app.layer_target_point);
        key(118,ImGuiKey_RightArrow);if(frame==122)check(at(11,12)&&t->selected==0,"arrow nudges overlay instead of browsing");key(122,ImGuiKey_DownArrow,ImGuiMod_Shift);
        if(frame==126)check(at(11,22)&&t->selected==0,"Shift arrow moves ten");key(128,ImGuiKey_Z,ImGuiMod_Ctrl);key(132,ImGuiKey_Z,ImGuiMod_Ctrl);
        if(frame==136)check(at(10,12)&&clean(),"nudge Undo");
        click(140,app.layer_visible_point);key(144,ImGuiKey_DownArrow);
        if(frame==148){check(t->selected==1&&!layer()->visible&&!app.layer_drag.active,"hidden overlay does not capture arrows");app.select(0);}
        click(150,app.layer_visible_point);
        if(frame==154){check(at(10,12)&&layer()->visible,"show overlay again");check(t->art->save((fs::u8path(app.prefs)/"move-source.IMG").u8string(),app.error),"save shown fixture");}
        if(frame==156)pixel(12.5f,14.5f);if(frame==157)io.AddMouseButtonEvent(0,true);if(frame==158)move(3,3);if(frame==160)io.AddFocusEvent(false);if(frame==161)io.AddMouseButtonEvent(0,false);
        if(frame==164)check(!app.layer_drag.active&&at(10,12)&&clean(),"focus loss cancels");if(frame==166)io.AddFocusEvent(true);
        if(frame==170)pixel(12.5f,14.5f);if(frame==171)io.AddMouseButtonEvent(0,true);if(frame==172)move(2,2);
        if(frame==174){t->art->color(0,1,0x7fff);t->art->commit();}if(frame==176)io.AddMouseButtonEvent(0,false);
        if(frame==178){check(!app.layer_drag.active&&at(10,12),"source edit invalidates drag");t->art->undo();}
        if(frame==180)pixel(12.5f,14.5f);if(frame==181)io.AddMouseButtonEvent(0,true);if(frame==182)move(4,-2);if(frame==184)app.help=true;if(frame==185)io.AddMouseButtonEvent(0,false);
        if(frame==186)check(!app.layer_drag.active&&app.help&&at(10,12)&&clean(),"modal cancels drag");click(188,app.help_done_point);
        if(frame==192)check(!app.help&&at(10,12)&&clean(),"modal leaves art unchanged");
        if(frame==196){auto file=fs::u8path(app.prefs)/"move-result.IMG";check(t->art->save(file.u8string(),app.error),"save result");auto disk=Artwork::open(file.u8string(),app.error);check(disk&&disk->disk_version()==0x634&&!disk->sprite(0)->layer_p,"same-format reopen");if(disk)for(int y=0;y<64;++y)for(int x=0;x<48;++x)check(visible_pixel(*disk->sprite(0),x,y)==visible_pixel(*t->art->sprite(0),x,y),"saved composite");}
        if(frame==200)pixel(12.5f,14.5f);if(frame==201)io.AddMouseButtonEvent(0,true);if(frame==202)move(-2,1);if(frame==204)app.select(1);if(frame==205)io.AddMouseButtonEvent(0,false);
        if(frame==208){check(!app.layer_drag.active&&at(10,12)&&clean(),"sprite change cancels");app.select(0);}
        if(frame==212)check(app.error.empty()&&at(10,12)&&clean(),"pre-edge health");
        if(frame==216)pixel(12.5f,14.5f);if(frame==217)io.AddMouseButtonEvent(0,true);if(frame==218)move(-13,-16);if(frame==220)io.AddMouseButtonEvent(0,false);
        if(frame==224)check(at(-3,-4)&&t->art->copy(0,0,0,48,64).pixels==base,"off-canvas drag clips without altering base");key(224,ImGuiKey_Z,ImGuiMod_Ctrl);
        if(frame==228){check(at(10,12)&&clean(),"off-canvas Undo");generation=t->art->generation();}
        if(frame==232)pixel(12.5f,14.5f);if(frame==233)io.AddMouseButtonEvent(0,true);if(frame==234)io.AddMouseButtonEvent(0,false);
        if(frame==236)check(at(10,12)&&clean()&&t->art->generation()==generation&&t->art->can_redo(),"click without movement adds no history");
        if(frame==240){pan=t->pan;pixel(12.5f,14.5f);io.AddKeyEvent(ImGuiKey_Space,true);}if(frame==241)io.AddMouseButtonEvent(0,true);if(frame==242)move(6,2);if(frame==244)io.AddMouseButtonEvent(0,false);if(frame==246)io.AddKeyEvent(ImGuiKey_Space,false);
        if(frame==248){check(at(10,12)&&clean()&&!app.mark_count(*t)&&!app.layer_drag.active&&(t->pan.x!=pan.x||t->pan.y!=pan.y),"Space drag pans without moving layer or marking");t->pan=pan;}
        if(frame==252)check(app.error.empty()&&at(10,12)&&clean(),"pan health");
        click(254,app.layer_visible_point);
        if(frame==258){check(!layer()->visible,"hide before click guard");t->selection=rectangle_selection(48,64,12,12,6,6);}
        if(frame==260)pixel(13.5f,13.5f);if(frame==261)io.AddMouseButtonEvent(0,true);if(frame==262)move(2,1);if(frame==264)io.AddMouseButtonEvent(0,false);
        if(frame==268)check(!app.floating.active&&!app.layer_drag.active&&at(10,12)&&t->art->copy(0,0,0,48,64).pixels==base,"hidden overlay target never starts a base move");click(270,app.layer_visible_point);
        if(frame==274)check(layer()->visible&&t->art->save((fs::u8path(app.prefs)/"move-result.IMG").u8string(),app.error),"final shown save");
        if(frame==278)check(app.error.empty()&&at(10,12)&&clean(),"final health");
    }
};
