struct LegacyImgSmoke {
    int failures=0;std::string original,edited;uint64_t uid=0;
    void prepare(App &app,ImGuiIO &io,int frame) {
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio legacy IMG UI check failed at frame %d: %s\n",frame,app.error.c_str());}};
        auto t=app.tab();if(!t){++failures;return;}
        if(frame==0) {
            io.AddFocusEvent(true);auto source=fs::u8path(app.prefs)/"legacy-modern-source.IMG";
            auto &a=*t->art;check(a.embedded_add_record(false,"STORED_WALK",app.error)==0);
            for(int i=0;i<8;++i)check(a.embedded_append(0,i,app.error));a.commit();check(a.save(source.u8string(),app.error));
            std::ifstream input(source,std::ios::binary);std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input),{}};
            check(project_img_layout(bytes,0x62e,app.error));original=(fs::u8path(app.prefs)/"legacy-artwork.IMG").u8string();
            {std::ofstream out(fs::u8path(original),std::ios::binary);out.write((const char *)bytes.data(),bytes.size());}
            app.open(original);uid=app.tab()->uid;check(app.tabs.size()==2&&app.tab()->art->disk_version()==0x62e&&!app.tab()->art->dirty());
            app.embedded_workspace(*app.tab());
        }
        if(frame==3){check(t->uid==uid&&t->embedded.records.size()==1&&t->embedded.records[0].entries.size()==8);app.embedded_seek(*t,2);}
        if(frame==5){check(t->embedded.entry==2&&t->selected==2);app.action(Action::Duplicate);}
        if(frame==7){check(t->embedded.records[0].entries.size()==9&&t->art->dirty());app.action(Action::Undo);}
        if(frame==9){check(t->embedded.records[0].entries.size()==8&&!t->art->dirty());t->art->anchor(0,25,61);t->art->commit();check(app.save(*t,false));}
        if(frame==11){auto reopened=Artwork::open(original,app.error);check(reopened&&reopened->disk_version()==0x62e&&reopened->sprite(0)->anix==25&&reopened->data.seqcnt==1);}
        if(frame==13){edited=(fs::u8path(app.prefs)/"legacy-copy.IMG").u8string();check(t->art->save(edited,app.error));check(t->art->disk_version()==0x62e);auto copy=Artwork::open(edited,app.error);check(copy&&copy->disk_version()==0x62e);app.open(original);}
        if(frame==15){check(app.tab()->art->disk_version()==0x62e);app.embedded_workspace(*app.tab());app.embedded_seek(*app.tab(),3);}
        if(frame==17){io.AddMousePosEvent(135,15);io.AddMouseButtonEvent(0,true);}
        if(frame==18)io.AddMouseButtonEvent(0,false);
    }
};
