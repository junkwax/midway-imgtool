struct FiltersSmoke {
    int failures=0;PixelSelection scope;std::vector<unsigned char> original,expected;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio filters UI check failed at frame %d: %s; tool=%d stroke=%d dirty=%d mask=%d\n",frame,app.error.c_str(),(int)app.tool,app.stroke,art.dirty(),t->selection.count());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};
        auto pixel=[&](float x,float y){point(t->canvas_origin+ImVec2(x*t->canvas_zoom,y*t->canvas_zoom));};
        auto value=[&](int x,int y){return ((unsigned char *)art.sprite(0)->data_p)[y*48+x];};
        auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        auto baseline=[&]{check(art.save((fs::u8path(app.prefs)/"filters-fixture.IMG").u8string(),app.error));original=art.copy(0,0,0,48,64).pixels;};
        auto reopen=[&]{auto file=(fs::u8path(app.prefs)/"filters-result.IMG").u8string();check(art.save(file,app.error));auto loaded=Artwork::open(file,app.error);check(loaded&&loaded->copy(0,0,0,48,64).pixels==expected);};
        if(frame==0){std::vector<unsigned short> words={0,0x7fff,0,0x4210,0x294a,0x56b5,0,0x7c00,0x03ff};words.resize(16);int pi=art.add_palette("FILTER",words,app.error);check(pi>=0&&art.assign_palette(0,pi,false,app.error));t->anchors=false;t->radius=1;t->blur_strength=t->smudge_strength=100;
            art.begin("Filter fixture");for(int x=4;x<=10;++x)art.paint(0,x,4,(unsigned char)(x==4||x==6?1:2));art.commit();baseline();io.AddFocusEvent(true);}
        if(frame==3){scope=combine_selection(rectangle_selection(48,64,4,4,7,1),rectangle_selection(48,64,8,4,1,1),SelectionOp::Subtract,48,64);t->selection=scope;point(app.tools_ui.points[(int)Tool::Blur]);}
        if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==6){check(app.tool==Tool::Blur);pixel(5.5f,4.5f);}if(frame==7)io.AddMouseButtonEvent(0,true);if(frame==8)pixel(10.5f,4.5f);if(frame==9)io.AddMouseButtonEvent(0,false);
        if(frame==10){check(!app.stroke&&!app.filter.owner&&art.dirty()&&std::string(art.undo_label())=="Blur pixels");check(value(4,4)==1&&value(5,4)==5&&value(6,4)==4&&value(7,4)==3&&value(8,4)==2&&value(9,4)==2&&value(10,4)==2);expected=art.copy(0,0,0,48,64).pixels;key(ImGuiKey_Z,true);}if(frame==11)key(ImGuiKey_Z,false);
        if(frame==12){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==original);key(ImGuiKey_Y,true);}if(frame==13)key(ImGuiKey_Y,false);
        if(frame==14){check(art.copy(0,0,0,48,64).pixels==expected);reopen();app.action(Action::Undo);baseline();}
        if(frame==16){art.begin("Smudge fixture");for(int x=4;x<=10;++x)art.paint(0,x,6,(unsigned char)(x==4?1:x==8?0:2));art.commit();baseline();scope=combine_selection(rectangle_selection(48,64,4,6,7,1),rectangle_selection(48,64,7,6,1,1),SelectionOp::Subtract,48,64);t->selection=scope;point(app.tools_ui.points[(int)Tool::Smudge]);}
        if(frame==17)io.AddMouseButtonEvent(0,true);if(frame==18)io.AddMouseButtonEvent(0,false);
        if(frame==19){check(app.tool==Tool::Smudge);pixel(4.5f,6.5f);}if(frame==20)io.AddMouseButtonEvent(0,true);
        if(frame==21){check(app.stroke&&app.filter.owner&&!art.dirty()&&art.copy(0,0,0,48,64).pixels==original);pixel(10.5f,6.5f);}if(frame==22)io.AddMouseButtonEvent(0,false);
        if(frame==23){check(!app.stroke&&!app.filter.owner&&art.dirty()&&std::string(art.undo_label())=="Smudge pixels");for(int x=4;x<=10;++x)check(value(x,6)==(x<=6?1:x==8?0:2));expected=art.copy(0,0,0,48,64).pixels;key(ImGuiKey_Z,true);}if(frame==24)key(ImGuiKey_Z,false);
        if(frame==25){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==original);key(ImGuiKey_Y,true);}if(frame==26)key(ImGuiKey_Y,false);
        if(frame==27){check(art.copy(0,0,0,48,64).pixels==expected);reopen();app.action(Action::Undo);baseline();t->selection=scope;pixel(4.5f,6.5f);}if(frame==28)io.AddMouseButtonEvent(0,true);if(frame==29)pixel(10.5f,6.5f);
        if(frame==30){check(app.stroke&&app.filter.owner&&art.copy(0,0,0,48,64).pixels==expected);io.AddKeyEvent(ImGuiKey_Escape,true);}if(frame==31){io.AddKeyEvent(ImGuiKey_Escape,false);io.AddMouseButtonEvent(0,false);}
        if(frame==32){check(!app.stroke&&!app.filter.owner&&!art.dirty()&&art.copy(0,0,0,48,64).pixels==original&&t->selection.count()==scope.count());io.AddKeyEvent(ImGuiKey_O,true);}if(frame==33)io.AddKeyEvent(ImGuiKey_O,false);
        if(frame==34){check(app.tool==Tool::Blur&&!app.path_dialog);io.AddKeyEvent(ImGuiKey_S,true);}if(frame==35)io.AddKeyEvent(ImGuiKey_S,false);
        if(frame==36){check(app.tool==Tool::Smudge&&!app.path_dialog);io.AddKeyEvent(ImGuiKey_RightBracket,true);}if(frame==37)io.AddKeyEvent(ImGuiKey_RightBracket,false);
        if(frame==38){check(t->radius==2);app.demo();}if(frame==40){app.tool=Tool::Smudge;t->radius=3;point(app.tools_ui.points[(int)Tool::Smudge]);check(app.tools_ui.wheel_size>=90&&app.tools_ui.wheel_origin.y+app.tools_ui.wheel_size<io.DisplaySize.y);}
    }
};
