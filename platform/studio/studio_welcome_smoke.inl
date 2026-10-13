struct WelcomeSmoke {
    int kind=0,failures=0;std::vector<std::string> paths;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Studio welcome check failed at frame %d: %s; tabs=%zu rows=%zu\n",frame,app.error.c_str(),app.tabs.size(),app.home.rows.size());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(p.x,p.y);};
        if(frame==0){app.recent.clear();if(kind!=1){auto root=fs::u8path(app.prefs)/"welcome-libraries";auto art=Artwork::demo();for(int n=0;n<(kind==2?10:2);++n){auto folder=root/(n==0?"Mortal Kombat II":n==1?"Alternate artwork":("Archive "+std::to_string(n)));fs::create_directories(folder);auto path=(folder/(kind==2&&n==2?"A_long_library_name_for_checking_filename_clipping_and_hover_details.IMG":"Sprites.IMG")).u8string();check(art->save(path,app.error));paths.push_back(path);app.recent.push_back(path);}if(kind==2)app.recent[4]=(root/"Unavailable.IMG").u8string();}io.AddFocusEvent(true);}
        if(frame==2){check(app.tabs.empty()&&app.home.rows.size()==app.recent.size());auto bounds=[&](ImVec2 p){return p.x>0&&p.y>0&&p.x<io.DisplaySize.x&&p.y<io.DisplaySize.y;};check(bounds(app.home.open_point)&&bounds(app.home.new_point)&&bounds(app.home.sample_point)&&bounds(app.home.project_point));if(kind==3)point(app.home.new_point);}
        if(kind!=3)return;
        if(frame==3)io.AddMouseButtonEvent(0,true);if(frame==4)io.AddMouseButtonEvent(0,false);
        if(frame==6){check(app.tab()&&app.tab()->art->count()==1);if(app.tab()){check(app.tab()->art->save((fs::u8path(app.prefs)/"welcome-new.IMG").u8string(),app.error));app.action(Action::Close);}}
        if(frame==9){check(app.tabs.empty());point(app.home.sample_point);}if(frame==10)io.AddMouseButtonEvent(0,true);if(frame==11)io.AddMouseButtonEvent(0,false);
        if(frame==13){check(app.tab()&&app.tab()->art->count()==8);if(app.tab()){check(app.tab()->art->save((fs::u8path(app.prefs)/"welcome-sample.IMG").u8string(),app.error));app.action(Action::Close);}}
        if(frame==16){check(app.tabs.empty()&&app.home.recent_points.size()==2);if(!app.home.recent_points.empty())point(app.home.recent_points[0]);}if(frame==17)io.AddMouseButtonEvent(0,true);if(frame==18)io.AddMouseButtonEvent(0,false);
        if(frame==20){check(app.tab()&&app.tab()->art->path()==paths[0]);if(app.tab())app.action(Action::Close);}if(frame==23)check(app.tabs.empty());
    }
};
