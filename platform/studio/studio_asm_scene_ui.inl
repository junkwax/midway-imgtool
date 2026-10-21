std::vector<AsmLibrary> asm_scene_libraries(){std::vector<AsmLibrary> libraries;for(auto &t:tabs)libraries.push_back({t->uid,t->art.get()});return libraries;}
void rebuild_asm_scene(){auto &u=asm_ui;auto &v=u.scene_review;auto animation=asm_animation();v.valid=animation&&capture_asm_scene(*animation,asm_scene_libraries(),u.preferred,u.assets.choices,v.options,v.snapshot,v.error);v.dirty=false;}
void open_asm_scene(){finish();auto &u=asm_ui;auto &v=u.scene_review;auto owner=find(u.preferred);if(!owner)owner=tab();v=AsmUi::SceneReview{};v.open=true;v.owner=owner?owner->uid:0;v.options.hold=u.hold;v.options.origin_x=u.origin_x;v.options.origin_y=u.origin_y;v.options.facing=u.facing;v.options.loop=u.player.loop;if(owner)v.options.convention=owner->convention;u.player.playing=false;rebuild_asm_scene();}
void asm_scene_dialog(){auto &u=asm_ui;auto &v=u.scene_review;if(!v.open)return;
    ImGui::OpenPopup("Create scene from ASM");auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(870.f,display.x-50),std::min(680.f,display.y-60)),ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal("Create scene from ASM",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    ImGui::TextUnformatted("Create an editable composition from the inspected frame table.");
    ImGui::TextWrapped("The hold is a preview choice; caller/game timing is not encoded here. Multipart pieces keep their IMG anchors. Scene edits save separately as .ISTUDIO.");
    ImGui::SetNextItemWidth(110);v.dirty|=ImGui::InputInt("Ticks/frame",&v.options.hold);v.hold_point=ImVec2(ImGui::GetItemRectMin().x+30,(ImGui::GetItemRectMin().y+ImGui::GetItemRectMax().y)*.5f);
    ImGui::SameLine();v.dirty|=ImGui::Checkbox("Facing left",&v.options.facing);ImGui::SameLine();v.dirty|=ImGui::Checkbox("Loop scene",&v.options.loop);
    int origin[]={v.options.origin_x,v.options.origin_y};ImGui::SetNextItemWidth(220);if(ImGui::InputInt2("Object origin",origin)){v.options.origin_x=origin[0];v.options.origin_y=origin[1];v.dirty=true;}
    if(ImGui::Button("Build review"))rebuild_asm_scene();v.build_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SameLine();ImGui::TextDisabled(v.dirty?"Settings changed - rebuild before creating.":"Source artwork is checked again when creating.");
    float height=std::max(100.f,ImGui::GetContentRegionAvail().y-100);
    if(v.valid&&v.snapshot){auto &s=v.snapshot->scene();ImGui::Text("%zu frames / %zu pieces / %zu source libraries",s.lanes[0].frames.size(),v.snapshot->sprite_count(),s.assets.size());
        int last=std::max(0,scene_duration(s)-1);v.tick=std::clamp(v.tick,0,last);ImGui::SetNextItemWidth(-1);ImGui::SliderInt("##asm-scene-tick",&v.tick,0,last,"Tick %d");
        ImGui::BeginChild("asm-scene-preview",ImVec2(0,std::max(75.f,height-45)),ImGuiChildFlags_Border);auto available=ImGui::GetContentRegionAvail();float zoom=std::max(.01f,std::min(available.x/s.width,available.y/s.height));auto start=ImGui::GetCursorScreenPos()+(available-ImVec2(s.width*zoom,s.height*zoom))*.5f;auto dl=ImGui::GetWindowDrawList();auto end=start+ImVec2(s.width*zoom,s.height*zoom);dl->AddRectFilled(start,end,IM_COL32(16,19,24,255));dl->PushClipRect(start,end,true);
        for(auto &pose:evaluate_scene(s,v.tick))draw_scene_pose(s,pose,dl,start,zoom,IM_COL32_WHITE);dl->PopClipRect();dl->AddRect(start,end,IM_COL32(85,91,102,255));ImGui::EndChild();
    }else{ImGui::BeginChild("asm-scene-problem",ImVec2(0,height),ImGuiChildFlags_Border);ImGui::TextWrapped("%s",v.error.c_str());ImGui::TextWrapped("Return to the inspector to resolve artwork or inspect unsupported source commands.");ImGui::EndChild();}
    if(v.valid&&!v.error.empty())ImGui::TextWrapped("%s",v.error.c_str());
    ImGui::BeginDisabled(!v.valid||v.dirty||!v.snapshot||!find(v.owner));
    if(ImGui::Button("Create scene",ImVec2(125,0))){auto a=asm_animation();auto project=a?create_asm_scene(*v.snapshot,*a,asm_scene_libraries(),u.preferred,u.assets.choices,v.error):nullptr;
        if(project){drop_pending_scene();pending_project=std::move(project);pending_scene_owner=v.owner;pending_scene_new=true;install_scene();v.open=false;u.open=false;u.player.playing=false;v.snapshot.reset();ImGui::CloseCurrentPopup();}}
    v.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Cancel",ImVec2(100,0))||(!ImGui::GetIO().WantTextInput&&ImGui::IsKeyPressed(ImGuiKey_Escape))){v.open=false;v.snapshot.reset();ImGui::CloseCurrentPopup();}v.cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndPopup();
}
