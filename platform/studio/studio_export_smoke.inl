struct ExportSmoke {
    int failures=0;uint64_t generation=0;std::string folder,table,irw,list,changed,final;std::vector<unsigned char> table_bytes,irw_bytes,list_bytes;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();if(!t){++failures;return;}auto &u=app.exporting;auto &art=*t->art;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio export UI check failed at frame %d: %s / %s; open=%d valid=%d ready=%d kind=%d included=%d blocked=%d depth=%d\n",frame,app.error.c_str(),u.error.c_str(),u.open,u.valid,u.plan.ready,(int)u.options.kind,u.plan.included,u.plan.blocked,u.options.bpp);}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};auto read=[&](const std::string &path){std::ifstream f(fs::u8path(path),std::ios::binary);return std::vector<unsigned char>(std::istreambuf_iterator<char>(f),{});};auto path=[&](const std::string &file){snprintf(u.path,sizeof u.path,"%s",file.c_str());u.build=true;};
        if(frame==0){auto dir=fs::u8path(app.prefs)/"export-smoke";fs::create_directories(dir);folder=dir.u8string();table=(dir/"review.TBL").u8string();irw=(dir/"review.IRW").u8string();list=(dir/"cancel.ASM").u8string();changed=(dir/"changed.ASM").u8string();final=(dir/"final.TBL").u8string();for(auto &file:{table,irw,list,final}){std::error_code ec;fs::remove(fs::u8path(file),ec);}check(art.save((dir/"source.IMG").u8string(),app.error));generation=art.generation();app.mark_sprite(*t,art.sprite_id(0),true);app.mark_sprite(*t,art.sprite_id(1),true);io.AddFocusEvent(true);}
        if(frame==3){app.action(Action::ExportTbl);path(table);}
        if(frame==6){check(u.open&&u.valid&&u.plan.ready&&u.destination_valid&&u.plan.included==2&&!art.dirty());point(u.mk3_point);}if(frame==7)io.AddMouseButtonEvent(0,true);if(frame==8)io.AddMouseButtonEvent(0,false);
        if(frame==10){check(u.options.mk3&&!u.valid);point(u.build_point);}if(frame==11)io.AddMouseButtonEvent(0,true);if(frame==12)io.AddMouseButtonEvent(0,false);
        if(frame==14){check(u.valid&&u.plan.ready&&u.lines.size()>8);point(u.row_points[1]);}if(frame==15)io.AddMouseButtonEvent(0,true);if(frame==16)io.AddMouseButtonEvent(0,false);
        if(frame==18){check(u.valid&&u.plan.included==1&&!u.plan.rows[1].enabled&&app.mark_count(*t)==2);point(u.bpp_min_point);}if(frame==19)io.AddMouseButtonEvent(0,true);if(frame==20)io.AddMouseButtonEvent(0,false);
        if(frame==22){check(u.options.bpp==1&&!u.valid);point(u.build_point);}if(frame==23)io.AddMouseButtonEvent(0,true);if(frame==24)io.AddMouseButtonEvent(0,false);
        if(frame==26){check(u.valid&&!u.plan.ready&&u.plan.blocked==1&&!fs::exists(fs::u8path(table)));point(u.depth_point);}if(frame==27)io.AddMouseButtonEvent(0,true);if(frame==28)io.AddMouseButtonEvent(0,false);
        if(frame==30)point(u.depth_choices[0]);if(frame==31)io.AddMouseButtonEvent(0,true);if(frame==32)io.AddMouseButtonEvent(0,false);
        if(frame==34){check(u.options.bpp==-1&&!u.valid);point(u.build_point);}if(frame==35)io.AddMouseButtonEvent(0,true);if(frame==36)io.AddMouseButtonEvent(0,false);
        if(frame==38){check(u.valid&&u.plan.ready&&u.plan.included==1&&u.plan.rows[0].bpp==4&&!art.dirty());table_bytes=u.plan.bytes;point(u.apply_point);}if(frame==39)io.AddMouseButtonEvent(0,true);if(frame==40)io.AddMouseButtonEvent(0,false);
        if(frame==42){check(!u.open&&read(table)==table_bytes&&art.generation()==generation&&!art.dirty()&&t->export_initialized);app.action(Action::ExportIrw);path(irw);}
        if(frame==45){check(u.valid&&u.plan.ready&&u.plan.included==1&&u.options.bpp==-1&&u.plan.rows[0].bpp==4&&u.plan.packed_bytes==1536);irw_bytes=u.plan.bytes;point(u.apply_point);}if(frame==46)io.AddMouseButtonEvent(0,true);if(frame==47)io.AddMouseButtonEvent(0,false);
        if(frame==49){check(!u.open&&read(irw)==irw_bytes&&art.generation()==generation&&!art.dirty());app.action(Action::ExportAnilst);path(list);}
        if(frame==52){check(u.valid&&u.plan.ready&&u.plan.included==1&&u.options.kind==ExportKind::Anilst);point(u.scope_point);}if(frame==53)io.AddMouseButtonEvent(0,true);if(frame==54)io.AddMouseButtonEvent(0,false);
        if(frame==56)point(u.scope_choices[2]);if(frame==57)io.AddMouseButtonEvent(0,true);if(frame==58)io.AddMouseButtonEvent(0,false);
        if(frame==60){check(u.scope==2&&!u.valid);point(u.build_point);}if(frame==61)io.AddMouseButtonEvent(0,true);if(frame==62)io.AddMouseButtonEvent(0,false);
        if(frame==64){check(u.valid&&u.plan.ready&&u.plan.included==8&&u.lines.size()==9);point(u.cancel_point);}if(frame==65)io.AddMouseButtonEvent(0,true);if(frame==66)io.AddMouseButtonEvent(0,false);
        if(frame==68){check(!u.open&&!fs::exists(fs::u8path(list))&&art.generation()==generation&&!art.dirty());{std::ofstream f(fs::u8path(changed));f<<"old";}app.action(Action::ExportAnilst);path(changed);}
        if(frame==71){check(u.valid&&u.plan.ready&&u.plan.included==1&&u.destination.existed);list_bytes=u.plan.bytes;{std::ofstream f(fs::u8path(changed));f<<"external";}point(u.apply_point);}if(frame==72)io.AddMouseButtonEvent(0,true);if(frame==73)io.AddMouseButtonEvent(0,false);
        if(frame==75){check(u.open&&!u.error.empty()&&read(changed)==std::vector<unsigned char>({'e','x','t','e','r','n','a','l'}));point(u.build_point);}if(frame==76)io.AddMouseButtonEvent(0,true);if(frame==77)io.AddMouseButtonEvent(0,false);
        if(frame==79){check(u.valid&&u.destination_valid&&u.error.empty());point(u.apply_point);}if(frame==80)io.AddMouseButtonEvent(0,true);if(frame==81)io.AddMouseButtonEvent(0,false);
        if(frame==83){check(!u.open&&read(changed)==list_bytes&&art.generation()==generation&&app.mark_count(*t)==2&&!art.dirty());app.action(Action::ExportTbl);path(final);}
        if(frame==86){check(u.valid&&u.plan.ready&&u.plan.included==1&&u.options.mk3);art.color(0,8,0x03e0);art.commit();point(u.apply_point);}if(frame==87)io.AddMouseButtonEvent(0,true);if(frame==88)io.AddMouseButtonEvent(0,false);
        if(frame==90){check(u.open&&u.plan.generation!=art.generation()&&!fs::exists(fs::u8path(final)));art.undo();point(u.build_point);}if(frame==91)io.AddMouseButtonEvent(0,true);if(frame==92)io.AddMouseButtonEvent(0,false);
        if(frame==95){check(u.valid&&u.plan.ready&&u.destination_valid&&u.plan.generation==art.generation()&&!art.dirty()&&app.mark_count(*t)==2&&!fs::exists(fs::u8path(final)));point(ImVec2(5,5));}
    }
};
