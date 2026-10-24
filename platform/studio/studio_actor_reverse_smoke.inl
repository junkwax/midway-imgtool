struct ActorReverseSmoke {
    ActorSplitSmoke fixture;int failures=0;
    void prepare(App &app,ImGuiIO &io,int frame){auto check=[&](bool ok,const char *label){if(!ok){++failures;fprintf(stderr,"Actor reverse UI failure %d (%s): %s / %s\n",frame,label,app.error.c_str(),app.scene_error.c_str());}};
        auto read=[&](const std::string &path){std::ifstream f(fs::u8path(path),std::ios::binary);return std::string{std::istreambuf_iterator<char>(f),{}};};
        auto click=[&](int at,ImVec2 p){if(frame==at-1)io.AddMousePosEvent(p.x,p.y);if(frame==at)io.AddMouseButtonEvent(0,true);if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto key=[&](int at,ImGuiKey k){if(frame==at||frame==at+1){bool down=frame==at;io.AddKeyEvent(ImGuiMod_Ctrl,down);io.AddKeyEvent(k,down);}};
        if(frame==0){fixture.prepare(app,io,0);check(!fixture.failures,"source fixture");auto t=app.find(fixture.owner);if(t){t->scene.frame=0;t->scene.tick=4;}}
        auto t=app.find(fixture.owner);if(!t||!t->project){check(false,"open composition");return;}auto &s=t->project->state;auto &u=t->scene;
        click(6,u.actor_actions_point);click(10,u.actor_reverse_point);
        if(frame==14){auto &actor=s.lanes[0];check(actor.frames[0].id==13&&actor.frames[2].id==11&&u.frame==2&&u.tick==4&&t->project->dirty(),"reverse button, selected identity and tick");check(actor.frames[0].pieces.size()==2&&actor.frames[0].visible_from==0&&actor.frames[0].visible_until==8&&actor.frames[0].x==14&&actor.frames[0].vx==-2&&actor.frames[1].x==11&&actor.frames[1].vx==-1,"multipart timed window and motion endpoint");check(s.lanes[1].trigger_frame==13&&scene_lane_start(s,1)==0,"effect follows same frame at new tick");}key(18,ImGuiKey_Z);
        if(frame==22)check(s.lanes[0].frames[0].id==11&&s.lanes[0].frames[2].id==13&&scene_lane_start(s,1)==12&&!t->project->dirty(),"one Undo restores frames and links");key(24,ImGuiKey_Y);
        if(frame==28)check(s.lanes[0].frames[0].id==13&&s.lanes[0].frames[2].id==11,"Redo keeps identities");
        if(frame==32){check(t->project->save(fixture.native,app.scene_source(),app.error),"save reversed composition");check(t->project->export_png(fixture.png,3,app.scene_source(),app.error),"export reversed scene");}if(frame==36)app.open_scene(fixture.native);
        if(frame==40){check(s.lanes[0].frames[0].id==13&&s.lanes[0].frames[0].vx==-2&&s.lanes[0].frames[0].pieces.size()==2&&!t->project->dirty(),"native reopen retains timed reversal");auto png=(fs::u8path(app.prefs)/"after.png").u8string();check(t->project->export_png(png,3,app.scene_source(),app.error)&&read(png)==read(fixture.png),"same PNG after native reopen");u.lane=0;u.frame=0;u.tick=3;}
        click(44,u.actor_actions_point);click(48,u.actor_reverse_point);if(frame==52){check(t->project->dirty()&&s.lanes[0].frames[0].id==11,"reversal after reopen");app.request_close_scene(*t);}if(frame==55)check(app.scene_guard_uid==fixture.owner,"dirty close guard");click(58,app.scene_cancel_point);
        if(frame==62){auto other=app.find(fixture.other);check(!app.scene_guard_uid&&t->project->dirty(),"Cancel close keeps scene");check(other&&t->art->generation()==fixture.generation&&other->art->generation()==fixture.other_generation&&!t->art->dirty()&&!other->art->dirty()&&read(fixture.source)==fixture.source_bytes&&read(other->art->path())==fixture.other_bytes&&app.mark_count(*t)==1,"source IMG format/bytes/history/marks unchanged");}key(64,ImGuiKey_Z);
        if(frame==68){check(!t->project->dirty()&&t->project->can_redo()&&s.lanes[0].frames[0].id==13,"Undo restores native baseline");u.lane=0;u.frame=0;u.tick=3;}key(70,ImGuiKey_Y);
        if(frame==74)check(t->project->dirty()&&s.lanes[0].frames[0].id==11,"Redo after canceled close");click(78,u.actor_actions_point);if(frame==82){check(u.actor_reverse_point.x>0,"reverse action visible");io.AddMousePosEvent(5,5);}
    }
};
