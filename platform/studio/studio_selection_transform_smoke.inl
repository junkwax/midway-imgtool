struct FloatingSmoke {
    int failures=0;PixelSelection selection;std::vector<unsigned char> original,expected;uint64_t generation=0;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;auto &u=app.floating;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio floating selection check failed at frame %d: %s / %s; active=%d dirty=%d selected=%d\n",frame,u.error.c_str(),app.error.c_str(),u.active,art.dirty(),t->selected);}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};auto pixel=[&](float x,float y){point(t->canvas_origin+ImVec2(x*t->canvas_zoom,y*t->canvas_zoom));};
        auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        if(frame==0){original=art.copy(0,0,0,48,64).pixels;check(art.save((fs::u8path(app.prefs)/"floating-original.IMG").u8string(),app.error));t->anchors=false;io.AddFocusEvent(true);}
        if(frame==3){selection=combine_selection(rectangle_selection(48,64,16,8,16,15),rectangle_selection(48,64,20,12,2,2),SelectionOp::Subtract,48,64);t->selection=selection;io.AddKeyEvent(ImGuiKey_V,true);}if(frame==4){io.AddKeyEvent(ImGuiKey_V,false);pixel(20.5f,10.5f);}if(frame==5)io.AddMouseButtonEvent(0,true);if(frame==6)point(u.drag_mouse+ImVec2(3*t->canvas_zoom,-t->canvas_zoom));if(frame==7)io.AddMouseButtonEvent(0,false);
        if(frame==8){check(u.active&&u.valid&&!art.dirty()&&u.stage.x==19&&u.stage.y==7&&art.copy(0,0,0,48,64).pixels==original);point(u.flip_point);}if(frame==9)io.AddMouseButtonEvent(0,true);if(frame==10)io.AddMouseButtonEvent(0,false);
        if(frame==11){check(u.stage.flip_h&&!art.dirty());point(u.rotate_point);}if(frame==12)io.AddMouseButtonEvent(0,true);if(frame==13)io.AddMouseButtonEvent(0,false);
        if(frame==14){check(u.stage.rotation==1&&u.stage.w==15&&u.stage.h==16&&!u.stage.flip_h&&u.stage.flip_v);io.AddKeyEvent(ImGuiKey_Escape,true);}if(frame==15)io.AddKeyEvent(ImGuiKey_Escape,false);
        if(frame==16)check(!u.active&&!art.dirty()&&t->selection.count()==selection.count()&&art.copy(0,0,0,48,64).pixels==original);
        if(frame==18){app.action(Action::TransformSelection);u.stage.x=4;u.stage.y=4;u.dirty=true;}
        if(frame==20)pixel((float)(u.stage.x+u.stage.w),(float)(u.stage.y+u.stage.h));if(frame==21)io.AddMouseButtonEvent(0,true);if(frame==22)point(u.drag_mouse+ImVec2(4*t->canvas_zoom,4*t->canvas_zoom));if(frame==23)io.AddMouseButtonEvent(0,false);
        if(frame==24){check(u.stage.w==20&&u.stage.h==19&&u.valid&&!art.dirty());point(u.copy_point);}if(frame==25)io.AddMouseButtonEvent(0,true);if(frame==26)io.AddMouseButtonEvent(0,false);
        if(frame==27){check(u.stage.copy);SelectionTransformPreview preview;check(preview_selection_transform(u.stage,preview,app.error));expected=preview.pixels;point(u.apply_point);}if(frame==28)io.AddMouseButtonEvent(0,true);if(frame==29)io.AddMouseButtonEvent(0,false);
        if(frame==30){check(!u.active&&art.dirty()&&art.copy(0,0,0,48,64).pixels==expected&&art.sprite(0)->anix==24&&art.sprite(0)->aniy==60);}
        if(frame==31)key(ImGuiKey_Z,true);if(frame==32)key(ImGuiKey_Z,false);
        if(frame==33){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==original);key(ImGuiKey_Y,true);}if(frame==34)key(ImGuiKey_Y,false);
        if(frame==35){check(art.dirty()&&art.copy(0,0,0,48,64).pixels==expected);auto file=(fs::u8path(app.prefs)/"floating-result.IMG").u8string();check(art.save(file,app.error));auto loaded=Artwork::open(file,app.error);check(loaded&&loaded->copy(0,0,0,48,64).pixels==expected);app.action(Action::Undo);}
        if(frame==37){check(art.copy(0,0,0,48,64).pixels==original);check(art.save((fs::u8path(app.prefs)/"floating-restored.IMG").u8string(),app.error));t->selection=selection;app.action(Action::TransformSelection);}
        if(frame==39)io.AddKeyEvent(ImGuiKey_UpArrow,true);if(frame==40)io.AddKeyEvent(ImGuiKey_UpArrow,false);
        if(frame==41){check(u.active&&u.stage.y==7&&t->selected==0&&!art.dirty());key(ImGuiKey_Z,true);}if(frame==42)key(ImGuiKey_Z,false);
        if(frame==43){check(!u.active&&!art.dirty()&&art.can_redo());t->selection=selection;app.action(Action::TransformSelection);}
        if(frame==45){art.color(0,1,0x7fff);art.commit();}if(frame==46){check(!u.active&&art.dirty());app.action(Action::Undo);}
        if(frame==47){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==original);t->selection=selection;app.action(Action::TransformSelection);u.stage.w=60;u.dirty=true;generation=art.generation();}
        if(frame==49){check(u.active&&!u.valid&&!u.error.empty()&&art.generation()==generation);app.apply_floating();check(art.generation()==generation);u.stage.w=20;u.stage.h=21;u.stage.x=8;u.stage.y=7;u.stage.rotation=1;u.stage.copy=true;u.stage.flip_h=true;u.dirty=true;}
        if(frame==51)check(u.active&&u.valid&&!art.dirty());
    }
};
