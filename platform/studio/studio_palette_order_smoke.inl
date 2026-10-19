struct PaletteOrderSmoke {
    int failures=0,expected_count=0;uint64_t generation=0;
    std::string path;std::vector<unsigned char> original,pixels,rgba;std::vector<unsigned short> sorted;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();auto &art=*t->art;auto &u=app.palettes;
        auto check=[&](bool ok,const char *label){if(!ok){++failures;fprintf(stderr,"Palette order UI failure %d (%s): %s / %s\n",frame,label,u.error.c_str(),u.usage_error.c_str());}};
        auto click=[&](int at,ImVec2 p){if(frame==at-1)io.AddMousePosEvent(p.x,p.y);if(frame==at)io.AddMouseButtonEvent(0,true);if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto key=[&](int at,ImGuiKey k){if(frame==at||frame==at+1){bool down=frame==at;io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);}};
        auto read=[&](const std::string &file){std::ifstream in(fs::u8path(file),std::ios::binary);return std::vector<unsigned char>{std::istreambuf_iterator<char>(in),{}};};
        if(frame==0){auto colors=copy_palette(art,0).words;std::vector<unsigned short> sparse(64);int slots[]={0,27,4,29,6,31,8,33,10,35,12,37,14,39,16,41};for(int i=0;i<16;++i)sparse[slots[i]]=colors[i];check(art.add_palette("SCATTER",sparse,app.error)==1,"fixture palette");
            for(int s=0;s<art.count();++s){check(art.assign_palette(s,1,false,app.error),"fixture assignment");auto im=art.sprite(s);auto data=(unsigned char *)im->data_p;int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x)data[y*stride+x]=(unsigned char)slots[data[y*stride+x]];}
            t->ppp=5;pixels=art.copy(0,0,0,48,64).pixels;app.clipboard=art.copy(0,0,0,48,64);app.palette_clipboard=copy_palette(art,0);path=(fs::u8path(app.prefs)/"order-original.IMG").u8string();check(art.save(path,app.error),"fixture save");original=read(path);generation=art.generation();app.open_palettes(*t);io.AddFocusEvent(true);io.AddMousePosEvent(5,5);}
        click(4,u.usage_point);
        if(frame==8)check(u.usage_mode&&u.usage_valid&&u.usage_options.order==PaletteColorOrder::Keep&&!art.dirty(),"open usage review");
        click(10,u.order_point);click(14,u.order_choices[2]);
        if(frame==18){check(u.usage_valid&&u.usage_options.order==PaletteColorOrder::ColorFamilies&&u.usage_plan.removed>40&&u.usage_plan.index_changes>0&&u.usage_plan.ppp_changed&&art.generation()==generation&&!art.dirty(),"group colors is a read-only review");rgba=render_palette_compaction(art,u.usage_plan,0,false);for(int i=0;i<art.count();++i)check(render_palette_compaction(art,u.usage_plan,i,false)==render_palette_compaction(art,u.usage_plan,i,true),"identical appearance across all users");}
        click(20,u.depth_point);
        if(frame==24){check(u.usage_plan.depth<=4&&art.palette(1)->bitspix==6,"declared depth is reviewed separately");sorted=u.usage_plan.words;expected_count=(int)sorted.size();}
        click(26,u.apply_point);
        if(frame==30)check(!u.usage_mode&&art.dirty()&&art.palette(1)->numc==expected_count&&art.palette(1)->bitspix<=4&&copy_palette(art,1).words==sorted&&art.sprite(0)->palnum==1,"apply organization");key(32,ImGuiKey_Z);
        if(frame==36)check(!art.dirty()&&art.palette(1)->numc==64&&art.copy(0,0,0,48,64).pixels==pixels,"one Undo restores original slots");key(38,ImGuiKey_Y);
        if(frame==42)check(art.dirty()&&copy_palette(art,1).words==sorted,"Redo");key(44,ImGuiKey_Z);
        if(frame==48)app.start_palette_usage(*t);click(52,u.order_point);click(56,u.order_choices[1]);
        if(frame==60)check(u.usage_valid&&u.usage_options.order==PaletteColorOrder::DarkToLight&&render_palette_compaction(art,u.usage_plan,0,true)==rgba&&!art.dirty(),"dark-to-light preview");click(62,u.cancel_usage_point);
        if(frame==66)check(!u.usage_mode&&!art.dirty()&&art.palette(1)->numc==64,"cancel keeps original");
        if(frame==68)app.start_palette_usage(*t);click(72,u.order_point);click(76,u.order_choices[2]);
        if(frame==80){art.color(1,1,0x1234);art.commit();}
        if(frame==84)check(!u.usage_valid&&!u.usage_error.empty(),"source revision invalidates review");click(86,u.apply_point);
        if(frame==90)check(u.usage_mode&&art.palette(1)->numc==64,"stale Apply disabled");click(92,u.cancel_usage_point);
        if(frame==96){art.undo();check(!art.dirty(),"restore source");app.start_palette_usage(*t);}click(100,u.cancel_usage_point);
        if(frame==104){auto im=art.sprite(0);im->opaltbl_p=malloc(16);memset(im->opaltbl_p,0xff,16);((unsigned char *)im->opaltbl_p)[0]=1;app.start_palette_usage(*t);}
        click(108,u.order_point);
        if(frame==112)check(u.usage_valid&&u.usage_plan.indices_locked&&u.usage_options.order==PaletteColorOrder::Keep&&!u.usage_plan.index_changes,"alternate users lock color order");click(114,u.cancel_usage_point);
        if(frame==118){free(art.sprite(0)->opaltbl_p);art.sprite(0)->opaltbl_p=nullptr;check(app.clipboard.pixels==pixels&&app.palette_clipboard.words==copy_palette(art,0).words,"independent clipboards");app.start_palette_usage(*t);}
        click(122,u.order_point);click(126,u.order_choices[2]);click(132,u.apply_point);
        if(frame==136){auto file=(fs::u8path(app.prefs)/"order-result.IMG").u8string();check(!u.usage_mode&&art.save(file,app.error),"save organized colors");auto disk=Artwork::open(file,app.error);check(disk&&disk->disk_version()==0x634&&disk->palette(1)->numc==expected_count,"same-format reopen");if(disk){PaletteCompactionPlan check_plan;check(plan_palette_compaction(*disk,{1,5},check_plan,app.error)&&render_palette_compaction(*disk,check_plan,0,false)==rgba,"reopened appearance");}art.undo();check(art.save(path,app.error)&&read(path)==original,"Undo saves exact original bytes");app.palette_reset(*t,1);app.start_palette_usage(*t);}
        click(142,u.order_point);click(146,u.order_choices[2]);
        if(frame==154){check(u.usage_valid&&u.usage_options.order==PaletteColorOrder::ColorFamilies&&!art.dirty()&&u.error.empty()&&app.error.empty(),"final organization review");io.AddMousePosEvent(5,5);}
    }
};
