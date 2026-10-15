void stage_size(const Tab &t,int &w,int &h,int &floor) const {
    w=t.alignment.world_w;h=t.alignment.world_h;floor=t.alignment.floor_y;
    if(t.embedded.enabled){w=400;h=254;floor=t.embedded.floor;}
    else if(t.project&&t.alignment.enabled){w=t.project->state.width;h=t.project->state.height;floor=t.project->state.floor;}
}
bool load_stage(Tab &t,const std::string &path){
    auto loaded=read_stage(path,stage_error);if(!loaded)return false;
    if(t.stage_texture)SDL_DestroyTexture(t.stage_texture);t.stage_texture=nullptr;t.stage_texture_error.clear();
    t.stage=std::move(loaded);t.stage_view={};int w,h,floor;stage_size(t,w,h,floor);
    stage_align_plane(*t.stage,t.stage_view,stage_playfield_plane(*t.stage),w,floor);
    status="Loaded reference scenery: "+t.stage->name+". IMG artwork is unchanged.";return true;
}
void show_stage_dialog(Tab &t){
    finish();if(!t.embedded.enabled&&!t.alignment.enabled)workspace(t,true);
    t.alignment.player.playing=false;t.embedded.playing=false;t.scene.playing=false;
    stage_doc=t.uid;stage_dialog_open=true;stage_error.clear();snprintf(stage_path,sizeof stage_path,"%s",t.stage?t.stage->bdd_path.c_str():"");
}
void draw_stage(Tab &t,ImDrawList *dl,ImVec2 origin,float zoom){
    if(!t.stage||!t.stage_view.enabled)return;auto &s=*t.stage;auto &v=t.stage_view;
    if(!t.stage_texture&&t.stage_texture_error.empty()){
        t.stage_texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ABGR8888,SDL_TEXTUREACCESS_STATIC,s.w,s.h);
        if(t.stage_texture){SDL_SetTextureBlendMode(t.stage_texture,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(t.stage_texture,SDL_ScaleModeNearest);
            if(SDL_UpdateTexture(t.stage_texture,nullptr,s.rgba.data(),s.w*4)){SDL_DestroyTexture(t.stage_texture);t.stage_texture=nullptr;}}
        if(!t.stage_texture)t.stage_texture_error=SDL_GetError();
    }
    if(t.stage_texture){auto a=origin+ImVec2((float)((int64_t)s.x-v.camera_x)*zoom,(float)((int64_t)s.y-v.camera_y)*zoom);
        dl->AddImage((ImTextureID)(intptr_t)t.stage_texture,a,a+ImVec2(s.w*zoom,s.h*zoom),ImVec2(0,0),ImVec2(1,1),IM_COL32(255,255,255,std::clamp(v.alpha,0,255)));}
}
void stage_dialog(){
    if(stage_dialog_open)ImGui::OpenPopup("Stage reference");
    ImGui::SetNextWindowSizeConstraints(ImVec2(540,0),ImVec2(720,630));
    if(!ImGui::BeginPopupModal("Stage reference",nullptr,ImGuiWindowFlags_AlwaysAutoResize))return;
    auto t=find(stage_doc);if(!t){stage_dialog_open=false;ImGui::CloseCurrentPopup();ImGui::EndPopup();return;}
    ImGui::TextWrapped("Preview your animation against stage scenery. Choose either half of a matching BDD/BDB pair.");
    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##stage-path","Path to a stage .BDD or .BDB",stage_path,sizeof stage_path);
    if(ImGui::Button("Browse...")){
#ifdef _WIN32
        auto path=native_file(window,false,stage_path,false,false,false,false,false,false,false,-1,4);if(!path.empty())snprintf(stage_path,sizeof stage_path,"%s",path.c_str());
#else
        stage_error="Enter a path above, then Load scenery.";
#endif
    }
    ImGui::SameLine();if(ImGui::Button("Load scenery"))load_stage(*t,stage_path);stage_load_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(t->stage){auto &s=*t->stage;auto &v=t->stage_view;int w,h,floor;stage_size(*t,w,h,floor);
        ImGui::SeparatorText(s.name.c_str());ImGui::TextDisabled("%d x %d world | %d placements",s.world_w,s.world_h,s.objects);
        if(s.skipped)ImGui::TextColored(accent_surface(1),"%d placements have missing artwork/palettes.",s.skipped);
        if(s.invalid_pixels)ImGui::TextColored(accent_surface(1),"%d pixels reference missing colors and remain transparent.",s.invalid_pixels);
        ImGui::Checkbox("Show scenery",&v.enabled);stage_visible_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
        ImGui::SameLine();if(ImGui::Button("Clear scenery")){if(t->stage_texture)SDL_DestroyTexture(t->stage_texture);t->stage_texture=nullptr;t->stage_texture_error.clear();t->stage.reset();stage_error.clear();}
        stage_clear_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
        // Clear can release the referenced backdrop; all subsequent work rechecks ownership.
        if(t->stage){
            const char *name=v.plane>=0&&v.plane<(int)s.planes.size()?s.planes[v.plane].name.c_str():"Whole stage";
            ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##stage-plane",name)){
                if(ImGui::Selectable("Whole stage",v.plane<0))stage_align_plane(s,v,-1,w,floor);
                for(int i=0;i<(int)s.planes.size();++i){ImGui::PushID(i);if(ImGui::Selectable(s.planes[i].name.c_str(),v.plane==i))stage_align_plane(s,v,i,w,floor);ImGui::PopID();}ImGui::EndCombo();}
            ImGui::TextDisabled("Plane: center its painted bounds and align to the floor");
            int xy[]={v.camera_x,v.camera_y};ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##stage-camera",xy)){v.camera_x=std::clamp(xy[0],-1000000,1000000);v.camera_y=std::clamp(xy[1],-1000000,1000000);}
            ImGui::TextDisabled("World coordinates at the playfield's top left");ImGui::SetNextItemWidth(-1);ImGui::SliderInt("##stage-opacity",&v.alpha,0,255,"Opacity: %d / 255");
            if(ImGui::Button("Align plane to floor"))stage_align_plane(s,v,v.plane,w,floor);stage_align_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
            if(!t->stage_texture_error.empty())ImGui::TextWrapped("Could not display scenery: %s",t->stage_texture_error.c_str());
        }
    }
    ImGui::Spacing();ImGui::TextWrapped("Reference settings stay in this library's session and appear in scene PNG exports. Reload scenery after reopening a project. Game camera/parallax rules and runtime floors require game source; placement here is manual.");
    if(!stage_error.empty())ImGui::TextWrapped("%s",stage_error.c_str());
    ImGui::Spacing();if(ImGui::Button("Done",ImVec2(100,0))){stage_dialog_open=false;ImGui::CloseCurrentPopup();}stage_close_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::EndPopup();
}
