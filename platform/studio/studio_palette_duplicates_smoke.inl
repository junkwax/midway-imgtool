struct PaletteDuplicatesSmoke {
    int failures=0;uint64_t generation=0,selection=0;
    std::string path;std::vector<unsigned char> original,pixels;std::vector<unsigned short> colors;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();auto &art=*t->art;auto &u=app.cleanup;
        auto check=[&](bool ok,const char *label){if(!ok){++failures;fprintf(stderr,"Duplicate palette UI failure %d (%s): %s / %s\n",frame,label,u.error.c_str(),app.error.c_str());}};
        auto click=[&](int at,ImVec2 p){if(frame==at-1)io.AddMousePosEvent(p.x,p.y);if(frame==at)io.AddMouseButtonEvent(0,true);if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto key=[&](int at,ImGuiKey k){if(frame==at||frame==at+1){bool down=frame==at;io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);}};
        auto read=[&](const std::string &file){std::ifstream in(fs::u8path(file),std::ios::binary);return std::vector<unsigned char>{std::istreambuf_iterator<char>(in),{}};};
        if(frame==0){colors=copy_palette(art,0).words;auto other=colors,high=colors;other[8]^=0x1f;high[8]^=0x8000;
            for(auto words:{colors,colors,colors,other,other,high}){int n=art.palette_count();check(art.add_palette("PAL"+std::to_string(n),words,app.error)==n,"fixture palette");}
            art.palette(1)->bitspix=5;art.palette(0)->file_spare=0x7331;art.palette(2)->file_spare=0x1234;
            check(art.assign_palette(1,2,false,app.error)&&art.assign_palette(2,5,false,app.error)&&art.assign_palette(3,6,false,app.error),"fixture references");
            auto im=art.sprite(0);im->opaltbl_p=malloc(16);memset(im->opaltbl_p,0xff,16);auto refs=(unsigned char *)im->opaltbl_p;refs[0]=6;refs[1]=1;refs[15]=5;
            selection=art.selection_id();pixels=art.copy(1,0,0,48,64).pixels;app.clipboard=art.copy(1,0,0,48,64);app.palette_clipboard=copy_palette(art,0);
            path=(fs::u8path(app.prefs)/"duplicates-original.IMG").u8string();check(art.save(path,app.error),"fixture save");original=read(path);generation=art.generation();app.action(Action::PaletteDuplicates);io.AddFocusEvent(true);io.AddMousePosEvent(5,5);}
        if(frame==4)check(u.open&&u.duplicates&&u.valid&&u.plan.removed==3&&u.plan.rows[2].match==0&&u.plan.rows[3].match==0&&u.plan.rows[5].match==4&&u.plan.rows[1].match==1&&u.plan.rows[6].match==6&&art.generation()==generation&&!art.dirty(),"exact matches and read-only review");
        click(6,u.row_points[2]);
        if(frame==10)check(u.keep.count(2)&&u.plan.removed==2&&!u.plan.rows[2].remove,"keep used duplicate");click(12,u.filter_point);
        if(frame==16)check(!u.only_unused,"show all palettes");click(18,u.row_points[0]);
        if(frame==22)check(!u.plan.rows[0].remove&&u.plan.removed==2,"first match stays protected");click(24,u.name_points[5]);
        if(frame==28)check(u.selected==5&&u.plan.rows[5].match==4,"inspect duplicate survivor");click(30,u.apply_point);
        if(frame==34)check(!u.open&&art.palette_count()==5&&art.sprite(1)->palnum==2&&art.sprite(2)->palnum==3&&art.sprite(3)->palnum==4&&((unsigned char *)art.sprite(0)->opaltbl_p)[0]==3&&((unsigned char *)art.sprite(0)->opaltbl_p)[1]==1&&((unsigned char *)art.sprite(0)->opaltbl_p)[15]==5&&art.selection_id()==selection&&art.copy(1,0,0,48,64).pixels==pixels&&art.copy(1,0,0,48,64).colors==colors&&art.dirty(),"apply remaps primary and alternate references exactly");
        key(36,ImGuiKey_Z);
        if(frame==40)check(art.palette_count()==7&&art.sprite(1)->palnum==2&&((unsigned char *)art.sprite(0)->opaltbl_p)[0]==6&&!art.dirty(),"one Undo restores references");key(42,ImGuiKey_Y);
        if(frame==46)check(art.palette_count()==5&&art.dirty(),"Redo");
        if(frame==48){auto file=(fs::u8path(app.prefs)/"duplicates-result.IMG").u8string();check(art.save(file,app.error),"save result");auto disk=Artwork::open(file,app.error);check(disk&&disk->disk_version()==0x634&&disk->palette_count()==5&&disk->sprite(2)->palnum==3&&((unsigned char *)disk->sprite(0)->opaltbl_p)[0]==3&&disk->copy(1,0,0,48,64).colors==colors,"same-format reopen");}
        if(frame==50){art.undo();check(art.save(path,app.error)&&read(path)==original,"exact Undo save");app.action(Action::PaletteDuplicates);}
        click(54,u.none_point);
        if(frame==58){check(u.valid&&!u.plan.removed,"keep all");generation=art.generation();}click(60,u.apply_point);
        if(frame==64)check(u.open&&art.generation()==generation&&!art.dirty(),"no-op Apply disabled");click(66,u.all_point);
        if(frame==70)check(u.plan.removed==3,"select all duplicates");click(72,u.cancel_point);
        if(frame==76)check(!u.open&&!art.dirty()&&art.palette_count()==7,"Cancel changes no data");
        if(frame==78)app.action(Action::PaletteDuplicates);
        if(frame==82){art.color(4,8,0x03e0);art.commit();}click(86,u.apply_point);
        if(frame==90)check(u.open&&art.palette_count()==7&&art.generation()!=u.generation,"stale review cannot apply");click(92,u.cancel_point);
        if(frame==96){art.undo();check(!art.dirty()&&read(path)==original&&app.clipboard.pixels==pixels&&app.palette_clipboard.words==colors,"clipboard and file isolation");app.action(Action::PaletteDuplicates);u.only_unused=false;u.selected=3;}
        if(frame==104){check(u.open&&u.valid&&u.plan.removed==3&&u.error.empty()&&app.error.empty()&&!art.dirty(),"final review health");io.AddMousePosEvent(5,5);}
    }
};
