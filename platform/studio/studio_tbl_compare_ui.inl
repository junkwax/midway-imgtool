void show_table_compare(Tab &t){finish();t.alignment.player.playing=false;t.scene.playing=false;t.embedded.playing=false;table_compare=TableCompareUi{};table_compare.open=true;table_compare.doc=t.uid;}
void rebuild_table_compare(){auto &u=table_compare;u.valid=false;auto t=find(u.doc);if(!t){u.error="The library is closed.";return;}
    std::vector<uint64_t> ids;for(int i=0;i<t->art->count();++i){auto id=t->art->sprite_id(i);bool include=u.scope==0?marked(*t,id):u.scope==1?t->filter.PassFilter(t->art->sprite(i)->n_s):u.scope==2?true:id==t->selected_id;if(include)ids.push_back(id);}
    u.valid=compare_table(*t->art,ids,u.path,u.report,u.error);u.selected=-1;if(u.valid)for(int i=0;i<(int)u.report.rows.size();++i)if(u.matches||u.report.rows[i].kind!=TblDiff_Match){u.selected=i;break;}
}
void table_compare_dialog(){auto &u=table_compare;if(u.open)ImGui::OpenPopup("Compare existing TBL");auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(1000.f,display.x-30),std::min(760.f,display.y-40)),ImGuiCond_Appearing);ImGui::SetNextWindowSizeConstraints(ImVec2(760,500),display-ImVec2(20,20));
    if(!ImGui::BeginPopupModal("Compare existing TBL",nullptr,ImGuiWindowFlags_NoSavedSettings))return;auto t=find(u.doc);
    ImGui::TextWrapped("Compare stored IMG geometry, animation anchors and palette labels with an existing assembly table. This view changes neither file.");
    ImGui::SetNextItemWidth(-1);if(ImGui::InputTextWithHint("##tbl-path","Existing .TBL file",u.path,sizeof u.path))u.valid=false;
    if(ImGui::Button("Browse...")){
#ifdef _WIN32
        auto path=native_file(window,false,u.path,false,false,false,false,false,false,false,3);if(!path.empty()){snprintf(u.path,sizeof u.path,"%s",path.c_str());u.valid=false;}
#endif
    }ImGui::SameLine();if(ImGui::Button("Compare / refresh"))rebuild_table_compare();u.compare_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();
    const char *scopes[]={"Marked sprites","Shown sprites","All sprites","Selected sprite"};ImGui::SetNextItemWidth(180);if(ImGui::Combo("##tbl-scope",&u.scope,scopes,4))u.valid=false;
    bool current=u.valid&&t&&t->art->generation()==u.report.generation;
    if(!u.error.empty())ImGui::TextWrapped("%s",u.error.c_str());
    if(u.valid&&!current)ImGui::TextColored(accent_surface(1),"IMG changed. Refresh this comparison.");
    if(current){auto &s=u.report.summary;ImGui::Text("%d match | %d differ | %d TBL only | %d IMG only",s.matched,s.differing,s.only_in_tbl,s.only_in_img);ImGui::SameLine();ImGui::Checkbox("Show matches",&u.matches);}
    ImGui::TextDisabled("Loaded TBL snapshot; Refresh rereads it. ROM addresses / DMA flags are informational.");
    float list_height=std::max(90.f,ImGui::GetContentRegionAvail().y-185);
    ImGui::BeginChild("table-comparison-rows",ImVec2(0,list_height),ImGuiChildFlags_Border);
    if(current&&ImGui::BeginTable("tbl-diff",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV|ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("Sprite",ImGuiTableColumnFlags_WidthStretch,2);ImGui::TableSetupColumn("Status");ImGui::TableSetupColumn("TBL line");ImGui::TableSetupColumn("Differences");ImGui::TableHeadersRow();
        const char *states[]={"Matches","Differs","TBL only","IMG only"};int shown=0;
        std::vector<int> visible;for(int i=0;i<(int)u.report.rows.size();++i)if(u.matches||u.report.rows[i].kind!=TblDiff_Match)visible.push_back(i);ImGuiListClipper clip;clip.Begin((int)visible.size());while(clip.Step())for(int v=clip.DisplayStart;v<clip.DisplayEnd;++v){int i=visible[v];auto &row=u.report.rows[i];ImGui::PushID(i);ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);
            if(ImGui::Selectable(row.name.c_str(),u.selected==i,ImGuiSelectableFlags_SpanAllColumns)){u.selected=i;if(row.img_index>=0&&t){auto id=u.report.sprites[row.img_index];int index=t->art->sprite_index(id);if(index>=0&&tab()==t)select(index);}}
            if(shown<4)u.row_points[shown]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;++shown;
            ImGui::TableSetColumnIndex(1);ImGui::TextUnformatted(states[row.kind]);ImGui::TableSetColumnIndex(2);if(row.tbl_index>=0)ImGui::Text("%d",u.report.table[row.tbl_index].line);else ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(3);ImGui::Text("%zu fields",row.fields.size());ImGui::PopID();}
        ImGui::EndTable();
    }else if(!current)ImGui::TextWrapped("Choose a table and Compare / refresh.");ImGui::EndChild();
    ImGui::BeginChild("table-comparison-details",ImVec2(0,110),ImGuiChildFlags_Border);
    if(current&&u.selected>=0&&u.selected<(int)u.report.rows.size()){auto &row=u.report.rows[u.selected];ImGui::TextUnformatted(row.name.c_str());
        for(auto &f:row.fields)ImGui::Text("%s | TBL: %s | IMG: %s",f.field.c_str(),f.tbl_value.c_str(),f.img_value.c_str());
        if(row.fields.empty())ImGui::TextWrapped(row.kind==TblDiff_Match?"All compared fields agree.":row.kind==TblDiff_OnlyInTbl?"No matching sprite in the chosen IMG scope.":"No matching record in this table.");}
    if(current&&!u.report.warnings.empty()){ImGui::Text("%zu parser warnings",u.report.warnings.size());for(auto &warning:u.report.warnings)ImGui::TextWrapped("%s",warning.c_str());}ImGui::EndChild();
    ImGui::BeginDisabled(!current);if(ImGui::Button("Copy report"))ImGui::SetClipboardText(u.report.report.c_str());ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Done",ImVec2(100,0))){u.open=false;ImGui::CloseCurrentPopup();}u.close_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndPopup();
}
