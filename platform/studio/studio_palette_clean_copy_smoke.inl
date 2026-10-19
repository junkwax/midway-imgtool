struct PaletteCleanCopySmoke {
    int failures=0,expected_count=0;uint64_t generation=0;void *layer=nullptr;
    std::string path;std::vector<unsigned char> original,pixels;std::vector<unsigned short> source,expected;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();auto &art=*t->art;auto &u=app.palettes;
        auto check=[&](bool ok,const char *label){if(!ok){++failures;fprintf(stderr,"Clean palette copy UI failure %d (%s): %s / %s\n",frame,label,u.error.c_str(),u.usage_error.c_str());}};
        auto click=[&](int at,ImVec2 p){if(frame==at-1)io.AddMousePosEvent(p.x,p.y);if(frame==at)io.AddMouseButtonEvent(0,true);if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto key=[&](int at,ImGuiKey k){if(frame==at||frame==at+1){bool down=frame==at;io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);}};
        auto read=[&](const std::string &file){std::ifstream in(fs::u8path(file),std::ios::binary);return std::vector<unsigned char>{std::istreambuf_iterator<char>(in),{}};};
        if(frame==0){auto colors=copy_palette(art,0).words;source.resize(64);int slots[]={0,27,4,29,6,31,8,33,10,35,12,37,14,39,16,41};for(int i=0;i<16;++i)source[slots[i]]=colors[i];source[55]=0x8421;check(art.add_palette("SCATTER",source,app.error)==1,"fixture palette");
            for(int i=0;i<art.count();++i){check(art.assign_palette(i,1,false,app.error),"fixture assignment");auto im=art.sprite(i);auto data=(unsigned char *)im->data_p;int stride=(im->w+3)&~3;for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x)data[y*stride+x]=(unsigned char)slots[data[y*stride+x]];}
            auto im=art.sprite(0);im->opaltbl_p=malloc(16);memset(im->opaltbl_p,0xff,16);((unsigned char *)im->opaltbl_p)[0]=4;((unsigned char *)im->opaltbl_p)[15]=4;
            auto clip=art.copy(0,0,0,2,2);clip.pixels={55,0,0,0};check(art.paste_layer(0,clip,60,0,app.error)&&art.edit_layer(0,60,0,false,false,false,app.error),"hidden off-canvas overlay");layer=im->layer_p;
            pixels=art.copy(0,0,0,48,64).pixels;app.clipboard=art.copy(0,0,0,48,64);app.palette_clipboard=copy_palette(art,0);path=(fs::u8path(app.prefs)/"clean-copy-original.IMG").u8string();check(art.save(path,app.error),"fixture save");original=read(path);generation=art.generation();app.open_palettes(*t);io.AddFocusEvent(true);io.AddMousePosEvent(5,5);}
        click(4,u.clean_copy_point);
        if(frame==8)check(u.usage_mode&&u.usage_valid&&u.usage_options.copy_only&&u.usage_plan.indices_locked&&u.usage_plan.mapping[55]&&u.usage_options.order==PaletteColorOrder::DarkToLight&&!art.dirty()&&art.generation()==generation,"copy review includes alternate and hidden overlay usage");
        click(10,u.order_point);click(14,u.order_choices[2]);
        if(frame==18)check(u.usage_valid&&u.usage_options.order==PaletteColorOrder::ColorFamilies&&render_palette_compaction(art,u.usage_plan,0,false)==render_palette_compaction(art,u.usage_plan,0,true),"copy can sort alternate users without edits");click(20,u.depth_point);
        if(frame==24)check(u.usage_plan.depth<6&&art.palette(1)->bitspix==6,"copy-only depth option");click(26,u.cancel_usage_point);
        if(frame==30)check(!u.usage_mode&&!art.dirty()&&art.palette_count()==2,"Cancel changes no source data");click(32,u.clean_copy_point);
        if(frame==36){check(u.usage_valid&&u.usage_plan.depth==6,"fresh copy keeps source depth by default");expected=u.usage_plan.words;expected_count=(int)expected.size();}
        click(38,u.apply_point);
        if(frame==42)check(!u.usage_mode&&u.index==2&&art.palette_count()==3&&palette_name(*art.palette(2))=="SCATTER_2"&&copy_palette(art,2).words==expected&&art.palette(2)->bitspix==6&&copy_palette(art,1).words==source&&art.copy(0,0,0,48,64).pixels==pixels&&art.sprite(0)->palnum==1&&((unsigned char *)art.sprite(0)->opaltbl_p)[0]==4&&((unsigned char *)art.sprite(0)->opaltbl_p)[15]==4&&art.sprite(0)->layer_p==layer&&std::string(art.undo_label())=="Create clean palette copy","create appends without source mutation or assignment");key(44,ImGuiKey_Z);
        if(frame==48)check(art.palette_count()==2&&!art.dirty()&&copy_palette(art,1).words==source,"one Undo removes copy");key(50,ImGuiKey_Y);
        if(frame==54)check(art.palette_count()==3&&copy_palette(art,2).words==expected,"Redo restores copy");
        if(frame==56){auto file=(fs::u8path(app.prefs)/"clean-copy-result.IMG").u8string();check(art.save(file,app.error),"save copy");auto disk=Artwork::open(file,app.error);check(disk&&disk->disk_version()==0x634&&disk->palette_count()==3&&disk->sprite(0)->palnum==1&&disk->palette(2)->numc==expected_count&&copy_palette(*disk,1).words==source&&copy_palette(*disk,2).words==expected,"same-format reopen");}
        if(frame==60){art.undo();check(art.save(path,app.error)&&read(path)==original,"Undo saves exact original bytes");app.palette_reset(*t,1);}click(64,u.clean_copy_point);
        if(frame==68){art.color(1,1,0x1234);art.commit();}
        if(frame==72)check(!u.usage_valid&&!u.usage_error.empty(),"source revision invalidates review");click(74,u.apply_point);
        if(frame==78)check(u.usage_mode&&art.palette_count()==2,"stale copy cannot append");click(80,u.cancel_usage_point);
        if(frame==84){art.undo();app.palette_reset(*t,1);}click(86,u.clean_copy_point);click(90,u.apply_point);
        if(frame==94){check(art.palette_count()==3&&palette_name(*art.palette(2))=="SCATTER_2","copy after stale cancel");app.palette_reset(*t,1);}
        click(100,u.clean_copy_point);click(104,u.apply_point);
        if(frame==108)check(art.palette_count()==4&&palette_name(*art.palette(3))=="SCATTER_3"&&art.sprite(0)->palnum==1,"repeated copy gets a unique name");key(110,ImGuiKey_Z);key(116,ImGuiKey_Z);
        if(frame==120){check(art.palette_count()==2&&!art.dirty()&&app.clipboard.pixels==pixels&&app.palette_clipboard.words==copy_palette(art,0).words,"history and clipboard isolation");app.palette_reset(*t,1);}
        click(122,u.clean_copy_point);click(126,u.order_point);click(130,u.order_choices[2]);
        if(frame==134){check(u.usage_valid&&u.usage_options.copy_only&&u.usage_plan.mapping[55]&&u.error.empty()&&app.error.empty()&&!art.dirty(),"final clean-copy review");io.AddMousePosEvent(5,5);}
    }
};
