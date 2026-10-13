struct RegionSmoke {
    int failures=0,body_count=0;PixelSelection start,moved,selected;std::vector<unsigned char> original;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;auto &u=app.regions;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio region UI check failed at frame %d: %s / %s / count=%d dirty=%d selected=%d undo=%s text=%d\n",frame,u.error.c_str(),app.error.c_str(),art.count(),art.dirty(),t->selected,art.undo_label(),io.WantTextInput);}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};
        auto boxpoint=[&](const PixelSelection &s){point(u.preview_origin+ImVec2((s.x+s.w*.5f)*u.preview_zoom,(s.y+s.h*.5f)*u.preview_zoom));};
        auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        if(frame==0){original=art.copy(0,0,0,48,64).pixels;check(art.save((fs::u8path(app.prefs)/"region-original.IMG").u8string(),app.error));io.AddFocusEvent(true);app.action(Action::SplitBody);}
        if(frame==3){check(u.open&&u.valid&&u.requests.size()==5&&!art.dirty());start=u.requests[0].selection;boxpoint(start);}if(frame==4)io.AddMouseButtonEvent(0,true);
        if(frame==5)point(u.drag_mouse+ImVec2(u.preview_zoom,u.preview_zoom));if(frame==6)io.AddMouseButtonEvent(0,false);
        if(frame==7){moved=u.requests[0].selection;check(moved.x==start.x+1&&moved.y==start.y+1&&!art.dirty());boxpoint(moved);}if(frame==8)io.AddMouseButtonEvent(0,true);if(frame==9)point(u.drag_mouse+ImVec2(2*u.preview_zoom,2*u.preview_zoom));
        if(frame==10)io.AddKeyEvent(ImGuiKey_Escape,true);if(frame==11){io.AddKeyEvent(ImGuiKey_Escape,false);io.AddMouseButtonEvent(0,false);}
        if(frame==12){check(u.open&&u.requests[0].selection.x==moved.x&&!art.dirty());point(u.cancel_point);}if(frame==13)io.AddMouseButtonEvent(0,true);if(frame==14)io.AddMouseButtonEvent(0,false);
        if(frame==15){check(!u.open&&art.count()==8&&!art.dirty());selected=combine_selection(rectangle_selection(48,64,16,8,16,15),rectangle_selection(48,64,20,12,2,2),SelectionOp::Subtract,48,64);t->selection=selected;app.action(Action::ExtractSelection);}
        if(frame==17){check(u.open&&u.valid&&u.plan.children.size()==1&&u.requests[0].selection.contains(16,8)&&!u.requests[0].selection.contains(20,12));point(u.cut_point);}if(frame==18)io.AddMouseButtonEvent(0,true);if(frame==19)io.AddMouseButtonEvent(0,false);
        if(frame==20){check(u.cut&&u.valid&&!art.dirty());u.view=1;u.dirty=true;}
        if(frame==21)point(u.apply_point);if(frame==22)io.AddMouseButtonEvent(0,true);if(frame==23)io.AddMouseButtonEvent(0,false);
        if(frame==24){check(!u.open&&art.count()==9&&art.dirty());auto pixels=art.copy(0,0,0,48,64).pixels;for(int y=0;y<64;++y)for(int x=0;x<48;++x)check(pixels[y*48+x]==(selected.contains(x,y)?0:original[y*48+x]));check(art.sprite(8)->anix==8&&art.sprite(8)->aniy==52);}
        if(frame==25)key(ImGuiKey_Z,true);if(frame==26)key(ImGuiKey_Z,false);
        if(frame==27){check(art.count()==8&&!art.dirty()&&art.copy(0,0,0,48,64).pixels==original);app.action(Action::Redo);}
        if(frame==28){check(art.count()==9);check(art.save((fs::u8path(app.prefs)/"region-cut.IMG").u8string(),app.error));auto loaded=Artwork::open((fs::u8path(app.prefs)/"region-cut.IMG").u8string(),app.error);check(loaded&&loaded->count()==9);app.action(Action::Undo);check(art.count()==8);app.action(Action::SplitBody);}
        if(frame==30){check(u.open&&u.valid&&!u.cut);body_count=(int)u.plan.children.size();point(u.apply_point);}if(frame==31)io.AddMouseButtonEvent(0,true);if(frame==32)io.AddMouseButtonEvent(0,false);
        if(frame==33){check(!u.open&&art.count()==8+body_count&&art.copy(0,0,0,48,64).pixels==original);}
        if(frame==34)key(ImGuiKey_Z,true);if(frame==35)key(ImGuiKey_Z,false);
        if(frame==36){check(art.count()==8&&art.copy(0,0,0,48,64).pixels==original);app.action(Action::SplitBody);}
    }
};
