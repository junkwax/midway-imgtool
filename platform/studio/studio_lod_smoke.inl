struct LodSmoke {
    int failures=0;uint64_t owner=0;std::string path,bfile;uint64_t generation=0;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto &u=app.lod;auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio LOD UI check failed at frame %d: %s / %s; open=%d valid=%d tabs=%zu\n",frame,app.error.c_str(),u.error.c_str(),u.open,u.valid,app.tabs.size());}};auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};
        if(frame==0){auto t=app.tab();if(!t){++failures;return;}auto dir=fs::u8path(app.prefs)/"lod-smoke";fs::create_directories(dir);auto afile=(dir/"ACTOR.IMG").u8string();bfile=(dir/"EFFECT.IMG").u8string();check(t->art->save(afile,app.error));auto b=Artwork::demo();check(b->save(bfile,app.error));path=(dir/"scene.LOD").u8string();{std::ofstream f(fs::u8path(path));f<<"; Library review fixture\nPPP> 2\nACTOR.IMG\nPPP> 0\nEFFECT.IMG\nMISSING.IMG\nPPP> 6\nEFFECT.IMG\nASM> SCENE.TBL\nZON>\n---> SENTINEL1\n";}owner=t->uid;t->ppp=7;t->art->anchor(0,27,61);t->art->commit();generation=t->art->generation();io.AddFocusEvent(true);}
        if(frame==3)app.open(path);
        if(frame==6){check(u.open&&u.valid&&u.review.libraries.size()==4&&app.tabs.size()==1&&app.find(owner)->art->dirty());point(u.cancel_point);}if(frame==7)io.AddMouseButtonEvent(0,true);if(frame==8)io.AddMouseButtonEvent(0,false);
        if(frame==10){check(!u.open&&app.tabs.size()==1&&app.find(owner)->art->generation()==generation);app.open(path);}
        if(frame==13){check(u.open&&u.valid&&u.review.libraries[2].enabled&&!u.review.libraries[3].enabled);point(u.row_points[2]);}if(frame==14)io.AddMouseButtonEvent(0,true);if(frame==15)io.AddMouseButtonEvent(0,false);
        if(frame==17){check(!u.review.libraries[2].enabled);point(u.name_points[1]);}if(frame==18)io.AddMouseButtonEvent(0,true);if(frame==19)io.AddMouseButtonEvent(0,false);
        if(frame==21){check(u.selected==1&&u.preview);point(u.apply_point);}if(frame==22)io.AddMouseButtonEvent(0,true);if(frame==23)io.AddMouseButtonEvent(0,false);
        if(frame==25){check(!u.open&&app.tabs.size()==2);check(app.find(owner)->ppp==7&&app.find(owner)->art->dirty()&&app.find(owner)->art->generation()==generation);check(app.tabs.back()->ppp==0);check(!app.tabs.back()->art->dirty());check(app.tabs.back()->art->path()==bfile);check(app.tab()==app.tabs.back().get());app.open(path);}
        if(frame==28){check(u.open&&u.valid&&app.lod_existing(u.review.libraries[0])==app.find(owner));point(u.row_points[2]);}if(frame==29)io.AddMouseButtonEvent(0,true);if(frame==30)io.AddMouseButtonEvent(0,false);
        if(frame==32){{std::ofstream f(fs::u8path(bfile),std::ios::binary|std::ios::app);f.put('x');}point(u.apply_point);}if(frame==33)io.AddMouseButtonEvent(0,true);if(frame==34)io.AddMouseButtonEvent(0,false);
        if(frame==36){check(u.open&&!u.error.empty()&&app.tabs.size()==2&&app.find(owner)->art->generation()==generation);auto b=Artwork::demo();check(b->save(bfile,app.error));point(u.rebuild_point);}if(frame==37)io.AddMouseButtonEvent(0,true);if(frame==38)io.AddMouseButtonEvent(0,false);
        if(frame==40){check(u.open&&u.valid&&u.error.empty()&&u.review.libraries[2].enabled);point(u.row_points[2]);}if(frame==41)io.AddMouseButtonEvent(0,true);if(frame==42)io.AddMouseButtonEvent(0,false);
        if(frame==44)point(u.apply_point);if(frame==45)io.AddMouseButtonEvent(0,true);if(frame==46)io.AddMouseButtonEvent(0,false);
        if(frame==48){check(!u.open&&app.tabs.size()==2&&app.find(owner)->ppp==7&&app.find(owner)->art->generation()==generation);app.open(path);}
        if(frame==51){snprintf(u.override_folder,sizeof u.override_folder,"%s",(fs::u8path(app.prefs)/"lod-smoke"/"missing-folder").u8string().c_str());u.settings_changed=true;point(u.rebuild_point);}if(frame==52)io.AddMouseButtonEvent(0,true);if(frame==53)io.AddMouseButtonEvent(0,false);
        if(frame==55){check(u.open&&!u.valid&&!u.error.empty()&&app.tabs.size()==2);io.AddKeyEvent(ImGuiKey_Escape,true);}if(frame==56)io.AddKeyEvent(ImGuiKey_Escape,false);
        if(frame==58){check(!u.open&&app.find(owner)->art->generation()==generation);app.open(path);}
        if(frame==61){check(u.open&&u.valid&&u.review.libraries[2].enabled&&u.review.libraries[0].ppp==2);point(u.name_points[0]);}if(frame==62)io.AddMouseButtonEvent(0,true);if(frame==63)io.AddMouseButtonEvent(0,false);
        if(frame==65){check(u.selected==0&&u.preview&&app.tabs.size()==2&&app.find(owner)->art->dirty()&&app.find(owner)->art->generation()==generation);point(ImVec2(5,5));}
    }
};
