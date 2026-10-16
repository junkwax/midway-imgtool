void reaction_choose_visible(){
    auto &u=asm_ui;auto &r=u.reactions;u.player.playing=false;
    r.visible=filter_asm_reactions(r.catalogue,r.family,r.show_all,r.search);
    bool selected=false;for(int row:r.visible)selected|=r.catalogue[row].animation==u.selected;
    if(!selected&&!r.visible.empty())asm_select(r.catalogue[r.visible.front()].animation);
}
void show_reactions(){show_asm();asm_ui.reaction_mode=true;reaction_choose_visible();}
void reaction_list(float height){
    auto &u=asm_ui;auto &r=u.reactions;bool changed=false;
    ImGui::TextUnformatted("Reaction directory");ImGui::SetNextItemWidth(-1);
    changed|=ImGui::InputTextWithHint("##reaction-search","Find reaction, thrower or rule...",r.search,sizeof r.search);
    r.search_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SetNextItemWidth(-1);const char *family=r.family<0?"All reaction families":ReactionKindName((ReactionKind)r.family);
    if(ImGui::BeginCombo("##reaction-family",family)){
        if(ImGui::Selectable("All reaction families",r.family<0)){r.family=-1;changed=true;}
        r.family_choices[0]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
        for(int i=1;i<kReactionKindCount;++i){
            if(ImGui::Selectable(ReactionKindName((ReactionKind)i),r.family==i)){r.family=i;changed=true;}
            r.family_choices[i]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
            ImGui::SetItemTooltip("%s",ReactionKindHelp((ReactionKind)i));
        }
        ImGui::EndCombo();
    }r.family_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    changed|=ImGui::Checkbox("Show other animations",&r.show_all);r.show_all_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(changed)reaction_choose_visible();
    ImGui::TextDisabled("%zu of %zu source entries",r.visible.size(),r.catalogue.size());
    ImGui::BeginChild("reaction-list",ImVec2(0,std::max(95.f,height*(height<550?.22f:.31f))),ImGuiChildFlags_Border);
    if(r.visible.empty())ImGui::TextWrapped(u.source?"No matching reactions. Clear the filter or show other animations.":"Load a character animation ASM to browse reactions.");
    int group=-1,choice=0;
    for(int index:r.visible){auto &row=r.catalogue[index];int kind=(int)row.classification.kind;
        if(group!=kind){group=kind;ImGui::SeparatorText(kind?ReactionKindName(row.classification.kind):"Other animations");}
        ImGui::PushID(row.animation);
        if(ImGui::Selectable(row.label.c_str(),row.animation==u.selected))asm_select(row.animation);
        if(choice<8)r.choice_points[choice++]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s\n%s%s%s",row.name.c_str(),row.classification.rule.c_str(),row.thrower.empty()?"":"\nThrown by: ",row.thrower.c_str());
        ImGui::PopID();
    }ImGui::EndChild();
}
void reaction_description(){
    auto &u=asm_ui;
    for(auto &r:u.reactions.catalogue)if(r.animation==u.selected){
        ImGui::TextWrapped("Family: %s",ReactionKindName(r.classification.kind));
        ImGui::TextWrapped("Reason: %s",r.classification.rule.c_str());
        if(!r.thrower.empty())ImGui::TextWrapped("Thrown by: %s",r.thrower.c_str());break;
    }
}
void reaction_refresh_anchors(){
    auto &u=asm_ui;auto &r=u.reactions;auto a=asm_animation();
    if(!a){r.anchors={};return;}
    std::vector<std::tuple<uint64_t,uint64_t,int>> signature;std::vector<ReactionLibrary> libraries;
    for(auto &t:tabs){signature.push_back({t->uid,t->art->generation(),(int)t->convention});libraries.push_back({t->uid,t->art.get(),t->convention});}
    if(r.cache_animation==u.selected&&r.cache_preferred==u.preferred&&r.cache_facing==u.facing&&r.signature==signature)return;
    try{auto review=review_reaction_anchors(*a,libraries,u.preferred,u.facing,u.assets.choices);r.anchors=std::move(review);r.error.clear();}
    catch(const std::bad_alloc &){r.anchors={};r.error="Not enough memory for the frame table. Try Refresh frames.";}
    r.cache_animation=u.selected;r.cache_preferred=u.preferred;r.cache_facing=u.facing;r.signature=std::move(signature);
    if(r.selected>=(int)r.anchors.rows.size())r.selected=-1;
}
void reaction_open_sprite(){
    auto &u=asm_ui;auto &r=u.reactions;if(r.selected<0||r.selected>=(int)r.anchors.rows.size())return;
    auto reference=r.anchors.rows[r.selected].reference;auto t=find(reference.doc);
    int index=t?t->art->sprite_index(reference.sprite):-1;
    if(index<0){r.cache_animation=-1;r.error="That sprite is unavailable. Refresh the frame table.";return;}
    for(int i=0;i<(int)tabs.size();++i)if(tabs[i]->uid==t->uid){active=i;select_uid=t->uid;break;}
    workspace(*t,false);select(index);t->zoom=0;t->pan={};t->anchors=true;
    u.open=false;u.player.playing=false;ImGui::CloseCurrentPopup();
    status="Opened reaction artwork. Anchor edits affect this IMG; source ASM stays unchanged.";
}
void reaction_anchor_table(){
    auto &u=asm_ui;auto &r=u.reactions;reaction_refresh_anchors();
    ImGui::SeparatorText("Frame anchors");
    bool available=false;
    if(r.selected>=0&&r.selected<(int)r.anchors.rows.size()){
        auto &ref=r.anchors.rows[r.selected].reference;auto t=find(ref.doc);
        available=t&&t->art->sprite_index(ref.sprite)>=0;
    }
    ImGui::BeginDisabled(!available);if(ImGui::Button("Open selected sprite"))reaction_open_sprite();
    r.open_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Refresh frames"))r.cache_animation=-1;r.refresh_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(!r.error.empty())ImGui::TextWrapped("%s",r.error.c_str());
    if(r.anchors.truncated)ImGui::TextColored(ImVec4(1,.65f,.4f,1),"First %zu pieces shown; remaining frames are omitted from this table.",r.anchors.rows.size());
    ImGui::TextDisabled("Stored anchors / placement relative to object origin; select a row to scrub");
    if(ImGui::BeginTable("reaction-anchors",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_ScrollY,ImVec2(0,std::max(65.f,ImGui::GetContentRegionAvail().y)))){
        ImGui::TableSetupColumn("Frame",ImGuiTableColumnFlags_WidthFixed,44);ImGui::TableSetupColumn("Piece",ImGuiTableColumnFlags_WidthStretch,1.3f);
        ImGui::TableSetupColumn("Anchor X/Y");ImGui::TableSetupColumn("Placed X/Y");ImGui::TableSetupColumn("Review");ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
        ImGuiListClipper clip;clip.Begin((int)r.anchors.rows.size());
        while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i){auto &row=r.anchors.rows[i];ImGui::PushID(i);ImGui::TableNextRow();ImGui::TableNextColumn();
            auto label=std::to_string(row.frame+1)+(row.frame==u.player.frame?" *":"");
            if(ImGui::Selectable(label.c_str(),r.selected==i,ImGuiSelectableFlags_SpanAllColumns)){r.selected=i;u.player.playing=false;if(auto a=asm_animation())u.player.seek(row.frame,(int)a->frames.size());}
            if(i<8)r.row_points[i]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
            ImGui::TableNextColumn();ImGui::TextUnformatted(row.reference.symbol.c_str());
            if(ImGui::IsItemHovered()){auto t=find(row.reference.doc);auto a=asm_animation();auto path=a&&row.frame<(int)a->frames.size()?a->frames[row.frame].source:std::string{};ImGui::SetTooltip("%s\nSource line %d / piece %d\n%s\n%d x %d / %s%s%s",path.empty()?u.source->path.c_str():path.c_str(),row.line,row.piece+1,t?t->name().c_str():"No unique library match",row.w,row.h,mirror_convention_label(row.convention),row.flip?" / H flip":"",row.flip_y?" / V flip":"");}
            ImGui::TableNextColumn();if(row.reference.sprite)ImGui::Text("%d, %d",row.anix,row.aniy);else ImGui::TextDisabled("--");
            ImGui::TableNextColumn();if(row.reference.sprite)ImGui::Text("%d, %d",row.x,row.y);else ImGui::TextDisabled("--");
            ImGui::TableNextColumn();if(row.reference.sprite)ImGui::TextDisabled("Resolved");else ImGui::TextColored(ImVec4(1,.65f,.4f,1),"%s",row.reference.stale?"Choice unavailable":row.reference.ambiguous?"Ambiguous":"Missing");
            ImGui::PopID();
        }ImGui::EndTable();
    }
}
