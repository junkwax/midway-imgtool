struct AsmSmoke {
    int failures=0;bool real=false;uint64_t generation=0,uid=0;std::string source_path,strike_path;ImVec2 corner;
    void prepare(App &app,ImGuiIO &io,int frame){auto &u=app.asm_ui;auto &v=app.strikes;
        if(real){if(frame==0){io.AddFocusEvent(true);app.tabs.clear();app.active=-1;for(int i=1;i<=10;++i){auto path=fs::path("../mk2-main/data")/("CAGE"+std::to_string(i)+".IMG");if(fs::exists(path))app.open(path.u8string());}auto logo=fs::path("../mk2-main/data/JCLOGO.IMG");if(fs::exists(logo))app.open(logo.u8string());
                app.load_strikes("../mk2-main/src/MKSTK.ASM");app.load_asm("../mk2-main/src/MKJC.ASM");app.show_asm();u.preferred=app.tabs.empty()?0:app.tabs[0]->uid;for(int i=0;u.source&&i<(int)u.source->animations.size();++i)if(u.source->animations[i].label=="a_jchikick")app.asm_select(i);u.hold=4;}
            if(frame==4){if(!app.asm_animation()||app.asm_animation()->frames.empty()||app.asm_strike_record()<0||!app.error.empty()){++failures;fprintf(stderr,"Real ASM preview failed: %s / %s\n",app.error.c_str(),u.error.c_str());}}
            if(frame==7){auto a=app.asm_animation();if(a)u.player.seek(std::min(1,(int)a->frames.size()-1),(int)a->frames.size());}
            return;}
        auto t=app.find(uid);if(frame==0)t=app.tab();if(!t){++failures;return;}
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"ASM UI failure at %d: %s / %s / %s\n",frame,app.error.c_str(),u.error.c_str(),v.error.c_str());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};
        if(frame==0){io.AddFocusEvent(true);uid=t->uid;generation=t->art->generation();source_path=(fs::u8path(app.prefs)/"preview-animation.ASM").u8string();strike_path=(fs::u8path(app.prefs)/"preview-boxes.ASM").u8string();
            {std::ofstream out(fs::u8path(source_path));out<<"demo_anitab\n\t.long a_demo ; Demo animation\na_demo\n\t.long GROUP\n\t.long ani_adjustxy\n\t.word 5,-3\n\t.long ani_flip,SENTINEL3\n\t.long 0\nGROUP\n\t.long SENTINEL1,SENTINEL2,0\n";}
            {std::ofstream out(fs::u8path(strike_path));out<<"stk_demo\n\t.word 50\n\t.word -30\n\t.word 30\n\t.word 25\n\t.word r_test\n\t.word 4\n\t.long 10\n\t.word sound\n";}
            app.load_strikes(strike_path);app.load_asm(source_path);app.show_asm();}
        if(frame==3){check(u.open&&u.source&&u.pieces.size()==2&&u.pieces[0].sprite&&u.pieces[1].sprite&&app.asm_strike_record()==0);point(u.next_point);}
        if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==7){check(u.player.frame==1&&u.pieces.size()==1&&u.pieces[0].sprite&&app.asm_frame()->dx==5&&app.asm_frame()->flip);point(u.previous_point);}
        if(frame==8)io.AddMouseButtonEvent(0,true);if(frame==9)io.AddMouseButtonEvent(0,false);
        if(frame==11){check(u.player.frame==0);u.player.loop=false;u.hold=1;point(u.play_point);}
        if(frame==12)io.AddMouseButtonEvent(0,true);if(frame==13)io.AddMouseButtonEvent(0,false);
        if(frame==17){check(u.player.frame==1&&!u.player.playing);auto before=u.source.get();check(!app.load_asm(source_path+".absent")&&u.source.get()==before);u.error.clear();app.set_asm_binding(std::string("-"));check(app.asm_strike_record()==-1);app.set_asm_binding(std::string("stk_demo"));check(app.asm_strike_record()==0);u.preferred=0;app.demo();}
        if(frame==20){check(u.pieces.size()==1&&u.pieces[0].ambiguous&&!u.pieces[0].sprite);u.preferred=uid;}
        if(frame==23){check(u.pieces[0].doc==uid&&u.pieces[0].sprite);app.tabs.pop_back();app.active=0;app.select_uid=uid;app.show_strikes();v.animation=true;v.origin_y=190;app.asm_select(0);}
        if(frame==26){check(v.open&&!u.open&&v.valid);corner=v.corner_points[2];point(corner);}
        if(frame==27)io.AddMouseButtonEvent(0,true);if(frame==28)point(corner+ImVec2(6*v.preview_zoom,2*v.preview_zoom));if(frame==29)io.AddMouseButtonEvent(0,false);
        if(frame==31){check(v.valid&&v.patch.changed&&!v.source->dirty()&&t->art->generation()==generation);point(v.apply_point);}
        if(frame==32)io.AddMouseButtonEvent(0,true);if(frame==33)io.AddMouseButtonEvent(0,false);
        if(frame==35){check(v.source->dirty()&&!t->art->dirty());v.source->undo();app.strike_select(0);point(u.next_point);}
        if(frame==36)io.AddMouseButtonEvent(0,true);if(frame==37)io.AddMouseButtonEvent(0,false);
        if(frame==39){check(u.player.frame==1&&!v.source->dirty());v.flip=true;corner=v.corner_points[2];}
        if(frame==40){corner=v.corner_points[2];point(corner);}if(frame==41)io.AddMouseButtonEvent(0,true);if(frame==42)point(corner+ImVec2(4*v.preview_zoom,2*v.preview_zoom));if(frame==43)io.AddMouseButtonEvent(0,false);
        if(frame==45){check(v.valid&&v.patch.changed&&!t->art->dirty());app.strike_select(0);app.show_asm();u.facing=true;}
        if(frame==48){check(u.open&&u.pieces.size()==1&&u.pieces[0].sprite&&!v.source->dirty()&&!t->art->dirty()&&t->art->generation()==generation);point(u.floor_point);}
        if(frame==49)io.AddMouseButtonEvent(0,true);if(frame==50)io.AddMouseButtonEvent(0,false);if(frame==51)check(u.origin_y==229&&!t->art->dirty());
    }
};
