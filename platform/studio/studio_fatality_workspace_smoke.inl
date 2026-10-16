struct FatalityWorkspaceSmoke {
    int failures=0;uint64_t image_uid=0,image_generation=0,aid=0,bid=0,cid=0,bgen=0;
    std::string ap,bp,cp,image_path,a,b,c,image_bytes;
    void prepare(App &app,ImGuiIO &io,int frame){auto &u=app.fatality;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Fatality workspace UI failure at %d: %s / %s / %s (id=%llu guard=%d sources=%zu dirty=%zu saving=%d)\n",frame,app.error.c_str(),u.error.c_str(),u.save_error.c_str(),(unsigned long long)u.document,u.guard,u.workspace.documents().size(),u.workspace.dirty_count(),u.saving);}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};
        auto click=[&](int at,ImVec2 p){if(frame==at){point(p);io.AddMouseButtonEvent(0,true);}if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto read=[&](const std::string &path){std::ifstream f(fs::u8path(path),std::ios::binary);return std::string{std::istreambuf_iterator<char>(f),{}};};
        auto edit=[&](const std::string &body){snprintf(u.body.data(),u.body.size(),"%s",body.c_str());u.dirty_preview=true;};
        if(frame==0){io.AddFocusEvent(true);auto t=app.tab();image_uid=t->uid;image_path=(fs::u8path(app.prefs)/"workspace-artwork.IMG").u8string();check(t->art->save(image_path,app.error));image_bytes=read(image_path);image_generation=t->art->generation();
            ap=(fs::u8path(app.prefs)/"workspace-moves.ASM").u8string();bp=(fs::u8path(app.prefs)/"workspace-buttons.ASM").u8string();cp=(fs::u8path(app.prefs)/"workspace-other.ASM").u8string();
            a="fatal_demo\n\tmovi r_fatal,a7\n\tmovi scom_shared,a11\n\tjmp fatality_xfer\na_external_tail\n\t.long SENTINEL1,0\n";
            b="scom_shared\n\t.word 20,sw_up\na_torso_ripped\n\t.long SENTINEL1,ani_jump,a_external_tail\n";
            c="scom_shared\n\t.word 30,sw_down\n";
            {std::ofstream f(fs::u8path(ap),std::ios::binary);f<<a;}{std::ofstream f(fs::u8path(bp),std::ios::binary);f<<b;}{std::ofstream f(fs::u8path(cp),std::ios::binary);f<<c;}
            app.add_fatality(ap);app.show_fatality();aid=u.document;
        }
        auto t=app.find(image_uid);if(!t){++failures;return;}
        auto intact=[&](){return !t->art->dirty()&&t->art->generation()==image_generation&&read(image_path)==image_bytes;};
        if(frame==3)check(u.source&&u.workspace.documents().size()==1&&u.valid&&u.workspace.matches("scom_shared",FatalityKind::Combo,aid).empty());
        if(frame==4)edit(u.source->body(0)+"\t; A edit\n");if(frame==8)check(u.valid&&u.patch.changed);click(9,u.apply_point);if(frame==13)check(u.source->dirty());
        if(frame==14)snprintf(u.path,sizeof u.path,"%s",bp.c_str());click(15,u.load_point);
        if(frame==19){bid=u.document;check(bid!=aid&&u.workspace.documents().size()==2&&u.workspace.document(aid)->dirty()&&!u.source->dirty());}
        if(frame==20)edit("\t.word 35,sw_up\n");if(frame==24)check(u.valid&&u.patch.changed);click(25,u.apply_point);
        if(frame==29){bgen=u.source->generation();check(u.workspace.dirty_count()==2&&read(ap)==a&&read(bp)==b&&intact());}
        click(30,u.documents_point);click(33,u.document_choices[0]);if(frame==37)check(u.document==aid&&u.block==0&&u.source->dirty()&&u.workspace.document(bid)->generation()==bgen);
        click(38,u.undo_point);if(frame==42)check(!u.source->dirty()&&u.workspace.document(bid)->dirty()&&u.workspace.document(bid)->generation()==bgen);
        click(43,u.redo_point);if(frame==47)check(u.source->dirty()&&u.workspace.dirty_count()==2);click(48,u.combo_point);
        if(frame==52)check(u.document==bid&&u.block==0&&u.source->dirty()&&u.workspace.document(aid)->dirty());
        if(frame==53)app.add_fatality(ap);if(frame==57)check(u.document==aid&&u.workspace.documents().size()==2&&u.source->bytes().find("A edit")!=std::string::npos);
        if(frame==58)snprintf(u.path,sizeof u.path,"%s",cp.c_str());click(59,u.load_point);
        if(frame==63){cid=u.document;check(cid!=aid&&cid!=bid&&u.workspace.documents().size()==3&&!u.source->dirty());}
        if(frame==64)app.activate_fatality(aid);if(frame==68)check(u.workspace.matches("scom_shared",FatalityKind::Combo,aid).size()==2);
        click(69,u.combo_point);click(72,u.combo_choices[1]);if(frame==76)check(u.document==cid&&!u.source->dirty()&&u.workspace.dirty_count()==2);
        click(77,u.close_source_point);if(frame==81)check(!u.workspace.document(cid)&&u.workspace.documents().size()==2&&u.document==aid&&u.source->dirty());
        click(82,u.save_point);if(frame==86)check(u.saving&&u.save_valid);if(frame==87){snprintf(u.save_path,sizeof u.save_path,"%s",bp.c_str());app.fatality_save_review();}
        if(frame==91)check(!u.save_valid&&!u.save_error.empty()&&read(bp)==b);click(92,u.cancel_save_point);if(frame==96)check(u.workspace.dirty_count()==2&&intact());
        if(frame==97)app.add_fatality(cp);if(frame==101){auto next=u.document;check(next>cid&&!u.workspace.document(cid));cid=next;}
        if(frame==102){app.activate_fatality(aid);app.action(Action::Quit);}if(frame==106)check(u.guard==3&&u.guard_document==aid&&!app.quitting);
        click(107,u.guard_discard_point);if(frame==111)check(u.guard==3&&u.document==bid&&u.guard_document==bid&&!u.workspace.document(aid)&&u.workspace.dirty_count()==1&&!app.quitting&&read(ap)==a);
        click(112,u.guard_cancel_point);if(frame==116)check(!u.guard&&u.document==bid&&u.source->dirty());
        click(117,u.close_source_point);if(frame==121)check(u.guard==4&&u.guard_document==bid);click(122,u.guard_cancel_point);if(frame==126)check(u.workspace.document(bid)&&u.source->dirty());
        click(127,u.reload_point);if(frame==131)check(u.guard==2&&u.pending_source);click(132,u.guard_save_point);if(frame==136)check(u.saving&&u.save_valid&&u.guard==2);click(137,u.write_point);
        if(frame==141)check(!u.guard&&!u.saving&&u.document==bid&&!u.source->dirty()&&!u.source->can_undo()&&u.source->bytes()==read(bp)&&u.source->bytes().find("35,sw_up")!=std::string::npos);
        if(frame==142)app.activate_fatality(cid);if(frame==143)edit("\t.word 55,sw_down\n");if(frame==147)check(u.valid&&u.patch.changed);click(148,u.apply_point);if(frame==152)check(u.source->dirty());
        click(153,u.close_source_point);if(frame==157)check(u.guard==4);click(158,u.guard_save_point);if(frame==162)check(u.saving&&u.save_valid);click(163,u.write_point);
        if(frame==167)check(!u.guard&&!u.saving&&!u.workspace.document(cid)&&u.workspace.documents().size()==1&&u.document==bid&&read(cp).find("55,sw_down")!=std::string::npos);
        click(168,u.close_source_point);if(frame==172)check(u.workspace.documents().empty()&&!u.source&&!u.document&&u.block==-1);
        if(frame==173)app.add_fatality(ap);if(frame==177){check(u.document>aid&&u.source->bytes()==a);aid=u.document;app.add_fatality(bp);bid=u.document;app.fatality_select(1);}
        if(frame==179){auto source=u.workspace.document(aid);for(int i=0;i<(int)source->blocks().size();++i)if(source->blocks()[i].label=="a_external_tail"){FatalityPatch patch;check(source->review(i,"\t.long SENTINEL2,0\n",patch,u.error)&&source->apply(patch,u.error));}}
        if(frame==182)check(u.block==1&&u.source->blocks()[1].kind==FatalityKind::Animation);click(183,u.preview_point);
        if(frame==187){auto a=app.asm_animation();check(app.asm_ui.open&&!u.open&&u.workspace.documents().size()==2&&a&&!a->partial&&a->frames.size()==2&&a->frames[1].name=="SENTINEL2"&&a->frames[1].source==ap&&u.workspace.document(aid)->dirty()&&read(ap)==this->a);check(u.workspace.document(aid)->undo(u.error));app.asm_ui.player.seek(1,2);check(app.asm_animation()->frames[1].name=="SENTINEL2");}click(188,u.back_point);if(frame==192)check(u.open&&u.document==bid&&u.block==1);
        click(193,u.documents_point);click(196,u.document_choices[0]);if(frame==200)check(u.document==aid&&u.block==0);click(201,u.combo_point);if(frame==205)check(u.document==bid&&u.block==0);
        if(frame==207){app.activate_fatality(aid);edit(u.source->body(0)+"\t; retained source edit\n");}if(frame==211)check(u.valid&&u.patch.changed);click(212,u.apply_point);
        if(frame==216)check(u.workspace.dirty_count()==1&&u.source->dirty()&&!u.workspace.document(bid)->dirty());
        if(frame==225){check(u.open&&u.document==aid&&u.workspace.documents().size()==2&&u.source->can_undo()&&read(ap)==a&&read(bp).find("35,sw_up")!=std::string::npos&&read(cp).find("55,sw_down")!=std::string::npos&&intact());point(ImVec2(5,5));}
    }
};
