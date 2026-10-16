const char *fatality_kind(FatalityKind kind){return kind==FatalityKind::Command?"Command":kind==FatalityKind::Combo?"Combo":"Animation";}
void fatality_select(int index){
    auto &u=fatality;u.valid=false;u.dirty_preview=false;u.patch={};u.error.clear();u.block=-1;u.body.clear();if(!u.source)return;
    u.seen=u.source->generation();if(u.source->blocks().empty())return;
    index=std::clamp(index,0,(int)u.source->blocks().size()-1);
    try{auto body=u.source->body(index);if(body.size()>256*1024){u.error="This block exceeds the 256 KiB editor limit.";return;}
        u.body.resize(256*1024+1);memcpy(u.body.data(),body.data(),body.size());u.body[body.size()]=0;u.body_baseline=body;u.block=index;
        u.valid=u.source->review(index,body,u.patch,u.error);
    }catch(const std::bad_alloc &){u.error="Not enough memory to open the block editor.";}
}
void show_fatality(){
    finish();asm_ui.open=false;asm_ui.player.playing=false;strikes.open=false;strike_phases.open=false;strike_phases.playing=false;
    fatality.open=true;fatality.return_preview=false;
    if(auto t=tab())t->alignment.player.playing=t->embedded.playing=t->scene.playing=false;
    if(!fatality.path[0]){auto candidate=fs::path("../mk2-main/src/MOVES.ASM");if(fs::exists(candidate))snprintf(fatality.path,sizeof fatality.path,"%s",fs::absolute(candidate).u8string().c_str());}
}
bool activate_fatality(uint64_t id,int block=-1){
    auto &u=fatality;auto source=u.workspace.document(id);if(!source){u.error="That source is no longer open.";return false;}
    if(u.document&&u.source)u.selected_blocks[u.document]=u.block;
    int selected=block>=0?block:u.selected_blocks.count(id)?u.selected_blocks[id]:0;
    u.source=std::move(source);u.document=id;u.kind=0;u.body_only=false;
    snprintf(u.path,sizeof u.path,"%s",u.source->path().c_str());fatality_select(selected);return true;
}
void fatality_after_close(){auto &u=fatality;u.selected_blocks.erase(u.document);u.source.reset();u.document=0;
    if(!u.workspace.documents().empty())activate_fatality(u.workspace.documents().front().id);
    else{u.source.reset();u.document=0;fatality_select(-1);}}
bool install_fatality(bool discard=false){auto &u=fatality;
    uint64_t id=u.workspace.adopt(std::move(u.pending_source),u.pending_document,discard,u.error);
    u.guard=0;u.saving=false;u.open=true;u.return_preview=false;u.pending_document=0;u.guard_document=0;
    if(!id)return false;return activate_fatality(id,0);
}
void load_fatality(const std::string &path){auto &u=fatality;auto next=FatalitySource::open(path,u.error);if(!next)return;
    u.pending_source=std::move(next);u.pending_document=u.document;
    if(u.source&&u.source->dirty()){u.guard=2;u.guard_document=u.document;}else install_fatality();
}
void navigate_fatality(const FatalityMatch &match){auto &u=fatality;std::shared_ptr<FatalitySource> source;
    if(u.workspace.resolve(match,source,u.error))activate_fatality(match.document,match.block);
}
void add_fatality(const std::string &path){auto &u=fatality;auto id=u.workspace.add(path,u.error);if(id){activate_fatality(id);u.open=true;}}
void close_fatality_source(){auto &u=fatality;if(!u.source)return;
    if(u.source->dirty()){u.guard=4;u.guard_document=u.document;return;}
    if(u.workspace.close(u.document,false,u.error))fatality_after_close();
}
void rebuild_fatality(){auto &u=fatality;u.valid=u.source&&u.block>=0&&!u.body.empty()&&u.source->review(u.block,u.body.data(),u.patch,u.error);u.dirty_preview=false;}
bool fatality_staged(){auto &u=fatality;return u.dirty_preview||(u.valid?u.patch.changed:!u.body.empty()&&u.body_baseline!=u.body.data());}
void fatality_reselect(const std::string &label,FatalityKind kind){auto &u=fatality;int selected=0;for(int i=0;i<(int)u.source->blocks().size();++i)if(u.source->blocks()[i].label==label&&u.source->blocks()[i].kind==kind){selected=i;break;}fatality_select(selected);}
void fatality_save_review(){
    auto &u=fatality;u.save_valid=false;u.save_error.clear();u.diff.clear();if(!u.source)return;
    if(fatality_staged()){u.save_error="Apply or Reset the current block before saving source.";return;}
    if(!u.workspace.review_save(u.document,u.save_path,u.write_review,u.save_error))return;
    std::vector<std::string> protected_paths;for(auto &t:tabs){protected_paths.push_back(t->art->path());if(t->project)protected_paths.push_back(t->project->path());}
    if(strikes.source)protected_paths.push_back(strikes.source->path());
    for(auto &path:protected_paths)if(!path.empty()){std::error_code ec;if(fs::equivalent(fs::u8path(path),fs::u8path(u.write_review.path),ec)){u.save_error="Fatality source cannot replace open artwork, a project or strike source.";return;}}
    std::istringstream before(u.write_review.previous),after(u.write_review.bytes);std::string a,b;int line=0,changed=0;
    while(true){bool have_a=(bool)std::getline(before,a),have_b=(bool)std::getline(after,b);if(!have_a&&!have_b)break;++line;
        if(!have_a)a.clear();if(!have_b)b.clear();if(a!=b||have_a!=have_b){u.diff+="Line "+std::to_string(line)+"\n- "+a+"\n+ "+b+"\n\n";if(++changed>=200){u.diff+="Further changed lines omitted from this view. The entire reviewed file will be written.\n";break;}}
    }
    if(u.diff.empty())u.diff="No file text differences.";u.save_valid=true;
}
void request_fatality_save(bool as){auto &u=fatality;if(!u.source)return;
    if(fatality_staged()){u.error="Apply or Reset the current block before saving source.";u.guard=0;u.open=true;return;}
    std::string path=u.source->path();
#ifdef _WIN32
    if(as){path=native_file(window,true,path,false,false,false,false,false,false,false,4);if(path.empty())return;}
#else
    (void)as;
#endif
    snprintf(u.save_path,sizeof u.save_path,"%s",path.c_str());u.saving=true;fatality_save_review();
}
void complete_fatality_guard(bool discard=false){auto &u=fatality;int reason=u.guard;uint64_t id=u.guard_document;u.saving=false;
    if(reason==2){
        // Reload after a reviewed save must read the newly saved disk bytes,
        // rather than installing a snapshot captured before that save.
        if(!u.pending_source){u.guard=0;u.error="No replacement source is prepared.";return;}
        auto next=FatalitySource::open(u.pending_source->path(),u.error);
        if(!next){u.guard=0;u.guard_document=0;u.pending_document=0;u.pending_source.reset();u.open=true;return;}
        u.pending_source=std::move(next);install_fatality(discard);return;
    }
    u.guard=0;u.guard_document=0;
    if((discard||reason==4)&&id){if(!u.workspace.close(id,discard,u.error)){u.open=true;return;}if(u.document==id)fatality_after_close();}
    if(reason==1)u.open=false;else if(reason==3){u.open=false;action(Action::Quit);}
}
void fatality_guard_dialog(){
    auto &u=fatality;ImGui::OpenPopup("Save fatality source changes?");ImGui::SetNextWindowSizeConstraints(ImVec2(520,0),ImVec2(700,700));
    if(!ImGui::BeginPopupModal("Save fatality source changes?",nullptr,ImGuiWindowFlags_AlwaysAutoResize))return;
    ImGui::TextWrapped("Save applied edits to %s? This ASM source has its own history and save flow.",u.source?u.source->path().c_str():"the source");
    if(ImGui::Button("Review save",ImVec2(120,0))){request_fatality_save(false);ImGui::CloseCurrentPopup();}u.guard_save_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();
    if(ImGui::Button("Discard source edits",ImVec2(160,0))){complete_fatality_guard(true);ImGui::CloseCurrentPopup();}u.guard_discard_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();
    if(ImGui::Button("Keep editing",ImVec2(120,0))){u.guard=0;u.guard_document=0;u.pending_document=0;u.pending_source.reset();u.open=true;quitting=false;ImGui::CloseCurrentPopup();}u.guard_cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndPopup();
}
void fatality_write_dialog(){
    auto &u=fatality;ImGui::OpenPopup("Review fatality source save");auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(880.f,display.x-30),std::min(700.f,display.y-40)),ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal("Review fatality source save",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    ImGui::TextWrapped("Review the destination's current text against the applied source edits. This writes one ASM file; IMG artwork is separate.");
    ImGui::SetNextItemWidth(-1);if(ImGui::InputText("##fatality-save-path",u.save_path,sizeof u.save_path))u.save_valid=false;
    if(ImGui::Button("Rebuild save review"))fatality_save_review();ImGui::SameLine();ImGui::TextDisabled(u.save_valid?(u.write_review.existed?"Replace existing file":"Create new file"):"Review required");
    ImGui::BeginChild("fatality-save-diff",ImVec2(0,std::max(100.f,ImGui::GetContentRegionAvail().y-80)),ImGuiChildFlags_Border);ImGui::TextUnformatted(u.diff.c_str());ImGui::EndChild();
    if(!u.save_error.empty())ImGui::TextWrapped("%s",u.save_error.c_str());
    ImGui::BeginDisabled(!u.save_valid||!u.source||u.write_review.generation!=u.source->generation());
    if(ImGui::Button("Write source",ImVec2(120,0))&&u.source){if(u.workspace.save(u.document,u.write_review,u.save_error)){u.seen=u.source->generation();status="Saved fatality ASM source: "+u.source->path();complete_fatality_guard();ImGui::CloseCurrentPopup();}}u.write_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Cancel",ImVec2(100,0))){u.saving=false;u.guard=0;u.guard_document=0;u.pending_document=0;u.pending_source.reset();u.open=true;ImGui::CloseCurrentPopup();}u.cancel_save_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndPopup();
}
void preview_fatality(){
    auto &u=fatality;if(!u.source||u.block<0||fatality_staged())return;auto label=u.source->blocks()[u.block].label;
    std::vector<AsmSourceInput> inputs;size_t root=0;
    try{for(auto &entry:u.workspace.documents()){if(entry.id==u.document)root=inputs.size();inputs.push_back({entry.source->path(),entry.source->bytes()});}}
    catch(const std::bad_alloc &){u.error="Not enough memory to snapshot the open sources.";return;}
    auto preview=AsmSource::parse_linked(inputs,root,label,u.error);if(!preview)return;int selected=-1;
    for(int i=0;i<(int)preview->animations.size();++i)if(preview->animations[i].label==label){selected=i;break;}
    if(selected<0){u.error="This label is not exposed by the bounded ASM animation inspector.";return;}
    show_asm();install_asm(std::move(preview));asm_select(selected);u.open=false;u.return_preview=true;ImGui::CloseCurrentPopup();
}
void fatality_dialog(){
    auto &u=fatality;if(u.saving){fatality_write_dialog();return;}if(u.guard){fatality_guard_dialog();return;}
    ImGui::OpenPopup("Fatality source");auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(1170.f,display.x-30),std::min(810.f,display.y-40)),ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal("Fatality source",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    ImGui::TextWrapped("Keep several MK2 ASM sources open. Each has its own edits, Undo and reviewed Save. Game routines and includes are not executed.");
    ImGui::SetNextItemWidth(std::max(100.f,ImGui::GetContentRegionAvail().x-280));ImGui::InputText("##fatality-path",u.path,sizeof u.path);ImGui::SameLine();
    if(ImGui::Button("Browse...")){
#ifdef _WIN32
        auto path=native_file(window,false,u.path,false,false,false,false,false,false,false,4);if(!path.empty())snprintf(u.path,sizeof u.path,"%s",path.c_str());
#endif
    }ImGui::SameLine();if(ImGui::Button("Add source"))add_fatality(u.path);u.load_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();ImGui::BeginDisabled(!u.source);if(ImGui::Button("Reload source"))load_fatality(u.source->path());u.reload_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();
    if(u.source)ImGui::TextWrapped("%s / %s",u.source->path().c_str(),u.source->dirty()?"Unsaved applied edits":"Source saved");
    if(u.source&&u.seen!=u.source->generation())fatality_select(u.block);
    float height=std::max(180.f,ImGui::GetContentRegionAvail().y-82);float left=std::clamp(ImGui::GetContentRegionAvail().x*.28f,240.f,320.f);
    ImGui::BeginChild("fatality-blocks",ImVec2(left,height),ImGuiChildFlags_Border);
    ImGui::Text("%zu open / %zu unsaved",u.workspace.documents().size(),u.workspace.dirty_count());
    auto active=u.source?fs::u8path(u.source->path()).filename().u8string():"Choose a source";ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##fatality-documents",active.c_str())){int choice=0;for(auto &entry:u.workspace.documents()){
        auto name=fs::u8path(entry.source->path()).filename().u8string()+(entry.source->dirty()?" *":"");ImGui::PushID((int)entry.id);
        if(ImGui::Selectable(name.c_str(),entry.id==u.document))activate_fatality(entry.id);
        if(choice<16)u.document_choices[choice++]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SetItemTooltip("%s",entry.source->path().c_str());ImGui::PopID();
    }ImGui::EndCombo();}u.documents_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SetNextItemWidth(-1);if(ImGui::InputTextWithHint("##fatality-filter","Find label, routine, trigger...",u.filter.InputBuf,sizeof u.filter.InputBuf))u.filter.Build();
    const char *kinds[]={"All blocks","Commands","Button combos","Animation data"};ImGui::SetNextItemWidth(-1);ImGui::Combo("##fatality-kind",&u.kind,kinds,4);
    ImGui::Checkbox("Body endings only",&u.body_only);
    if(u.source){int choice=0;int group=-1;for(int i=0;i<(int)u.source->blocks().size();++i){auto &b=u.source->blocks()[i];
        if((u.kind>0&&(int)b.kind!=u.kind-1)||(u.body_only&&!b.body_ending)||(!u.filter.PassFilter(b.label.c_str())&&!u.filter.PassFilter(b.summary.c_str())))continue;
        if(group!=(int)b.kind){group=(int)b.kind;ImGui::SeparatorText(fatality_kind(b.kind));}
        ImGui::PushID(i);if(ImGui::Selectable(b.label.c_str(),u.block==i))fatality_select(i);if(choice<8)u.row_points[choice++]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SetItemTooltip("Lines %d-%d\n%s",b.first,b.last,b.summary.c_str());ImGui::PopID();
    }}ImGui::EndChild();ImGui::SameLine();ImGui::BeginChild("fatality-editor",ImVec2(0,height),ImGuiChildFlags_Border);
    if(u.source&&u.block>=0){auto b=u.source->blocks()[u.block];ImGui::TextWrapped("%s / lines %d-%d",b.label.c_str(),b.first,b.last);ImGui::TextWrapped("%s",b.summary.c_str());
        if(!b.combo.empty()){
            auto choices=u.workspace.matches(b.combo,FatalityKind::Combo,u.document);
            if(choices.size()<=1){ImGui::BeginDisabled(choices.empty());if(ImGui::Button("Open linked button combo")&&!choices.empty())navigate_fatality(choices.front());u.combo_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();
                if(choices.empty())ImGui::TextDisabled("Combo not found in open sources");}
            else{if(ImGui::BeginCombo("##fatality-combo-source","Choose linked combo source...")){int choice=0;for(auto &match:choices){auto doc=u.workspace.document(match.document);auto name=(doc?doc->path():"Closed source")+" / line "+std::to_string(doc?doc->blocks()[match.block].first:0);ImGui::PushID((int)match.document);ImGui::PushID(match.block);if(ImGui::Selectable(name.c_str()))navigate_fatality(match);if(choice<16)u.combo_choices[choice++]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::PopID();ImGui::PopID();}ImGui::EndCombo();}u.combo_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;}
        }
        ImGui::TextDisabled("Fixed label line: %s",u.source->header(u.block).c_str());
        ImGui::TextDisabled("Edit body below; indent instructions / directives. Apply before switching blocks or sources.");
        if(ImGui::InputTextMultiline("##fatality-body",u.body.data(),u.body.size(),ImVec2(-1,std::max(80.f,ImGui::GetContentRegionAvail().y-12)),ImGuiInputTextFlags_AllowTabInput))u.dirty_preview=true;
        u.body_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    }else ImGui::TextWrapped(u.source?"No editable block selected. Undo can restore a removed command body.":"Load MOVES.ASM for commands and button combos, or a character ASM for animation data.");ImGui::EndChild();
    if(u.dirty_preview)rebuild_fatality();if(!u.error.empty())ImGui::TextWrapped("%s",u.error.c_str());else ImGui::TextDisabled(fatality_staged()?"Unapplied block review. Apply or Reset before saving source.":"Applied source edits have separate Undo / Save. IMG artwork stays unchanged.");
    ImGui::BeginDisabled(!u.valid||!u.patch.changed);if(ImGui::Button("Apply block")){auto b=u.source->blocks()[u.block];if(u.source->apply(u.patch,u.error))fatality_reselect(b.label,b.kind);}u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!u.source||u.block<0);if(ImGui::Button("Reset block"))fatality_select(u.block);u.reset_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!u.source||!u.source->can_undo());if(ImGui::Button("Undo source")&&u.source->undo(u.error))fatality_select(u.block);u.undo_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!u.source||!u.source->can_redo());if(ImGui::Button("Redo source")&&u.source->redo(u.error))fatality_select(u.block);u.redo_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!u.source||fatality_staged());if(ImGui::Button("Save source..."))request_fatality_save(false);u.save_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();if(ImGui::Button("Save As..."))request_fatality_save(true);ImGui::EndDisabled();ImGui::SameLine();
    bool animation=u.source&&u.block>=0&&u.source->blocks()[u.block].kind==FatalityKind::Animation;
    ImGui::BeginDisabled(!animation||fatality_staged());if(ImGui::Button("Inspect animation..."))preview_fatality();u.preview_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!u.source);if(ImGui::Button("Close source"))close_fatality_source();u.close_source_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Done")){if(u.source&&u.source->dirty()){u.guard=1;u.guard_document=u.document;}else u.open=false;ImGui::CloseCurrentPopup();}u.close_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(!ImGui::GetIO().WantTextInput&&u.source){if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_Z,ImGuiInputFlags_RouteGlobal)&&u.source->undo(u.error))fatality_select(u.block);if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_Y,ImGuiInputFlags_RouteGlobal)&&u.source->redo(u.error))fatality_select(u.block);if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_S,ImGuiInputFlags_RouteGlobal))request_fatality_save(false);}
    ImGui::EndPopup();
}
