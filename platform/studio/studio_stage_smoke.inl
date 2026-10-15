struct StageSmoke {
    int failures=0;uint64_t generation=0;std::string path;std::shared_ptr<const StageBackdrop> original;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();if(!t){++failures;return;}
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Stage UI failure at %d: %s / %s\n",frame,app.error.c_str(),app.stage_error.c_str());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};
        if(frame==0){io.AddFocusEvent(true);generation=t->art->generation();app.workspace(*t,true);
            auto real=fs::path("../mk2-main/data/DEDPOOL.BDD");if(fs::exists(real))path=fs::absolute(real).u8string();
            else{auto root=fs::u8path(app.prefs);path=(root/"reference.BDD").u8string();std::string art="1\n1 2 2 0\n";art.append("\0\1\1\1",4);art+="PAL 2\n";art.append("\0\0\0\x7c",4);
                std::ofstream(root/"reference.BDD",std::ios::binary).write(art.data(),art.size());std::ofstream(root/"reference.BDB")<<"SCENERY 400 254 255 0 1 1\n4000 199 230 1 0\n";}
            check(app.load_stage(*t,path));original=t->stage;app.show_stage_dialog(*t);}
        if(frame==3){check(t->stage&&t->stage_texture&&app.stage_dialog_open);point(app.stage_close_point);}if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==7){check(!app.stage_dialog_open&&!t->art->dirty());app.show_stage_dialog(*t);}
        if(frame==10)point(app.stage_visible_point);if(frame==11)io.AddMouseButtonEvent(0,true);if(frame==12)io.AddMouseButtonEvent(0,false);
        if(frame==14){check(!t->stage_view.enabled);point(app.stage_visible_point);}if(frame==15)io.AddMouseButtonEvent(0,true);if(frame==16)io.AddMouseButtonEvent(0,false);
        if(frame==18){check(t->stage_view.enabled);snprintf(app.stage_path,sizeof app.stage_path,"%s",(fs::u8path(app.prefs)/"missing.BDD").u8string().c_str());point(app.stage_load_point);}
        if(frame==19)io.AddMouseButtonEvent(0,true);if(frame==20)io.AddMouseButtonEvent(0,false);
        if(frame==22){check(!app.stage_error.empty()&&t->stage==original&&t->art->generation()==generation);point(app.stage_clear_point);}if(frame==23)io.AddMouseButtonEvent(0,true);if(frame==24)io.AddMouseButtonEvent(0,false);
        if(frame==26){check(!t->stage&&!t->stage_texture&&app.stage_error.empty());snprintf(app.stage_path,sizeof app.stage_path,"%s",path.c_str());point(app.stage_load_point);}if(frame==27)io.AddMouseButtonEvent(0,true);if(frame==28)io.AddMouseButtonEvent(0,false);
        if(frame==30){check(t->stage&&t->stage!=original&&app.stage_error.empty());point(app.stage_close_point);}if(frame==31)io.AddMouseButtonEvent(0,true);if(frame==32)io.AddMouseButtonEvent(0,false);
        if(frame==34){check(!app.stage_dialog_open&&t->art->generation()==generation&&!t->art->dirty());app.new_scene(*t);}
        if(frame==37){check(t->project&&t->stage);auto png=(fs::u8path(app.prefs)/"stage-scene.png").u8string();check(t->project->export_png(png,0,app.scene_source(),app.error,t->stage.get(),&t->stage_view));app.embedded_workspace(*t);}
        if(frame==39){check(t->embedded.enabled&&t->stage_texture);app.workspace(*t,true);}
    }
};
