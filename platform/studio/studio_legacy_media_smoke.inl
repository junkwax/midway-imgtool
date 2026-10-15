struct LegacyMediaSmoke {
    int failures=0,count=0,palettes=0;uint64_t generation=0;std::string lbm,tga,output;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();if(!t){++failures;return;}auto &u=app.legacy_media;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Indexed media UI failure at %d: %s / %s\n",frame,app.error.c_str(),u.error.c_str());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};
        if(frame==0){io.AddFocusEvent(true);count=t->art->count();palettes=t->art->palette_count();generation=t->art->generation();lbm=(fs::u8path(app.prefs)/"legacy-source.lbm").u8string();tga=(fs::u8path(app.prefs)/"legacy-source.tga").u8string();check(export_indexed_sprite(*t->art,0,lbm,LegacyMediaKind::Lbm,app.error));check(export_indexed_sprite(*t->art,0,tga,LegacyMediaKind::Tga,app.error));app.open(lbm);}
        if(frame==3){check(u.open&&u.valid&&u.preview&&u.batch.words.size()==16);point(u.cancel_point);}if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==7){check(!u.open&&t->art->count()==count&&t->art->generation()==generation);app.open(tga);}
        if(frame==10){check(u.open&&u.valid&&!u.dirty&&u.batch.words.size()==16);point(u.apply_point);}if(frame==11)io.AddMouseButtonEvent(0,true);if(frame==12)io.AddMouseButtonEvent(0,false);
        if(frame==14){check(!u.open&&t->art->count()==count+1&&t->art->palette_count()==palettes+1);check(t->art->copy(count,0,0,48,64).pixels==t->art->copy(0,0,0,48,64).pixels);app.action(Action::Undo);}
        if(frame==17){check(t->art->count()==count&&t->art->palette_count()==palettes);app.select(0);app.request_indexed_export(LegacyMediaKind::Lbm);output=(fs::u8path(app.prefs)/"legacy-export.lbm").u8string();snprintf(u.path,sizeof u.path,"%s",output.c_str());u.dirty=true;}
        if(frame==20)point(u.rebuild_point);if(frame==21)io.AddMouseButtonEvent(0,true);if(frame==22)io.AddMouseButtonEvent(0,false);
        if(frame==24){check(u.valid&&!u.dirty&&u.preview);point(u.apply_point);}if(frame==25)io.AddMouseButtonEvent(0,true);if(frame==26)io.AddMouseButtonEvent(0,false);
        if(frame==28){check(!u.open&&fs::exists(fs::u8path(output)));IndexedMedia decoded;check(read_indexed_media(output,decoded,app.error));check(decoded.pixels==t->art->copy(0,0,0,48,64).pixels);check(!t->art->dirty());app.request_indexed_export(LegacyMediaKind::Tga);snprintf(u.path,sizeof u.path,"%s",tga.c_str());u.dirty=true;}
        if(frame==31)point(u.rebuild_point);if(frame==32)io.AddMouseButtonEvent(0,true);if(frame==33)io.AddMouseButtonEvent(0,false);
        if(frame==35){check(u.valid&&!u.dirty&&u.review.existed);{std::ofstream file(fs::u8path(tga),std::ios::binary|std::ios::app);file.put('x');}point(u.apply_point);}if(frame==36)io.AddMouseButtonEvent(0,true);if(frame==37)io.AddMouseButtonEvent(0,false);
        if(frame==39){check(u.open&&u.error.find("destination changed")!=std::string::npos&&!t->art->dirty());point(u.rebuild_point);}if(frame==40)io.AddMouseButtonEvent(0,true);if(frame==41)io.AddMouseButtonEvent(0,false);
        if(frame==43){check(u.valid&&u.error.empty());point(u.apply_point);}if(frame==44)io.AddMouseButtonEvent(0,true);if(frame==45)io.AddMouseButtonEvent(0,false);
        if(frame==47){check(!u.open&&!t->art->dirty());app.open(lbm);}
        if(frame==50){check(u.open&&u.valid&&u.preview);u.palette=0;u.anchor[0]=-9;u.anchor[1]=17;u.dirty=true;point(u.rebuild_point);}if(frame==51)io.AddMouseButtonEvent(0,true);if(frame==52)io.AddMouseButtonEvent(0,false);
        if(frame==54){check(u.valid&&!u.dirty&&u.batch.palette==0&&u.batch.frames[0].anix==-9&&u.batch.frames[0].aniy==17);point(ImVec2(5,5));}
    }
};
