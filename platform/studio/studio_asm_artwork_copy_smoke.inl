struct AsmArtworkCopySmoke {
    int failures=0,count=0,palettes=0,added_palettes=0;uint64_t first=0,second=0,target=0,fg=0,sg=0,tg=0;
    std::string ap,bp,tp,output,source_path,a,b,t,source;std::shared_ptr<const SpriteClipboard> clipboard;
    void prepare(App &app,ImGuiIO &io,int frame){auto &v=app.asm_ui.assets;auto &u=app.library_copy;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"ASM artwork copy UI failure at %d: %s / %s (copy=%d artwork=%d target=%llu valid=%d dirty=%d)\n",frame,u.error.c_str(),v.error.c_str(),u.open,v.library_review,(unsigned long long)u.target,u.valid,u.dirty);}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};auto click=[&](int at,ImVec2 p){if(frame==at){point(p);io.AddMouseButtonEvent(0,true);}if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto read=[](const std::string &p){std::ifstream f(fs::u8path(p),std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});};
        if(frame==0){io.AddFocusEvent(true);ap=(fs::u8path(app.prefs)/"copy-first.IMG").u8string();bp=(fs::u8path(app.prefs)/"copy-other.IMG").u8string();tp=(fs::u8path(app.prefs)/"copy-target.IMG").u8string();output=(fs::u8path(app.prefs)/"copied.IMG").u8string();
            auto tab=app.tab();first=tab->uid;check(tab->art->save(ap,app.error));a=read(ap);fg=tab->art->generation();auto staging=std::make_unique<Artwork>();LibraryCopyPlan plan;check(plan_library_copy(*tab->art,*staging,{tab->art->sprite_id(0)},{},plan,app.error));clipboard=SpriteClipboard::capture(*tab->art,plan,"Unrelated clipboard",app.error);check((bool)clipboard);app.sprite_clipboard=clipboard;
            app.demo();tab=app.tab();second=tab->uid;check(tab->art->rename_sprite(0,"FOREIGN1",app.error));tab->art->color(0,1,0x03e0);tab->art->commit();check(tab->art->save(bp,app.error));b=read(bp);sg=tab->art->generation();
            app.demo();tab=app.tab();target=tab->uid;count=tab->art->count();palettes=tab->art->palette_count();check(tab->art->save(tp,app.error));t=read(tp);tg=tab->art->generation();
            source_path=(fs::u8path(app.prefs)/"copy-source.ASM").u8string();source="a_copy\n\t.long GROUP,SENTINEL2,0\nGROUP\n\t.long SENTINEL1,FOREIGN1,0\n";{std::ofstream f(fs::u8path(source_path),std::ios::binary);f<<source;}
            check(app.load_asm(source_path));app.show_asm();app.asm_ui.preferred=first;
        }
        click(4,v.show_point);click(9,v.prepare_library_point);if(frame==14)check(v.library_review&&v.library_snapshot&&v.library_preview);click(15,v.copy_library_point);
        if(frame==20)check(u.open&&u.held_artwork&&u.target==target&&u.valid&&u.plan.rows.size()==3&&!u.plan.rows[0].error.empty()&&!u.plan.rows[1].error.empty()&&app.sprite_clipboard==clipboard);
        click(21,u.prefix_point);if(frame==25)io.AddInputCharactersUTF8("CP_");click(29,u.name_points[1]);
        if(frame==34)check(u.valid&&!u.dirty&&u.plan.options.prefix=="CP_"&&u.plan.added_palettes==1&&std::none_of(u.plan.rows.begin(),u.plan.rows.end(),[](const LibraryCopyRow &r){return !r.error.empty();}));
        click(35,u.row_points[2]);if(frame==40)check(u.valid&&!u.dirty&&!u.plan.rows[2].enabled&&u.plan.added_palettes==0&&app.find(target)->art->generation()==tg&&app.find(first)->art->generation()==fg&&app.find(second)->art->generation()==sg);
        if(frame==41){app.tabs.pop_back();app.active=0;app.select_uid=first;}
        if(frame==45)check(!u.open&&!u.held_artwork&&app.asm_ui.open&&v.open&&v.library_review&&v.library_snapshot&&app.sprite_clipboard==clipboard&&!app.find(first)->art->dirty()&&!app.find(second)->art->dirty());
        if(frame==46){app.open(tp);check(app.tab()->uid!=target);target=app.tab()->uid;tg=app.tab()->art->generation();}
        click(50,v.copy_library_point);if(frame==55){check(u.open&&u.target==target);snprintf(u.prefix,sizeof u.prefix,"CP_");u.dirty=true;}
        if(frame==59){check(u.valid&&!u.dirty);auto tab=app.find(target);tab->art->anchor(0,2,3);tab->art->commit();}
        if(frame==63)check(u.open&&app.find(target)->art->generation()!=u.target_generation&&app.find(target)->art->count()==count);click(64,u.cancel_point);
        if(frame==69){check(!u.open&&v.open&&v.library_review&&v.library_snapshot);app.find(target)->art->undo();check(!app.find(target)->art->dirty());}
        click(70,v.copy_library_point);if(frame==75){check(u.open&&u.target==target);snprintf(u.prefix,sizeof u.prefix,"CP_");u.dirty=true;}
        if(frame==80){added_palettes=u.plan.added_palettes;check(u.valid&&!u.dirty&&u.plan.rows.size()==3&&added_palettes>=1&&app.find(target)->art->count()==count&&app.sprite_clipboard==clipboard);point(ImVec2(5,5));}
        if(frame==81){check(app.find(first)->art->rename_sprite(0,"SESSIONEDIT",app.error));app.tabs.erase(app.tabs.begin()+1);for(int i=0;i<(int)app.tabs.size();++i)if(app.tabs[i]->uid==target){app.active=i;app.select_uid=target;}}
        if(frame==86)check(u.open&&u.valid&&u.held_artwork&&u.held_artwork->artwork().count()==3&&!app.find(second)&&app.find(target)->art->generation()==u.target_generation);click(87,u.apply_point);
        if(frame==92){auto tab=app.find(target);check(!u.open&&!u.held_artwork&&!app.asm_ui.open&&tab->art->count()==count+3&&tab->art->palette_count()==palettes+added_palettes&&tab->art->dirty()&&tab->art->can_undo()&&std::string(tab->art->sprite(count)->n_s)=="CP_SENTINEL1"&&app.sprite_clipboard==clipboard);}
        if(frame==93)app.action(Action::Undo);if(frame==98)check(app.find(target)->art->count()==count&&app.find(target)->art->palette_count()==palettes&&!app.find(target)->art->dirty());if(frame==99)app.action(Action::Redo);
        if(frame==104)check(app.find(target)->art->count()==count+3&&app.find(target)->art->dirty());
        if(frame==105){auto tab=app.find(target);check(tab->art->save(output,app.error));auto loaded=Artwork::open(output,app.error);check(loaded&&loaded->count()==count+3&&loaded->disk_version()==0x634&&std::string(loaded->sprite(count+2)->n_s)=="CP_FOREIGN1");}
        if(frame==109)app.find(first)->art->undo();
        if(frame==117){check(!app.find(first)->art->dirty()&&!app.find(target)->art->dirty()&&read(ap)==a&&read(bp)==b&&read(tp)==t&&read(source_path)==source&&app.sprite_clipboard==clipboard&&!u.open);point(ImVec2(5,5));}
    }
};
