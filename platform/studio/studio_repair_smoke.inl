struct RepairSmoke {
    int failures=0;PixelSelection scope;std::vector<unsigned char> original,expected;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio repair UI check failed at frame %d: %s; tool=%d stroke=%d dirty=%d mask=%d\n",frame,app.error.c_str(),(int)app.tool,app.stroke,art.dirty(),t->selection.count());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};auto pixel=[&](float x,float y){point(t->canvas_origin+ImVec2(x*t->canvas_zoom,y*t->canvas_zoom));};
        auto value=[&](int x,int y){return ((unsigned char *)art.sprite(0)->data_p)[y*48+x];};auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        auto baseline=[&]{check(art.save((fs::u8path(app.prefs)/"repair-fixture.IMG").u8string(),app.error));original=art.copy(0,0,0,48,64).pixels;};
        auto reopen=[&]{auto file=(fs::u8path(app.prefs)/"repair-result.IMG").u8string();check(art.save(file,app.error));auto loaded=Artwork::open(file,app.error);check(loaded&&loaded->copy(0,0,0,48,64).pixels==expected);};
        auto clear=[&]{art.begin("Repair fixture");for(int y=0;y<10;++y)for(int x=0;x<48;++x)art.paint(0,x,y,0);};
        auto inspector=[&]{point(ImVec2(io.DisplaySize.x-120,io.DisplaySize.y-140));};
        if(frame==0){std::vector<unsigned short> words={0,0x001f,0x7c00,0x03ff,0,0x7fff,0x03e0,0,0x3c0f};words.resize(16);int pi=art.add_palette("REPAIR",words,app.error);check(pi>=0&&art.assign_palette(0,pi,false,app.error));t->anchors=false;t->radius=1;clear();for(int y=3;y<=5;++y)for(int x=4;x<=10;++x)art.paint(0,x,y,2);for(int x=5;x<=9;++x)art.paint(0,x,4,1);art.commit();baseline();io.AddFocusEvent(true);}
        if(frame==3){scope=combine_selection(rectangle_selection(48,64,3,2,9,5),rectangle_selection(48,64,7,4,1,1),SelectionOp::Subtract,48,64);t->selection=scope;point(app.tools_ui.points[(int)Tool::Heal]);}if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==6){check(app.tool==Tool::Heal);pixel(5.5f,4.5f);}if(frame==7)io.AddMouseButtonEvent(0,true);if(frame==8)pixel(9.5f,4.5f);if(frame==9)io.AddMouseButtonEvent(0,false);
        if(frame==10){check(!app.stroke&&!app.repair.owner&&art.dirty()&&std::string(art.undo_label())=="Heal pixels");for(int x=5;x<=9;++x)check(value(x,4)==(x==7?1:2));check(value(3,4)==0);expected=art.copy(0,0,0,48,64).pixels;key(ImGuiKey_Z,true);}if(frame==11)key(ImGuiKey_Z,false);
        if(frame==12){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==original);key(ImGuiKey_Y,true);}if(frame==13)key(ImGuiKey_Y,false);
        if(frame==14){check(art.copy(0,0,0,48,64).pixels==expected);reopen();app.action(Action::Undo);baseline();t->selection=scope;pixel(5.5f,4.5f);}if(frame==15)io.AddMouseButtonEvent(0,true);if(frame==16)pixel(9.5f,4.5f);
        if(frame==17){check(app.stroke&&app.repair.owner&&art.copy(0,0,0,48,64).pixels==expected);io.AddKeyEvent(ImGuiKey_Escape,true);}if(frame==18){io.AddKeyEvent(ImGuiKey_Escape,false);io.AddMouseButtonEvent(0,false);}
        if(frame==19){check(!app.stroke&&!app.repair.owner&&!art.dirty()&&art.copy(0,0,0,48,64).pixels==original&&t->selection.count()==scope.count());clear();for(int y=2;y<=6;++y)for(int x=5;x<=9;++x)art.paint(0,x,y,(unsigned char)(x==5||x==9||y==2||y==6?1:2));art.commit();baseline();scope=combine_selection(rectangle_selection(48,64,4,1,7,7),rectangle_selection(48,64,5,2,1,1),SelectionOp::Subtract,48,64);t->selection=scope;point(app.tools_ui.points[(int)Tool::Defringe]);}
        if(frame==20)io.AddMouseButtonEvent(0,true);if(frame==21)io.AddMouseButtonEvent(0,false);
        if(frame==22){check(app.tool==Tool::Defringe);pixel(5.5f,4.5f);}if(frame==23)io.AddMouseButtonEvent(0,true);if(frame==24)io.AddMouseButtonEvent(0,false);
        if(frame==25){check(value(5,4)==2&&value(5,2)==1&&art.dirty());app.action(Action::Undo);check(art.copy(0,0,0,48,64).pixels==original);}
        if(frame==28)inspector();if(frame==29&&app.tools_ui.edges_point.y>io.DisplaySize.y-55)io.AddMouseWheelEvent(0,-1);if(frame==31)point(app.tools_ui.edges_point);if(frame==32)io.AddMouseButtonEvent(0,true);if(frame==33)io.AddMouseButtonEvent(0,false);
        if(frame==34){check(art.dirty()&&std::string(art.undo_label())=="Defringe edges");for(int y=2;y<=6;++y)for(int x=5;x<=9;++x)check(value(x,y)==(x==5&&y==2?1:2));check(value(4,4)==0);expected=art.copy(0,0,0,48,64).pixels;reopen();app.action(Action::Undo);baseline();}
        if(frame==36){clear();for(int y=2;y<=4;++y){art.paint(0,4,y,1);art.paint(0,5,y,3);for(int x=6;x<=8;++x)art.paint(0,x,y,2);}art.commit();baseline();scope=combine_selection(rectangle_selection(48,64,3,1,7,5),rectangle_selection(48,64,5,3,1,1),SelectionOp::Subtract,48,64);t->selection=scope;io.AddKeyEvent(ImGuiKey_U,true);}if(frame==37)io.AddKeyEvent(ImGuiKey_U,false);
        if(frame==38){check(app.tool==Tool::SmartErase&&t->erase_cleanup);pixel(4.5f,3.5f);}if(frame==39)io.AddMouseButtonEvent(0,true);if(frame==40)io.AddMouseButtonEvent(0,false);
        if(frame==41){check(art.dirty()&&std::string(art.undo_label())=="Smart erase and clean edges");for(int y=2;y<=4;++y){check(value(4,y)==0);check(value(5,y)==(y==3?3:2));}expected=art.copy(0,0,0,48,64).pixels;key(ImGuiKey_Z,true);}if(frame==42)key(ImGuiKey_Z,false);
        if(frame==43){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==original);key(ImGuiKey_Y,true);}if(frame==44)key(ImGuiKey_Y,false);
        if(frame==45){check(art.copy(0,0,0,48,64).pixels==expected);reopen();app.action(Action::Undo);baseline();t->selection=scope;inspector();}if(frame==46)io.AddMouseWheelEvent(0,-8);if(frame==48)point(app.tools_ui.cleanup_point);if(frame==49)io.AddMouseButtonEvent(0,true);if(frame==50)io.AddMouseButtonEvent(0,false);
        if(frame==51){check(!t->erase_cleanup&&!art.dirty());pixel(4.5f,3.5f);}if(frame==52)io.AddMouseButtonEvent(0,true);if(frame==53)io.AddMouseButtonEvent(0,false);
        if(frame==54){check(art.dirty()&&std::string(art.undo_label())=="Smart erase pixels");for(int y=2;y<=4;++y)check(value(4,y)==0&&value(5,y)==3);app.action(Action::Undo);check(art.copy(0,0,0,48,64).pixels==original);io.AddKeyEvent(ImGuiKey_J,true);}if(frame==55)io.AddKeyEvent(ImGuiKey_J,false);
        if(frame==56){check(app.tool==Tool::Heal);io.AddKeyEvent(ImGuiKey_F,true);}if(frame==57)io.AddKeyEvent(ImGuiKey_F,false);
        if(frame==58){check(app.tool==Tool::Defringe);app.demo();}if(frame==60){app.tool=Tool::Heal;t->radius=3;check(app.tools_ui.wheel_size>=90&&app.tools_ui.wheel_origin.y+app.tools_ui.wheel_size<io.DisplaySize.y);point(ImVec2(5,5));}
    }
};
