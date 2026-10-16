struct AsmArtworkReuseSmoke {
    int failures=0,count=0,palettes=0;uint64_t first=0,second=0,target=0,fg=0,sg=0,tg=0,first_id=0;void *baseline=nullptr;
    std::string ap,bp,tp,output,source_path,a,b,t,source,saved;std::shared_ptr<const SpriteClipboard> clipboard;
    void prepare(App &app,ImGuiIO &io,int frame){auto &v=app.asm_ui.assets;auto &u=app.library_copy;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"ASM reuse UI failure at %d: %s (open=%d valid=%d dirty=%d reuse=%d reused=%zu)\n",frame,u.error.c_str(),u.open,u.valid,u.dirty,u.reuse_artwork,u.plan.reused.size());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};auto click=[&](int at,ImVec2 p){if(frame==at){point(p);io.AddMouseButtonEvent(0,true);}if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto read=[](const std::string &p){std::ifstream f(fs::u8path(p),std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});};
        if(frame==0){io.AddFocusEvent(true);ap=(fs::u8path(app.prefs)/"reuse-first.IMG").u8string();bp=(fs::u8path(app.prefs)/"reuse-other.IMG").u8string();tp=(fs::u8path(app.prefs)/"reuse-target.IMG").u8string();output=(fs::u8path(app.prefs)/"reused.IMG").u8string();
            auto tab=app.tab();first=tab->uid;check(tab->art->save(ap,app.error));a=read(ap);fg=tab->art->generation();auto staging=std::make_unique<Artwork>();LibraryCopyPlan plan;check(plan_library_copy(*tab->art,*staging,{tab->art->sprite_id(0)},{},plan,app.error));clipboard=SpriteClipboard::capture(*tab->art,plan,"Unrelated clipboard",app.error);app.sprite_clipboard=clipboard;check((bool)clipboard);
            app.demo();tab=app.tab();second=tab->uid;check(tab->art->rename_sprite(0,"FOREIGN1",app.error));tab->art->color(0,1,0x03e0);tab->art->commit();check(tab->art->save(bp,app.error));b=read(bp);sg=tab->art->generation();
            {std::ofstream f(fs::u8path(tp),std::ios::binary);f<<a;}app.open(tp);tab=app.tab();target=tab->uid;t=read(tp);count=tab->art->count();palettes=tab->art->palette_count();tg=tab->art->generation();first_id=tab->art->sprite_id(0);baseline=tab->art->sprite(0)->baseline_p;
            source_path=(fs::u8path(app.prefs)/"reuse-source.ASM").u8string();source="a_reuse\n\t.long GROUP,SENTINEL2,0\nGROUP\n\t.long SENTINEL1,FOREIGN1,0\n";{std::ofstream f(fs::u8path(source_path),std::ios::binary);f<<source;}
            check(app.load_asm(source_path));app.show_asm();app.asm_ui.preferred=first;
        }
        click(4,v.show_point);click(9,v.prepare_library_point);click(15,v.copy_library_point);
        if(frame==20)check(u.open&&u.valid&&!u.reuse_artwork&&u.plan.reused.empty()&&!u.plan.rows[0].error.empty());click(21,u.reuse_artwork_point);
        if(frame==26)check(u.valid&&!u.dirty&&u.reuse_artwork&&u.plan.reused.size()==2&&!u.plan.rows[0].enabled&&!u.plan.rows[1].enabled&&u.plan.rows[2].enabled&&u.options.excluded.empty());click(27,u.row_points[0]);
        if(frame==32)check(u.plan.reused.size()==1&&u.options.excluded.count(u.plan.rows[0].sprite)&&!u.plan.rows[0].enabled);click(33,u.row_points[0]);
        if(frame==37)check(u.plan.reused.size()==2&&u.options.excluded.empty());click(38,u.reuse_artwork_point);if(frame==43)check(!u.reuse_artwork&&u.plan.reused.empty()&&!u.plan.rows[0].error.empty());click(44,u.reuse_artwork_point);
        if(frame==49)check(u.reuse_artwork&&u.plan.reused.size()==2&&u.plan.added_palettes==1);click(50,u.name_points[0]);if(frame==53)point(ImVec2(5,5));click(55,u.apply_point);
        if(frame==60){auto tab=app.find(target);check(!u.open&&tab->art->count()==count+1&&tab->art->palette_count()==palettes+1&&tab->art->sprite_id(0)==first_id&&tab->art->sprite(0)->baseline_p==baseline&&tab->art->dirty()&&app.sprite_clipboard==clipboard);}
        if(frame==61)app.action(Action::Undo);if(frame==66)check(app.find(target)->art->count()==count&&!app.find(target)->art->dirty());if(frame==67)app.action(Action::Redo);
        if(frame==72)check(app.find(target)->art->count()==count+1&&app.find(target)->art->dirty());if(frame==73){check(app.find(target)->art->save(output,app.error));saved=read(output);auto loaded=Artwork::open(output,app.error);check(loaded&&loaded->count()==count+1&&loaded->disk_version()==0x634);}
        if(frame==77){app.show_asm();app.asm_ui.preferred=target;}
        click(82,v.show_point);click(87,v.prepare_library_point);click(93,v.copy_library_point);
        if(frame==98)check(u.open&&!u.reuse_artwork&&u.plan.reused.empty()&&app.find(target)->art->can_undo());click(99,u.reuse_artwork_point);
        if(frame==104){auto tab=app.find(target);check(u.reuse_artwork&&u.plan.reused.size()==3&&u.plan.added_pixels==0&&u.plan.added_palettes==0&&std::none_of(u.plan.rows.begin(),u.plan.rows.end(),[](const LibraryCopyRow &r){return r.enabled;}));tg=tab->art->generation();point(ImVec2(5,5));}click(105,u.apply_point);
        if(frame==110){auto tab=app.find(target);check(!u.open&&!tab->art->dirty()&&tab->art->generation()==tg&&tab->art->count()==count+1&&app.sprite_clipboard==clipboard);}
        if(frame==111)app.action(Action::Undo);if(frame==116)check(app.find(target)->art->count()==count&&app.find(target)->art->dirty());if(frame==117)app.action(Action::Redo);
        if(frame==122)check(app.find(target)->art->count()==count+1&&!app.find(target)->art->dirty()&&app.find(first)->art->generation()==fg&&app.find(second)->art->generation()==sg&&read(ap)==a&&read(bp)==b&&read(tp)==t&&read(source_path)==source&&read(output)==saved&&app.sprite_clipboard==clipboard);
    }
};
