struct CloneSmoke {
    int failures=0;PixelSelection scope;std::vector<unsigned char> original,expected;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio clone UI check failed at frame %d: %s; tool=%d stroke=%d source=%llu offset=%d,%d dirty=%d\n",frame,app.error.c_str(),(int)app.tool,app.stroke,(unsigned long long)t->clone_sprite,t->clone_dx,t->clone_dy,art.dirty());}};
        auto pixel=[&](float x,float y){auto p=t->canvas_origin+ImVec2(x*t->canvas_zoom,y*t->canvas_zoom);io.AddMousePosEvent(p.x,p.y);};
        auto value=[&](int x,int y){return ((unsigned char *)art.sprite(0)->data_p)[y*48+x];};
        auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        auto baseline=[&]{check(art.save((fs::u8path(app.prefs)/"clone-fixture.IMG").u8string(),app.error));};
        if(frame==0){t->anchors=false;art.begin("Clone fixture");for(int y=4;y<=6;++y)for(int x=4;x<14;++x)art.paint(0,x,y,(unsigned char)(x-3));art.commit();baseline();original=art.copy(0,0,0,48,64).pixels;io.AddFocusEvent(true);}
        if(frame==3){scope=combine_selection(rectangle_selection(48,64,5,4,9,3),rectangle_selection(48,64,8,4,1,1),SelectionOp::Subtract,48,64);t->selection=scope;t->radius=1;io.AddKeyEvent(ImGuiKey_C,true);}
        if(frame==4){io.AddKeyEvent(ImGuiKey_C,false);pixel(4.5f,4.5f);io.AddKeyEvent(ImGuiMod_Alt,true);}if(frame==6)io.AddMouseButtonEvent(0,true);if(frame==7)io.AddMouseButtonEvent(0,false);if(frame==8)io.AddKeyEvent(ImGuiMod_Alt,false);
        if(frame==9){check(app.tool==Tool::Clone&&t->clone_sprite==art.sprite_id(0)&&t->clone_x==4&&t->clone_y==4&&!t->clone_offset&&!art.dirty());pixel(5.5f,4.5f);}if(frame==10)io.AddMouseButtonEvent(0,true);if(frame==11)pixel(13.5f,4.5f);if(frame==12)io.AddMouseButtonEvent(0,false);
        if(frame==13){check(!app.stroke&&!app.clone.owner&&t->clone_dx==-1&&t->clone_dy==0&&art.dirty());for(int x=5;x<14;++x)check(value(x,4)==(x==8?5:x-4));check(value(4,4)==1);expected=art.copy(0,0,0,48,64).pixels;key(ImGuiKey_Z,true);}if(frame==14)key(ImGuiKey_Z,false);
        if(frame==15){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==original);key(ImGuiKey_Y,true);}if(frame==16)key(ImGuiKey_Y,false);
        if(frame==17){check(art.copy(0,0,0,48,64).pixels==expected);auto file=(fs::u8path(app.prefs)/"clone-result.IMG").u8string();check(art.save(file,app.error));auto reopened=Artwork::open(file,app.error);check(reopened&&reopened->copy(0,0,0,48,64).pixels==expected);app.action(Action::Undo);baseline();}
        if(frame==19)pixel(10.5f,6.5f);if(frame==20)io.AddMouseButtonEvent(0,true);if(frame==21)io.AddMouseButtonEvent(0,false);
        if(frame==22){check(value(10,6)==6&&t->clone_dx==-1);app.action(Action::Undo);check(art.copy(0,0,0,48,64).pixels==original);t->clone_aligned=false;t->clone_offset=false;pixel(10.5f,6.5f);}if(frame==23)io.AddMouseButtonEvent(0,true);if(frame==24)io.AddMouseButtonEvent(0,false);
        if(frame==25){check(value(10,6)==1&&t->clone_dx==-6&&t->clone_dy==-2);app.action(Action::Undo);check(!art.dirty());pixel(5.5f,4.5f);}if(frame==26)io.AddMouseButtonEvent(0,true);if(frame==27)pixel(13.5f,4.5f);
        if(frame==28){check(app.stroke&&app.clone.owner&&art.copy(0,0,0,48,64).pixels==expected);io.AddKeyEvent(ImGuiKey_Escape,true);}if(frame==29){io.AddKeyEvent(ImGuiKey_Escape,false);io.AddMouseButtonEvent(0,false);}
        if(frame==30){check(!app.stroke&&!app.clone.owner&&!art.dirty()&&art.copy(0,0,0,48,64).pixels==original&&t->selection.count()==scope.count());app.select(1);}
        if(frame==31){check(!t->clone_sprite);app.select(0);}if(frame==32){check(!t->clone_sprite);pixel(5.5f,4.5f);}if(frame==33)io.AddMouseButtonEvent(0,true);if(frame==34)io.AddMouseButtonEvent(0,false);
        if(frame==35){check(!app.stroke&&!art.dirty());io.AddKeyEvent(ImGuiKey_P,true);}if(frame==36)io.AddKeyEvent(ImGuiKey_P,false);if(frame==37)key(ImGuiKey_C,true);if(frame==38)key(ImGuiKey_C,false);
        if(frame==39){check(app.tool==Tool::Pencil&&app.clipboard.w>0);io.AddKeyEvent(ImGuiKey_C,true);}if(frame==40){io.AddKeyEvent(ImGuiKey_C,false);t->selection={};t->radius=3;t->clone_aligned=true;t->clone_offset=false;pixel(18.5f,22.5f);io.AddKeyEvent(ImGuiMod_Alt,true);}if(frame==42)io.AddMouseButtonEvent(0,true);if(frame==43)io.AddMouseButtonEvent(0,false);if(frame==44)io.AddKeyEvent(ImGuiMod_Alt,false);
        if(frame==45){check(t->clone_x==18&&t->clone_y==22&&!art.dirty());pixel(28.5f,26.5f);}if(frame==46)io.AddMouseButtonEvent(0,true);if(frame==47)io.AddMouseButtonEvent(0,false);
        if(frame==48){check(!app.stroke&&t->clone_dx==-10&&t->clone_dy==-4);pixel(30.5f,30.5f);}
    }
};
