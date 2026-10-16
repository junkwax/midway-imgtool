struct ReactionsSmoke {
    int failures=0;bool real=false;uint64_t uid=0,generation=0,sprite=0,duplicate_uid=0;
    std::string source_path,image_path,original_text;std::vector<unsigned char> source_bytes,image_bytes,pixel_clip;
    std::vector<unsigned short> palette_clip;
    void prepare(App &app,ImGuiIO &io,int frame){auto &u=app.asm_ui;auto &r=u.reactions;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Reactions UI failure at %d: %s / %s / %s (mode=%d selected=%d visible=%zu anchors=%zu)\n",frame,app.error.c_str(),u.error.c_str(),r.error.c_str(),u.reaction_mode,u.selected,r.visible.size(),r.anchors.rows.size());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};
        auto click=[&](int at,ImVec2 p){if(frame==at){point(p);io.AddMouseButtonEvent(0,true);}if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto read=[&](const std::string &file){std::ifstream f(fs::u8path(file),std::ios::binary);return std::vector<unsigned char>{std::istreambuf_iterator<char>(f),{}};};
        if(real){
            if(frame==0){io.AddFocusEvent(true);app.tabs.clear();app.active=-1;
                for(int i=1;i<=10;++i){auto path=fs::path("../mk2-main/data")/("CAGE"+std::to_string(i)+".IMG");if(fs::exists(path))app.open(path.u8string());}
                check(app.load_asm("../mk2-main/src/MKJC.ASM"));app.show_reactions();u.preferred=app.tabs.empty()?0:app.tabs[0]->uid;
                for(int i=0;u.source&&i<(int)u.source->animations.size();++i)if(u.source->animations[i].label=="a_jchithi")app.asm_select(i);
            }
            if(frame==4)check(u.open&&u.reaction_mode&&r.catalogue.size()>20&&r.visible.size()>10&&!r.anchors.rows.empty());
            if(frame==8){u.player.seek(1,(int)app.asm_animation()->frames.size());r.selected=1;}
            if(frame==13){for(auto &t:app.tabs)check(!t->art->dirty());point(ImVec2(5,5));}return;
        }
        auto t=frame?app.find(uid):app.tab();if(!t){++failures;return;}auto &art=*t->art;
        auto intact=[&](){return !art.dirty()&&read(source_path)==source_bytes&&read(image_path)==image_bytes&&app.clipboard.pixels==pixel_clip&&app.palette_clipboard.words==palette_clip;};
        if(frame==0){
            io.AddFocusEvent(true);uid=t->uid;for(int i=0;i<art.count();++i){art.anchor(i,10,60);art.commit();}sprite=art.sprite_id(0);
            image_path=(fs::u8path(app.prefs)/"reactions-source.IMG").u8string();check(art.save(image_path,app.error));image_bytes=read(image_path);generation=art.generation();
            original_text="demo_anitab\n\t.long a_attack ; hi punch\n\t.long a_jchithi ; 10 - hit high\n\t.long a_jc_fb_lk ; flipped by liu kang\n\t.long a_back_breaker ; back breaker\n\t.long a_torso_ripped ; torso ripped\n\t.long a_jckdown ; knocked down\na_attack\n\t.long SENTINEL1,0\na_jchithi\n\t.long GROUP\n\t.long ani_adjustxy\n\t.word 5,-3\n\t.long ani_flip,SENTINEL3\n\t.long ani_flip_v,SENTINEL2\n\t.long ani_calla,r_runtime\n\t.long 0\na_jc_fb_lk\n\t.long SENTINEL1,0\na_back_breaker\n\t.long SENTINEL1,0\na_torso_ripped\n\t.long SENTINEL1,0\na_jckdown\n\t.long SENTINEL1,0\nGROUP\n\t.long SENTINEL1,SENTINEL2,MISSING,0\n";
            source_path=(fs::u8path(app.prefs)/"reactions-preview.ASM").u8string();{std::ofstream out(fs::u8path(source_path),std::ios::binary);out<<original_text;}source_bytes=read(source_path);
            app.clipboard=art.copy(0,0,0,48,64);pixel_clip=app.clipboard.pixels;app.palette_clipboard=copy_palette(art,0);palette_clip=app.palette_clipboard.words;
            check(app.load_asm(source_path));app.show_reactions();
        }
        if(frame==3)check(u.open&&u.reaction_mode&&u.selected==1&&r.visible.size()==4&&r.anchors.rows.size()==5&&!r.anchors.rows[2].reference.sprite&&intact());
        click(4,u.next_point);if(frame==8)check(u.player.frame==1);click(9,r.row_points[0]);if(frame==13)check(u.player.frame==0&&r.selected==0);
        click(14,r.facing_point);if(frame==18)check(u.facing&&r.anchors.rows[0].x==-38&&intact()&&art.generation()==generation);
        click(19,r.open_point);if(frame==23)check(!u.open&&app.tab()->uid==uid&&app.tab()->selected_id==sprite&&!t->alignment.enabled&&!t->embedded.enabled&&t->anchors&&intact());
        if(frame==24){art.anchor(0,12,61);art.commit();app.show_reactions();}if(frame==28)check(r.anchors.rows[0].anix==12&&r.anchors.rows[0].aniy==61&&art.dirty()&&read(source_path)==source_bytes);
        if(frame==29)art.undo();if(frame==33)check(r.anchors.rows[0].anix==10&&intact());
        if(frame==34){app.demo();duplicate_uid=app.tab()->uid;u.preferred=0;}
        if(frame==38)check(r.anchors.rows[0].reference.ambiguous&&!r.anchors.rows[0].reference.sprite);
        if(frame==39)r.selected=0;click(40,r.open_point);if(frame==44)check(u.open&&app.tabs.size()==2);
        if(frame==45)u.preferred=uid;if(frame==49)check(r.anchors.rows[0].reference.doc==uid&&r.anchors.rows[0].reference.sprite==sprite);
        if(frame==50){check(app.tabs.back()->uid==duplicate_uid);app.tabs.pop_back();app.active=0;app.select_uid=uid;}
        click(55,r.search_point);if(frame==57)io.AddInputCharactersUTF8("liu kang");if(frame==61)check(r.visible.size()==1&&u.selected==2&&r.anchors.rows.size()==1);
        if(frame==62){io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);}if(frame==63){io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);io.AddInputCharactersUTF8("nomatch");}
        if(frame==67)check(r.visible.empty());
        if(frame==68){r.search[0]=0;app.reaction_choose_visible();ImGui::ClearActiveID();}if(frame==72)check(r.visible.size()==4);
        click(73,r.family_point);click(76,r.family_choices[(int)ReactionKind::Knockdown]);if(frame==80)check(r.family==(int)ReactionKind::Knockdown&&r.visible.size()==1&&u.selected==5);
        click(81,r.family_point);click(84,r.family_choices[0]);if(frame==88)check(r.family==-1&&r.visible.size()==4);
        click(89,r.show_all_point);if(frame==93){check(r.show_all&&r.visible.size()==6);snprintf(r.search,sizeof r.search,"breaker");app.reaction_choose_visible();}
        if(frame==97)check(u.selected==3&&r.visible.size()==1);click(98,r.show_all_point);if(frame==102)check(r.visible.empty());click(103,r.show_all_point);if(frame==107)check(r.visible.size()==1);
        if(frame==108){auto old=u.source.get();check(!app.load_asm(source_path+".missing")&&u.source.get()==old);u.error.clear();
            auto replacement=original_text;size_t at=0;while((at=replacement.find("a_back_breaker",at))!=replacement.npos){replacement.replace(at,14,"a_jcstumble");at+=11;}
            auto path=(fs::u8path(app.prefs)/"reactions-reload.ASM").u8string();{std::ofstream out(fs::u8path(path),std::ios::binary);out<<replacement;}snprintf(r.search,sizeof r.search,"stumble");check(app.load_asm(path));
        }
        if(frame==112)check(r.catalogue.size()==6&&r.visible.size()==1&&u.source->animations[u.selected].label=="a_jcstumble");
        if(frame==113){r.search[0]=0;r.show_all=false;r.family=-1;check(app.load_asm(source_path));u.facing=false;}
        if(frame==117)check(art.delete_sprite(0,app.error));if(frame==121)check(!r.anchors.rows[0].reference.sprite&&read(image_path)==image_bytes);
        if(frame==122)art.undo();if(frame==126)check(r.anchors.rows[0].reference.sprite==sprite&&art.sprite_index(sprite)==0&&intact());
        if(frame==127){u.origin_x+=13;u.origin_y+=7;}if(frame==131)check(r.anchors.rows[0].x==-10&&r.anchors.rows[0].y==-60&&intact());
        click(132,r.row_points[1]);if(frame==136){check(r.selected==1&&u.player.frame==0&&r.anchors.rows[1].reference.sprite==art.sprite_id(1)&&intact());point(ImVec2(5,5));}
    }
};
