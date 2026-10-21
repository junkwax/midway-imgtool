struct PngBatchSmoke {
    int failures=0;uint64_t generation=0,selected=0;std::string folder;std::vector<unsigned char> base;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();if(!t){++failures;return;}auto &u=app.png_batch;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Bulk PNG UI failure at %d: %s / %s\n",frame,app.error.c_str(),u.error.c_str());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};
        auto open=[&](){app.action(Action::ExportPngBatch);snprintf(u.folder,sizeof u.folder,"%s",folder.c_str());u.build=true;};
        if(frame==0){io.AddFocusEvent(true);folder=(fs::u8path(app.prefs)/"png-frames").u8string();fs::create_directories(fs::u8path(folder));for(int i=0;i<4;++i)app.mark_sprite(*t,t->art->sprite_id(i),true);generation=t->art->generation();selected=t->selected_id;auto im=t->art->sprite(0);base=t->art->copy(0,0,0,im->w,im->h).pixels;open();}
        if(frame==3){check(u.open&&u.valid&&u.plan.ready&&u.plan.included==4&&u.scope==0&&u.preview);point(u.cancel_point);}if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==7){check(!u.open&&fs::is_empty(fs::u8path(folder))&&app.mark_count(*t)==4);open();}
        if(frame==10)point(u.row_points[0]);if(frame==11)io.AddMouseButtonEvent(0,true);if(frame==12)io.AddMouseButtonEvent(0,false);
        if(frame==14){check(u.valid&&u.plan.included==3&&app.marked(*t,t->art->sprite_id(0)));point(u.apply_point);}if(frame==15)io.AddMouseButtonEvent(0,true);if(frame==16)io.AddMouseButtonEvent(0,false);
        if(frame==18){check(!u.open);int files=0;for(auto &e:fs::directory_iterator(fs::u8path(folder))){check(e.path().extension()==".png");++files;}check(files==3&&t->art->generation()==generation&&!t->art->dirty()&&t->selected_id==selected&&app.mark_count(*t)==4);open();}
        if(frame==21){check(u.plan.replacements==3&&!u.allow_replace);point(u.apply_point);}if(frame==22)io.AddMouseButtonEvent(0,true);if(frame==23)io.AddMouseButtonEvent(0,false);
        if(frame==25){check(u.open);point(u.replace_point);}if(frame==26)io.AddMouseButtonEvent(0,true);if(frame==27)io.AddMouseButtonEvent(0,false);
        if(frame==29){check(u.allow_replace);std::ofstream f(fs::u8path(u.plan.rows[1].path),std::ios::binary|std::ios::app);f.put('x');point(u.apply_point);}if(frame==30)io.AddMouseButtonEvent(0,true);if(frame==31)io.AddMouseButtonEvent(0,false);
        if(frame==33){check(u.open&&u.error.find("changed after review")!=std::string::npos);point(u.build_point);}if(frame==34)io.AddMouseButtonEvent(0,true);if(frame==35)io.AddMouseButtonEvent(0,false);
        if(frame==37){check(u.valid&&!u.allow_replace&&u.error.empty());point(u.replace_point);}if(frame==38)io.AddMouseButtonEvent(0,true);if(frame==39)io.AddMouseButtonEvent(0,false);
        if(frame==41)point(u.apply_point);if(frame==42)io.AddMouseButtonEvent(0,true);if(frame==43)io.AddMouseButtonEvent(0,false);
        if(frame==45){check(!u.open);open();}
        if(frame==48){check(u.valid&&u.plan.replacements==4);t->art->anchor(0,12,14);t->art->commit();u.allow_replace=true;point(u.apply_point);}if(frame==49)io.AddMouseButtonEvent(0,true);if(frame==50)io.AddMouseButtonEvent(0,false);
        if(frame==52){check(u.open&&u.plan.generation!=t->art->generation());t->art->undo();point(u.build_point);}if(frame==53)io.AddMouseButtonEvent(0,true);if(frame==54)io.AddMouseButtonEvent(0,false);
        if(frame==56){check(u.valid&&!t->art->dirty()&&u.plan.generation==t->art->generation());point(u.scope_point);}if(frame==57)io.AddMouseButtonEvent(0,true);if(frame==58)io.AddMouseButtonEvent(0,false);
        if(frame==60)point(u.scope_choices[3]);if(frame==61)io.AddMouseButtonEvent(0,true);if(frame==62)io.AddMouseButtonEvent(0,false);
        if(frame==64){check(u.scope==3&&u.plan.included==1);point(u.scope_point);}if(frame==65)io.AddMouseButtonEvent(0,true);if(frame==66)io.AddMouseButtonEvent(0,false);
        if(frame==68)point(u.scope_choices[2]);if(frame==69)io.AddMouseButtonEvent(0,true);if(frame==70)io.AddMouseButtonEvent(0,false);
        if(frame==72){check(u.scope==2&&u.plan.included==t->art->count());point(u.scope_point);}if(frame==73)io.AddMouseButtonEvent(0,true);if(frame==74)io.AddMouseButtonEvent(0,false);
        if(frame==76)point(u.scope_choices[1]);if(frame==77)io.AddMouseButtonEvent(0,true);if(frame==78)io.AddMouseButtonEvent(0,false);
        if(frame==80){check(u.scope==1&&u.plan.included==t->art->count());point(u.scope_point);}if(frame==81)io.AddMouseButtonEvent(0,true);if(frame==82)io.AddMouseButtonEvent(0,false);
        if(frame==84)point(u.scope_choices[0]);if(frame==85)io.AddMouseButtonEvent(0,true);if(frame==86)io.AddMouseButtonEvent(0,false);
        if(frame==88){auto im=t->art->sprite(0);check(u.scope==0&&u.plan.included==4&&u.preview&&t->selected_id==selected&&app.mark_count(*t)==4&&!t->art->dirty()&&t->art->copy(0,0,0,im->w,im->h).pixels==base);point(ImVec2(5,5));}
    }
};
