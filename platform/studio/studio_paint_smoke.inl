struct PaintSmoke {
    int failures=0;PixelSelection scope;std::vector<unsigned char> original,expected,fixture;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;auto &u=app.paint_line;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio paint UI check failed at frame %d: %s / %s; tool=%d line=%d dirty=%d endpoint=%d,%d changed=%d shift=%d\n",frame,u.error.c_str(),app.error.c_str(),(int)app.tool,u.active,art.dirty(),u.review.x1,u.review.y1,u.changed,io.KeyShift);}};
        auto pixel=[&](float x,float y){auto p=t->canvas_origin+ImVec2(x*t->canvas_zoom,y*t->canvas_zoom);io.AddMousePosEvent(p.x,p.y);};
        auto value=[&](int x,int y){return ((unsigned char *)art.sprite(0)->data_p)[y*48+x];};
        auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        if(frame==0){original=art.copy(0,0,0,48,64).pixels;check(art.save((fs::u8path(app.prefs)/"paint-original.IMG").u8string(),app.error));t->anchors=false;io.AddFocusEvent(true);}
        if(frame==3){scope=combine_selection(rectangle_selection(48,64,2,2,14,12),rectangle_selection(48,64,8,4,2,2),SelectionOp::Subtract,48,64);t->selection=scope;t->color=9;t->radius=1;io.AddKeyEvent(ImGuiKey_K,true);}if(frame==4){io.AddKeyEvent(ImGuiKey_K,false);pixel(4.5f,4.5f);}if(frame==5)io.AddMouseButtonEvent(0,true);if(frame==6){io.AddKeyEvent(ImGuiMod_Shift,true);pixel(14.5f,7.5f);}
        if(frame==8){check(u.active&&u.valid&&u.review.x1==14&&u.review.y1==4&&u.changed==9&&!art.dirty()&&art.copy(0,0,0,48,64).pixels==original);expected=u.pixels;io.AddKeyEvent(ImGuiKey_Escape,true);}if(frame==9){io.AddKeyEvent(ImGuiKey_Escape,false);io.AddMouseButtonEvent(0,false);io.AddKeyEvent(ImGuiMod_Shift,false);}
        if(frame==10)check(!u.active&&!art.dirty()&&t->selection.count()==scope.count()&&art.copy(0,0,0,48,64).pixels==original);
        if(frame==10)pixel(4.5f,4.5f);if(frame==11)io.AddMouseButtonEvent(0,true);if(frame==12)pixel(14.5f,4.5f);if(frame==13)io.AddMouseButtonEvent(0,false);
        if(frame==14){check(!u.active&&art.dirty()&&art.copy(0,0,0,48,64).pixels==expected&&value(8,4)==0&&value(9,4)==0);key(ImGuiKey_Z,true);}if(frame==15)key(ImGuiKey_Z,false);
        if(frame==16){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==original);key(ImGuiKey_Y,true);}if(frame==17)key(ImGuiKey_Y,false);
        if(frame==18){check(art.dirty()&&art.copy(0,0,0,48,64).pixels==expected);auto file=(fs::u8path(app.prefs)/"paint-line.IMG").u8string();check(art.save(file,app.error));auto loaded=Artwork::open(file,app.error);check(loaded&&loaded->copy(0,0,0,48,64).pixels==expected);app.action(Action::Undo);}
        if(frame==20){check(art.copy(0,0,0,48,64).pixels==original);art.begin("Paint fixture");for(int x=4;x<10;++x)art.paint(0,x,4,(unsigned char)(x==6||x==7?9:8));art.commit();check(art.save((fs::u8path(app.prefs)/"paint-fixture.IMG").u8string(),app.error));fixture=art.copy(0,0,0,48,64).pixels;t->selection=combine_selection(rectangle_selection(48,64,3,3,12,4),rectangle_selection(48,64,8,4,1,1),SelectionOp::Subtract,48,64);t->match_colors=false;t->replace_all=false;t->color=7;io.AddKeyEvent(ImGuiKey_B,true);}if(frame==21){io.AddKeyEvent(ImGuiKey_B,false);pixel(4.5f,4.5f);}if(frame==22)io.AddMouseButtonEvent(0,true);if(frame==23)pixel(9.5f,4.5f);if(frame==24)io.AddMouseButtonEvent(0,false);
        if(frame==25){check(value(4,4)==7&&value(5,4)==7&&value(6,4)==9&&value(7,4)==9&&value(8,4)==8&&value(9,4)==7&&art.dirty());key(ImGuiKey_Z,true);}if(frame==26)key(ImGuiKey_Z,false);
        if(frame==27){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==fixture);t->fill_connected=false;t->color=6;io.AddKeyEvent(ImGuiKey_G,true);}if(frame==28){io.AddKeyEvent(ImGuiKey_G,false);pixel(4.5f,4.5f);}if(frame==29)io.AddMouseButtonEvent(0,true);if(frame==30)io.AddMouseButtonEvent(0,false);
        if(frame==31){check(value(4,4)==6&&value(5,4)==6&&value(9,4)==6&&value(8,4)==8&&value(6,4)==9);key(ImGuiKey_Z,true);}if(frame==32)key(ImGuiKey_Z,false);
        if(frame==33){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==fixture);io.AddKeyEvent(ImGuiKey_U,true);}if(frame==34){io.AddKeyEvent(ImGuiKey_U,false);pixel(6.5f,4.5f);}if(frame==35)io.AddMouseButtonEvent(0,true);if(frame==36)io.AddMouseButtonEvent(0,false);
        if(frame==37){check(value(6,4)==0&&value(7,4)==0&&value(4,4)==8&&value(8,4)==8&&value(9,4)==8);key(ImGuiKey_Z,true);}if(frame==38)key(ImGuiKey_Z,false);
        if(frame==39){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==fixture);t->brush_shape=BrushShape::Square;t->color=9;t->radius=1;t->selection=combine_selection(rectangle_selection(48,64,3,3,3,3),rectangle_selection(48,64,4,4,1,1),SelectionOp::Subtract,48,64);io.AddKeyEvent(ImGuiKey_P,true);}if(frame==40)io.AddKeyEvent(ImGuiKey_P,false);
        if(frame==41)io.AddKeyEvent(ImGuiKey_RightBracket,true);if(frame==42){io.AddKeyEvent(ImGuiKey_RightBracket,false);pixel(4.5f,4.5f);}if(frame==43)io.AddMouseButtonEvent(0,true);if(frame==44)io.AddMouseButtonEvent(0,false);
        if(frame==45){check(t->radius==2&&value(4,4)==8&&art.dirty());for(int y=3;y<6;++y)for(int x=3;x<6;++x)if(x!=4||y!=4)check(value(x,y)==9);key(ImGuiKey_Z,true);}if(frame==46)key(ImGuiKey_Z,false);
        if(frame==47){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==fixture);t->selection=scope;io.AddKeyEvent(ImGuiKey_K,true);}if(frame==48){io.AddKeyEvent(ImGuiKey_K,false);pixel(4.5f,6.5f);}if(frame==49)io.AddMouseButtonEvent(0,true);
        if(frame==50){check(u.active&&!art.dirty());art.color(0,1,0x7fff);art.commit();}if(frame==51)io.AddMouseButtonEvent(0,false);
        if(frame==52){check(!u.active&&art.dirty()&&art.copy(0,0,0,48,64).pixels==fixture);app.action(Action::Undo);}
        if(frame==54){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==fixture);t->selection=combine_selection(rectangle_selection(48,64,16,8,24,32),rectangle_selection(48,64,20,18,2,2),SelectionOp::Subtract,48,64);t->color=9;t->radius=2;pixel(20.5f,12.5f);}if(frame==55)io.AddMouseButtonEvent(0,true);if(frame==56){io.AddKeyEvent(ImGuiMod_Shift,true);pixel(32.5f,26.5f);}
        if(frame==58)check(u.active&&u.valid&&u.changed>0&&!art.dirty()&&art.copy(0,0,0,48,64).pixels==fixture);
    }
};
