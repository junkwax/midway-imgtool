struct AnimationDamageSmoke {
    int failures=0;uint64_t source_uid=0,target_uid=0,generation=0;
    std::string source_path,target_path;std::vector<unsigned char> source_bytes,target_bytes;
    void prepare(App &app,ImGuiIO &io,int frame){auto &u=app.animation_copy;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Animation damage UI failure at %d: %s / %s\n",frame,app.error.c_str(),u.error.c_str());}};
        auto activate=[&](uint64_t uid){for(int i=0;i<(int)app.tabs.size();++i)if(app.tabs[i]->uid==uid){app.active=i;app.select_uid=uid;}};
        auto click=[&](int at,ImVec2 p){if(frame==at){io.AddMousePosEvent(std::round(p.x),std::round(p.y));io.AddMouseButtonEvent(0,true);}if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto key=[&](ImGuiKey k,bool down){io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);};
        auto read=[](const std::string &path){std::ifstream f(fs::u8path(path),std::ios::binary);return std::vector<unsigned char>{std::istreambuf_iterator<char>(f),{}};};
        auto table=[](Artwork &art,const std::vector<unsigned char> &b){art.data.damtbl_p=malloc(b.size());memcpy(art.data.damtbl_p,b.data(),b.size());art.data.damtblbytes=(unsigned)b.size();art.data.damcnt=(unsigned)b.size()/4;};
        if(frame==0){auto source=app.tab();if(!source){++failures;return;}source_uid=source->uid;auto &art=*source->art;io.AddFocusEvent(true);
            check(art.embedded_add_record(false,"BASE",app.error)==0&&art.embedded_append(0,0,app.error)&&art.embedded_add_record(false,"DAMAGE",app.error)==1&&art.embedded_append(1,1,app.error)&&art.embedded_add_record(true,"REACT",app.error)==2&&art.embedded_append(2,1,app.error));
            table(art,{0,0,0,0,0x40,0,0,0});auto raw=(unsigned char *)art.data.scrseqmem_p;raw[88]=0;raw[116+88]=1;
            source_path=(fs::u8path(app.prefs)/"damage-source.IMG").u8string();check(art.save(source_path,app.error));source_bytes=read(source_path);generation=art.generation();source->embedded.enabled=true;source->embedded.record=0;
            app.demo();auto target=app.tab();target_uid=target->uid;check(target->art->embedded_add_record(false,"IDLE",app.error)==0&&target->art->embedded_append(0,2,app.error));table(*target->art,{0x40,0,0,0});((unsigned char *)target->art->data.scrseqmem_p)[88]=0;
            target_path=(fs::u8path(app.prefs)/"damage-target.IMG").u8string();check(target->art->save(target_path,app.error));target_bytes=read(target_path);activate(source_uid);app.action(Action::AnimationCopy);
        }
        auto target=app.find(target_uid);if(!target){++failures;return;}
        if(frame==3)check(u.open&&u.valid&&u.plan.added_sequences==2&&u.plan.added_scripts==1&&u.plan.rows[1].dependency&&u.plan.rows[2].dependency&&u.plan.damage_bindings.size()==2&&u.plan.result_damage.size()==12);click(4,u.cancel_point);
        if(frame==8){check(!u.open&&!target->art->dirty()&&target->art->data.damcnt==1);app.action(Action::AnimationCopy);}click(12,u.apply_point);
        if(frame==16){std::vector<EmbeddedRecord> r;check(!u.open&&target->art->data.seqcnt==3&&target->art->data.scrcnt==1&&target->art->data.damcnt==3&&read_embedded(*target->art,r,app.error)&&r[1].raw[88]==1&&r[2].raw[88]==2&&r[3].entries[0].values.index==2);}
        if(frame==18)key(ImGuiKey_Z,true);if(frame==19)key(ImGuiKey_Z,false);if(frame==22)check(target->art->data.seqcnt==1&&target->art->data.damcnt==1&&!target->art->dirty());
        if(frame==23)key(ImGuiKey_Y,true);if(frame==24)key(ImGuiKey_Y,false);if(frame==27)check(target->art->data.seqcnt==3&&target->art->data.damcnt==3);
        if(frame==28){auto path=(fs::u8path(app.prefs)/"damage-copied.IMG").u8string();check(target->art->save(path,app.error));auto loaded=Artwork::open(path,app.error);check(loaded&&loaded->disk_version()==0x634&&loaded->data.damcnt==3&&!memcmp(loaded->data.damtbl_p,target->art->data.damtbl_p,12));}
        if(frame==30){app.action(Action::Undo);check(target->art->save(target_path,app.error)&&read(target_path)==target_bytes);activate(source_uid);app.action(Action::SplitAnimations);}click(34,u.apply_point);
        if(frame==38){auto t=app.tab();check(!u.open&&t->uid!=source_uid&&t->uid!=target_uid&&t->art->count()==2&&t->art->data.seqcnt==2&&t->art->data.scrcnt==1&&t->art->data.damcnt==2&&!t->art->can_undo()&&t->art->save((fs::u8path(app.prefs)/"damage-split.IMG").u8string(),app.error));}
        if(frame==40){activate(source_uid);app.action(Action::CopyAnimations);}click(44,u.apply_point);
        if(frame==48){auto source=app.find(source_uid);check(!u.open&&app.animation_clipboard&&app.animation_clipboard->artwork().data.damcnt==2&&source&&source->art->generation()==generation&&!source->art->dirty()&&read(source_path)==source_bytes);app.action(Action::Close);}
        if(frame==52){check(!app.find(source_uid));activate(target_uid);app.action(Action::PasteAnimations);}click(56,u.apply_point);
        if(frame==60)check(!u.open&&target->art->data.damcnt==3&&target->art->data.seqcnt==3&&target->art->data.scrcnt==1);
        if(frame==62)key(ImGuiKey_Z,true);if(frame==63)key(ImGuiKey_Z,false);if(frame==66)check(target->art->data.damcnt==1&&!target->art->dirty());
        if(frame==67)key(ImGuiKey_Y,true);if(frame==68)key(ImGuiKey_Y,false);if(frame==72)check(target->art->data.damcnt==3);
        if(frame==73){auto path=(fs::u8path(app.prefs)/"damage-pasted.IMG").u8string();check(target->art->save(path,app.error));auto loaded=Artwork::open(path,app.error);check(loaded&&loaded->data.damcnt==3&&loaded->disk_version()==0x634&&read(source_path)==source_bytes&&read(target_path)==target_bytes);}
        if(frame==75){app.action(Action::Undo);check(target->art->save(target_path,app.error)&&read(target_path)==target_bytes);app.action(Action::PasteAnimations);snprintf(u.prefix,sizeof u.prefix,"CP_");u.dirty=true;}
        if(frame==79){check(u.open&&u.valid&&u.plan.damage_bindings.size()==2&&u.plan.rows[0].error.empty()&&u.plan.rows[2].error.empty());io.AddMousePosEvent(5,5);}
    }
};
