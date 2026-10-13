struct VideoSmoke {
    int failures=0,phase=0,entered=0,pick=0,count=0,palettes=0;
    bool finished=false,result=false;std::string file;std::vector<unsigned char> original;
    std::vector<DigitizeSource> captured;
    float initial_zoom=0;ImVec2 before_pan;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();if(!t){++failures;finished=true;return;}auto &art=*t->art;auto &u=app.video;auto &d=app.digitizing;if(phase>=8&&(d.job||d.pending)){app.digitize_poll();++entered;return;}auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio video UI check failed at phase %d frame %d: %s / %s / %s\n",phase,frame,u.error.c_str(),d.error.c_str(),app.error.c_str());}};auto advance=[&](int p){phase=p;entered=frame;};int age=frame-entered;auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};auto video_pixel=[&](int x,int y){point(u.preview_origin+ImVec2((x+.5f)*u.preview_zoom,(y+.5f)*u.preview_zoom));};auto digi_pixel=[&](int x,int y){point(d.preview_origin+ImVec2((x+.5f)*d.preview_zoom,(y+.5f)*d.preview_zoom));};auto click=[&](ImVec2 p,int button=0){if(age==1)point(p);if(age==2)io.AddMouseButtonEvent(button,true);if(age==3)io.AddMouseButtonEvent(button,false);};
        if(frame>=590){check(false);finished=true;return;}
        if(phase==0){count=art.count();palettes=art.palette_count();original=art.copy(0,0,0,48,64).pixels;check(art.save((fs::u8path(app.prefs)/"video-original.IMG").u8string(),app.error));check(make_video_fixture(fs::u8path(app.prefs)/"video-fixture",file));if(file.empty()){finished=true;return;}app.request_video(file);io.AddFocusEvent(true);advance(1);return;}
        if(phase==1){if(u.ready&&u.shown==0&&!u.job){initial_zoom=u.zoom;advance(18);}return;}
        if(phase==18){if(age==1)video_pixel(64,32);if(age==2)io.AddMouseWheelEvent(0,1);if(age==4){check(u.zoom>initial_zoom);before_pan=u.pan;io.AddMouseButtonEvent(2,true);}if(age==6){auto p=io.MousePos;point(p+ImVec2(20,-12));}if(age==8)io.AddMouseButtonEvent(2,false);if(age==10)check(std::abs(u.pan.x-before_pan.x)>18&&std::abs(u.pan.y-before_pan.y)>10);if(age==11)point(u.fit_point);if(age==12)io.AddMouseButtonEvent(0,true);if(age==13)io.AddMouseButtonEvent(0,false);if(age==15){check(std::abs(u.zoom-initial_zoom)<.001&&u.pan.x==0&&u.pan.y==0);advance(2);}return;}
        if(phase==2){if(age==1)video_pixel(33,9);if(age==2)io.AddMouseButtonEvent(0,true);if(age==3)video_pixel(95,53);if(age==4)io.AddMouseButtonEvent(0,false);if(age==5){check(u.settings.crop.x==33&&u.settings.crop.y==9&&u.settings.crop.w==63&&u.settings.crop.h==45);advance(3);}return;}
        if(phase==3){if(age==1)video_pixel(64,60);if(age==2)io.AddMouseButtonEvent(1,true);if(age==3)io.AddMouseButtonEvent(1,false);if(age==4){check(u.settings.custom_floor&&u.settings.floor_x==64&&u.settings.floor_y==60);u.current=1;advance(4);}return;}
        if(phase==4){if(u.shown==u.current&&!u.job)advance(5);return;}
        if(phase==5){click(u.pick_point);if(age==4){check(u.settings.picks.count(u.current)&&u.settings.manual);if(pick==0){io.AddKeyEvent(ImGuiKey_I,true);advance(6);}else if(pick==2){io.AddKeyEvent(ImGuiKey_O,true);advance(6);}else{++pick;u.current=7;advance(4);}}return;}
        if(phase==6){if(age==1){io.AddKeyEvent(ImGuiKey_I,false);io.AddKeyEvent(ImGuiKey_O,false);}if(age==2){if(pick==0){check(u.settings.first==1);++pick;u.current=3;advance(4);}else{check(u.settings.last==7&&u.settings.picks.size()==3&&u.plan_valid&&!art.dirty());advance(7);}}return;}
        if(phase==7){click(u.capture_point);if(age==4)advance(8);return;}
        if(phase==8){if(d.open){check(!u.open&&d.session.frames.size()==3&&d.session.custom_anchor&&d.session.anchor_x==31&&d.session.anchor_y==51&&!art.dirty());for(auto &f:d.session.frames)captured.push_back(f.source);advance(9);}return;}
        if(phase==9){click(d.add_point);if(age==4){check(d.session.materials.size()==1&&d.view==2);advance(10);}return;}
        if(phase==10){if(age==1)digi_pixel(5,20);if(age==2)io.AddMouseButtonEvent(0,true);if(age==3)io.AddMouseButtonEvent(0,false);if(age==4){check(d.session.frames[0].mask[20*63+5]==1);advance(11);}return;}
        if(phase==11){click(d.add_point);if(age==4){check(d.session.materials.size()==2);advance(12);}return;}
        if(phase==12){if(age==1)digi_pixel(50,20);if(age==2)io.AddMouseButtonEvent(0,true);if(age==3)io.AddMouseButtonEvent(0,false);if(age==4){check(d.session.frames[0].mask[20*63+50]==2);advance(13);}return;}
        if(phase==13){click(d.fit_point);if(age==4){check(d.valid&&!d.dirty&&d.batch.frames.size()==3&&d.batch.frames[0].anix==31&&d.batch.frames[0].aniy==51&&!art.dirty());advance(14);}return;}
        if(phase==14){click(d.apply_point);if(age==4){check(!d.open&&art.count()==count+3&&art.palette_count()==palettes+1&&art.dirty());auto path=(fs::u8path(app.prefs)/"video-result.IMG").u8string();check(art.save(path,app.error));auto loaded=Artwork::open(path,app.error);std::vector<EmbeddedRecord> records;check(loaded&&loaded->count()==count+3&&read_embedded(*loaded,records,app.error)&&embedded_duration(records.back())==68);app.action(Action::Undo);check(art.count()==count&&art.palette_count()==palettes&&art.copy(0,0,0,48,64).pixels==original);app.action(Action::Redo);check(art.count()==count+3);app.action(Action::Undo);
            if(result){app.start_digitize(captured);d.session.custom_anchor=true;d.session.anchor_x=31;d.session.anchor_y=51;advance(19);}else{app.request_video(file);advance(15);}}return;}
        if(phase==15){if(u.ready&&u.shown==0&&!u.job){u.settings.manual=true;u.settings.picks={1,3,7};u.settings.crop={33,9,63,45};u.settings.custom_floor=true;u.settings.floor_x=64;u.settings.floor_y=60;u.plan_dirty=true;u.current=7;advance(16);}return;}
        if(phase==16){if(u.shown==7&&!u.job&&u.plan_valid)advance(17);return;}
        if(phase==19){app.digitize_add_material();edit_digitize_mask(d.session,0,5,20,1,false,1,d.error);app.digitize_add_material();edit_digitize_mask(d.session,0,50,20,2,false,1,d.error);app.digitize_fit();advance(20);return;}
        if(phase==20){check(d.valid);advance(17);return;}
        if(phase==17&&age>=3)finished=true;
    }
};
