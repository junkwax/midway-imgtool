// Project controller and widgets. Included inside App so source tabs and their
// texture caches remain owned by the application, not by the scene codec.
SceneSource scene_source(){return [this](uint64_t uid)->Artwork *{auto t=find(uid);return t?t->art.get():nullptr;};}
std::string scene_name(const Tab &t){return t.project->path().empty()?"Untitled scene":fs::u8path(t.project->path()).filename().u8string();}
void clamp_scene_ui(Tab &t){
    auto &s=t.project->state;auto &u=t.scene;
    u.lane=std::clamp(u.lane,0,(int)s.lanes.size()-1);
    u.frame=std::clamp(u.frame,0,(int)s.lanes[u.lane].frames.size()-1);
    u.tick=std::clamp(u.tick,0.,(double)scene_last_tick(s));
}
void scene_seek(Tab &t,int frame){finish();auto &u=t.scene;auto &s=t.project->state;auto &l=s.lanes[u.lane];u.playing=false;u.frame=std::clamp(frame,0,(int)l.frames.size()-1);u.tick=scene_frame_seek_tick(s,u.lane,u.frame);}
void scene_step(Tab &t,int delta){auto &u=t.scene;int n=(int)t.project->state.lanes[u.lane].frames.size();scene_seek(t,(u.frame+delta+n)%n);}
void toggle_scene_playback(Tab &t){finish();auto &u=t.scene;if(!u.playing&&u.tick>=scene_last_tick(t.project->state))u.tick=0;u.playing=!u.playing;}
int scene_asset(SceneState &s,Tab &source){
    for(int i=0;i<(int)s.assets.size();++i)if(s.assets[i].runtime_doc==source.uid)return i;
    if(s.assets.size()>=64)return -1;
    s.assets.push_back({source.art->path(),source.art->path(),{},source.uid});return (int)s.assets.size()-1;
}
void drop_pending_scene(){pending_project.reset();pending_scene_new=false;staged_scene_tabs.clear();scene_issues.clear();scene_relocations.clear();pending_scene_owner=0;}
void attach_scene(){
    if(!pending_project)return;
    for(auto &t:staged_scene_tabs)tabs.push_back(std::move(t));staged_scene_tabs.clear();
    auto owner=find(pending_scene_owner);if(!owner){drop_pending_scene();return;}
    if(!pending_scene_new)pending_project->establish_loaded();pending_scene_new=false;owner->project=std::move(pending_project);owner->scene={};
    owner->scene.source_doc=owner->uid;owner->scene.source_sprite=owner->selected_id;owner->embedded.enabled=false;owner->embedded.playing=false;owner->alignment.enabled=true;owner->alignment.player.playing=false;
    for(int i=0;i<(int)tabs.size();++i)if(tabs[i].get()==owner)active=i;
    select_uid=owner->uid;paste_mode=false;
    status=owner->project->imported_wax()?"Imported WAX rows. Save as .ISTUDIO to keep this composition.":"Opened "+scene_name(*owner);
    scene_issues.clear();scene_relocations.clear();pending_scene_owner=0;scene_error.clear();
}
void install_scene(){
    auto owner=find(pending_scene_owner);
    if(owner&&owner->project&&owner->project->dirty()){scene_guard_uid=owner->uid;scene_guard_reason=3;owner->scene.playing=false;}
    else attach_scene();
}
void new_scene(Tab &t){
    finish();workspace(t,true);auto &v=t.alignment;SceneState s;
    s.width=v.world_w;s.height=v.world_h;s.origin_x=v.origin_x;s.origin_y=v.origin_y;s.floor=v.floor_y;s.onion=v.onion;
    s.loop=v.player.loop;s.ghost_facing=v.facing==2;s.convention=t.convention;s.tick_hz=v.tick_hz;
    int a=scene_asset(s,t);SceneLane lane;lane.name=v.sequences[v.sequence].name;lane.flip_x=v.facing==1;
    for(int i=0;i<(int)v.sequences[v.sequence].frames.size();++i){auto &f=v.sequences[v.sequence].frames[i];SceneFrame frame;frame.name=f.name;frame.ticks=v.holds[v.sequence][i];
        for(int index:preview_pieces(f,v.multipart))frame.pieces.push_back(scene_sprite_reference(*t.art,a,index));lane.frames.push_back(std::move(frame));}
    s.lanes.push_back(std::move(lane));
    if(auto reference=find(v.reference_doc)){int index=reference->art->sprite_index(v.reference_id);if(index>=0){
        SceneLane l;l.name="Reference";l.x=v.reference_x;l.y=v.reference_y;l.alpha=150;l.loop=false;l.flip_x=v.reference_flip;
        SceneFrame f;f.name=reference->art->sprite(index)->n_s;f.pieces.push_back(scene_sprite_reference(*reference->art,scene_asset(s,*reference),index));l.frames.push_back(std::move(f));s.lanes.push_back(std::move(l));}}
    drop_pending_scene();pending_project=std::make_unique<SceneProject>(std::move(s));pending_scene_owner=t.uid;pending_scene_new=true;
    // New projects are authored state, not a clean file read.
    install_scene();if(t.project&&t.project->path().empty())status="New scene. Positions and timing save to .ISTUDIO; artwork saves to IMG.";
}
void stage_scene(){
    if(!pending_project)return;
    staged_scene_tabs.clear();scene_issues.clear();std::vector<std::string> load_errors(pending_project->state.assets.size());
    auto binding=[&](int a,const SceneAsset &asset)->SceneBinding{
        std::string path=a<(int)scene_relocations.size()&&!scene_relocations[a].empty()?scene_relocations[a]:scene_asset_candidate(asset,pending_project->opened_from());
        if(path.empty())return {};
        std::error_code ec;auto absolute=fs::absolute(fs::u8path(path),ec).lexically_normal();if(ec){load_errors[a]=ec.message();return {};}
        auto match=[&](const std::vector<std::unique_ptr<Tab>> &list)->Tab *{for(auto &t:list)if(!t->art->path().empty()&&fs::equivalent(absolute,fs::u8path(t->art->path()),ec))return t.get();return nullptr;};
        auto found=match(tabs);if(!found)found=match(staged_scene_tabs);
        if(found){if(!found->art->dirty()&&!found->art->disk_matches()){load_errors[a]="This open IMG changed on disk. Reopen it or locate another copy.";return {};}return {found->uid,found->art.get()};}
        auto art=Artwork::open(absolute.u8string(),load_errors[a]);if(!art)return {};
        auto t=std::make_unique<Tab>();t->uid=next_uid++;t->art=std::move(art);t->selected_id=t->art->sprite_id(0);t->color=1;t->alignment.initialize(*t->art);sync_alignment(*t);
        SceneBinding result{t->uid,t->art.get()};staged_scene_tabs.push_back(std::move(t));return result;
    };
    bool ok=pending_project->bind(binding,scene_issues);
    for(auto &issue:scene_issues)if(issue.asset>=0&&!load_errors[issue.asset].empty())issue.message=load_errors[issue.asset];
    if(ok){pending_scene_owner=pending_project->state.assets.front().runtime_doc;install_scene();}
}
void open_scene(const std::string &path){
    finish();auto candidate=SceneProject::read(path,error);if(!candidate)return;
    drop_pending_scene();pending_project=std::move(candidate);scene_relocations.resize(pending_project->state.assets.size());scene_error.clear();stage_scene();
}
void choose_scene(){
#ifdef _WIN32
    auto path=native_file(window,false,{},false,true);if(!path.empty())open_scene(path);
#else
    scene_path_dialog=true;scene_path_save=scene_path_export=false;scene_path_uid=0;scene_path_input[0]=0;
#endif
}
bool save_scene(Tab &t,bool as){
    t.project->commit();t.scene.playing=false;std::string path=t.project->path();scene_error.clear();
    if(as||path.empty()){
#ifdef _WIN32
        path=native_file(window,true,path.empty()?"scene.ISTUDIO":path,false,true);if(path.empty())return false;
#else
        scene_path_dialog=true;scene_path_save=true;scene_path_export=false;scene_path_uid=t.uid;snprintf(scene_path_input,sizeof scene_path_input,"%s",path.c_str());return false;
#endif
    }
    bool ok=t.project->save(path,scene_source(),scene_error);if(ok)status="Saved "+scene_name(t);
    else if(!scene_guard_uid)error=scene_error;return ok;
}
void export_scene(Tab &t){
    finish();t.scene.playing=false;scene_error.clear();
#ifdef _WIN32
    auto path=native_file(window,true,"scene.png",true);if(!path.empty()){if(t.project->export_png(path,(int)t.scene.tick,scene_source(),error,t.stage.get(),&t.stage_view))status="Exported current scene PNG.";}
#else
    scene_path_dialog=true;scene_path_save=false;scene_path_export=true;scene_path_uid=t.uid;snprintf(scene_path_input,sizeof scene_path_input,"scene.png");
#endif
}
void save_scene_sources(Tab &t){
    finish();scene_error.clear();std::vector<uint64_t> used;
    auto &s=t.project->state;for(auto &lane:s.lanes)for(auto &frame:lane.frames)for(auto &piece:frame.pieces){uint64_t uid=s.assets[piece.asset].runtime_doc;if(std::find(used.begin(),used.end(),uid)==used.end())used.push_back(uid);}
    for(uint64_t uid:used)if(auto source=find(uid))if(source->art->dirty()||source->art->path().empty())if(!save(*source,false)){scene_error=error;error.clear();return;}
    status="Referenced IMG artwork saved. Save the project to keep scene edits.";
}
void request_close_scene(Tab &t){finish();t.scene.playing=false;if(t.project->dirty()){scene_guard_uid=t.uid;scene_guard_reason=1;scene_error.clear();}else{t.project.reset();t.scene={};}}
void complete_scene_guard(){
    auto owner=find(scene_guard_uid);int reason=scene_guard_reason;
    if(owner){owner->project.reset();owner->scene={};}
    scene_guard_uid=0;scene_guard_reason=0;scene_error.clear();if(reason==3)attach_scene();
}
void project_dialogs(){
    if(quitting&&!scene_guard_uid&&!scene_path_dialog){
        if(pending_project)drop_pending_scene();
        for(auto &t:tabs)if(t->project){if(t->project->dirty()){scene_guard_uid=t->uid;scene_guard_reason=4;scene_error.clear();break;}t->project.reset();t->scene={};}
    }
    if(close_uid&&!scene_guard_uid){auto owner=find(close_uid);
        if(owner&&!quitting){for(auto &t:tabs)if(t.get()!=owner&&t->project&&t->project->uses_doc(owner->uid)){error="This IMG is used by an open project or its undo history. Close that project before closing its source library.";close_uid=0;break;}}
        if(close_uid&&owner&&owner->project){if(owner->project->dirty()){scene_guard_uid=owner->uid;scene_guard_reason=2;scene_error.clear();}else{owner->project.reset();owner->scene={};}}
    }
    if(pending_project&&!scene_issues.empty())ImGui::OpenPopup("Locate project artwork");
    if(ImGui::BeginPopupModal("Locate project artwork",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+550);ImGui::TextUnformatted("The project is waiting for its sources. Locate missing libraries or correct sprite references in their original IMG. Your open artwork stays in place.");ImGui::PopTextWrapPos();
        for(auto &issue:scene_issues)if(issue.asset<0)ImGui::TextWrapped("%s",issue.message.c_str());
        if(pending_project)for(int a=0;a<(int)pending_project->state.assets.size();++a){bool failed=false;for(auto &i:scene_issues)if(i.asset==a)failed=true;if(!failed)continue;
            ImGui::PushID(a);ImGui::Separator();ImGui::TextWrapped("%s",pending_project->state.assets[a].path.c_str());
            for(auto &i:scene_issues)if(i.asset==a)ImGui::TextWrapped("%s",i.message.c_str());
            char replacement[2048];snprintf(replacement,sizeof replacement,"%s",scene_relocations[a].c_str());ImGui::SetNextItemWidth(440);
            if(ImGui::InputText("Replacement IMG",replacement,sizeof replacement))scene_relocations[a]=replacement;
#ifdef _WIN32
            if(ImGui::Button("Locate IMG...")){auto path=native_file(window,false,{});if(!path.empty())scene_relocations[a]=path;}
#endif
            ImGui::PopID();
        }
        if(ImGui::Button("Retry loading",ImVec2(130,0))){ImGui::CloseCurrentPopup();stage_scene();}
        ImGui::SameLine();if(ImGui::Button("Cancel",ImVec2(100,0))){drop_pending_scene();ImGui::CloseCurrentPopup();}scene_sources_cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndPopup();
    }
    if(scene_guard_uid&&!scene_path_dialog&&!path_dialog)ImGui::OpenPopup("Save project changes?");
    if(ImGui::BeginPopupModal("Save project changes?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        auto owner=find(scene_guard_uid);ImGui::Text("Save changes to %s?",owner&&owner->project?scene_name(*owner).c_str():"this project");
        ImGui::TextUnformatted("Scene positions and timing have their own file and undo history.");
        if(!scene_error.empty()){ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+500);ImGui::TextColored(accent_surface(1),"%s",scene_error.c_str());ImGui::PopTextWrapPos();}
        if(ImGui::Button("Save referenced IMGs")&&owner){save_scene_sources(*owner);if(path_dialog)ImGui::CloseCurrentPopup();}
        if(ImGui::Button("Save project",ImVec2(120,0))&&owner){if(save_scene(*owner,false)){complete_scene_guard();ImGui::CloseCurrentPopup();}else if(scene_path_dialog)ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(ImGui::Button("Discard scene",ImVec2(120,0))){complete_scene_guard();ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(ImGui::Button("Cancel",ImVec2(100,0))){if(scene_guard_reason==3)drop_pending_scene();scene_guard_uid=0;scene_guard_reason=0;scene_error.clear();quitting=false;close_uid=0;ImGui::CloseCurrentPopup();}scene_cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndPopup();
    }
    if(scene_path_dialog)ImGui::OpenPopup("Project path");
    if(ImGui::BeginPopupModal("Project path",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        ImGui::SetNextItemWidth(500);ImGui::InputText("Path",scene_path_input,sizeof scene_path_input);
        if(!scene_error.empty())ImGui::TextWrapped("%s",scene_error.c_str());
        if(ImGui::Button(scene_path_save?"Save project":scene_path_export?"Export PNG":"Open project")){
            auto owner=find(scene_path_uid);bool ok=false;std::error_code ec;
            if((scene_path_save||scene_path_export)&&fs::exists(fs::u8path(scene_path_input),ec)&&(!owner||!owner->project||owner->project->path()!=scene_path_input))scene_error="Choose a new filename to avoid replacing another file.";
            else if(scene_path_save&&owner&&owner->project)ok=owner->project->save(scene_path_input,scene_source(),scene_error);
            else if(scene_path_export&&owner&&owner->project)ok=owner->project->export_png(scene_path_input,(int)owner->scene.tick,scene_source(),scene_error,owner->stage.get(),&owner->stage_view);
            else if(!scene_path_save&&!scene_path_export){open_scene(scene_path_input);ok=true;}
            if(ok){scene_path_dialog=false;ImGui::CloseCurrentPopup();if(scene_guard_uid&&scene_path_save)complete_scene_guard();}
        }
        ImGui::SameLine();if(ImGui::Button("Cancel")){scene_path_dialog=false;ImGui::CloseCurrentPopup();}ImGui::EndPopup();
    }
}
void project_controls(Tab &t){
    auto &s=t.project->state;auto &u=t.scene;clamp_scene_ui(t);
    ImGui::Text("%s%s",scene_name(t).c_str(),t.project->dirty()?" *":"");
    ImGui::SetNextItemWidth(190);if(ImGui::BeginCombo("Lane",s.lanes[u.lane].name.c_str())){for(int i=0;i<(int)s.lanes.size();++i){ImGui::PushID(i);if(ImGui::Selectable(s.lanes[i].name.c_str(),u.lane==i)){finish();u.lane=i;u.frame=0;u.playing=false;}ImGui::PopID();}ImGui::EndCombo();}
    ImGui::SameLine();if(ImGui::Button("Fit")){u.zoom=0;u.pan=ImVec2();}
    ImGui::TextDisabled("Drag actors to place them. Scene edits save separately from IMG.");
}
SpritePlacement scene_placement(const IMG &im,const ScenePose &pose,const SceneState &s){return scene_pose_placement(im,pose,s.convention);}
Tab *scene_sprite(const SceneState &s,const SceneSprite &ref,int &index){auto source=find(s.assets[ref.asset].runtime_doc);index=source?source->art->sprite_index(ref.runtime_id):-1;return index>=0?source:nullptr;}
void draw_scene_pose(const SceneState &s,const ScenePose &pose,ImDrawList *dl,ImVec2 stage,float zoom,ImU32 color){
    int index;auto source=scene_sprite(s,pose.sprite,index);if(!source)return;auto im=source->art->sprite(index);auto p=scene_placement(*im,pose,s);
    if(auto tex=texture(*source,index))dl->AddImage((ImTextureID)(intptr_t)tex,stage+ImVec2(p.x*zoom,p.y*zoom),stage+ImVec2((p.x+p.w)*zoom,(p.y+p.h)*zoom),ImVec2(pose.flip_x?1.f:0.f,pose.flip_y?1.f:0.f),ImVec2(pose.flip_x?0.f:1.f,pose.flip_y?0.f:1.f),color);
}
void project_canvas(Tab &t,float height){
    auto &s=t.project->state;auto &u=t.scene;
    ImGui::BeginChild("scene-canvas",ImVec2(0,height),ImGuiChildFlags_Border,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    auto &io=ImGui::GetIO();ImVec2 pos=ImGui::GetCursorScreenPos(),avail=ImMax(ImGui::GetContentRegionAvail(),ImVec2(10,10));
    ImGui::InvisibleButton("##scene-surface",avail,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);bool hovered=ImGui::IsItemHovered();
    float fit=std::max(.1f,std::min((avail.x-24)/s.width,(avail.y-40)/s.height)),z=u.zoom>0?u.zoom:fit;
    if(hovered&&io.MouseWheel&&!u.dragging){u.zoom=std::clamp(z*(io.MouseWheel>0?1.25f:.8f),.1f,32.f);z=u.zoom;}
    if(hovered&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle)&&!u.dragging)u.pan+=io.MouseDelta;
    ImVec2 stage=pos+(avail-ImVec2(s.width*z,s.height*z))*.5f+u.pan,end=stage+ImVec2(s.width*z,s.height*z);
    auto dl=ImGui::GetWindowDrawList();dl->PushClipRect(pos,pos+avail,true);dl->AddRectFilled(pos,pos+avail,IM_COL32(18,21,27,255));dl->AddRectFilled(stage,end,IM_COL32(s.background[0],s.background[1],s.background[2],255));
    dl->PushClipRect(ImMax(pos,stage),ImMin(pos+avail,end),true);
    draw_stage(t,dl,stage,z);
    for(int x=0;x<=s.width;x+=50)dl->AddLine(stage+ImVec2(x*z,0),stage+ImVec2(x*z,s.height*z),IM_COL32(49,55,65,130));
    for(int y=0;y<=s.height;y+=50)dl->AddLine(stage+ImVec2(0,y*z),stage+ImVec2(s.width*z,y*z),IM_COL32(49,55,65,130));
    dl->AddLine(stage+ImVec2(0,s.floor*z),stage+ImVec2(s.width*z,s.floor*z),IM_COL32(172,151,108,220));
    auto poses=evaluate_scene(s,(int)u.tick);
    if(s.onion){int f=scene_frame_at(s,u.lane,(int)u.tick);if(f>0){for(auto &p:evaluate_scene(s,scene_frame_seek_tick(s,u.lane,f-1)))if(p.lane==u.lane&&p.frame==f-1)draw_scene_pose(s,p,dl,stage,z,IM_COL32(167,191,221,55));}}
    bool missing=false;u.probe_zoom=z;
    for(auto &pose:poses){int index;auto source=scene_sprite(s,pose.sprite,index);if(!source){missing=true;continue;}auto im=source->art->sprite(index);auto p=scene_placement(*im,pose,s);
        if(s.ghost_facing&&pose.lane==u.lane){auto ghost=pose;ghost.flip_x=!ghost.flip_x;draw_scene_pose(s,ghost,dl,stage,z,IM_COL32(255,152,163,80));}
        draw_scene_pose(s,pose,dl,stage,z,IM_COL32(255,255,255,pose.alpha));
        if(pose.lane==u.lane&&pose.frame==u.frame){dl->AddRect(stage+ImVec2(p.x*z,p.y*z),stage+ImVec2((p.x+p.w)*z,(p.y+p.h)*z),pose.dual?IM_COL32(235,164,92,190):IM_COL32(226,86,104,190));
            if(u.smoke){bool found=false;for(int y=0;y<im->h&&!found;++y)for(int x=0;x<im->w;++x)if(((unsigned char *)im->data_p)[y*((im->w+3)&~3)+x]){u.probe=stage+ImVec2((p.x+((pose.flip_x?im->w-1-x:x)+.5f)*pose.magnification)*z,(p.y+((pose.flip_y?im->h-1-y:y)+.5f)*pose.magnification)*z);found=true;break;}}
        }
    }
    ImVec2 anchor=stage+ImVec2(s.origin_x*z,s.origin_y*z);dl->AddLine(anchor-ImVec2(8,0),anchor+ImVec2(8,0),IM_COL32(239,193,94,255));dl->AddLine(anchor-ImVec2(0,8),anchor+ImVec2(0,8),IM_COL32(239,193,94,255));
    dl->PopClipRect();dl->AddRect(stage,end,IM_COL32(90,96,106,255));char label[120];snprintf(label,sizeof label,"SCENE  %d x %d  |  %.0f%%  |  tick %d",s.width,s.height,z*100,(int)u.tick);dl->AddText(pos+ImVec2(10,6),IM_COL32(165,176,192,255),label);if(missing)dl->AddText(pos+ImVec2(10,28),IM_COL32(255,100,120,255),"Missing sprite. Replace its frame in the inspector.");dl->PopClipRect();
    if(hovered&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)){float wx=(io.MousePos.x-stage.x)/z,wy=(io.MousePos.y-stage.y)/z;
        for(auto it=poses.rbegin();it!=poses.rend();++it){int index;auto source=scene_sprite(s,it->sprite,index);if(!source||!it->alpha)continue;auto im=source->art->sprite(index);auto p=scene_placement(*im,*it,s);int x=(int)std::floor((wx-p.x)/it->magnification),y=(int)std::floor((wy-p.y)/it->magnification);
            if(x<0||y<0||x>=im->w||y>=im->h)continue;if(it->flip_x)x=im->w-1-x;if(it->flip_y)y=im->h-1-y;
            if(!((unsigned char *)im->data_p)[y*((im->w+3)&~3)+x])continue;
            t.project->commit();t.project->begin("Move actor");u.lane=it->lane;u.frame=it->frame;u.playing=false;u.dragging=true;u.drag_mouse=io.MousePos;u.drag_x=s.lanes[u.lane].x;u.drag_y=s.lanes[u.lane].y;break;
        }
    }
    if(u.dragging&&ImGui::IsMouseDown(ImGuiMouseButton_Left)){auto &lane=s.lanes[u.lane];lane.x=std::clamp(u.drag_x+(int)std::round((io.MousePos.x-u.drag_mouse.x)/z),-4096,4096);lane.y=std::clamp(u.drag_y+(int)std::round((io.MousePos.y-u.drag_mouse.y)/z),-4096,4096);}
    if(u.dragging&&ImGui::IsMouseReleased(ImGuiMouseButton_Left)){t.project->commit();u.dragging=false;status="Moved scene actor. IMG anchors are unchanged.";}
    if(hovered)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);ImGui::EndChild();
}
void project_timeline(Tab &t,float height){
    auto &s=t.project->state;auto &u=t.scene;auto &lane=s.lanes[u.lane];
    ImGui::BeginChild("scene-timeline",ImVec2(0,height),ImGuiChildFlags_Border);
    if(ImGui::Button(u.playing?"Pause":"Play"))toggle_scene_playback(t);
    ImGui::SameLine();if(ImGui::Button("<"))scene_step(t,-1);ImGui::SameLine();if(ImGui::Button(">"))scene_step(t,1);ImGui::SameLine();ImGui::Checkbox("Loop scene",&s.loop);
    int tick=(int)u.tick;ImGui::SetNextItemWidth(-1);if(ImGui::SliderInt("##scene-tick",&tick,0,scene_last_tick(s),"Tick %d")){u.tick=tick;u.playing=false;int frame=scene_frame_at(s,u.lane,tick);if(frame>=0)u.frame=frame;}
    if(u.playing){int frame=scene_frame_at(s,u.lane,(int)u.tick);if(frame>=0)u.frame=frame;}
    auto &current=lane.frames[u.frame];ImGui::SetNextItemWidth(80);if(ImGui::InputInt("Ticks/frame",&current.ticks)){current.ticks=std::clamp(current.ticks,1,255);u.playing=false;}
    ImGui::SameLine();if(ImGui::Button("Copy frame"))u.command=SceneCommand::DuplicateFrame;
    ImGui::SameLine();ImGui::BeginDisabled(u.frame==0);if(ImGui::Button("Move <"))u.command=SceneCommand::MoveLeft;ImGui::EndDisabled();
    ImGui::SameLine();ImGui::BeginDisabled(u.frame+1==(int)lane.frames.size());if(ImGui::Button("Move >"))u.command=SceneCommand::MoveRight;ImGui::EndDisabled();
    ImGui::BeginChild("scene-frame-strip",ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
    int lane_start=scene_lane_start(s,u.lane),cursor_tick=0;
    for(int f=0;f<(int)lane.frames.size();++f){int seek=lane_start+(lane.frames[f].scheduled()?lane.frames[f].visible_from:cursor_tick);cursor_tick+=lane.frames[f].ticks;if(f)ImGui::SameLine(0,6);ImGui::PushID(f);ImVec2 a=ImGui::GetCursorScreenPos();
        if(ImGui::Selectable("##frame",u.frame==f,0,ImVec2(85,85)))scene_seek(t,f);
        auto &frame=lane.frames[f];int left=INT_MAX,top=INT_MAX,right=INT_MIN,bottom=INT_MIN;std::vector<ScenePose> poses;
        for(auto &ref:frame.pieces){ScenePose pose{ref,0,0,0,255,u.lane,f,lane.flip_x!=frame.flip_x,frame.flip_y,false,lane.magnification};int index;if(auto source=scene_sprite(s,ref,index)){auto p=scene_placement(*source->art->sprite(index),pose,s);left=std::min(left,p.x);top=std::min(top,p.y);right=std::max(right,p.x+p.w);bottom=std::max(bottom,p.y+p.h);}poses.push_back(pose);}
        auto dl=ImGui::GetWindowDrawList();dl->PushClipRect(a,a+ImVec2(85,85),true);
        if(right>left&&bottom>top){float z=std::min(68.f/(right-left),42.f/(bottom-top));ImVec2 origin=a+ImVec2((85-(right-left)*z)*.5f,(48-(bottom-top)*z)*.5f)-ImVec2(left*z,top*z);for(auto &p:poses)draw_scene_pose(s,p,dl,origin,z,IM_COL32(255,255,255,255));}
        dl->AddText(a+ImVec2(5,49),ImGui::GetColorU32(ImGuiCol_Text),frame.name.c_str());char text[40];if(frame.scheduled())snprintf(text,sizeof text,"@%d%s",frame.visible_from,frame.dual?"  2 copies":"");else snprintf(text,sizeof text,"%d ticks%s",frame.ticks,frame.dual?"  x2":"");dl->AddText(a+ImVec2(5,67),ImGui::GetColorU32(ImGuiCol_TextDisabled),text);dl->PopClipRect();
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s | %d pieces | scene tick %d\nMotion %d, %d pixels/tick | %s",frame.name.c_str(),(int)frame.pieces.size(),seek,frame.vx,frame.vy,frame.scheduled()?"Scheduled entry: may overlap other frames":"Sequential entry");ImGui::PopID();
    }ImGui::EndChild();ImGui::EndChild();
}
void project_inspector(Tab &t){
    auto &s=t.project->state;auto &u=t.scene;clamp_scene_ui(t);auto &lane=s.lanes[u.lane];auto &frame=lane.frames[u.frame];
    if(u.motion_smoke&&ImGui::GetWindowHeight()<650)ImGui::SetScrollY(300);
    if(u.subframe_smoke&&ImGui::GetWindowHeight()<650)ImGui::SetScrollY(220);
    ImGui::TextUnformatted("SCENE ACTOR");ImGui::Separator();char name[129];snprintf(name,sizeof name,"%s",lane.name.c_str());ImGui::SetNextItemWidth(-1);if(ImGui::InputText("##lane-name",name,sizeof name))lane.name=name;
    int xy[]={lane.x,lane.y};ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##lane-position",xy)){lane.x=std::clamp(xy[0],-4096,4096);lane.y=std::clamp(xy[1],-4096,4096);}ImGui::TextDisabled("Actor offset X / Y");
    ImGui::Checkbox("Visible",&lane.visible);ImGui::SameLine();ImGui::Checkbox("Facing left",&lane.flip_x);
    ImGui::Checkbox("Loop actor",&lane.loop);ImGui::SameLine();ImGui::Checkbox("Hold end",&lane.hold_end);
    ImGui::SetNextItemWidth(100);if(ImGui::InputInt(lane.trigger_lane?"Delay after source":"Start tick",&lane.delay))lane.delay=std::clamp(lane.delay,0,100000);ImGui::SetNextItemWidth(-1);ImGui::SliderInt("Opacity",&lane.alpha,0,255);
    const char *scales[]={"1x","2x","3x","4x"};int scale=lane.magnification-1;ImGui::SetNextItemWidth(90);
    if(ImGui::BeginCombo("Magnification",scales[scale])){for(int i=0;i<4;++i){if(ImGui::Selectable(scales[i],i==scale)){lane.magnification=i+1;u.playing=false;}u.magnification_choices[i]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;}ImGui::EndCombo();}u.magnification_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SetItemTooltip("Scales this actor around its origin, including frame offsets and motion. Preview and PNG only; IMG pixels and anchors stay unchanged.");
    project_trigger_controls(t,lane);
    ImGui::Spacing();ImGui::Text("FRAME %d / %d",u.frame+1,(int)lane.frames.size());ImGui::TextWrapped("%s",frame.name.c_str());
    int offset[]={frame.x,frame.y};ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##frame-offset",offset)){frame.x=std::clamp(offset[0],-4096,4096);frame.y=std::clamp(offset[1],-4096,4096);}ImGui::TextDisabled("Frame offset X / Y");
    ImGui::SetNextItemWidth(100);if(ImGui::InputInt("Depth",&frame.z))frame.z=std::clamp(frame.z,-4096,4096);ImGui::Checkbox("Flip frame X",&frame.flip_x);ImGui::SameLine();ImGui::Checkbox("Flip Y",&frame.flip_y);
    project_motion_controls(t,lane,frame);
    if(ImGui::Button("Swap with subframes..."))show_subframe_swap(t);u.subframe_swap_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(ImGui::Button("Cut at waterline..."))show_waterline(t);u.waterline_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    bool frame_used=scene_uses_frame(s,frame.id);ImGui::BeginDisabled(lane.frames.size()==1||frame_used);if(ImGui::Button("Remove frame"))u.command=SceneCommand::DeleteFrame;ImGui::EndDisabled();if(frame_used&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("An effect follows this frame. Detach the effect before removing its source.");
    ImGui::Spacing();ImGui::SeparatorText("Add artwork");
    if(ImGui::Button("Build dummy body..."))show_dummy_body(t);u.dummy_body_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    auto source=find(u.source_doc);if(!source){source=&t;u.source_doc=t.uid;u.source_sprite=t.selected_id;}
    ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##source-library",source->name().c_str())){for(auto &candidate:tabs){ImGui::PushID((int)candidate->uid);if(ImGui::Selectable(candidate->name().c_str(),candidate.get()==source)){u.source_doc=candidate->uid;u.source_sprite=candidate->selected_id;}ImGui::PopID();}ImGui::EndCombo();}
    source=find(u.source_doc);int index=source->art->sprite_index(u.source_sprite);if(index<0){index=source->selected;u.source_sprite=source->art->sprite_id(index);}
    ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##source-sprite",source->art->sprite(index)->n_s)){for(int i=0;i<source->art->count();++i){ImGui::PushID(i);if(ImGui::Selectable(source->art->sprite(i)->n_s,i==index))u.source_sprite=source->art->sprite_id(i);ImGui::PopID();}ImGui::EndCombo();}
    if(ImGui::Button("Add frame"))u.command=SceneCommand::AddFrame;ImGui::SameLine();if(ImGui::Button("Replace frame"))u.command=SceneCommand::ReplaceFrame;
    ImGui::BeginDisabled(s.lanes.size()>=32);if(ImGui::Button("Add actor lane"))u.command=SceneCommand::AddLane;ImGui::EndDisabled();ImGui::SameLine();bool lane_used=scene_uses_lane(s,lane.id);ImGui::BeginDisabled(s.lanes.size()==1||lane_used);if(ImGui::Button("Remove lane"))u.command=SceneCommand::RemoveLane;ImGui::EndDisabled();if(lane_used&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("An effect follows this actor. Detach it before removing the source actor.");
    if(ImGui::CollapsingHeader("Playfield & preview")){
        if(ImGui::Button("Stage reference..."))show_stage_dialog(t);
        int size[]={s.width,s.height};ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##scene-size",size)){s.width=std::clamp(size[0],64,2048);s.height=std::clamp(size[1],64,2048);s.floor=std::min(s.floor,s.height);u.zoom=0;}ImGui::TextDisabled("Playfield width / height");
        int origin[]={s.origin_x,s.origin_y};ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##scene-origin",origin)){s.origin_x=std::clamp(origin[0],-4096,4096);s.origin_y=std::clamp(origin[1],-4096,4096);}ImGui::TextDisabled("Anchor origin X / Y");
        ImGui::SetNextItemWidth(100);if(ImGui::InputInt("Floor",&s.floor))s.floor=std::clamp(s.floor,0,s.height);
        ImGui::SetNextItemWidth(100);if(ImGui::InputDouble("Tick Hz",&s.tick_hz,0,0,"%.4f")){if(!std::isfinite(s.tick_hz))s.tick_hz=mk2_tick_hz;s.tick_hz=std::clamp(s.tick_hz,1.,240.);}
        ImGui::Checkbox("Onion skin",&s.onion);ImGui::Checkbox("Other facing ghost",&s.ghost_facing);
        float background[]={s.background[0]/255.f,s.background[1]/255.f,s.background[2]/255.f};if(ImGui::ColorEdit3("Background",background,ImGuiColorEditFlags_NoInputs))for(int c=0;c<3;++c)s.background[c]=(int)std::round(std::clamp(background[c],0.f,1.f)*255);
        int convention=s.convention;ImGui::SetNextItemWidth(-1);if(ImGui::Combo("##scene-mirror",&convention,"Ani2 (w - x)\0ganiof (w - 1 - x)\0"))s.convention=(MirrorConvention)convention;
    }
    ImGui::Spacing();if(ImGui::Button("Save referenced IMGs")){save_scene_sources(t);if(!scene_error.empty())error=scene_error;}ImGui::TextWrapped("Save IMG artwork first, then save the project. Project undo changes composition; Artwork undo changes sprites.");
}
void apply_scene_command(Tab &t){
    auto &u=t.scene;auto command=u.command;u.command=SceneCommand::None;if(command==SceneCommand::None||!t.project)return;
    t.project->commit();t.project->begin("Edit scene frames");u.playing=false;auto &s=t.project->state;
    auto &lane=s.lanes[u.lane];auto source=find(u.source_doc);int index=source?source->art->sprite_index(u.source_sprite):-1;
    auto make_frame=[&](){SceneFrame f;f.id=t.project->allocate_id();f.name=source->art->sprite(index)->n_s;f.pieces.push_back(scene_sprite_reference(*source->art,scene_asset(s,*source),index));return f;};
    bool known_source=false;if(source)for(auto &a:s.assets)if(a.runtime_doc==source->uid)known_source=true;
    if((command==SceneCommand::AddFrame||command==SceneCommand::ReplaceFrame||command==SceneCommand::AddLane)&&(!source||index<0||(!known_source&&s.assets.size()>=64))){t.project->cancel();error="Choose an available source sprite (at most 64 IMG libraries).";return;}
    if(command==SceneCommand::DuplicateFrame||command==SceneCommand::AddFrame){if(lane.frames.size()>=4096){t.project->cancel();error="A lane can hold at most 4096 frames.";return;}auto f=command==SceneCommand::DuplicateFrame?lane.frames[u.frame]:make_frame();if(command==SceneCommand::DuplicateFrame)f.id=t.project->allocate_id();lane.frames.insert(lane.frames.begin()+u.frame+1,std::move(f));++u.frame;}
    else if(command==SceneCommand::ReplaceFrame){auto replacement=make_frame();lane.frames[u.frame].name=std::move(replacement.name);lane.frames[u.frame].pieces=std::move(replacement.pieces);}
    else if(command==SceneCommand::MoveLeft&&u.frame>0){std::swap(lane.frames[u.frame],lane.frames[u.frame-1]);--u.frame;}
    else if(command==SceneCommand::MoveRight&&u.frame+1<(int)lane.frames.size()){std::swap(lane.frames[u.frame],lane.frames[u.frame+1]);++u.frame;}
    else if(command==SceneCommand::DeleteFrame&&lane.frames.size()>1){if(scene_uses_frame(s,lane.frames[u.frame].id)){t.project->cancel();error="Detach effects following this frame before deleting it.";return;}lane.frames.erase(lane.frames.begin()+u.frame);}
    else if(command==SceneCommand::RemoveLane&&s.lanes.size()>1){if(scene_uses_lane(s,lane.id)){t.project->cancel();error="Detach effects following this actor before deleting it.";return;}s.lanes.erase(s.lanes.begin()+u.lane);u.frame=0;}
    else if(command==SceneCommand::AddLane&&s.lanes.size()<32){SceneLane added;added.id=t.project->allocate_id();added.name=source->art->sprite(index)->n_s;added.frames.push_back(make_frame());s.lanes.push_back(std::move(added));u.lane=(int)s.lanes.size()-1;u.frame=0;}
    else if(command==SceneCommand::FollowFrame){if(!scene_set_trigger(s,u.lane,u.trigger_source_lane,u.trigger_source_frame,error)){t.project->cancel();return;}}
    else if(command==SceneCommand::DetachTrigger){if(!scene_detach_trigger(s,u.lane,error)){t.project->cancel();return;}}
    std::string why;if(!validate_scene(s,why)){t.project->cancel();error=why;}else t.project->commit();clamp_scene_ui(t);u.tick=scene_frame_seek_tick(s,u.lane,u.frame);
}
void project_motion_controls(Tab &t,SceneLane &lane,SceneFrame &frame){
    if(t.scene.motion_smoke)ImGui::SetNextItemOpen(true,ImGuiCond_Always);
    if(!ImGui::CollapsingHeader("Motion & timing"))return;
    bool changed=false;
    changed|=ImGui::Checkbox("Independent timing",&frame.independent_timing);
    ImGui::SetItemTooltip("Schedules this frame independently, including an open-ended start at actor tick zero. Show/Hide also enable scheduling.");
    ImGui::TextDisabled("Frame visibility (actor ticks)");
    ImGui::SetNextItemWidth(105);if(ImGui::InputInt("Show at",&frame.visible_from)){frame.visible_from=std::clamp(frame.visible_from,0,99999);changed=true;}
    ImGui::SetItemTooltip("Starts at this actor tick, inclusive. A nonzero Show or Hide schedules the entry independently of the frame sequence.");
    ImGui::SetNextItemWidth(105);if(ImGui::InputInt("Hide at",&frame.visible_until)){frame.visible_until=std::clamp(frame.visible_until,0,99999);changed=true;}
    ImGui::SetItemTooltip("Hidden at this tick and later. Zero means no end. Show=0 and Hide=0 with Independent timing off restores sequential playback.");
    int actor_now=std::clamp((int)t.scene.tick-scene_lane_start(t.project->state,t.scene.lane),0,99999);if(lane.stop_tick>0)actor_now=std::min(actor_now,lane.stop_tick);
    if(ImGui::SmallButton("Show here")){frame.visible_from=actor_now;frame.independent_timing=true;changed=true;}t.scene.show_here_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();
    if(ImGui::SmallButton("Hide here")){frame.visible_until=actor_now;changed=true;}ImGui::SameLine();
    if(ImGui::SmallButton("Clear")){frame.visible_from=frame.visible_until=0;frame.independent_timing=false;changed=true;}
    if(frame.visible_until&&frame.visible_until<=frame.visible_from)ImGui::TextColored(accent_surface(1),"Empty window: this entry stays hidden.");
    int velocity[]={frame.vx,frame.vy};ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##frame-speed",velocity)){frame.vx=std::clamp(velocity[0],-128,128);frame.vy=std::clamp(velocity[1],-128,128);changed=true;}
    ImGui::TextDisabled("Motion X / Y (pixels per tick)");ImGui::SetItemTooltip("Positive X moves right; positive Y moves down. Actor magnification scales this motion and maximum travel. Motion starts at Show at for scheduled entries, or at the first frame tick otherwise. Actor loops keep motion running; scene loops restart it.");
    int caps[]={frame.cap_x,frame.cap_y};ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##frame-travel",caps)){frame.cap_x=std::clamp(caps[0],0,99999);frame.cap_y=std::clamp(caps[1],0,99999);changed=true;}
    ImGui::TextDisabled("Maximum travel X / Y (0 = unlimited)");
    ImGui::SetNextItemWidth(105);if(ImGui::InputInt("Freeze actor at",&lane.stop_tick)){lane.stop_tick=std::clamp(lane.stop_tick,0,99999);changed=true;}
    ImGui::SetItemTooltip("Freezes this actor's clock and motion at the chosen actor tick. Zero disables it. Other actors continue playing.");
    if(ImGui::Checkbox("Second copy",&frame.dual))changed=true;
    if(frame.dual){int xy[]={frame.dual_x,frame.dual_y};ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##second-offset",xy)){frame.dual_x=std::clamp(xy[0],-4096,4096);frame.dual_y=std::clamp(xy[1],-4096,4096);changed=true;}
        ImGui::TextDisabled("Second-copy offset X / Y");ImGui::SetItemTooltip("Absolute offset from the actor origin, independent of the primary frame offset. Both copies share motion, facing and visibility.");
        ImGui::SetNextItemWidth(105);if(ImGui::InputInt("Second depth",&frame.dual_z)){frame.dual_z=std::clamp(frame.dual_z,-4096,4096);changed=true;}
    }
    if(changed)t.scene.playing=false;
}
void project_trigger_controls(Tab &t,SceneLane &lane){
    auto &s=t.project->state;auto &u=t.scene;
    if(u.trigger_smoke)ImGui::SetNextItemOpen(true,ImGuiCond_Always);
    if(!ImGui::CollapsingHeader("Follow frame timing"))return;
    if(lane.trigger_lane){for(auto &parent:s.lanes)if(parent.id==lane.trigger_lane)for(size_t f=0;f<parent.frames.size();++f)if(parent.frames[f].id==lane.trigger_frame)ImGui::TextWrapped("Follows %s / frame %d (%s)",parent.name.c_str(),(int)f+1,parent.frames[f].name.c_str());}
    ImGui::TextDisabled("Starts at scene tick %d",scene_lane_start(s,u.lane));
    SceneLane *selected=nullptr;for(auto &parent:s.lanes)if(parent.id==u.trigger_source_lane&&parent.id!=lane.id)selected=&parent;
    if(!selected){for(auto &parent:s.lanes)if(parent.id==lane.trigger_lane&&parent.id!=lane.id)selected=&parent;if(!selected)for(auto &parent:s.lanes)if(parent.id!=lane.id){selected=&parent;break;}if(selected){u.trigger_source_lane=selected->id;u.trigger_source_frame=lane.trigger_frame;}}
    if(selected){
        ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##trigger-actor",selected->name.c_str())){for(auto &parent:s.lanes)if(parent.id!=lane.id){ImGui::PushID((int)parent.id);if(ImGui::Selectable(parent.name.c_str(),parent.id==selected->id)){u.trigger_source_lane=parent.id;u.trigger_source_frame=parent.frames.front().id;}ImGui::PopID();}ImGui::EndCombo();}
        for(auto &parent:s.lanes)if(parent.id==u.trigger_source_lane)selected=&parent;
        int chosen=0;for(int f=0;f<(int)selected->frames.size();++f)if(selected->frames[f].id==u.trigger_source_frame)chosen=f;u.trigger_source_frame=selected->frames[chosen].id;
        auto frame_label=[&](int f){return std::to_string(f+1)+"  "+selected->frames[f].name;};ImGui::SetNextItemWidth(-1);
        if(ImGui::BeginCombo("##trigger-frame",frame_label(chosen).c_str())){for(int f=0;f<(int)selected->frames.size();++f){ImGui::PushID((int)selected->frames[f].id);if(ImGui::Selectable(frame_label(f).c_str(),f==chosen))u.trigger_source_frame=selected->frames[f].id;ImGui::PopID();}ImGui::EndCombo();}
        if(ImGui::Button("Follow frame"))u.command=SceneCommand::FollowFrame;u.follow_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
        ImGui::SetItemTooltip("Uses the source frame's first Show/sequence tick, plus this actor's delay. Reordering and retiming the source keep the same frame binding. This follows timing, not position or visibility.");
    }else ImGui::TextWrapped("Add another actor lane to choose a source frame.");
    if(lane.trigger_lane){if(selected)ImGui::SameLine();if(ImGui::Button("Detach"))u.command=SceneCommand::DetachTrigger;u.detach_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SetItemTooltip("Keeps the current start tick and turns this into an independent actor.");}
}
