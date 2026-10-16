struct LibrarySplitAnimationsSmoke {
    int failures=0;uint64_t source_uid=0,result_uid=0,empty_uid=0,generation=0;
    std::vector<uint64_t> ids;std::string path;std::vector<unsigned char> original,pixel_clip;
    std::vector<unsigned short> palette_clip;
    const SpriteClipboard *sprite_guard=nullptr;const AnimationClipboard *animation_guard=nullptr;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto source=frame?app.find(source_uid):app.tab();if(!source){++failures;return;}
        auto &art=*source->art;auto &u=app.library_copy;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Library split animations UI failure at %d: %s / %s (open=%d valid=%d sprites=%zu sequences=%u scripts=%u)\n",frame,app.error.c_str(),u.error.c_str(),u.open,u.valid,u.plan.rows.size(),u.split_animation_plan.sequences,u.split_animation_plan.scripts);}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};
        auto click=[&](int at,ImVec2 p){if(frame==at){point(p);io.AddMouseButtonEvent(0,true);}if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto read=[&](const std::string &file){std::ifstream f(fs::u8path(file),std::ios::binary);return std::vector<unsigned char>{std::istreambuf_iterator<char>(f),{}};};
        auto activate=[&](uint64_t uid){for(int i=0;i<(int)app.tabs.size();++i)if(app.tabs[i]->uid==uid){app.active=i;app.select_uid=uid;}};
        auto clipboards=[&](){return app.sprite_clipboard.get()==sprite_guard&&app.animation_clipboard.get()==animation_guard&&app.clipboard.pixels==pixel_clip&&app.palette_clipboard.words==palette_clip;};
        if(frame==0){
            source_uid=source->uid;io.AddFocusEvent(true);check(art.rename_sprite(1,"HERO1",app.error)&&art.rename_sprite(2,"HERO1A",app.error));
            for(int i=0;i<art.count();++i)ids.push_back(art.sprite_id(i));
            check(art.embedded_add_record(false,"PARTIAL",app.error)==0&&art.embedded_append(0,1,app.error)&&art.embedded_append(0,4,app.error));
            check(art.embedded_add_record(false,"FULL",app.error)==1&&art.embedded_append(1,1,app.error)&&art.embedded_append(1,2,app.error));
            check(art.embedded_add_record(false,"EMPTY",app.error)==2);
            check(art.embedded_add_record(false,"DAMAGE",app.error)==3&&art.embedded_append(3,1,app.error));
            check(art.embedded_add_record(false,"OTHER",app.error)==4&&art.embedded_append(4,1,app.error));
            check(art.embedded_add_record(true,"COMPLETE",app.error)==5&&art.embedded_append(5,1,app.error)&&art.embedded_append(5,4,app.error));
            check(art.embedded_add_record(true,"MIXED",app.error)==6&&art.embedded_append(6,0,app.error));
            // DAMAGE follows PARTIAL(2 entries), FULL(2), EMPTY(0).
            ((unsigned char *)art.data.scrseqmem_p)[3*98+4*18+88]=0;
            path=(fs::u8path(app.prefs)/"sprite-animation-split-source.IMG").u8string();check(art.save(path,app.error));original=read(path);generation=art.generation();
            auto empty=std::make_unique<Artwork>();LibraryCopyPlan sc;check(plan_library_copy(art,*empty,{ids[1]},{},sc,app.error));app.sprite_clipboard=SpriteClipboard::capture(art,sc,"guard",app.error);sprite_guard=app.sprite_clipboard.get();
            auto staging=art.split_target();AnimationCopyOptions options;options.copy_missing=true;options.requested={1};AnimationCopyPlan ac;check(plan_animation_copy(art,*staging,options,ac,app.error));app.animation_clipboard=AnimationClipboard::capture(art,ac,"guard",app.error);animation_guard=app.animation_clipboard.get();check(sprite_guard&&animation_guard);
            app.clipboard=art.copy(1,0,0,48,64);pixel_clip=app.clipboard.pixels;app.palette_clipboard=copy_palette(art,0);palette_clip=app.palette_clipboard.words;
            app.mark_sprite(*source,ids[1],true);app.action(Action::SplitLibrary);
        }
        if(frame==3)check(u.open&&u.valid&&!u.split_animations&&!u.move_source&&u.plan.rows.size()==1);
        click(4,u.animations_point);if(frame==8)check(u.valid&&u.split_animations&&u.split_animation_plan.sequences==1&&u.split_animation_plan.scripts==0);
        click(9,u.children_point);if(frame==13)check(u.valid&&u.plan.rows.size()==2&&u.split_animation_plan.sequences==2&&u.split_animation_plan.scripts==1&&!u.split_animation_plan.rows[3].included);
        click(14,u.animation_details_point);click(19,u.row_points[1]);if(frame==23)check(u.valid&&!u.plan.rows[1].enabled&&u.split_animation_plan.sequences==1&&u.split_animation_plan.scripts==0);
        click(24,u.row_points[1]);if(frame==28)check(u.valid&&u.split_animation_plan.sequences==2&&u.split_animation_plan.scripts==1);
        click(29,u.move_point);if(frame==33)check(!u.move_source&&u.split_animations);click(34,u.cancel_point);
        if(frame==38)check(!u.open&&app.tabs.size()==1&&!art.dirty()&&art.generation()==generation&&read(path)==original&&clipboards());
        if(frame==39)app.action(Action::SplitLibrary);click(43,u.children_point);click(47,u.animations_point);
        if(frame==51)check(u.valid&&u.split_animations&&u.split_animation_plan.sequences==2&&u.split_animation_plan.scripts==1);click(52,u.apply_point);
        if(frame==56){
            auto next=app.tab();result_uid=next->uid;std::vector<EmbeddedRecord> records;
            check(!u.open&&result_uid!=source_uid&&app.tabs.size()==2&&next->art->count()==2&&next->art->dirty()&&!next->art->can_undo()&&next->art->path().empty()&&next->art->disk_version()==art.disk_version()&&read_embedded(*next->art,records,app.error)&&records.size()==3&&records[0].name=="FULL"&&records[0].entries[0].values.index==0&&records[0].entries[1].values.index==1&&records[1].name=="OTHER"&&records[2].name=="COMPLETE"&&records[2].entries[0].values.index==0&&records[2].entries[1].values.index==1&&art.count()==8&&!art.dirty()&&art.generation()==generation&&clipboards());
            app.embedded_workspace(*next);
        }
        if(frame==57)app.action(Action::Close);if(frame==61)check(app.close_uid==result_uid&&app.find(result_uid));
        if(frame==62){
            auto next=app.find(result_uid);auto output=(fs::u8path(app.prefs)/"sprite-animation-split-result.IMG").u8string();check(next&&next->art->save(output,app.error));auto saved=read(output);auto reopened=Artwork::open(output,app.error);std::vector<EmbeddedRecord> records;check(reopened&&reopened->count()==2&&reopened->disk_version()==art.disk_version()&&read_embedded(*reopened,records,app.error)&&records.size()==3);
            next->art->paint(0,0,0,2);next->art->commit();check(next->art->can_undo());next->art->undo();check(next->art->save(output,app.error)&&read(output)==saved&&read(path)==original);
            app.close_uid=0;ImGui::ClosePopupToLevel(0,true);activate(source_uid);
        }
        if(frame==67)app.action(Action::SplitLibrary);click(71,u.animations_point);
        if(frame==75){art.color(0,8,0x03e0);art.commit();}click(76,u.apply_point);if(frame==80)check(u.open&&app.tabs.size()==2&&art.count()==8);click(81,u.cancel_point);
        if(frame==85){art.undo();check(art.save(path,app.error)&&read(path)==original);}
        if(frame==86)app.action(Action::SplitLibrary);click(90,u.animations_point);if(frame==94){u.scope=2;u.dirty=true;}
        if(frame==98)check(u.valid&&u.plan.rows.size()==8&&u.split_animation_plan.sequences==3&&u.split_animation_plan.scripts==2);click(100,u.cancel_point);
        if(frame==105){source->marks.clear();app.mark_sprite(*source,ids[6],true);app.action(Action::SplitLibrary);}click(109,u.animations_point);
        if(frame==113)check(u.valid&&u.split_animation_plan.sequences==0&&u.split_animation_plan.scripts==0&&u.split_animation_plan.rows.size()==7);click(114,u.apply_point);
        if(frame==118){auto next=app.tab();empty_uid=next->uid;check(!u.open&&next->art->count()==1&&next->art->data.seqcnt==0&&next->art->data.scrcnt==0&&next->art->dirty()&&!next->art->can_undo());check(next->art->save((fs::u8path(app.prefs)/"sprite-animation-split-no-records.IMG").u8string(),app.error));}
        if(frame==119)app.action(Action::Close);
        if(frame==123){check(!app.find(empty_uid));activate(source_uid);source->marks.clear();app.mark_sprite(*source,ids[1],true);app.action(Action::SplitLibrary);u.split_animations=true;u.dirty=true;}
        click(127,u.children_point);if(frame==131)check(u.valid&&u.split_animation_plan.sequences==2&&u.split_animation_plan.scripts==1);
        if(frame==136){check(read(path)==original&&!art.dirty()&&clipboards());point(ImVec2(5,5));}
    }
};
