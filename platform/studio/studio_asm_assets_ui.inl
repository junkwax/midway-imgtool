void asm_assets_refresh(){
    auto &u=asm_ui;auto &v=u.assets;auto a=asm_animation();if(!a){v.valid=false;v.review={};return;}
    std::vector<AsmLibrary> libs;std::vector<std::pair<uint64_t,uint64_t>> signature;
    for(auto &t:tabs){libs.push_back({t->uid,t->art.get()});signature.push_back({t->uid,t->art->generation()});}
    if(v.cache_animation==u.selected&&v.cache_preferred==u.preferred&&v.signature==signature)return;
    v.valid=review_asm_assets(*a,libs,u.preferred,v.choices,v.review,v.error);
    v.cache_animation=u.selected;v.cache_preferred=u.preferred;v.signature=std::move(signature);
    if(v.selected<0||v.selected>=(int)v.review.rows.size())v.selected=v.review.rows.empty()?-1:0;
}
void asm_assets_choose(const std::string &symbol,uint64_t doc=0,uint64_t sprite=0){
    auto &u=asm_ui;auto key=asm_artwork_key(symbol);
    try{if(doc&&sprite)u.assets.choices[key]={doc,sprite};else u.assets.choices.erase(key);}
    catch(const std::bad_alloc &){u.assets.error="Not enough memory to retain this choice.";return;}
    u.assets.cache_animation=-1;u.cache_anim=-1;u.reactions.cache_animation=-1;
}
void asm_assets_open_sprite(){
    auto &u=asm_ui;auto &v=u.assets;asm_assets_refresh();if(!v.valid||v.selected<0)return;
    auto p=v.review.rows[v.selected].resolved;auto t=find(p.doc);int index=t?t->art->sprite_index(p.sprite):-1;
    if(index<0){v.cache_animation=-1;v.error="The chosen artwork is unavailable. Review again.";return;}
    for(int i=0;i<(int)tabs.size();++i)if(tabs[i]->uid==t->uid){active=i;select_uid=t->uid;break;}
    workspace(*t,false);select(index);t->zoom=0;t->pan={};t->anchors=true;
    v.open=false;u.open=false;u.player.playing=false;ImGui::CloseCurrentPopup();
    status="Opened animation artwork. IMG edits use its own Undo and Save; ASM stays unchanged.";
}
void asm_assets_dialog(){
    auto &u=asm_ui;auto &v=u.assets;
    if(!v.library_review){v.library_preview.reset();v.library_snapshot.reset();asm_assets_refresh();}auto animation=asm_animation();
    ImGui::OpenPopup("Animation artwork");auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(900.f,display.x-50),std::min(700.f,display.y-60)),ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal("Animation artwork",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    if(v.library_review){asm_artwork_library_review();ImGui::EndPopup();return;}
    ImGui::TextWrapped("Required artwork / %s",animation?animation->label.c_str():"No animation");
    ImGui::TextWrapped("Review every piece in this animation. Choose among matching sprites in open IMG libraries; choices affect this source preview only.");
    if(animation&&animation->partial)ImGui::TextColored(ImVec4(1,.65f,.4f,1),"Partial ASM preview: this list covers only the frames the inspector could expand.");
    if(!v.error.empty())ImGui::TextWrapped("%s",v.error.c_str());
    if(v.valid)ImGui::Text("%zu symbols / %d resolved / %d missing / %d ambiguous / %d unavailable choices",v.review.rows.size(),v.review.resolved,v.review.missing,v.review.ambiguous,v.review.stale);
    ImGui::SetNextItemWidth(std::max(150.f,ImGui::GetContentRegionAvail().x-200));if(ImGui::InputTextWithHint("##artwork-search","Find artwork symbol...",v.filter.InputBuf,sizeof v.filter.InputBuf))v.filter.Build();v.filter_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SameLine();ImGui::Checkbox("Unresolved only",&v.unresolved);v.unresolved_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    std::vector<int> visible;if(v.valid)for(int i=0;i<(int)v.review.rows.size();++i){auto &row=v.review.rows[i];if(v.filter.PassFilter(row.symbol.c_str())&&(!v.unresolved||!row.resolved.sprite))visible.push_back(i);}
    if(ImGui::BeginTable("animation-artwork",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_ScrollY,ImVec2(0,std::max(100.f,ImGui::GetContentRegionAvail().y-(animation&&animation->partial?260:225))))){
        ImGui::TableSetupColumn("Symbol",ImGuiTableColumnFlags_WidthStretch,1.1f);ImGui::TableSetupColumn("Uses",ImGuiTableColumnFlags_WidthFixed,50);
        ImGui::TableSetupColumn("Status",ImGuiTableColumnFlags_WidthStretch,1.f);ImGui::TableSetupColumn("Library",ImGuiTableColumnFlags_WidthStretch,1.4f);ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
        ImGuiListClipper clip;clip.Begin((int)visible.size());while(clip.Step())for(int n=clip.DisplayStart;n<clip.DisplayEnd;++n){int i=visible[n];auto &row=v.review.rows[i];auto &p=row.resolved;
            ImGui::PushID(i);ImGui::TableNextRow();ImGui::TableNextColumn();if(ImGui::Selectable(row.symbol.c_str(),i==v.selected,ImGuiSelectableFlags_SpanAllColumns))v.selected=i;
            if(n<8)v.row_points[n]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
            ImGui::SetItemTooltip("First used in frame %d, line %d\n%s",row.first_frame+1,row.line,row.source.empty()?(u.source?u.source->path.c_str():""):row.source.c_str());
            ImGui::TableNextColumn();ImGui::Text("%d",row.uses);ImGui::TableNextColumn();
            if(p.sprite)ImGui::TextDisabled(p.chosen?"Chosen":"Resolved");else ImGui::TextColored(ImVec4(1,.65f,.4f,1),"%s",p.stale?"Choice unavailable":p.ambiguous?"Ambiguous":"Missing");
            ImGui::TableNextColumn();auto t=find(p.doc);ImGui::TextUnformatted(t?t->name().c_str():"--");if(t)ImGui::SetItemTooltip("%s",t->art->path().empty()?t->name().c_str():t->art->path().c_str());ImGui::PopID();
        }ImGui::EndTable();
    }
    if(v.valid&&v.selected>=0&&v.selected<(int)v.review.rows.size()){
        auto &row=v.review.rows[v.selected];auto chosen=row.resolved;auto doc=find(chosen.doc);int index=doc?doc->art->sprite_index(chosen.sprite):-1;
        ImGui::TextWrapped("%s / first use: frame %d, line %d",row.symbol.c_str(),row.first_frame+1,row.line);
        auto caption=chosen.stale?std::string("Chosen sprite unavailable"):doc?doc->name()+" / #"+std::to_string(index+1):std::string("Choose matching artwork...");
        ImGui::SetNextItemWidth(-1);std::pair<uint64_t,uint64_t> selected{};bool changed=false,automatic=false;
        if(ImGui::BeginCombo("##animation-artwork-choice",caption.c_str())){
            if(ImGui::Selectable("Automatic (preferred library, then unique name)",!chosen.chosen))automatic=true;v.auto_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
            int n=0;for(auto &candidate:row.candidates){auto t=find(candidate.doc);auto label=(t?t->name():"Closed library")+" / #"+std::to_string(t?t->art->sprite_index(candidate.sprite)+1:0);
                ImGui::PushID(n);if(ImGui::Selectable(label.c_str(),candidate.doc==chosen.doc&&candidate.sprite==chosen.sprite)){selected={candidate.doc,candidate.sprite};changed=true;}
                if(t)ImGui::SetItemTooltip("%s",t->art->path().empty()?t->name().c_str():t->art->path().c_str());
                if(n<8)v.choice_points[n]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::PopID();++n;
            }ImGui::EndCombo();
        }v.choice_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
        if(automatic)asm_assets_choose(row.symbol);else if(changed)asm_assets_choose(row.symbol,selected.first,selected.second);
        if(ImGui::Button("Show first frame")){u.player.playing=false;u.player.seek(row.first_frame,animation?(int)animation->frames.size():0);v.open=false;ImGui::CloseCurrentPopup();}v.frame_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();
        ImGui::BeginDisabled(index<0||changed||automatic);if(ImGui::Button("Open selected sprite"))asm_assets_open_sprite();v.open_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();
    }else ImGui::TextWrapped("No artwork selected. Clear the filter or load an animation.");
    ImGui::Separator();if(ImGui::Button("Back to preview",ImVec2(140,0))){v.open=false;ImGui::CloseCurrentPopup();}v.close_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();
    ImGui::BeginDisabled(!v.valid);if(ImGui::Button("Copy unresolved names")){std::string names;for(auto &row:v.review.rows)if(!row.resolved.sprite){if(!names.empty())names+='\n';names+=row.symbol;}ImGui::SetClipboardText(names.c_str());}v.copy_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();
    ImGui::SameLine();if(ImGui::Button("Open IMG library...")){auto path=native_file(window,false,{},false,false,false,false,false,false,false,6);if(!path.empty()){auto extension=fs::u8path(path).extension().u8string();for(auto &c:extension)c=(char)std::toupper((unsigned char)c);if(extension==".IMG")open(path);else v.error="Choose an IMG sprite library.";}}
    ImGui::SameLine();if(ImGui::Button("Refresh"))v.cache_animation=-1;
    if(animation&&animation->partial){ImGui::Checkbox("Include expanded frames only",&v.allow_partial);v.partial_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;}
    ImGui::BeginDisabled(!v.valid||v.review.rows.empty()||v.review.missing||v.review.ambiguous||v.review.stale||(animation&&animation->partial&&!v.allow_partial));
    if(ImGui::Button("Prepare artwork library...",ImVec2(210,0)))asm_assets_prepare_library();v.prepare_library_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();
    ImGui::EndPopup();
}
