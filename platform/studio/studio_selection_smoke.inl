struct SelectionSmoke {
    int failures=0;std::vector<unsigned char> original;PixelSelection before_wand;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio selection UI check failed at frame %d: %s; selected=%d; tool=%d; error=%s\n",frame,art.undo_label(),t->selection.count(),(int)app.tool,app.error.c_str());}};
        auto point=[&](float x,float y){auto p=t->canvas_origin+ImVec2(x*t->canvas_zoom,y*t->canvas_zoom);io.AddMousePosEvent(p.x,p.y);};
        auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        auto pixel=[&](int x,int y){return ((unsigned char *)art.sprite(0)->data_p)[y*48+x];};
        if(frame==0){original=art.copy(0,0,0,48,64).pixels;check(art.save((fs::u8path(app.prefs)/"selection-original.IMG").u8string(),app.error));t->anchors=false;io.AddFocusEvent(true);}
        if(frame==3)point(4.5f,4.5f);if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)point(9.5f,9.5f);if(frame==6)io.AddMouseButtonEvent(0,false);
        if(frame==7){check(t->selection.count()==36&&!art.dirty());io.AddKeyEvent(ImGuiMod_Shift,true);point(12.5f,4.5f);}if(frame==8)io.AddMouseButtonEvent(0,true);if(frame==9)point(14.5f,6.5f);if(frame==10)io.AddMouseButtonEvent(0,false);
        if(frame==11){check(t->selection.count()==45);io.AddKeyEvent(ImGuiMod_Shift,false);}
        if(frame==12){io.AddKeyEvent(ImGuiMod_Alt,true);point(6.5f,6.5f);}if(frame==13)io.AddMouseButtonEvent(0,true);if(frame==14)point(7.5f,7.5f);if(frame==15)io.AddMouseButtonEvent(0,false);
        if(frame==16){io.AddKeyEvent(ImGuiMod_Alt,false);check(t->selection.count()==41&&!t->selection.contains(6,6));app.tool=Tool::Pencil;t->color=9;point(6.5f,6.5f);}if(frame==17)io.AddMouseButtonEvent(0,true);if(frame==18)io.AddMouseButtonEvent(0,false);
        if(frame==19){check(!art.dirty()&&pixel(6,6)==0);point(4.5f,4.5f);}if(frame==20)io.AddMouseButtonEvent(0,true);if(frame==21)io.AddMouseButtonEvent(0,false);
        if(frame==22){check(art.dirty()&&pixel(4,4)==9);key(ImGuiKey_C,true);}if(frame==23)key(ImGuiKey_C,false);
        if(frame==24){check(!app.clipboard.mask.empty()&&app.clipboard.mask[0]&&!app.clipboard.mask[2*11+2]);key(ImGuiKey_X,true);}if(frame==25)key(ImGuiKey_X,false);
        if(frame==26){check(pixel(4,4)==0);key(ImGuiKey_Z,true);}if(frame==27)key(ImGuiKey_Z,false);
        if(frame==28){check(pixel(4,4)==9);key(ImGuiKey_V,true);}if(frame==29)key(ImGuiKey_V,false);
        if(frame==30){check(app.paste_mode);point(20.5f,4.5f);}if(frame==31)io.AddMouseButtonEvent(0,true);if(frame==32)io.AddMouseButtonEvent(0,false);
        if(frame==33){check(!app.paste_mode&&pixel(20,4)==9);app.action(Action::Undo);app.action(Action::Undo);check(!art.dirty());app.tool=Tool::Lasso;point(4,4);}
        if(frame==34)io.AddMouseButtonEvent(0,true);if(frame==35)point(10,4);if(frame==36)point(10,10);if(frame==37)point(4,10);if(frame==38)io.AddMouseButtonEvent(0,false);
        if(frame==39){check(t->selection.count()==36&&t->selection.contains(6,6));point(20,20);}if(frame==40)io.AddMouseButtonEvent(0,true);if(frame==41)point(23,23);
        if(frame==42)io.AddKeyEvent(ImGuiKey_Escape,true);if(frame==43){io.AddKeyEvent(ImGuiKey_Escape,false);io.AddMouseButtonEvent(0,false);}
        if(frame==44){check(t->selection.count()==36&&!art.dirty());app.tool=Tool::Wand;point(20.5f,12.5f);}if(frame==45)io.AddMouseButtonEvent(0,true);if(frame==46)io.AddMouseButtonEvent(0,false);
        if(frame==47){check(t->selection.valid&&t->selection.contains(20,12)&&!t->selection.contains(0,0));before_wand=t->selection;io.AddKeyEvent(ImGuiMod_Alt,true);io.AddKeyEvent(ImGuiKey_RightArrow,true);}
        if(frame==48){io.AddKeyEvent(ImGuiKey_RightArrow,false);io.AddKeyEvent(ImGuiMod_Alt,false);}
        if(frame==49){check(t->selected==0&&t->selection.x==before_wand.x+1&&art.dirty()&&art.sprite(0)->anix==24&&art.sprite(0)->aniy==60);key(ImGuiKey_Z,true);}if(frame==50)key(ImGuiKey_Z,false);
        if(frame==51){check(!art.dirty()&&art.copy(0,0,0,48,64).pixels==original);key(ImGuiKey_A,true);}if(frame==52)key(ImGuiKey_A,false);
        if(frame==53){check(t->selection.count()==48*64);key(ImGuiKey_D,true);}if(frame==54)key(ImGuiKey_D,false);
        if(frame==55){check(!t->selection.valid&&!art.dirty());app.action(Action::SelectOpaque);app.tool=Tool::Lasso;}
    }
};
