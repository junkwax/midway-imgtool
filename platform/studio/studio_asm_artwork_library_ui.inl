void asm_assets_prepare_library(){
    auto &u=asm_ui;auto &v=u.assets;auto a=asm_animation();if(!a)return;
    try{
        std::vector<AsmLibrary> libs;for(auto &t:tabs)libs.push_back({t->uid,t->art.get()});
        auto snapshot=AsmArtworkLibrary::capture(*a,libs,u.preferred,v.choices,v.allow_partial,v.error);if(!snapshot)return;
        auto preview=std::make_unique<Tab>();preview->art=snapshot->create_library(v.error);if(!preview->art)return;
        preview->untitled="Animation artwork";preview->selected_id=preview->art->selection_id();preview->selected=preview->art->sprite_index(preview->selected_id);
        preview->alignment.initialize(*preview->art);sync_alignment(*preview);
        v.library_snapshot=std::move(snapshot);v.library_preview=std::move(preview);v.library_row=0;v.library_error.clear();v.library_review=true;
    }catch(const std::exception &e){v.error=e.what();}
}
bool asm_assets_create_library(){
    auto &u=asm_ui;auto &v=u.assets;if(!v.library_snapshot||!v.library_preview){v.library_error="Prepare an artwork snapshot first.";return false;}
    try{auto completed="Created artwork library for "+v.library_snapshot->label()+". Save the new IMG when ready; source libraries are unchanged.";
        tabs.reserve(tabs.size()+1);v.library_preview->uid=next_uid;tabs.push_back(std::move(v.library_preview));active=(int)tabs.size()-1;select_uid=next_uid++;
        status=std::move(completed);v.library_review=false;v.library_snapshot.reset();v.open=false;u.open=false;u.player.playing=false;return true;
    }catch(const std::bad_alloc &){v.library_error="Not enough memory to open the new tab. The reviewed snapshot is still available.";return false;}
}
void asm_artwork_library_review(){
    auto &v=asm_ui.assets;auto snapshot=v.library_snapshot;auto preview=v.library_preview.get();if(!snapshot||!preview){v.library_review=false;return;}
    ImGui::TextWrapped("Create artwork library / %s",snapshot->label().c_str());
    ImGui::Text("%d sprites / %d palettes / inherited IMG layout 0x%X",preview->art->count(),preview->art->palette_count(),preview->art->disk_version());
    ImGui::TextWrapped("This owned snapshot stays fixed if source files change or close. Create opens a separate unsaved IMG tab; save it when ready.");
    ImGui::TextWrapped("Pixels, anchors, points and alternate palettes are copied. ASM, embedded sequences, scripts and game routines are not included.");
    if(snapshot->partial())ImGui::TextColored(ImVec4(1,.65f,.4f,1),"Partial preview: only the expanded animation frames are represented.");
    auto &rows=snapshot->origins();float height=std::max(160.f,ImGui::GetContentRegionAvail().y-55),width=ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("collected-artwork-origins",ImVec2(width*.54f,height),ImGuiChildFlags_Border);
    if(ImGui::BeginTable("collected-artwork-table",3,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_ScrollY,ImVec2(0,ImGui::GetContentRegionAvail().y))){
        ImGui::TableSetupColumn("Sprite");ImGui::TableSetupColumn("Uses",ImGuiTableColumnFlags_WidthFixed,42);ImGui::TableSetupColumn("Source library");ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
        ImGuiListClipper clip;clip.Begin((int)rows.size());while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i){auto &row=rows[i];ImGui::PushID(i);ImGui::TableNextRow();ImGui::TableNextColumn();
            if(ImGui::Selectable(row.name.c_str(),v.library_row==i,ImGuiSelectableFlags_SpanAllColumns))v.library_row=i;
            if(i<8)v.library_row_points[i]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SetItemTooltip("ASM symbol: %s\nSource generation: %llu",row.symbol.c_str(),(unsigned long long)row.generation);
            ImGui::TableNextColumn();ImGui::Text("%d",row.uses);ImGui::TableNextColumn();ImGui::TextUnformatted(fs::u8path(row.library).filename().u8string().c_str());ImGui::SetItemTooltip("%s",row.library.c_str());ImGui::PopID();
        }ImGui::EndTable();
    }ImGui::EndChild();ImGui::SameLine();ImGui::BeginChild("collected-artwork-preview",ImVec2(0,height),ImGuiChildFlags_Border);
    v.library_row=std::clamp(v.library_row,0,(int)rows.size()-1);auto &row=rows[v.library_row];auto im=preview->art->sprite(row.target);
    ImGui::TextWrapped("%s / %d x %d",im->n_s,im->w,im->h);ImGui::Text("Anchor %d, %d / palette %d",(short)im->anix,(short)im->aniy,im->palnum);
    ImGui::TextDisabled("%s / %s",im->pttbl_p?"Point table":"No point table",im->opaltbl_p?"Alternate appearances":"Primary palette only");
    auto pos=ImGui::GetCursorScreenPos(),size=ImMax(ImGui::GetContentRegionAvail(),ImVec2(1,1));ImGui::InvisibleButton("##collected-artwork-preview",size);auto dl=ImGui::GetWindowDrawList();dl->PushClipRect(pos,pos+size,true);
    for(float y=pos.y;y<pos.y+size.y;y+=14)for(float x=pos.x;x<pos.x+size.x;x+=14)dl->AddRectFilled(ImVec2(x,y),ImMin(ImVec2(x+14,y+14),pos+size),(((int)((x-pos.x)/14)+(int)((y-pos.y)/14))&1)?IM_COL32(38,41,48,255):IM_COL32(29,32,39,255));
    if(auto tex=texture(*preview,row.target)){float z=std::max(.01f,std::min(12.f,std::min((size.x-16)/im->w,(size.y-16)/im->h)));auto extent=ImVec2(im->w*z,im->h*z),start=pos+(size-extent)*.5f;dl->AddImage((ImTextureID)(intptr_t)tex,start,start+extent);}dl->PopClipRect();ImGui::EndChild();
    if(!v.library_error.empty())ImGui::TextWrapped("%s",v.library_error.c_str());
    if(ImGui::Button("Create unsaved IMG",ImVec2(160,0))&&asm_assets_create_library())ImGui::CloseCurrentPopup();v.create_library_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();
    if(ImGui::Button("Back to artwork review",ImVec2(180,0)))v.library_review=false;v.cancel_library_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
}
