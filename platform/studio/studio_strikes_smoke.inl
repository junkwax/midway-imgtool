struct StrikesSmoke {
    int failures=0;uint64_t generation=0;std::string path,original,edited;ImVec2 corner;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();if(!t){++failures;return;}auto &u=app.strikes;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Strike UI failure at %d: %s / %s / %s\n",frame,app.error.c_str(),u.error.c_str(),u.save_error.c_str());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};auto bytes=[&](){std::ifstream in(fs::u8path(path),std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
        if(frame==0){io.AddFocusEvent(true);generation=t->art->generation();path=(fs::u8path(app.prefs)/"reviewed-strikes.ASM").u8string();original="; owned UI fixture\r\ndemo_strikes\r\n\t.long stk_demo\r\n\t.long stk_alias\r\nstk_demo\r\nstk_alias\r\n\t.word 70 ; x offset\r\n\t.word -20\r\n\t.word 40\r\n\t.word 30\r\n\t.word r_custom\r\n\t.word 02008h\r\n\t.long 0\r\n\t.word sf_squeeze\r\n";
            std::ofstream out(fs::u8path(path),std::ios::binary);out<<original;out.close();app.show_strikes();app.load_strikes(path);}
        if(frame==3){check(u.source&&u.valid&&!u.source->dirty()&&u.source->records()[0].aliases.size()==2);corner=u.corner_points[2];point(corner);}if(frame==4)io.AddMouseButtonEvent(0,true);
        if(frame==5)point(corner+ImVec2(6*u.preview_zoom,-3*u.preview_zoom));if(frame==6)io.AddMouseButtonEvent(0,false);
        if(frame==8){check(u.valid&&u.patch.changed&&!u.source->dirty()&&!t->art->dirty());edited=u.patch.after;point(u.apply_point);}if(frame==9)io.AddMouseButtonEvent(0,true);if(frame==10)io.AddMouseButtonEvent(0,false);
        if(frame==12){check(u.source->dirty()&&u.source->bytes()==edited&&t->art->generation()==generation);point(u.undo_point);}if(frame==13)io.AddMouseButtonEvent(0,true);if(frame==14)io.AddMouseButtonEvent(0,false);
        if(frame==16){check(!u.source->dirty()&&u.source->bytes()==original);point(u.redo_point);}if(frame==17)io.AddMouseButtonEvent(0,true);if(frame==18)io.AddMouseButtonEvent(0,false);
        if(frame==20){check(u.source->dirty()&&u.source->bytes()==edited);point(u.save_point);}if(frame==21)io.AddMouseButtonEvent(0,true);if(frame==22)io.AddMouseButtonEvent(0,false);
        if(frame==24){check(u.saving&&u.save_valid);std::ofstream out(fs::u8path(path),std::ios::binary|std::ios::app);out<<"external";out.close();point(u.write_point);}if(frame==25){point(u.write_point);io.AddMouseButtonEvent(0,true);}if(frame==26)io.AddMouseButtonEvent(0,false);
        if(frame==28){check(u.saving&&!u.save_error.empty()&&u.source->dirty()&&bytes()==original+"external");std::ofstream out(fs::u8path(path),std::ios::binary);out<<original;out.close();app.strike_save_review();point(u.write_point);}if(frame==29){point(u.write_point);io.AddMouseButtonEvent(0,true);}if(frame==30)io.AddMouseButtonEvent(0,false);
        if(frame==32){check(!u.saving&&!u.source->dirty()&&bytes()==edited&&!t->art->dirty());u.source->undo();app.strike_select(0);}
        if(frame==35){check(u.source->dirty());app.action(Action::Quit);}
        if(frame==38){check(u.guard==3&&!app.quitting&&app.tabs.size()==1);point(u.guard_cancel_point);}if(frame==39)io.AddMouseButtonEvent(0,true);if(frame==40)io.AddMouseButtonEvent(0,false);
        if(frame==42){check(u.guard==0&&u.source->dirty()&&u.open&&!app.quitting);point(u.close_point);}if(frame==43)io.AddMouseButtonEvent(0,true);if(frame==44)io.AddMouseButtonEvent(0,false);
        if(frame==46){check(u.guard==1);point(u.guard_discard_point);}if(frame==47)io.AddMouseButtonEvent(0,true);if(frame==48)io.AddMouseButtonEvent(0,false);
        if(frame==50){check(!u.source&&!u.open&&bytes()==edited);app.load_strikes(path);app.strike_set(0,90);}
        if(frame==53){check(u.valid&&u.patch.changed&&!u.source->dirty());io.AddKeyEvent(ImGuiKey_Escape,true);}if(frame==54)io.AddKeyEvent(ImGuiKey_Escape,false);
        if(frame==56){check(!u.patch.changed&&!u.source->dirty());app.strike_set(0,91);}
        if(frame==59)point(u.apply_point);if(frame==60)io.AddMouseButtonEvent(0,true);if(frame==61)io.AddMouseButtonEvent(0,false);
        if(frame==63){check(u.source->dirty());auto other=(fs::u8path(app.prefs)/"other-strikes.ASM").u8string();std::ofstream out(fs::u8path(other),std::ios::binary);out<<original;out.close();app.load_strikes(other);}
        if(frame==66){check(u.guard==2&&u.pending_source&&u.source->bytes()!=original);point(u.guard_cancel_point);}if(frame==67)io.AddMouseButtonEvent(0,true);if(frame==68)io.AddMouseButtonEvent(0,false);
        if(frame==70){check(!u.guard&&!u.pending_source&&u.source->dirty());app.request_strike_save(false);auto copy=(fs::u8path(app.prefs)/"strike-copy.ASM").u8string();snprintf(u.save_path,sizeof u.save_path,"%s",copy.c_str());app.strike_save_review();}
        if(frame==73){check(u.saving&&u.save_valid);point(u.cancel_save_point);}if(frame==74)io.AddMouseButtonEvent(0,true);if(frame==75)io.AddMouseButtonEvent(0,false);
        if(frame==77){check(!u.saving&&u.open&&u.source->dirty()&&!t->art->dirty()&&t->art->generation()==generation);u.flip=true;}
        if(frame==80){auto other=(fs::u8path(app.prefs)/"other-strikes.ASM").u8string();app.load_strikes(other);}
        if(frame==83){check(u.guard==2&&u.pending_source);point(u.guard_save_point);}if(frame==84)io.AddMouseButtonEvent(0,true);if(frame==85)io.AddMouseButtonEvent(0,false);
        if(frame==87){check(u.saving&&u.save_valid&&u.guard==2);point(u.write_point);}if(frame==88){point(u.write_point);io.AddMouseButtonEvent(0,true);}if(frame==89)io.AddMouseButtonEvent(0,false);
        if(frame==92){check(!u.saving&&!u.guard&&u.source&&!u.source->dirty()&&u.source->bytes()==original&&!t->art->dirty()&&t->art->generation()==generation);u.flip=true;}
        if(frame==94){t->art->paint(0,0,0,8);t->art->commit();check(t->art->dirty()&&!u.source->dirty());app.action(Action::Quit);check(app.quitting&&!u.open&&!u.guard&&!u.saving);app.quitting=false;app.close_uid=0;t->art->undo();u.open=true;}
    }
};
