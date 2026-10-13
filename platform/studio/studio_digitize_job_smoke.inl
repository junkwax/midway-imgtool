struct DigitizeJobSmoke {
    int failures=0,phase=0,entered=0;bool finished=false,preview=false;
    uint64_t generation=0;int count=0,palettes=0;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();if(!t){++failures;finished=true;return;}auto &u=app.digitizing;auto &art=*t->art;auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Digitize worker UI check failed at phase %d frame %d: %s\n",phase,frame,u.error.c_str());}};auto next=[&](int value){phase=value;entered=frame;};int age=frame-entered;
        if(frame>590){check(false);finished=true;return;}
        if(phase==0){std::vector<DigitizeSource> sources;for(int i=0;i<24;++i){DigitizeSource f;f.w=f.h=768;f.label="Large frame "+std::to_string(i);f.rgba.resize(768*768*4);for(size_t p=0;p<f.rgba.size();p+=4){f.rgba[p]=140;f.rgba[p+1]=60;f.rgba[p+2]=20;f.rgba[p+3]=255;}sources.push_back(std::move(f));}generation=art.generation();count=art.count();palettes=art.palette_count();app.start_digitize(std::move(sources));u.session.matte.shrink=8;io.AddFocusEvent(true);next(1);return;}
        if(phase==1){if(u.job){check(u.job->input->frames.size()==24&&art.generation()==generation&&!u.valid);next(2);}return;}
        if(phase==2){if(preview){if(age>=2){check(u.job&&art.generation()==generation);finished=true;}return;}if(age==1)io.AddMousePosEvent(u.cancel_job_point.x,u.cancel_job_point.y);if(age==2)io.AddMouseButtonEvent(0,true);if(age==3)io.AddMouseButtonEvent(0,false);if(age==4)next(3);return;}
        if(phase==3){if(!u.job&&!u.pending){check(u.open&&u.session.frames.size()==24&&!u.session.matte_ready&&u.matte_dirty&&!u.valid&&art.generation()==generation&&art.count()==count&&art.palette_count()==palettes&&u.error.find("cancelled")!=std::string::npos);next(4);}return;}
        if(phase==4){if(age==1)io.AddMousePosEvent(u.cancel_point.x,u.cancel_point.y);if(age==2)io.AddMouseButtonEvent(0,true);if(age==3)io.AddMouseButtonEvent(0,false);if(age==4){check(!u.open&&art.generation()==generation);finished=true;}return;}
    }
};
