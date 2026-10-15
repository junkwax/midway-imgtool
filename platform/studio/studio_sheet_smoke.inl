struct SheetSmoke {
    int failures=0;uint64_t generation=0;std::string path;uint64_t mark=0;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();if(!t){++failures;return;}auto &u=app.sheet;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Sheet UI failure at %d: %s / %s\n",frame,app.error.c_str(),u.error.c_str());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};
        if(frame==0){io.AddFocusEvent(true);for(int i=0;i<5;++i)app.mark_sprite(*t,t->art->sprite_id(i),true);mark=t->art->sprite_id(0);generation=t->art->generation();path=(fs::u8path(app.prefs)/"reviewed-sheet.tga").u8string();app.open_sheet(*t);snprintf(u.path,sizeof u.path,"%s",path.c_str());u.build=true;}
        if(frame==3){check(u.open&&u.valid&&u.plan.ready&&u.plan.included==5&&u.plan.options.anf&&u.destinations.size()==2&&u.preview);point(u.cancel_point);}if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==7){check(!u.open&&app.mark_count(*t)==5&&t->art->generation()==generation&&!t->art->dirty());app.open_sheet(*t);snprintf(u.path,sizeof u.path,"%s",path.c_str());u.build=true;}
        if(frame==10)point(u.row_points[0]);if(frame==11)io.AddMouseButtonEvent(0,true);if(frame==12)io.AddMouseButtonEvent(0,false);
        if(frame==14){check(u.valid&&u.plan.included==4&&app.marked(*t,mark));point(u.name_points[2]);}if(frame==15)io.AddMouseButtonEvent(0,true);if(frame==16)io.AddMouseButtonEvent(0,false);
        if(frame==18){check(u.selected==2&&u.destination_valid);point(u.apply_point);}if(frame==19)io.AddMouseButtonEvent(0,true);if(frame==20)io.AddMouseButtonEvent(0,false);
        if(frame==22){auto anf=fs::u8path(path);anf.replace_extension(".ANF");check(!u.open&&fs::exists(fs::u8path(path))&&fs::exists(anf)&&app.mark_count(*t)==5&&t->art->generation()==generation&&!t->art->dirty());app.open_sheet(*t);snprintf(u.path,sizeof u.path,"%s",path.c_str());u.build=true;}
        if(frame==25){check(u.valid&&u.plan.included==5&&u.destinations[0].existed);{std::ofstream out(fs::u8path(path),std::ios::binary|std::ios::app);out.put('x');}point(u.apply_point);}if(frame==26)io.AddMouseButtonEvent(0,true);if(frame==27)io.AddMouseButtonEvent(0,false);
        if(frame==29){check(u.open&&u.error.find("output changed")!=std::string::npos&&t->art->generation()==generation);point(u.build_point);}if(frame==30)io.AddMouseButtonEvent(0,true);if(frame==31)io.AddMouseButtonEvent(0,false);
        if(frame==33){check(u.valid&&u.error.empty());point(u.apply_point);}if(frame==34)io.AddMouseButtonEvent(0,true);if(frame==35)io.AddMouseButtonEvent(0,false);
        if(frame==37){check(!u.open&&!t->art->dirty());app.open_sheet(*t);snprintf(u.path,sizeof u.path,"%s",path.c_str());u.build=true;}
        if(frame==40)point(u.rgba_point);if(frame==41)io.AddMouseButtonEvent(0,true);if(frame==42)io.AddMouseButtonEvent(0,false);
        if(frame==44){check(!u.valid&&u.options.rgba&&!u.options.anf);point(u.build_point);}if(frame==45)io.AddMouseButtonEvent(0,true);if(frame==46)io.AddMouseButtonEvent(0,false);
        if(frame==48){check(u.valid&&u.plan.ready&&u.destinations.size()==1&&u.plan.anf.empty());t->art->anchor(0,25,61);t->art->commit();point(u.apply_point);}if(frame==49)io.AddMouseButtonEvent(0,true);if(frame==50)io.AddMouseButtonEvent(0,false);
        if(frame==52){check(u.open&&u.plan.generation!=t->art->generation());t->art->undo();point(u.build_point);}if(frame==53)io.AddMouseButtonEvent(0,true);if(frame==54)io.AddMouseButtonEvent(0,false);
        if(frame==56){check(u.valid&&u.plan.generation==t->art->generation()&&app.mark_count(*t)==5&&!t->art->dirty());point(u.rgba_point);}if(frame==57)io.AddMouseButtonEvent(0,true);if(frame==58)io.AddMouseButtonEvent(0,false);
        if(frame==60){u.options.anf=true;u.build=true;}
        if(frame==63){check(u.valid&&u.plan.ready&&u.destinations.size()==2&&u.preview);point(ImVec2(5,5));}
    }
};
