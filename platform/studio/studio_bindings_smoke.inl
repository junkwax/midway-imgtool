struct BindingsSmoke {
    int failures=0;uint64_t uid=0,generation=0;std::string source,other,sidecar,original,edited;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=frame?app.find(uid):app.tab();if(!t){++failures;return;}auto &u=app.binding_ui;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Bindings UI failure at %d: %s / %s / %s\n",frame,app.error.c_str(),u.error.c_str(),u.save_error.c_str());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};
        auto write=[&](const std::string &path,const std::string &bytes){std::ofstream out(fs::u8path(path),std::ios::binary);out<<bytes;};auto read=[&](){std::ifstream in(fs::u8path(sidecar),std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
        if(frame==0){io.AddFocusEvent(true);uid=t->uid;generation=t->art->generation();source=(fs::u8path(app.prefs)/"bindings-first.ASM").u8string();other=(fs::u8path(app.prefs)/"bindings-second.ASM").u8string();sidecar=strike_bind::SidecarPathFor(source);
            auto record=[](const std::string &label){return label+"\n\t.word 50\n\t.word -30\n\t.word 30\n\t.word 25\n\t.word r_test\n\t.word 4\n\t.long 10\n\t.word sound\n";};write(source,record("stk_demo")+record("stk_other"));write(other,record("stk_demo")+record("stk_other"));
            original="\xef\xbb\xbf" "# preserve annotations\r\na_demo = stk_other\r\nother_anim = stk_other\r\nopaque original text\r\n";write(sidecar,original);write(strike_bind::SidecarPathFor(other),"# second source\n");
            auto anim=(fs::u8path(app.prefs)/"bindings-animation.ASM").u8string();write(anim,"a_demo\n\t.long SENTINEL1,0\n");app.load_strikes(source);app.load_asm(anim);app.show_asm();}
        auto doc=app.current_bindings();
        if(frame==3){check(doc&&!doc->dirty()&&app.asm_strike_record()==1);app.set_asm_binding(std::string("stk_demo"));edited=doc->bytes();app.show_bindings();}
        if(frame==6){check(doc->dirty()&&doc->bytes()==edited&&t->art->generation()==generation);point(u.undo_point);}if(frame==7)io.AddMouseButtonEvent(0,true);if(frame==8)io.AddMouseButtonEvent(0,false);
        if(frame==10){check(!doc->dirty()&&doc->bytes()==original&&app.asm_strike_record()==1);point(u.redo_point);}if(frame==11)io.AddMouseButtonEvent(0,true);if(frame==12)io.AddMouseButtonEvent(0,false);
        if(frame==14){check(doc->dirty()&&app.asm_strike_record()==0);point(u.save_point);}if(frame==15)io.AddMouseButtonEvent(0,true);if(frame==16)io.AddMouseButtonEvent(0,false);
        if(frame==18){check(u.saving&&u.valid&&u.review.bytes==edited);write(sidecar,original+"external");point(u.write_point);}if(frame==19){point(u.write_point);io.AddMouseButtonEvent(0,true);}if(frame==20)io.AddMouseButtonEvent(0,false);
        if(frame==22){check(u.saving&&!u.save_error.empty()&&doc->dirty()&&read()==original+"external");write(sidecar,original);app.bindings_save_review();point(u.write_point);}if(frame==23){point(u.write_point);io.AddMouseButtonEvent(0,true);}if(frame==24)io.AddMouseButtonEvent(0,false);
        if(frame==26){check(!u.saving&&!doc->dirty()&&read()==edited);snprintf(u.animation,sizeof u.animation,"a_demo");snprintf(u.strike,sizeof u.strike,"-");point(u.apply_point);}if(frame==27)io.AddMouseButtonEvent(0,true);if(frame==28)io.AddMouseButtonEvent(0,false);
        if(frame==30){check(doc->dirty()&&app.asm_strike_record()==-1);point(u.save_point);}if(frame==31)io.AddMouseButtonEvent(0,true);if(frame==32)io.AddMouseButtonEvent(0,false);
        if(frame==34){check(u.saving&&u.valid);point(u.write_point);}if(frame==35){point(u.write_point);io.AddMouseButtonEvent(0,true);}if(frame==36)io.AddMouseButtonEvent(0,false);
        if(frame==39){check(!u.saving&&!doc->dirty());auto reopened=StrikeBindings::open_for(source,u.error);check(reopened&&reopened->lookup("a_demo")==std::optional<std::string>("-"));app.load_asm((fs::u8path(app.prefs)/"bindings-animation.ASM").u8string());check(app.asm_strike_record()==-1);doc->set("a_extra",std::string("stk_other"),u.error);app.bindings_reload();}
        if(frame==42){check(u.guard==2&&u.pending_reload&&doc->dirty());point(u.guard_cancel_point);}if(frame==43)io.AddMouseButtonEvent(0,true);if(frame==44)io.AddMouseButtonEvent(0,false);
        if(frame==46){check(u.guard==0&&doc->dirty());app.bindings_reload();}
        if(frame==49)point(u.guard_discard_point);if(frame==50)io.AddMouseButtonEvent(0,true);if(frame==51)io.AddMouseButtonEvent(0,false);
        if(frame==53){check(!u.guard&&!u.pending_reload&&!doc->dirty()&&!doc->lookup("a_extra"));app.set_asm_binding(std::string("stk_demo"));app.load_strikes(other);app.show_bindings();}
        if(frame==56){check(u.documents.size()==2&&app.dirty_bindings()&&!doc->dirty());app.set_asm_binding(std::string("stk_other"));app.action(Action::Quit);check(u.guard==1&&!app.quitting&&!app.asm_ui.open&&!app.strikes.open);}
        if(frame==59)point(u.guard_cancel_point);if(frame==60)io.AddMouseButtonEvent(0,true);if(frame==61)io.AddMouseButtonEvent(0,false);
        if(frame==63){check(!u.guard&&u.open&&app.dirty_bindings()&&!app.quitting);app.action(Action::Quit);}
        if(frame==66)point(u.guard_save_point);if(frame==67)io.AddMouseButtonEvent(0,true);if(frame==68)io.AddMouseButtonEvent(0,false);
        if(frame==70){check(u.saving&&u.valid&&u.guard==1);point(u.write_point);}if(frame==71){point(u.write_point);io.AddMouseButtonEvent(0,true);}if(frame==72)io.AddMouseButtonEvent(0,false);
        if(frame==74){check(!u.saving&&u.guard==1&&app.dirty_bindings()&&read()==edited);}
        if(frame==77)point(u.guard_discard_point);if(frame==78)io.AddMouseButtonEvent(0,true);if(frame==79)io.AddMouseButtonEvent(0,false);
        if(frame==80){check(!app.dirty_bindings()&&app.quitting&&!u.guard);app.quitting=false;app.close_uid=app.remove_uid=0;app.show_bindings();}
        if(frame==83){check(!doc->dirty()&&!app.strikes.source->dirty()&&!t->art->dirty()&&t->art->generation()==generation);app.set_asm_binding(std::string("stk_demo"));app.load_strikes(source);app.show_bindings();check(app.asm_strike_record()==0&&app.dirty_bindings());}
        if(frame==86){std::array<std::string,8> values;for(int i=0;i<8;++i)values[i]=app.strikes.source->records()[0].fields[i].raw;values[0]="51";StrikePatch patch;check(app.strikes.source->review(0,values,patch,u.error)&&app.strikes.source->apply(patch,u.error));app.action(Action::Quit);check(u.guard==1&&!app.strikes.guard);}
        if(frame==89)point(u.guard_discard_point);if(frame==90)io.AddMouseButtonEvent(0,true);if(frame==91)io.AddMouseButtonEvent(0,false);
        if(frame==93){check(!u.guard&&!app.dirty_bindings()&&app.strikes.guard==3&&app.strikes.source->dirty());point(app.strikes.guard_cancel_point);}if(frame==94){point(app.strikes.guard_cancel_point);io.AddMouseButtonEvent(0,true);}if(frame==95)io.AddMouseButtonEvent(0,false);
        if(frame==97){check(!app.strikes.guard&&!app.quitting&&app.strikes.source->dirty());app.strikes.source->undo();app.strike_select(0);app.show_bindings();check(!t->art->dirty()&&t->art->generation()==generation&&read()==edited);}
    }
};
