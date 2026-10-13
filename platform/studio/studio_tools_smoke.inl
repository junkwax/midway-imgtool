struct ToolsSmoke {
    int failures=0;unsigned short original=0,edited=0,transparent=0;std::string file;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();if(!t){++failures;return;}auto &art=*t->art;auto &u=app.tools_ui;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio side-tools check failed at frame %d: %s; tool=%d slot=%d dirty=%d editing=%d palette=%d word=%04x\n",frame,app.error.c_str(),(int)app.tool,t->color,art.dirty(),t->wheel.editing,t->wheel.palette,palette_word_at((unsigned char *)art.palette(0)->data_p,8));}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        auto word=[&](int slot){return palette_word_at((unsigned char *)art.palette(0)->data_p,slot);};
        auto hue=[&](float angle){float bar=ImGui::GetFrameHeight(),size=u.wheel_size-bar-ImGui::GetStyle().ItemInnerSpacing.x;ImVec2 center=u.wheel_origin+ImVec2((size+bar)*.5f,size*.5f);float r=size*.46f;point(center+ImVec2(std::cos(angle)*r,std::sin(angle)*r));};
        if(frame==0){file=(fs::u8path(app.prefs)/"side-tools-original.IMG").u8string();check(art.save(file,app.error));original=word(8);transparent=word(0);io.AddFocusEvent(true);}
        if(frame==3){check(u.wheel_size>=90&&u.wheel_origin.y+u.wheel_size<io.DisplaySize.y);point(u.points[(int)Tool::Wand]);}if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==6){check(app.tool==Tool::Wand);io.AddKeyEvent(ImGuiKey_R,true);}if(frame==7)io.AddKeyEvent(ImGuiKey_R,false);if(frame==8){check(app.tool==Tool::Select);point(u.points[(int)Tool::Pencil]);}if(frame==9)io.AddMouseButtonEvent(0,true);if(frame==10)io.AddMouseButtonEvent(0,false);
        if(frame==11){check(app.tool==Tool::Pencil);hue(2.094395f);}if(frame==12)io.AddMouseButtonEvent(0,true);if(frame==13)hue(3.141592f);if(frame==14)hue(4.188790f);if(frame==15)io.AddMouseButtonEvent(0,false);
        if(frame==16){edited=word(8);check(edited!=original&&art.dirty()&&!t->wheel.editing&&std::string(art.undo_label())=="Edit shared palette color");key(ImGuiKey_Z,true);}if(frame==17)key(ImGuiKey_Z,false);
        if(frame==18){check(!art.dirty()&&word(8)==original);key(ImGuiKey_Y,true);}if(frame==19)key(ImGuiKey_Y,false);
        if(frame==20){check(word(8)==edited&&art.dirty());app.action(Action::Undo);point(u.transparent_point);}if(frame==21)io.AddMouseButtonEvent(0,true);if(frame==22)io.AddMouseButtonEvent(0,false);
        if(frame==23){check(t->color==0&&!art.dirty());hue(2.094395f);}if(frame==24)io.AddMouseButtonEvent(0,true);if(frame==25)io.AddMouseButtonEvent(0,false);
        if(frame==26){check(t->color==0&&word(0)==transparent&&!art.dirty());point(u.transparent_point);}if(frame==27)io.AddMouseButtonEvent(0,true);if(frame==28)io.AddMouseButtonEvent(0,false);
        if(frame==29){check(t->color==8);hue(2.094395f);}if(frame==30)io.AddMouseButtonEvent(0,true);if(frame==31)hue(1.047197f);
        if(frame==32){check(t->wheel.editing&&word(8)!=original);io.AddKeyEvent(ImGuiKey_Escape,true);}if(frame==33){io.AddKeyEvent(ImGuiKey_Escape,false);io.AddMouseButtonEvent(0,false);}
        if(frame==34){check(!t->wheel.editing&&word(8)==original&&!art.dirty());point(u.detail_point);}if(frame==35)io.AddMouseButtonEvent(0,true);if(frame==36)io.AddMouseButtonEvent(0,false);
        if(frame==38){check(ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel));io.AddKeyEvent(ImGuiKey_Escape,true);}if(frame==39)io.AddKeyEvent(ImGuiKey_Escape,false);if(frame==40)check(!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel));
        if(frame==42){check(art.save((fs::u8path(app.prefs)/"side-tools-undo.IMG").u8string(),app.error));app.demo();auto second=app.tab();second->art->color(0,8,0x7c00);second->art->commit();}if(frame==44)check(t->uid!=app.tabs[0]->uid&&word(8)==0x7c00);
        if(frame==45){app.finish();app.active=0;app.select_uid=app.tabs[0]->uid;}if(frame==47)check(t->uid==app.tabs[0]->uid&&word(8)==original&&t->color==8);
        if(frame==47){auto loaded=Artwork::open((fs::u8path(app.prefs)/"side-tools-undo.IMG").u8string(),app.error);check(loaded&&palette_word_at((unsigned char *)loaded->palette(0)->data_p,8)==original&&!art.dirty());}
    }
};
