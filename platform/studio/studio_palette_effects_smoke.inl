struct PaletteEffectsSmoke {
    int failures=0;uint64_t generation=0;
    std::vector<unsigned short> original,inverted,reversed,combined,shifted;
    std::vector<unsigned char> pixels,exact;std::string path;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto t=app.tab();auto &u=app.palettes;auto &art=*t->art;
        auto check=[&](bool ok,const char *label){if(!ok){++failures;fprintf(stderr,"Palette effects UI failure %d (%s): %s / %s\n",frame,label,u.error.c_str(),app.error.c_str());}};
        auto click=[&](int at,ImVec2 point,bool ctrl=false){if(frame==at-1)io.AddMousePosEvent(point.x,point.y);if(frame==at){if(ctrl)io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddMouseButtonEvent(0,true);}if(frame==at+1){io.AddMouseButtonEvent(0,false);if(ctrl)io.AddKeyEvent(ImGuiMod_Ctrl,false);}};
        auto key=[&](int at,ImGuiKey k,bool shift=false){if(frame==at||frame==at+1){bool down=frame==at;io.AddKeyEvent(ImGuiMod_Ctrl,down);if(shift)io.AddKeyEvent(ImGuiMod_Shift,down);io.AddKeyEvent(k,down);}};
        if(frame==0){
            // Distinct high bits make complete-word reversal observable.
            auto colors=(unsigned char *)art.palette(0)->data_p;colors[5]|=0x80;colors[17]|=0x80;
            original=copy_palette(art,0).words;inverted=original;for(int c:{2,8,15})inverted[c]^=0x7fff;
            reversed=original;std::swap(reversed[2],reversed[15]);combined=inverted;std::swap(combined[2],combined[15]);
            pixels=art.copy(0,0,0,48,64).pixels;app.palette_clipboard=copy_palette(art,0);app.clipboard=art.copy(0,0,0,48,64);check(art.copy_palette_swatches(0,{},8,app.swatch_clipboard,app.error),"seed independent clipboard");
            path=(fs::u8path(app.prefs)/"effects.IMG").u8string();check(art.save(path,app.error),"fixture save");std::ifstream file(fs::u8path(path),std::ios::binary);exact.assign(std::istreambuf_iterator<char>(file),{});
            generation=art.generation();app.open_palettes(*t);io.AddFocusEvent(true);io.AddMousePosEvent(5,5);
        }
        click(4,u.swatch_points[2]);click(8,u.swatch_points[8],true);click(12,u.swatch_points[15],true);
        if(frame==16)check(u.selected[2]&&u.selected[8]&&u.selected[15]&&!u.selected[1]&&!art.dirty(),"select nonadjacent slots");
        click(18,u.invert_colors_point);
        if(frame==22)check(app.palette_pending()&&app.palette_words()==inverted&&copy_palette(art,0).words==original&&art.generation()==generation&&!art.dirty(),"invert is preview only");
        click(24,u.invert_colors_point);
        if(frame==28)check(!app.palette_pending()&&app.palette_words()==original&&art.generation()==generation&&!art.dirty(),"invert twice cancels without history");
        click(30,u.reverse_colors_point);
        if(frame==34)check(app.palette_pending()&&app.palette_words()==reversed&&app.palette_words()[0]==original[0]&&!art.dirty(),"reverse stages selected words");
        click(36,u.invert_colors_point);
        if(frame==40)check(app.palette_words()==combined&&copy_palette(art,0).words==original,"effects compose in one preview");
        click(42,u.reset_point);
        if(frame==46)check(!app.palette_pending()&&app.palette_words()==original&&!art.dirty(),"Reset discards both effects");
        click(48,u.reverse_colors_point);click(52,u.apply_point);
        if(frame==56)check(!app.palette_pending()&&copy_palette(art,0).words==reversed&&art.dirty()&&art.copy(0,0,0,48,64).pixels==pixels&&art.palette(0)->numc==16&&art.palette(0)->bitspix==4,"Apply retains indices and palette size/depth");
        key(58,ImGuiKey_Z);
        if(frame==62)check(copy_palette(art,0).words==original&&!art.dirty(),"one Undo restores effect");key(64,ImGuiKey_Z,true);
        if(frame==68)check(copy_palette(art,0).words==reversed&&art.dirty(),"Redo");key(70,ImGuiKey_Z);
        if(frame==74){check(!art.dirty(),"restored baseline");u.hue=50;shifted=app.palette_words();}
        click(76,u.invert_colors_point);
        if(frame==80){auto expected=shifted;for(int c:{2,8,15})expected[c]^=0x7fff;check(u.hue==0&&u.saturation==0&&u.lightness==0&&app.palette_words()==expected&&!art.dirty(),"effect bakes current HSL preview once");}
        click(82,u.reset_point);
        // One color can invert; reversing one selected slot is disabled.
        click(88,u.swatch_points[8]);click(92,u.reverse_colors_point);
        if(frame==96)check(!app.palette_pending()&&!art.dirty(),"one-slot reverse disabled");click(98,u.invert_colors_point);
        if(frame==102){auto expected=original;expected[8]^=0x7fff;check(app.palette_words()==expected&&app.palette_pending(),"one-slot invert");}
        click(104,u.reset_point);click(108,u.swatch_points[0]);click(112,u.invert_colors_point);click(116,u.reverse_colors_point);
        if(frame==120)check(!app.palette_pending()&&app.palette_words()==original&&!art.dirty(),"zero-only selection cannot invert or reverse");
        click(122,u.swatch_points[2]);click(126,u.swatch_points[8],true);click(130,u.swatch_points[15],true);click(134,u.reverse_colors_point);
        key(138,ImGuiKey_C);key(142,ImGuiKey_V);
        if(frame==146)check(app.palette_pending()&&art.palette_count()==1&&app.palette_clipboard.words==original&&app.clipboard.pixels==pixels&&app.swatch_clipboard.slots.size()==1&&app.swatch_clipboard.slots[0].word==original[8],"pending effect protects independent clipboards");
        click(148,u.close_point);
        if(frame==152){check(!u.open&&!art.dirty()&&copy_palette(art,0).words==original,"Close discards unapplied effect");app.open_palettes(*t);u.selected.fill(false);u.selected[2]=u.selected[8]=u.selected[15]=true;}
        click(156,u.invert_colors_point);click(160,u.apply_point);
        if(frame==164){check(copy_palette(art,0).words==inverted&&art.save(path,app.error),"save applied inversion");auto disk=Artwork::open(path,app.error);check(disk&&disk->disk_version()==0x634&&copy_palette(*disk,0).words==inverted&&disk->copy(0,0,0,48,64).pixels==pixels,"same-format reopen");art.undo();check(art.save(path,app.error),"save restored file");std::ifstream file(fs::u8path(path),std::ios::binary);std::vector<unsigned char> bytes(std::istreambuf_iterator<char>(file),{});check(bytes==exact,"Undo saves exact original bytes");app.palette_reset(*t,0);u.selected.fill(false);u.selected[2]=u.selected[8]=u.selected[15]=true;}
        click(168,u.reverse_colors_point);
        if(frame==172){check(u.open&&app.palette_pending()&&app.palette_words()==reversed&&!art.dirty()&&u.error.empty()&&app.error.empty(),"final staged preview");io.AddMousePosEvent(5,5);}
    }
};
