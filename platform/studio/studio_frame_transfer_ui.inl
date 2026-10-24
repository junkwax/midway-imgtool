void show_frame_transfer(Tab &t,int frame){
    if(!can_edit_actor(&t))return;finish();t.scene.playing=false;
    try{auto &s=t.project->state;auto &actor=s.lanes[t.scene.lane];if(frame<0||frame>=(int)actor.frames.size())return;
        FrameTransferUi next;next.open=true;next.owner=t.uid;next.source=actor.id;next.destination=s.lanes[(t.scene.lane+1)%s.lanes.size()].id;next.selected.insert(actor.frames[frame].id);next.before=std::make_unique<SceneState>(s);
        for(auto &l:s.lanes)if(l.id==next.destination)next.insertion=(int)l.frames.size();frame_transfer=std::move(next);
    }catch(const std::bad_alloc &){error="Not enough memory to review frame transfers. The composition is unchanged.";}
}
void apply_frame_transfer(){
    auto &u=frame_transfer;auto t=find(u.owner);if(!u.open||!u.before||!t||!t->project){u.error="The reviewed composition is no longer available.";return;}
    try{std::vector<uint64_t> picks(u.selected.begin(),u.selected.end());std::string done=u.move?"Moved actor frames. Save the project to keep the transfer.":"Copied actor frames. Save the project to keep the transfer.";
        t->project->commit();int at=t->project->transfer_frames(u.source,picks,u.destination,u.insertion,u.move,u.error,u.before.get());if(at<0)return;
        auto &s=t->project->state;for(int i=0;i<(int)s.lanes.size();++i)if(s.lanes[i].id==u.destination)t->scene.lane=i;t->scene.frame=at;t->scene.playing=false;t->scene.tick=scene_frame_seek_tick(s,t->scene.lane,at);clamp_scene_ui(*t);status=std::move(done);u.open=false;u.before.reset();
    }catch(const std::bad_alloc &){u.error="Not enough memory to transfer frames. Retry or cancel the review.";}
}
void frame_transfer_dialog(){
    auto &u=frame_transfer;if(!u.open||!u.before)return;auto owner=find(u.owner);if(!owner||!owner->project){u=FrameTransferUi{};return;}
    auto &s=*u.before;int src=-1,dst=-1;for(int i=0;i<(int)s.lanes.size();++i){if(s.lanes[i].id==u.source)src=i;if(s.lanes[i].id==u.destination)dst=i;}if(src<0||dst<0){u=FrameTransferUi{};return;}
    ImGui::OpenPopup("Copy / move actor frames");auto display=ImGui::GetIO().DisplaySize;ImGui::SetNextWindowSize(ImVec2(std::min(720.f,display.x-50),std::min(620.f,display.y-50)),ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal("Copy / move actor frames",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    ImGui::TextWrapped("Source: %s / %zu frames selected",s.lanes[src].name.c_str(),u.selected.size());
    ImGui::Checkbox("Move frames (otherwise copy)",&u.move);u.move_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##transfer-destination",s.lanes[dst].name.c_str())){for(int i=0;i<(int)s.lanes.size();++i){ImGui::PushID(i);if(ImGui::Selectable(s.lanes[i].name.c_str(),i==dst)){u.destination=s.lanes[i].id;u.insertion=(int)s.lanes[i].frames.size();dst=i;}if(i<32)u.destination_choices[i]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::PopID();}ImGui::EndCombo();}u.destination_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::TextDisabled("Destination actor");
    auto &destination=s.lanes[dst];int n=(int)destination.frames.size();u.insertion=std::clamp(u.insertion,0,n);std::string label=u.insertion==n?"At end":"Before "+std::to_string(u.insertion+1)+": "+destination.frames[u.insertion].name;
    ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##transfer-insertion",label.c_str())){for(int i=0;i<=n;++i){auto item=i==n?"At end":"Before "+std::to_string(i+1)+": "+destination.frames[i].name;ImGui::PushID(i);if(ImGui::Selectable(item.c_str(),u.insertion==i))u.insertion=i;if(i<5)u.insertion_choices[i]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::PopID();}ImGui::EndCombo();}u.insertion_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::TextDisabled("Insertion point in the original destination list");
    ImGui::TextWrapped("Frames keep their holds, Show/Hide windows, offsets, depth, motion, flips and multipart IMG references. They use the destination actor's placement, facing, scale and clock.");
    ImGui::TextWrapped(u.move?"Effects following moved frames follow those frames in the destination; their start time can change. Cycles are refused.":"Copies receive new identities. Existing effects keep following the originals.");
    if(ImGui::SmallButton("Select all"))for(auto &f:s.lanes[src].frames)u.selected.insert(f.id);u.all_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();if(ImGui::SmallButton("Clear selection"))u.selected.clear();u.clear_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::BeginChild("transfer-frame-list",ImVec2(0,std::max(90.f,ImGui::GetContentRegionAvail().y-100)),ImGuiChildFlags_Border);
    for(int i=0;i<(int)s.lanes[src].frames.size();++i){auto &f=s.lanes[src].frames[i];bool selected=u.selected.count(f.id)!=0;ImGui::PushID(i);if(ImGui::Checkbox("##transfer-pick",&selected)){if(selected)u.selected.insert(f.id);else u.selected.erase(f.id);}if(i<8)u.pick_points[i]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();ImGui::TextWrapped("%d. %s / %d ticks / %zu pieces%s",i+1,f.name.c_str(),f.ticks,f.pieces.size(),f.scheduled()?" / scheduled":"");ImGui::PopID();}ImGui::EndChild();
    bool empty_source=u.move&&src!=dst&&u.selected.size()==s.lanes[src].frames.size();if(empty_source)ImGui::TextWrapped("Leave at least one source frame, or use Copy. Empty actors are not stored in Studio projects.");
    if(!u.error.empty())ImGui::TextWrapped("%s",u.error.c_str());
    ImGui::BeginDisabled(u.selected.empty()||empty_source);bool apply=ImGui::Button(u.move?"Move selected frames":"Copy selected frames",ImVec2(180,0));u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();bool cancel=ImGui::Button("Cancel",ImVec2(100,0))||(!ImGui::GetIO().WantTextInput&&ImGui::IsKeyPressed(ImGuiKey_Escape));u.cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(cancel){u.open=false;u.before.reset();ImGui::CloseCurrentPopup();}else if(apply){apply_frame_transfer();if(!u.open)ImGui::CloseCurrentPopup();}ImGui::EndPopup();
}
