struct AsmArtworkLibrarySmoke {
    int failures=0;uint64_t first=0,second=0,first_generation=0,second_generation=0,created=0;int anchor=0;
    std::string path,first_path,second_path,output,first_bytes,second_bytes,source;
    void prepare(App &app,ImGuiIO &io,int frame){auto &u=app.asm_ui;auto &v=u.assets;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"ASM library UI failure at %d: %s / %s (open=%d library=%d snapshot=%d tabs=%zu)\n",frame,v.error.c_str(),v.library_error.c_str(),v.open,v.library_review,(bool)v.library_snapshot,app.tabs.size());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};
        auto click=[&](int at,ImVec2 p){if(frame==at){point(p);io.AddMouseButtonEvent(0,true);}if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto read=[](const std::string &p){std::ifstream f(fs::u8path(p),std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});};
        if(frame==0){io.AddFocusEvent(true);auto t=app.tab();first=t->uid;anchor=(short)t->art->sprite(0)->anix;
            first_path=(fs::u8path(app.prefs)/"collect-first.IMG").u8string();second_path=(fs::u8path(app.prefs)/"collect-other.IMG").u8string();output=(fs::u8path(app.prefs)/"collected.IMG").u8string();
            check(t->art->save(first_path,app.error));first_bytes=read(first_path);first_generation=t->art->generation();
            app.demo();t=app.tab();second=t->uid;check(t->art->rename_sprite(0,"FOREIGN1",app.error));t->art->color(0,1,0x03e0);t->art->commit();check(t->art->save(second_path,app.error));second_bytes=read(second_path);second_generation=t->art->generation();
            path=(fs::u8path(app.prefs)/"collect.ASM").u8string();source="a_collect\n\t.long GROUP,SENTINEL2,0\nGROUP\n\t.long SENTINEL1,FOREIGN1,0\n";{std::ofstream f(fs::u8path(path),std::ios::binary);f<<source;}
            check(app.load_asm(path));app.show_asm();u.preferred=first;
        }
        click(4,v.show_point);if(frame==9)check(v.valid&&v.review.resolved==3);click(10,v.prepare_library_point);
        if(frame==15){check(v.library_review&&v.library_snapshot&&v.library_preview&&v.library_preview->art->count()==3&&v.library_preview->art->palette_count()==2&&!v.library_preview->art->can_undo()&&v.library_preview->art->dirty());check(app.find(first)->art->generation()==first_generation&&app.find(second)->art->generation()==second_generation);}
        click(16,v.library_row_points[1]);if(frame==20)check(v.library_row==1&&v.library_snapshot->origins()[1].name=="FOREIGN1");click(21,v.cancel_library_point);
        if(frame==25)check(!v.library_review&&!v.library_snapshot&&!v.library_preview&&app.tabs.size()==2);click(26,v.prepare_library_point);
        if(frame==31)check(v.library_review&&v.library_snapshot&&v.library_preview);
        if(frame==32){auto t=app.find(first);t->art->anchor(0,anchor+30,55);t->art->commit();app.tabs.pop_back();app.active=0;app.select_uid=first;}
        if(frame==37){check(v.library_review&&v.library_snapshot->artwork().sprite(0)->anix==anchor&&v.library_preview->art->sprite(0)->anix==anchor&&app.tabs.size()==1&&read(first_path)==first_bytes&&read(second_path)==second_bytes);point(ImVec2(5,5));}
        click(38,v.create_library_point);
        if(frame==43){auto t=app.tab();created=t->uid;check(created!=first&&!u.open&&!v.open&&!v.library_review&&!v.library_preview&&app.tabs.size()==2&&t->art->count()==3&&t->art->palette_count()==2&&t->art->dirty()&&!t->art->can_undo()&&t->art->path().empty()&&t->art->disk_version()==0x634&&t->art->sprite(0)->anix==anchor);app.find(first)->art->undo();check(!app.find(first)->art->dirty());check(t->art->save(output,app.error));auto reopened=Artwork::open(output,app.error);check(reopened&&reopened->count()==3&&reopened->palette_count()==2&&std::string(reopened->sprite(2)->n_s)=="FOREIGN1"&&reopened->disk_version()==0x634);}
        if(frame==47){auto t=app.find(created);t->art->anchor(0,12,13);t->art->commit();check(t->art->can_undo());t->art->undo();check(!t->art->dirty()&&t->art->count()==3&&t->art->sprite(0)->anix==anchor);}
        if(frame==51){auto partial=AsmSource::parse("a_partial\n\t.long SENTINEL1,ani_calla,runtime,FOREIGN1,0\n",app.error);check(partial&&partial->animations[0].partial);app.install_asm(std::move(partial));app.show_asm();u.preferred=created;}
        click(56,v.show_point);if(frame==61)check(v.valid&&v.review.resolved==2&&!v.allow_partial);click(62,v.prepare_library_point);
        if(frame==66)check(!v.library_review&&!v.library_snapshot);click(67,v.partial_point);if(frame==72)check(v.allow_partial);click(73,v.prepare_library_point);
        if(frame==78)check(v.library_review&&v.library_snapshot->partial()&&v.library_preview->art->count()==2);click(79,v.cancel_library_point);
        if(frame==84)check(!v.library_review&&!v.library_preview&&app.tabs.size()==2);click(85,v.prepare_library_point);if(frame==90)check(v.library_review&&v.library_snapshot->partial());click(91,v.create_library_point);
        if(frame==96)check(app.tabs.size()==3&&app.tab()->art->count()==2&&app.tab()->art->dirty()&&!app.tab()->art->can_undo()&&app.tab()->art->path().empty()&&app.tab()->art->disk_version()==0x634&&!u.open&&!v.open);
        if(frame==107){check(read(first_path)==first_bytes&&read(second_path)==second_bytes&&read(path)==source&&!app.find(first)->art->dirty()&&!app.find(created)->art->dirty());point(ImVec2(5,5));}
    }
};
