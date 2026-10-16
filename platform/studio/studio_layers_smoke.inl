struct LayersSmoke {
    int failures=0;uint64_t uid=0;std::vector<unsigned char> base,file_bytes;int start_x=0;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.find(uid);auto check=[&](bool ok,const char *label){if(!ok){++failures;fprintf(stderr,"Overlay UI failure at %d (%s): %s / %s\n",frame,label,app.error.c_str(),app.status.c_str());}};
        auto key=[&](int at,ImGuiKey k,ImGuiKeyChord mods=0){if(frame==at||frame==at+1){bool down=frame==at;io.AddKeyEvent(ImGuiMod_Ctrl,down&&(mods&ImGuiMod_Ctrl));io.AddKeyEvent(ImGuiMod_Shift,down&&(mods&ImGuiMod_Shift));io.AddKeyEvent(k,down);}};
        auto click=[&](int at,ImVec2 pos){if(frame==at-1)io.AddMousePosEvent(pos.x,pos.y);if(frame==at)io.AddMouseButtonEvent(0,true);if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto pixels=[&](){return t->art->copy(0,0,0,48,64).pixels;};
        if(frame==0){t=app.tab();uid=t->uid;app.select(0);base=pixels();auto file=fs::u8path(app.prefs)/"overlay-base.IMG";check(t->art->save(file.u8string(),app.error),"base save");std::ifstream f(file,std::ios::binary);file_bytes.assign(std::istreambuf_iterator<char>(f),{});app.clipboard=t->art->copy(0,0,0,3,2);app.clipboard.pixels={9,0,10,11,12,13};app.clipboard.mask={1,1,1,1,0,1};app.paste_x=15;app.paste_y=10;io.AddFocusEvent(true);io.AddMousePosEvent(5,5);}
        key(4,ImGuiKey_V,ImGuiMod_Ctrl);key(8,ImGuiKey_L);
        if(frame==12){check(t->art->sprite(0)->layer_p&&!app.paste_mode&&!app.floating.active&&pixels()==base,"L retains separate layer");start_x=img_layer(t->art->sprite(0))?img_layer(t->art->sprite(0))->x:0;}
        click(14,app.layer_flip_point);
        if(frame==18)check(t->art->sprite(0)->layer_p&&layer_pixels(img_layer(t->art->sprite(0)))[0]==10&&pixels()==base,"flip overlay only");
        key(20,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==24)check(t->art->sprite(0)->layer_p&&layer_pixels(img_layer(t->art->sprite(0)))[0]==9,"flip Undo");
        key(24,ImGuiKey_Y,ImGuiMod_Ctrl);click(30,app.layer_right_point);
        if(frame==34)check(img_layer(t->art->sprite(0))&&img_layer(t->art->sprite(0))->x==start_x+1,"nudge button");
        click(36,app.layer_visible_point);
        if(frame==40){check(t->art->sprite(0)->layer_p&&!img_layer(t->art->sprite(0))->visible&&pixels()==base,"hidden layer");auto file=fs::u8path(app.prefs)/"overlay-hidden.IMG";check(t->art->save(file.u8string(),app.error),"hidden save");std::ifstream f(file,std::ios::binary);std::vector<unsigned char> b{std::istreambuf_iterator<char>(f),{}};check(b==file_bytes,"hidden save has exact original bytes");}
        click(42,app.layer_visible_point);
        if(frame==46){auto file=fs::u8path(app.prefs)/"overlay-visible.IMG";check(t->art->save(file.u8string(),app.error),"visible save");auto disk=Artwork::open(file.u8string(),app.error);check(disk&&!disk->sprite(0)->layer_p&&disk->disk_version()==t->art->disk_version()&&pixels()==base,"visible flatten on disk only");if(disk)for(int y=0;y<64;++y)for(int x=0;x<48;++x)check(visible_pixel(*t->art->sprite(0),x,y)==visible_pixel(*disk->sprite(0),x,y),"disk appearance");}
        click(50,app.layer_flatten_point);if(frame==54)check(!t->art->sprite(0)->layer_p&&pixels()!=base,"flatten button");
        key(56,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==60)check(t->art->sprite(0)->layer_p&&pixels()==base,"flatten Undo restores base and layer");
        click(62,app.layer_remove_point);if(frame==66)check(!t->art->sprite(0)->layer_p&&pixels()==base,"remove button");
        key(68,ImGuiKey_Z,ImGuiMod_Ctrl);if(frame==72)check(t->art->sprite(0)->layer_p&&pixels()==base,"remove Undo");
        click(74,app.layer_remove_point);key(78,ImGuiKey_V,ImGuiMod_Ctrl);key(82,ImGuiKey_T,ImGuiMod_Ctrl);key(86,ImGuiKey_H);key(90,ImGuiKey_L);
        if(frame==94)check(t->art->sprite(0)->layer_p&&!app.floating.active&&pixels()==base&&!app.help,"transformed paste L");
        key(98,ImGuiKey_H);if(frame==102)check(app.help,"Help outside paste");key(104,ImGuiKey_L);
        if(frame==108){check(app.help&&t->art->sprite(0)->layer_p&&pixels()==base,"modal key isolation");}
        click(110,app.help_done_point);
        if(frame==114)check(!app.help&&app.error.empty()&&t->art->disk_version()==0x634,"format and UI health");
    }
};
