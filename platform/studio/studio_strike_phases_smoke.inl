struct StrikePhasesSmoke {
    int failures=0;bool real=false,edit=false;uint64_t uid=0,generation=0;std::string original,path;
    void prepare(App &app,ImGuiIO &io,int frame){auto &u=app.strike_phases;
        if(real){if(frame==0){io.AddFocusEvent(true);app.tabs.clear();app.active=-1;for(int i=1;i<=10;++i){auto file=fs::path("../mk2-main/data")/("KANG"+std::to_string(i)+".IMG");if(fs::exists(file))app.open(file.u8string());}app.load_strikes("../mk2-main/src/MKSTK.ASM");app.load_asm("../mk2-main/src/MKLK.ASM");app.show_strike_phases();u.options.enabled=true;u.animation=app.asm_ui.source!=nullptr;for(int i=0;app.asm_ui.source&&i<(int)app.asm_ui.source->animations.size();++i)if(app.asm_ui.source->animations[i].label=="a_lklokick"){app.asm_select(i);app.asm_ui.player.seek(1,(int)app.asm_ui.source->animations[i].frames.size());}app.asm_ui.preferred=app.tabs.empty()?0:app.tabs[0]->uid;}
            if(frame==4&&(!u.valid||!u.pick.valid||u.pick.record!="stk_lklokick_early")){++failures;fprintf(stderr,"Real phase preview failed: %s / %s\n",u.error.c_str(),u.pick.error.c_str());}return;}
        auto t=frame?app.find(uid):app.tab();if(!t){++failures;return;}auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Phase UI failure at %d: %s / %s / %s\n",frame,app.error.c_str(),u.error.c_str(),u.pick.error.c_str());}};auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};
        if(frame==0){io.AddFocusEvent(true);uid=t->uid;generation=t->art->generation();path=(fs::u8path(app.prefs)/"countdown-strikes.ASM").u8string();
            auto record=[](const std::string &label,int x){return label+"\n\t.word "+std::to_string(x)+"\n\t.word -20\n\t.word 30\n\t.word 25\n\t.word r_test\n\t.word 4\n\t.long 10\n\t.word sound\n";};
            original="PHASE_MAX .set 2\nstrike_tables\n\t.long demo_strikes\n\t.if wunit\n\t.long external_strikes\n\t.endif\n\t.if stkphase\nstk_phase_tab\n\t.long demo_phases\n\t.if wunit\n\t.long demo_phases\n\t.endif\ndemo_phases\n\t.long 0,move_phases\n\t.long -1\nmove_phases\n\t.word 4,2\n\t.word 0,0\n\t.endif\ndemo_strikes\n\t.long stk_base\nhelper_slot\n\t.long stk_other\n\t.if stkphase\n\t.long stk_early\n\t.endif\n"+record("stk_base",60)+record("stk_other",70)+record("stk_early",35);
            std::ofstream out(fs::u8path(path),std::ios::binary);out<<original;out.close();app.load_strikes(path);app.show_strike_phases();}
        if(edit){
            auto click=[&](int at,ImVec2 p){if(frame==at){point(p);io.AddMouseButtonEvent(0,true);}if(frame==at+1)io.AddMouseButtonEvent(0,false);};
            auto expected=original;expected.replace(expected.find("4,2"),3,"5,1");
            if(frame==3){u.options.enabled=true;u.remaining=5;}click(5,u.rows_edit_point);
            if(frame==8){check(u.editing&&u.draft_valid&&!u.patch.changed&&!app.strikes.source->dirty());u.draft[0]={5,1};}
            if(frame==11){check(u.draft_valid&&u.patch.changed&&u.pick.record=="stk_other"&&app.strikes.source->bytes()==original);}
            click(12,u.rows_cancel_point);if(frame==16)check(!u.editing&&app.strikes.source->bytes()==original&&u.pick.record=="stk_early");click(17,u.rows_edit_point);
            if(frame==21){check(u.editing);u.draft[0][1]=65536;}if(frame==24){check(!u.draft_valid&&!app.strikes.source->dirty());u.draft[0]={5,1};}
            if(frame==27)check(u.draft_valid&&u.pick.record=="stk_other");click(28,u.rows_apply_point);
            if(frame==32)check(!u.editing&&app.strikes.source->dirty()&&app.strikes.source->bytes()==expected&&u.pick.record=="stk_other"&&t->art->generation()==generation);click(33,u.source_undo_point);
            if(frame==37)check(!app.strikes.source->dirty()&&app.strikes.source->bytes()==original&&u.pick.record=="stk_early");click(38,u.source_redo_point);
            if(frame==42)check(app.strikes.source->bytes()==expected&&app.strikes.source->dirty());click(43,u.source_save_point);
            if(frame==47)check(!u.open&&app.strikes.saving&&app.strikes.save_valid&&app.strikes.write_review.bytes==expected);click(48,app.strikes.write_point);
            if(frame==52){std::ifstream in(fs::u8path(path),std::ios::binary);std::string disk{std::istreambuf_iterator<char>(in),{}};check(!app.strikes.source->dirty()&&disk==expected);app.show_strike_phases();u.remaining=5;}
            click(56,u.rows_edit_point);if(frame==60){check(u.editing&&u.draft_valid&&!u.patch.changed);u.draft[0]={4,2};}if(frame==63)check(u.draft_valid&&app.strikes.source->bytes()==expected);click(64,u.rows_apply_point);
            if(frame==68)check(!u.editing&&app.strikes.source->dirty()&&app.strikes.source->bytes()==original&&!t->art->dirty());if(frame==69)app.action(Action::Quit);
            if(frame==72)check(!u.open&&app.strikes.guard==3&&app.running);click(74,app.strikes.guard_cancel_point);
            if(frame==79){check(app.running&&app.strikes.guard==0&&app.strikes.source->dirty());app.strikes.source->undo();app.show_strike_phases();u.remaining=6;}
            if(frame==83)check(!app.strikes.source->dirty()&&app.strikes.source->bytes()==expected&&t->art->generation()==generation&&!t->art->dirty());click(89,u.rows_edit_point);
            if(frame==94){check(u.editing);u.draft[0]={6,2};}if(frame==99)check(u.draft_valid&&u.patch.changed&&u.pick.record=="stk_early"&&!app.strikes.source->dirty());return;
        }
        if(frame==3){check(u.valid&&u.pick.valid&&!u.pick.phased&&u.pick.record=="stk_base"&&!app.strikes.source->dirty());point(u.enable_point);}if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==7){check(u.options.enabled&&u.pick.phased&&u.pick.row==0&&u.pick.record=="stk_early");point(u.next_point);}if(frame==8)io.AddMouseButtonEvent(0,true);if(frame==9)io.AddMouseButtonEvent(0,false);
        if(frame==11){check(u.remaining==5&&u.pick.row==0);point(u.next_point);}if(frame==12)io.AddMouseButtonEvent(0,true);if(frame==13)io.AddMouseButtonEvent(0,false);
        if(frame==15){check(u.remaining==4&&u.pick.row==0);point(u.next_point);}if(frame==16)io.AddMouseButtonEvent(0,true);if(frame==17)io.AddMouseButtonEvent(0,false);
        if(frame==19){check(u.remaining==3&&u.pick.row==1&&u.pick.record=="stk_base"&&t->art->generation()==generation&&!app.strikes.source->dirty());app.phase_step(-3);}
        if(frame==22){check(u.remaining==0&&!u.pick.active);u.window=3;point(u.play_point);}if(frame==23)io.AddMouseButtonEvent(0,true);if(frame==24)io.AddMouseButtonEvent(0,false);
        if(frame==32){check(u.remaining==0&&!u.playing);u.window=6;u.remaining=6;point(u.wolf_point);}if(frame==33)io.AddMouseButtonEvent(0,true);if(frame==34)io.AddMouseButtonEvent(0,false);
        if(frame==37){check(u.options.wolf&&u.pick.valid);u.character=1;}
        if(frame==40){check(!u.pick.valid&&!u.pick.error.empty());u.character=0;u.options.wolf=false;}
        if(frame==43){check(u.pick.valid&&u.pick.record=="stk_early");point(u.edit_point);}if(frame==44){point(u.edit_point);io.AddMouseButtonEvent(0,true);}if(frame==45)io.AddMouseButtonEvent(0,false);
        if(frame==48){check(!u.open&&app.strikes.open&&app.strikes.source->records()[app.strikes.record].label=="stk_early"&&app.strikes.valid);app.strike_set(0,77);}
        if(frame==51)point(app.strikes.apply_point);if(frame==52)io.AddMouseButtonEvent(0,true);if(frame==53)io.AddMouseButtonEvent(0,false);
        if(frame==56){check(app.strikes.source->dirty()&&t->art->generation()==generation);app.show_strike_phases();}
        if(frame==59){int record=app.phase_record(u.pick.record);check(u.valid&&u.generation==app.strikes.source->generation()&&record>=0&&app.strikes.source->records()[record].fields[0].value==77);app.strikes.source->undo();}
        if(frame==62){check(!app.strikes.source->dirty()&&app.strikes.source->bytes()==original&&!t->art->dirty()&&t->art->generation()==generation&&u.generation==app.strikes.source->generation());point(u.close_point);}if(frame==63)io.AddMouseButtonEvent(0,true);if(frame==64)io.AddMouseButtonEvent(0,false);
        if(frame==67){check(!u.open&&app.strikes.open);app.show_strike_phases();u.flip=true;}
        if(frame==71){check(u.pick.valid&&u.pick.phased&&u.pick.record=="stk_early"&&!t->art->dirty()&&!app.strikes.source->dirty());}
    }
};
