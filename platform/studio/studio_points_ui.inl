void show_points(Tab &t){
    finish();t.alignment.player.playing=false;t.scene.playing=false;t.embedded.playing=false;
    points=PointsUi{};points.doc=t.uid;points.open=true;points.valid=read_point_table(*t.art,t.selected_id,points.edit,points.error);
}
void points_preview(Tab &t,int index){
    auto &u=points;auto im=t.art->sprite(index);auto dl=ImGui::GetWindowDrawList();auto pos=ImGui::GetCursorScreenPos(),avail=ImMax(ImGui::GetContentRegionAvail(),ImVec2(1,1));
    ImGui::InvisibleButton("##points-canvas",avail,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle|ImGuiButtonFlags_MouseButtonRight);bool hovered=ImGui::IsItemHovered();auto &io=ImGui::GetIO();
    float z=u.zoom>0?u.zoom:std::clamp(std::min((avail.x-44)/im->w,(avail.y-44)/im->h),.25f,16.f);
    if(hovered&&io.MouseWheel){u.zoom=std::clamp(z*std::pow(1.2f,io.MouseWheel),.25f,32.f);z=u.zoom;}
    if(hovered&&!u.dragging&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle))u.pan+=io.MouseDelta;
    ImVec2 start=pos+(avail-ImVec2(im->w*z,im->h*z))*.5f+u.pan;u.preview_origin=start;u.preview_zoom=z;
    dl->PushClipRect(pos,pos+avail,true);dl->AddRectFilled(pos,pos+avail,IM_COL32(18,21,27,255));dl->AddRectFilled(start,start+ImVec2(im->w*z,im->h*z),IM_COL32(35,40,48,255));
    if(auto tex=texture(t,index))dl->AddImage((ImTextureID)(intptr_t)tex,start,start+ImVec2(im->w*z,im->h*z));
    if(u.edit.enabled){for(int i=0;i<10;++i){u.handles[i]=start+ImVec2(u.edit.points[i*2]*z,u.edit.points[i*2+1]*z);}
        if(u.polygon)for(int i=0;i<10;++i)dl->AddLine(u.handles[i],u.handles[(i+1)%10],IM_COL32(211,76,95,180),1.5f);
        for(int i=0;i<10;++i){auto at=u.handles[i];bool chosen=i==u.selected;dl->AddCircleFilled(at,chosen?6.f:4.f,chosen?IM_COL32(239,193,94,255):IM_COL32(228,75,91,255));
            char name[12];snprintf(name,sizeof name,"%d",i+1);dl->AddText(at+ImVec2(7,-9),ImGui::GetColorU32(ImGuiCol_Text),name);}
    }
    if(hovered&&u.edit.enabled&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        // Prefer the selected handle when several points share a coordinate.
        int hit=-1;for(int pass=0;pass<10;++pass){int i=(u.selected+pass)%10;auto delta=io.MousePos-u.handles[i];if(delta.x*delta.x+delta.y*delta.y<=64){hit=i;break;}}
        if(hit>=0){u.selected=hit;u.dragging=true;}
    }
    if(hovered&&u.edit.enabled&&ImGui::IsMouseClicked(ImGuiMouseButton_Right)){
        u.edit.points[u.selected*2]=std::clamp((int)std::round((io.MousePos.x-start.x)/z),-32768,32767);
        u.edit.points[u.selected*2+1]=std::clamp((int)std::round((io.MousePos.y-start.y)/z),-32768,32767);
    }
    if(u.dragging&&ImGui::IsMouseDown(ImGuiMouseButton_Left)){
        u.edit.points[u.selected*2]=std::clamp((int)std::round((io.MousePos.x-start.x)/z),-32768,32767);
        u.edit.points[u.selected*2+1]=std::clamp((int)std::round((io.MousePos.y-start.y)/z),-32768,32767);
    }
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left))u.dragging=false;
    dl->PopClipRect();
}
void points_dialog(){auto &u=points;if(u.open)ImGui::OpenPopup("IMG point table");auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(850.f,display.x-30),std::min(740.f,display.y-40)),ImGuiCond_Appearing);ImGui::SetNextWindowSizeConstraints(ImVec2(700,480),display-ImVec2(20,20));
    if(!ImGui::BeginPopupModal("IMG point table",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    auto t=find(u.doc);int index=t?t->art->sprite_index(u.edit.sprite):-1;auto im=t?t->art->sprite(index):nullptr;
    bool current=u.valid&&t&&im&&t->art->generation()==u.edit.generation;
    if(im)ImGui::Text("%s | ten X / Y coordinate pairs",im->n_s);
    ImGui::TextWrapped("Drag a numbered point, or right-click to place the selected point. Apply stores this sprite's existing IMG point-table block.");
    float height=ImGui::GetContentRegionAvail().y-50;
    ImGui::BeginChild("point-settings",ImVec2(240,height),ImGuiChildFlags_Border);ImGui::BeginDisabled(!current);
    ImGui::Checkbox("Enable point table",&u.edit.enabled);u.enable_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::BeginDisabled(!u.edit.enabled);
    for(int i=0;i<10;++i){ImGui::PushID(i);char name[12];snprintf(name,sizeof name,"%d",i+1);if(ImGui::Selectable(name,u.selected==i,0,ImVec2(22,22)))u.selected=i;ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##point",&u.edit.points[i*2])){u.edit.points[i*2]=std::clamp(u.edit.points[i*2],-32768,32767);u.edit.points[i*2+1]=std::clamp(u.edit.points[i*2+1],-32768,32767);}ImGui::PopID();}
    ImGui::Checkbox("Connect points (preview)",&u.polygon);ImGui::EndDisabled();ImGui::EndDisabled();
    if(ImGui::Button("Fit sprite")){u.zoom=0;u.pan={};}ImGui::Spacing();
    ImGui::TextWrapped("Point coordinates are stored as signed 16-bit values. They may sit outside the sprite. These are generic IMG points; game strike boxes belong to their source tables.");
    if(!current)ImGui::TextWrapped("The sprite changed. Reload its stored points before editing.");
    if(!u.error.empty())ImGui::TextWrapped("%s",u.error.c_str());ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("point-preview",ImVec2(0,height),ImGuiChildFlags_Border,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::BeginDisabled(!current);if(im)points_preview(*t,index);ImGui::EndDisabled();ImGui::EndChild();
    ImGui::BeginDisabled(!current);if(ImGui::Button("Apply point table",ImVec2(150,0))&&t){if(apply_point_table(*t->art,u.edit,u.error)){status="Updated IMG point table in "+t->name()+".";u.open=false;ImGui::CloseCurrentPopup();}}
    u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!t||!im);if(ImGui::Button("Reload stored points")&&t){u.valid=read_point_table(*t->art,u.edit.sprite,u.edit,u.error);u.dragging=false;}u.reload_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Cancel",ImVec2(100,0))){u.open=false;u.dragging=false;ImGui::CloseCurrentPopup();}u.cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(!ImGui::GetIO().WantTextInput&&ImGui::IsKeyPressed(ImGuiKey_Escape)){u.open=false;u.dragging=false;ImGui::CloseCurrentPopup();}ImGui::EndPopup();
}
