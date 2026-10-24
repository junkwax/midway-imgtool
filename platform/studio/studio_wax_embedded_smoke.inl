struct WaxEmbeddedSmoke {
    int failures=0,dx=0,dy=0;uint64_t owner=0,generation=0,lane_id=0,frame_id=0;
    std::string img,wax,native,missing,script,img_bytes,wax_bytes;ImVec2 drag;SceneProject *installed=nullptr;
    void prepare(App &app,ImGuiIO &io,int frame){
        auto check=[&](bool ok,const char *label){if(!ok){++failures;fprintf(stderr,"WAX embedded UI failure %d (%s): %s / %s\n",frame,label,app.error.c_str(),app.scene_error.c_str());}};
        auto read=[](const std::string &p){std::ifstream f(fs::u8path(p),std::ios::binary);return std::string{std::istreambuf_iterator<char>(f),{}};};
        auto write=[](const std::string &p,const std::string &text){std::ofstream f(fs::u8path(p),std::ios::binary);f<<text;};
        auto key=[&](int at,ImGuiKey k){if(frame==at||frame==at+1){bool down=frame==at;io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);}};
        auto click=[&](int at,ImVec2 p){if(frame==at-1)io.AddMousePosEvent(p.x,p.y);if(frame==at)io.AddMouseButtonEvent(0,true);if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        if(frame==0){auto t=app.tab();if(!t){++failures;return;}owner=t->uid;io.AddFocusEvent(true);
            check(t->art->embedded_add_record(false,"WALK",app.error)==0&&t->art->embedded_append(0,0,app.error)&&t->art->embedded_append(0,1,app.error),"source record");
            img=(fs::u8path(app.prefs)/"embedded-source.IMG").u8string();wax=(fs::u8path(app.prefs)/"embedded.WAX").u8string();native=(fs::u8path(app.prefs)/"embedded.ISTUDIO").u8string();missing=(fs::u8path(app.prefs)/"missing-frame.WAX").u8string();script=(fs::u8path(app.prefs)/"script.WAX").u8string();
            check(t->art->save(img,app.error),"save source");img_bytes=read(img);generation=t->art->generation();
            wax_bytes="format=imgtool_world_project\nversion=1\nworld.origin_y=210\ndoc.count=1\ndoc.0.path="+img+"\nslot.count=24\nslot.source_tabs=20\nstate.embedded_active=1\nstate.embedded_is_script=0\nstate.embedded_record_index=0\nstate.embedded_doc_idx=0\nstate.embedded_name=WALK\nstate.embedded_label_count=1\nembedded_label.0.text=Saved stance\nslot.23.sequence_frames=0,1\nslot.23.frame_delays=4,6\nslot.23.mirror=1\nslot.23.zoom=2\nslot.23.local_dx=-40,-40\nslot.23.hold_end=0\n";
            write(wax,wax_bytes);auto bad=wax_bytes;bad.replace(bad.find("sequence_frames=0,1"),19,"sequence_frames=0,1999");write(missing,bad);bad=wax_bytes;bad.replace(bad.find("embedded_is_script=0"),20,"embedded_is_script=1");write(script,bad);app.open_scene(wax);
        }
        auto t=app.find(owner);if(!t||!t->project){check(false,"imported project remains");return;}auto &s=t->project->state;
        if(frame==4){check(!app.pending_project&&t->project->imported_wax()&&t->project->dirty()&&s.lanes.size()==1,"embedded-only import");auto &l=s.lanes[0];check(l.name=="Sequence: WALK"&&l.frames.size()==2&&l.frames[0].name=="Saved stance"&&l.frames[1].ticks==6&&l.magnification==2&&l.flip_x&&s.loop,"saved settings");lane_id=l.id;frame_id=l.frames[0].id;t->scene.lane=0;t->scene.frame=0;t->scene.smoke=true;}
        // SDL's hidden-window mouse update rounds screen positions. At compact
        // zoom an edge pixel can round outside the silhouette; use an opaque
        // 5x5 interior and keep the same actor-drag/Undo/Redo assertions.
        if(frame==8){drag=t->scene.probe;auto poses=evaluate_scene(s,(int)t->scene.tick);if(!poses.empty()){auto &pose=poses.front();int index=-1;auto source=app.scene_sprite(s,pose.sprite,index);auto im=source?source->art->sprite(index):nullptr;if(im){int fx=-1,fy=0,cx=-1,cy=0,stride=(im->w+3)&~3;auto pixel=(const unsigned char *)im->data_p;for(int y=0;y<im->h&&fx<0;++y)for(int x=0;x<im->w;++x)if(pixel[y*stride+x]){fx=x;fy=y;break;}for(int y=2;y<im->h-2&&cx<0;++y)for(int x=2;x<im->w-2;++x){bool solid=true;for(int yy=y-2;yy<=y+2&&solid;++yy)for(int xx=x-2;xx<=x+2;++xx)if(!pixel[yy*stride+xx]){solid=false;break;}if(solid){cx=x;cy=y;break;}}check(fx>=0&&cx>=0,"opaque drag interior");if(cx>=0)drag+=ImVec2((pose.flip_x?fx-cx:cx-fx)*pose.magnification*t->scene.probe_zoom,(pose.flip_y?fy-cy:cy-fy)*pose.magnification*t->scene.probe_zoom);}}drag=ImFloor(drag);io.AddMousePosEvent(drag.x,drag.y);}if(frame==9)io.AddMouseButtonEvent(0,true);
        if(frame==10){auto target=ImFloor(drag+ImVec2(8*t->scene.probe_zoom,-4*t->scene.probe_zoom));dx=(int)std::round((target.x-drag.x)/t->scene.probe_zoom);dy=(int)std::round((target.y-drag.y)/t->scene.probe_zoom);io.AddMousePosEvent(target.x,target.y);}if(frame==11)io.AddMouseButtonEvent(0,false);
        if(frame==14)check(s.lanes[0].x==dx&&s.lanes[0].y==dy,"drag");key(16,ImGuiKey_Z);if(frame==20)check(s.lanes[0].x==0&&s.lanes[0].y==0,"Undo");key(22,ImGuiKey_Y);
        if(frame==26){check(s.lanes[0].x==dx&&s.lanes[0].y==dy,"Redo");check(t->project->save(native,app.scene_source(),app.error)&&t->project->export_png((fs::u8path(app.prefs)/"embedded.png").u8string(),4,app.scene_source(),app.error),"native save and PNG");}
        if(frame==30)app.open_scene(native);
        if(frame==34){check(!t->project->imported_wax()&&!t->project->dirty()&&s.lanes[0].id==lane_id&&s.lanes[0].frames[0].id==frame_id&&s.lanes[0].frames[0].name=="Saved stance","native reopening");installed=t->project.get();app.open_scene(missing);}
        if(frame==38)check(app.pending_project&&!app.scene_issues.empty()&&t->project.get()==installed&&!t->project->dirty()&&!app.pending_project->state.assets[0].runtime_doc,"missing-frame failure is atomic");click(42,app.scene_sources_cancel_point);
        if(frame==46)check(!app.pending_project&&app.scene_issues.empty()&&t->project.get()==installed,"Cancel failed import");
        if(frame==48){app.open_scene(script);check(!app.pending_project&&t->project.get()==installed&&app.error.find("script")!=std::string::npos,"script preview stays guarded");app.error.clear();}
        if(frame==52){check(read(img)==img_bytes&&read(wax)==wax_bytes&&!t->art->dirty()&&t->art->generation()==generation,"source bytes and history unchanged");t->scene.lane=0;t->scene.frame=0;t->scene.tick=4;io.AddMousePosEvent(5,5);}
    }
};
