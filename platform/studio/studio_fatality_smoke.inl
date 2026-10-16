struct FatalitySmoke {
    int failures=0;bool real=false;uint64_t uid=0,generation=0;
    std::string path,other,output,original,saved,image_path,image_bytes;
    void prepare(App &app,ImGuiIO &io,int frame){auto &u=app.fatality;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Fatality UI failure at %d: %s / %s / %s (open=%d guard=%d saving=%d block=%d valid=%d dirty=%d)\n",frame,app.error.c_str(),u.error.c_str(),u.save_error.c_str(),u.open,u.guard,u.saving,u.block,u.valid,u.source&&u.source->dirty());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};
        auto click=[&](int at,ImVec2 p){if(frame==at){point(p);io.AddMouseButtonEvent(0,true);}if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto read=[&](const std::string &file){std::ifstream f(fs::u8path(file),std::ios::binary);return std::string{std::istreambuf_iterator<char>(f),{}};};
        if(real){if(frame==0){io.AddFocusEvent(true);path="../mk2-main/src/MOVES.ASM";original=read(path);app.load_fatality(path);app.show_fatality();}
            if(frame==4)check(u.source&&u.source->blocks().size()>20&&!u.source->dirty()&&u.block>=0&&u.valid);
            if(frame==8){check(read(path)==original);point(ImVec2(5,5));}return;
        }
        auto t=frame?app.find(uid):app.tab();if(!t){++failures;return;}
        auto intact=[&](){return !t->art->dirty()&&t->art->generation()==generation&&read(image_path)==image_bytes;};
        if(frame==0){io.AddFocusEvent(true);uid=t->uid;image_path=(fs::u8path(app.prefs)/"fatality-artwork.IMG").u8string();check(t->art->save(image_path,app.error));image_bytes=read(image_path);generation=t->art->generation();
            path=(fs::u8path(app.prefs)/"fatality-preview.ASM").u8string();other=(fs::u8path(app.prefs)/"fatality-other.ASM").u8string();output=(fs::u8path(app.prefs)/"fatality-result.ASM").u8string();
            original="; source fixture\r\nfatal_demo\r\n\tmovi r_fatal,a7 ; routine\r\n\tmovi scom_demo,a11\r\n\tmovi l_hp,a0\r\n\tcmpi 3,winner_status\r\n\tjmp fatality_xfer\r\nscom_demo\r\n\t.word 20,sw_down,sw_up\r\n\t.word sw_hp\r\na_torso_ripped\r\n\t.long SENTINEL1\r\n\t.long 0\r\ntail_label\r\n\t.long 123\r\n";
            {std::ofstream out(fs::u8path(path),std::ios::binary);out<<original;}{std::ofstream out(fs::u8path(other),std::ios::binary);out<<original;}
            app.load_fatality(path);app.show_fatality();
        }
        if(frame==3)check(u.open&&u.source&&u.source->blocks().size()==3&&u.block==0&&u.valid&&!u.source->dirty());
        click(4,u.body_point);if(frame==6){io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_End,true);}if(frame==7){io.AddKeyEvent(ImGuiKey_End,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);io.AddInputCharactersUTF8("\t; reviewed note\n");}
        if(frame==11)check(u.valid&&u.patch.changed&&!u.source->dirty()&&read(path)==original&&intact());click(12,u.apply_point);
        if(frame==16)check(u.source->dirty()&&u.source->can_undo()&&u.source->bytes().find("reviewed note")!=std::string::npos&&read(path)==original&&intact());
        click(17,u.undo_point);if(frame==21)check(!u.source->dirty()&&u.source->bytes()==original);click(22,u.redo_point);if(frame==26)check(u.source->dirty());
        click(27,u.save_point);if(frame==31)check(u.saving&&u.save_valid&&u.diff.find("reviewed note")!=std::string::npos&&u.write_review.previous==original);click(32,u.cancel_save_point);
        if(frame==36)check(!u.saving&&u.source->dirty()&&read(path)==original);click(37,u.close_point);if(frame==41)check(u.guard==1);
        click(42,u.guard_cancel_point);if(frame==46)check(u.open&&u.guard==0&&u.source->dirty());
        if(frame==47)app.load_fatality(other);if(frame==51)check(u.guard==2&&u.pending_source&&u.source->path()==path);
        click(52,u.guard_cancel_point);if(frame==56)check(u.guard==0&&!u.pending_source&&u.source->dirty());
        if(frame==57){auto old=u.source.get();app.load_fatality(path+".missing");check(u.source.get()==old);u.error.clear();}click(58,u.undo_point);if(frame==62)check(!u.source->dirty()&&u.source->bytes()==original);
        click(63,u.combo_point);if(frame==67)check(u.block==1&&u.source->blocks()[u.block].kind==FatalityKind::Combo);
        if(frame==68){snprintf(u.body.data(),u.body.size(),"\t.word 35,sw_down\n\t.word sw_hp\n");u.dirty_preview=true;}
        if(frame==72)check(u.valid&&u.patch.changed);click(73,u.apply_point);if(frame==77)check(u.source->dirty()&&u.source->blocks()[1].summary.find("35")!=std::string::npos);
        click(78,u.save_point);if(frame==82)check(u.saving&&u.save_valid);click(83,u.write_point);
        if(frame==87){saved=read(path);check(!u.saving&&!u.source->dirty()&&saved.find("35,sw_down")!=std::string::npos&&saved==u.source->bytes()&&intact());}
        click(88,u.undo_point);if(frame==92)check(u.source->dirty()&&u.source->bytes()==original);click(93,u.redo_point);if(frame==97)check(!u.source->dirty());
        if(frame==98)app.fatality_select(2);if(frame==102)check(u.block==2&&!app.fatality_staged());click(103,u.preview_point);
        if(frame==107)check(!u.open&&u.return_preview&&app.asm_ui.open&&app.asm_animation()&&app.asm_animation()->label=="a_torso_ripped"&&app.asm_ui.pieces.size()==1&&app.asm_ui.pieces[0].sprite);
        click(108,u.back_point);if(frame==112)check(u.open&&!app.asm_ui.open&&!u.return_preview&&u.block==2);
        if(frame==113){snprintf(u.body.data(),u.body.size(),"\t.long SENTINEL1\n\t.long SENTINEL2\n\t.long 0\n");u.dirty_preview=true;}
        if(frame==117)check(u.valid&&u.patch.changed);click(118,u.apply_point);if(frame==122)check(u.source->dirty());
        if(frame==123)app.action(Action::Quit);if(frame==127)check(u.guard==3&&!app.quitting&&app.tabs.size()==1&&u.source->dirty());
        click(128,u.guard_cancel_point);if(frame==132)check(u.guard==0&&u.open&&u.source->dirty());
        click(133,u.save_point);if(frame==137){check(u.saving);snprintf(u.save_path,sizeof u.save_path,"%s",output.c_str());app.fatality_save_review();}
        if(frame==141)check(u.save_valid&&u.write_review.path==output);click(142,u.write_point);
        if(frame==146)check(!u.saving&&!u.source->dirty()&&u.source->path()==output&&read(output)==u.source->bytes()&&read(path)==saved&&intact());
        if(frame==147)app.load_fatality(other);if(frame==151)check(u.source->path()==other&&!u.source->dirty());
        if(frame==152){auto old=u.source.get();app.load_fatality(path+".missing");check(u.source.get()==old);u.error.clear();}
        if(frame==154){snprintf(u.body.data(),u.body.size(),"new_label\n");u.dirty_preview=true;}
        if(frame==158)check(!u.valid&&app.fatality_staged());click(159,u.reset_point);if(frame==163)check(u.valid&&!app.fatality_staged()&&!u.source->dirty());
        click(165,u.close_point);if(frame==169)check(!u.open&&intact());if(frame==170){app.show_fatality();app.fatality_select(2);}
        if(frame==174){check(u.open&&u.valid&&u.block==2&&!u.source->dirty()&&intact()&&read(path)==saved&&read(other)==original);point(ImVec2(5,5));}
        if(frame==175){app.fatality_select(1);snprintf(u.body.data(),u.body.size(),"\t.word 35,sw_down\n");u.dirty_preview=true;}
        if(frame==179)check(u.valid&&u.patch.changed);click(180,u.apply_point);if(frame==184)check(u.source->dirty());
        if(frame==185)app.load_fatality(path);if(frame==189)check(u.guard==2&&u.pending_source);
        click(190,u.guard_discard_point);if(frame==194)check(u.open&&u.guard==0&&u.source&&u.source->path()==path&&!u.source->dirty()&&read(path)==saved&&read(other)==original);
        if(frame==195){auto body=u.source->body(0)+"\t; save on close\n";snprintf(u.body.data(),u.body.size(),"%s",body.c_str());u.dirty_preview=true;}
        if(frame==199)check(u.valid&&u.patch.changed);click(200,u.apply_point);if(frame==204)check(u.source->dirty());
        click(205,u.close_point);if(frame==209)check(u.guard==1);click(210,u.guard_save_point);if(frame==214)check(u.saving&&u.save_valid&&u.guard==1);click(215,u.write_point);
        if(frame==219){check(!u.open&&!u.saving&&u.guard==0&&u.source&&!u.source->dirty()&&read(path)==u.source->bytes()&&intact());saved=read(path);}
        if(frame==220){app.show_fatality();app.fatality_select(2);}
        if(frame==225){check(u.open&&u.valid&&!u.source->dirty()&&read(path)==saved&&read(other)==original&&intact());point(ImVec2(5,5));}

    }
};
