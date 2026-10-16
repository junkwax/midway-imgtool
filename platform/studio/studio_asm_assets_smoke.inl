struct AsmAssetsSmoke {
    int failures=0;uint64_t first=0,second=0,generation=0,other_generation=0;std::string path,art_path,other_path,bytes,other_bytes,source;
    void prepare(App &app,ImGuiIO &io,int frame){auto &u=app.asm_ui;auto &v=u.assets;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"ASM artwork UI failure at %d: %s / %s (open=%d valid=%d selected=%d resolved=%d missing=%d ambiguous=%d stale=%d)\n",frame,u.error.c_str(),v.error.c_str(),v.open,v.valid,v.selected,v.review.resolved,v.review.missing,v.review.ambiguous,v.review.stale);}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};
        auto click=[&](int at,ImVec2 p){if(frame==at){point(p);io.AddMouseButtonEvent(0,true);}if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto read=[](const std::string &p){std::ifstream f(fs::u8path(p),std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});};
        if(frame==0){io.AddFocusEvent(true);auto t=app.tab();first=t->uid;
            art_path=(fs::u8path(app.prefs)/"artwork-first.IMG").u8string();check(t->art->save(art_path,app.error));bytes=read(art_path);generation=t->art->generation();
            app.demo();t=app.tab();second=t->uid;other_path=(fs::u8path(app.prefs)/"artwork-other.IMG").u8string();check(t->art->save(other_path,app.error));other_bytes=read(other_path);other_generation=t->art->generation();
            path=(fs::u8path(app.prefs)/"artwork-review.ASM").u8string();source="a_review\n\t.long GROUP,SENTINEL2,SENTINEL3,0\nGROUP\n\t.long SENTINEL1,SENTINEL1,MISSING,0\n";
            {std::ofstream f(fs::u8path(path),std::ios::binary);f<<source;}check(app.load_asm(path));app.show_asm();u.preferred=0;
        }
        click(4,v.show_point);
        if(frame==9)check(v.open&&v.valid&&v.review.rows.size()==4&&v.review.ambiguous==3&&v.review.missing==1&&v.review.rows[0].uses==2);
        click(10,v.choice_point);click(14,v.choice_points[1]);
        if(frame==19){check(v.review.resolved==1&&v.review.rows[0].resolved.doc==second&&v.choices.size()==1);app.asm_resolve();check(u.pieces[0].doc==second&&u.pieces[1].doc==second);}
        click(20,v.row_points[2]);if(frame==25)check(v.selected==2&&v.review.rows[2].first_frame==1);click(26,v.frame_point);
        if(frame==31)check(u.player.frame==1&&!v.open);click(32,v.show_point);
        click(38,v.choice_point);click(42,v.choice_points[0]);if(frame==47){app.asm_resolve();check(v.review.resolved==2&&u.pieces[0].doc==first);}
        click(48,v.copy_point);if(frame==53){std::string copied=ImGui::GetClipboardText();copied.erase(std::remove(copied.begin(),copied.end(),'\r'),copied.end());check(copied=="MISSING\nSENTINEL3");}
        click(54,v.unresolved_point);if(frame==59)check(v.unresolved);click(60,v.row_points[0]);if(frame==65)check(v.selected==1);
        click(66,v.filter_point);if(frame==70)io.AddInputCharactersUTF8("MISSING");if(frame==74)check(v.filter.PassFilter("MISSING")&&!v.filter.PassFilter("SENTINEL3"));
        if(frame==77){v.filter.Clear();v.unresolved=false;v.selected=0;}
        if(frame==80){auto t=app.find(second);check(t&&t->art->generation()==other_generation&&!t->art->dirty());app.tabs.pop_back();app.active=0;app.select_uid=first;}
        if(frame==84){check(v.valid&&v.review.stale==1&&!v.review.rows[0].resolved.sprite&&v.review.rows[0].candidates.size()==1);u.player.seek(0,3);app.asm_resolve();check(u.pieces[0].stale&&!u.pieces[0].sprite);app.open(other_path);check(app.tabs.size()==2&&app.error.empty());}
        if(frame==88)check(v.review.stale==1&&v.review.rows[0].candidates.size()==2&&app.tab()->uid!=second);
        click(89,v.choice_point);click(93,v.auto_point);if(frame==98)check(v.review.stale==0&&v.review.ambiguous==2&&v.choices.size()==1);
        click(99,v.choice_point);click(103,v.choice_points[0]);if(frame==108)check(v.review.rows[0].resolved.doc==first&&v.review.resolved==2);click(109,v.open_point);
        if(frame==114){check(!v.open&&!u.open&&app.tab()->uid==first&&app.tab()->selected_id==app.tab()->art->sprite_id(0));app.show_reactions();u.preferred=0;u.reactions.show_all=true;app.reaction_choose_visible();}
        if(frame==118){app.reaction_refresh_anchors();check(u.reactions.anchors.rows.size()==5&&u.reactions.anchors.rows[0].reference.doc==first&&u.reactions.anchors.rows[3].reference.doc==first);}
        click(119,v.show_point);if(frame==124)check(v.open&&v.valid&&v.review.resolved==2&&v.choices.size()==2);click(125,v.close_point);
        if(frame==130){check(!v.open);auto before=u.source.get();check(!app.load_asm(path+".missing")&&u.source.get()==before&&v.choices.size()==2);u.error.clear();check(app.load_asm(path)&&v.choices.empty());}
        click(134,v.show_point);
        if(frame==139)check(v.open&&v.valid&&v.review.ambiguous==3&&v.review.missing==1&&v.choices.empty());
        if(frame==159){auto t=app.find(first);check(t&&t->art->generation()==generation&&!t->art->dirty()&&read(art_path)==bytes&&read(other_path)==other_bytes&&read(path)==source);point(ImVec2(5,5));}
    }
};
