SceneSource append_scene_source(){return [this](uint64_t uid)->Artwork *{if(auto t=find(uid))return t->art.get();for(auto &t:staged_scene_tabs)if(t->uid==uid)return t->art.get();return nullptr;};}
void append_scene(Tab &owner,const std::string &path){
    if(!owner.project)return;finish();owner.scene.playing=false;auto incoming=SceneProject::read(path,error);if(!incoming)return;
    drop_pending_scene();pending_project=std::move(incoming);pending_scene_append=true;pending_scene_owner=owner.uid;scene_relocations.resize(pending_project->state.assets.size());scene_error.clear();stage_scene();
}
void choose_append_scene(Tab &owner){
#ifdef _WIN32
    auto path=native_file(window,false,{},false,true);if(!path.empty())append_scene(owner,path);
#else
    scene_path_dialog=true;scene_path_save=scene_path_export=false;scene_path_append=true;scene_path_uid=owner.uid;scene_path_input[0]=0;
#endif
}
void apply_scene_append(){
    auto owner=find(pending_scene_owner);if(!pending_scene_append||!pending_append_ready||!pending_project||!owner||!owner->project){scene_error="The append target or reviewed sources are no longer available.";return;}
    try{
        auto sources=append_scene_source();for(auto &asset:pending_project->state.assets){auto art=sources(asset.runtime_doc);if(!art||(!art->dirty()&&!art->disk_matches())){scene_error="A reviewed IMG changed on disk or is unavailable. Cancel and reopen the append review.";return;}}
        // Prepare tab publication and UI strings before the model commits.
        tabs.reserve(tabs.size()+staged_scene_tabs.size());std::string done="Appended "+std::to_string(pending_project->state.lanes.size())+" actors. Save the current .ISTUDIO project to keep them.";
        owner->project->commit();int added=owner->project->append_actors(pending_project->state,append_scene_source(),scene_error);if(added<0)return;
        for(auto &t:staged_scene_tabs)tabs.push_back(std::move(t));staged_scene_tabs.clear();
        owner->embedded.enabled=owner->embedded.playing=false;owner->alignment.enabled=true;owner->alignment.player.playing=false;owner->scene.playing=false;owner->scene.lane=added;owner->scene.frame=0;clamp_scene_ui(*owner);
        for(int i=0;i<(int)tabs.size();++i)if(tabs[i].get()==owner)active=i;select_uid=owner->uid;status=std::move(done);drop_pending_scene();scene_error.clear();
    }catch(const std::bad_alloc &){scene_error="Not enough memory to append actors. The current project and open artwork remain in place.";}
}
void scene_append_dialog(){
    if(!pending_scene_append||!pending_append_ready||!pending_project)return;auto owner=find(pending_scene_owner);if(!owner||!owner->project){drop_pending_scene();return;}
    ImGui::OpenPopup("Append project actors");auto display=ImGui::GetIO().DisplaySize;ImGui::SetNextWindowSize(ImVec2(std::min(720.f,display.x-50),std::min(610.f,display.y-60)),ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal("Append project actors",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    auto &incoming=pending_project->state;auto &current=owner->project->state;
    ImGui::TextWrapped("Append %s to %s",fs::u8path(pending_project->opened_from()).filename().u8string().c_str(),scene_name(*owner).c_str());
    size_t frames=0,pieces=0;for(auto &l:incoming.lanes){frames+=l.frames.size();for(auto &f:l.frames)pieces+=f.pieces.size();}
    size_t all_frames=frames,all_pieces=pieces;for(auto &l:current.lanes){all_frames+=l.frames.size();for(auto &f:l.frames)all_pieces+=f.pieces.size();}
    size_t source_slots=current.assets.size();std::set<uint64_t> sources;for(auto &a:current.assets)sources.insert(a.runtime_doc);for(auto &a:incoming.assets)if(sources.insert(a.runtime_doc).second)++source_slots;
    bool room=current.lanes.size()+incoming.lanes.size()<=32&&all_frames<=8192&&all_pieces<=65536&&source_slots<=64;
    ImGui::Text("%zu actors / %zu frames / %zu pieces",incoming.lanes.size(),frames,pieces);ImGui::Text("%zu IMG libraries reused / %zu new tabs",incoming.assets.size()-staged_scene_tabs.size(),staged_scene_tabs.size());
    ImGui::TextWrapped("The current playfield, origin, background, clock and mirror convention remain in use. Imported actor coordinates and tick counts stay as authored.");
    ImGui::TextDisabled("Current: %dx%d / %.3f Hz / origin %d, %d",current.width,current.height,current.tick_hz,current.origin_x,current.origin_y);ImGui::TextDisabled("Incoming: %dx%d / %.3f Hz / origin %d, %d",incoming.width,incoming.height,incoming.tick_hz,incoming.origin_x,incoming.origin_y);
    ImGui::TextWrapped("Actors receive fresh identities; links between incoming effects stay together. Your existing actors, marks and IMG artwork stay in place. One Undo removes the append.");
    ImGui::BeginChild("append-actor-list",ImVec2(0,std::max(90.f,ImGui::GetContentRegionAvail().y-110)),ImGuiChildFlags_Border);
    for(auto &l:incoming.lanes){ImGui::TextWrapped("%s / %zu frames / offset %d, %d%s",l.name.c_str(),l.frames.size(),l.x,l.y,l.trigger_lane?" / linked timing":"");}
    for(auto &note:pending_project->import_notes()){ImGui::Spacing();ImGui::TextWrapped("WAX: %s",note.c_str());}ImGui::EndChild();
    if(!room)ImGui::TextWrapped("The complete append exceeds the 32-actor, 64-source, 8192-frame or 65536-piece limits. No actors will be skipped.");
    if(!scene_error.empty())ImGui::TextWrapped("%s",scene_error.c_str());
    ImGui::BeginDisabled(!room);bool apply=ImGui::Button("Append actors",ImVec2(130,0));scene_append_apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    bool cancel=ImGui::Button("Cancel",ImVec2(100,0))||(!ImGui::GetIO().WantTextInput&&ImGui::IsKeyPressed(ImGuiKey_Escape));scene_append_cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(cancel){drop_pending_scene();ImGui::CloseCurrentPopup();}else if(apply){apply_scene_append();if(!pending_scene_append)ImGui::CloseCurrentPopup();}ImGui::EndPopup();
}
