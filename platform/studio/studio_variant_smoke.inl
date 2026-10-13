struct VariantSmoke {
    int failures=0,alt=0;uint64_t gen=0;PixelSelection scope;std::vector<unsigned char> original,expected;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;auto &u=app.variant;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio variant UI check failed at frame %d: %s / %s; tool=%d active=%d dragging=%d valid=%d changed=%d new=%d grows=%d dirty=%d\n",frame,u.error.c_str(),app.error.c_str(),(int)app.tool,u.active,u.dragging,u.valid,u.plan.changed,u.plan.new_slots,(int)u.plan.grown_palettes.size(),art.dirty());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};auto pixel=[&](float x,float y){point(t->canvas_origin+ImVec2(x*t->canvas_zoom,y*t->canvas_zoom));};
        auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        auto baseline=[&]{check(art.save((fs::u8path(app.prefs)/"variant-fixture.IMG").u8string(),app.error));gen=art.generation();};
        if(frame==0){t->anchors=false;t->ppp=4;std::vector<unsigned short> words(16);for(int i=0;i<16;++i)words[i]=(unsigned short)(i*321);alt=art.add_palette("ALT",words,app.error);check(alt==1);art.begin("Variant fixture");for(int x=0;x<16;++x)art.paint(1,x,0,(unsigned char)x);for(int x=4;x<=10;++x)art.paint(0,x,4,(unsigned char)(x==10?0:x==6||x==7?7:8));art.commit();baseline();original=art.copy(0,0,0,48,64).pixels;io.AddFocusEvent(true);}
        if(frame==3){scope=combine_selection(rectangle_selection(48,64,4,4,7,1),rectangle_selection(48,64,8,4,1,1),SelectionOp::Subtract,48,64);t->selection=scope;t->color=9;t->radius=1;io.AddKeyEvent(ImGuiKey_T,true);}if(frame==4){io.AddKeyEvent(ImGuiKey_T,false);pixel(4.5f,4.5f);}if(frame==5)io.AddMouseButtonEvent(0,true);if(frame==6)pixel(10.5f,4.5f);if(frame==7)io.AddMouseButtonEvent(0,false);
        if(frame==8){check(u.active&&!u.dragging&&u.valid&&u.plan.changed==5&&u.plan.transparent==1&&u.plan.new_slots==2&&u.plan.grown_palettes.size()==2&&u.plan.ppp_breaks.size()==2&&art.generation()==gen&&!art.dirty()&&art.palette(0)->numc==16&&art.copy(0,0,0,48,64).pixels==original);expected=u.plan.pixels;io.AddKeyEvent(ImGuiKey_Escape,true);}if(frame==9)io.AddKeyEvent(ImGuiKey_Escape,false);
        if(frame==10){check(!u.active&&!art.dirty()&&t->selection.count()==scope.count());pixel(4.5f,4.5f);}if(frame==11)io.AddMouseButtonEvent(0,true);if(frame==12)pixel(10.5f,4.5f);if(frame==13)io.AddMouseButtonEvent(0,false);
        if(frame==14){check(u.valid&&u.plan.pixels==expected&&!art.dirty());point(u.apply_point);}if(frame==15)io.AddMouseButtonEvent(0,true);if(frame==16)io.AddMouseButtonEvent(0,false);
        if(frame==17){check(!u.active&&art.dirty()&&art.palette(0)->numc==18&&art.palette(1)->numc==18&&art.copy(0,0,0,48,64).pixels==expected);key(ImGuiKey_Z,true);}if(frame==18)key(ImGuiKey_Z,false);
        if(frame==19){check(!art.dirty()&&art.palette(0)->numc==16&&art.palette(1)->numc==16&&art.copy(0,0,0,48,64).pixels==original);key(ImGuiKey_Y,true);}if(frame==20)key(ImGuiKey_Y,false);
        if(frame==21){check(art.copy(0,0,0,48,64).pixels==expected);auto file=(fs::u8path(app.prefs)/"variant-result.IMG").u8string();check(art.save(file,app.error));auto reopened=Artwork::open(file,app.error);check(reopened&&reopened->palette(0)->numc==18&&reopened->copy(0,0,0,48,64).pixels==expected);app.action(Action::Undo);baseline();}
        if(frame==23)point(u.selection_point);if(frame==24)io.AddMouseButtonEvent(0,true);if(frame==25)io.AddMouseButtonEvent(0,false);
        if(frame==28){check(u.active&&u.valid&&u.plan.pixels==expected&&!art.dirty()&&art.generation()==gen);point(u.cancel_point);}if(frame==29)io.AddMouseButtonEvent(0,true);if(frame==30)io.AddMouseButtonEvent(0,false);
        if(frame==31){check(!u.active&&!art.dirty()&&art.generation()==gen);app.begin_variant(*t,true);}if(frame==32){check(u.active&&u.valid);art.color(0,1,0x7fff);art.commit();}
        if(frame==33){check(!u.active&&art.dirty()&&art.palette(0)->numc==16&&art.copy(0,0,0,48,64).pixels==original);app.action(Action::Undo);baseline();}
        if(frame==35)app.begin_variant(*t,true);if(frame==36){check(u.active&&u.valid);u.preview_palette=alt;u.dirty=true;}if(frame==37)check(u.valid&&u.preview_palette==alt&&!art.dirty());
        if(frame==38){u.preview_palette=0;u.dirty=true;}if(frame==39)point(u.original_point);if(frame==40)io.AddMouseButtonEvent(0,true);if(frame==41)io.AddMouseButtonEvent(0,false);
        if(frame==42){check(u.show_original&&!art.dirty());point(u.original_point);}if(frame==43)io.AddMouseButtonEvent(0,true);if(frame==44)io.AddMouseButtonEvent(0,false);
        if(frame==45)check(!u.show_original&&u.valid&&art.generation()==gen);if(frame==46)key(ImGuiKey_Z,true);if(frame==47)key(ImGuiKey_Z,false);
        if(frame==48){check(!u.active&&!art.dirty());app.begin_variant(*t,true);}if(frame==49){check(u.valid&&u.plan.changed==5&&!art.dirty());io.AddKeyEvent(ImGuiKey_Enter,true);}if(frame==50)io.AddKeyEvent(ImGuiKey_Enter,false);
        if(frame==51){check(!u.active&&art.dirty()&&art.copy(0,0,0,48,64).pixels==expected);app.action(Action::Undo);baseline();}if(frame==53)app.begin_variant(*t,true);if(frame==54)check(u.valid&&u.plan.changed==5&&!art.dirty());
    }
};
