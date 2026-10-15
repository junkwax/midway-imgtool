std::string bindings_key(std::string path){
#ifdef _WIN32
    for(char &c:path)if(c>='A'&&c<='Z')c+=32;
#endif
    return path;
}
StrikeBindings *selected_bindings(){auto &u=binding_ui;auto it=u.documents.find(u.selected);return it==u.documents.end()?nullptr:it->second.get();}
void sync_binding_fields(){auto &u=binding_ui;auto doc=selected_bindings();if(!doc)return;auto target=doc->lookup(u.animation);snprintf(u.strike,sizeof u.strike,"%s",target?target->c_str():"");}
StrikeBindings *current_bindings(){if(!strikes.source)return nullptr;auto &u=binding_ui;auto key=bindings_key(strike_bind::SidecarPathFor(strikes.source->path()));auto found=u.documents.find(key);if(found!=u.documents.end())return found->second.get();if(u.failed==key)return nullptr;
    if(u.documents.size()>=32){u.failed=key;u.error="32 association documents are already open. Save them and restart before opening another.";return nullptr;}
    auto doc=StrikeBindings::open_for(strikes.source->path(),u.error);if(!doc){u.failed=key;return nullptr;}auto result=doc.get();u.documents.emplace(key,std::move(doc));if(u.selected.empty())u.selected=key;return result;
}
StrikeBindings *dirty_bindings(){for(auto &p:binding_ui.documents)if(p.second->dirty())return p.second.get();return nullptr;}
void show_bindings(){auto &u=binding_ui;u.return_asm=asm_ui.open;asm_ui.open=false;asm_ui.player.playing=false;strikes.open=false;u.open=true;
    if(auto doc=current_bindings()){u.selected=bindings_key(doc->path());sync_binding_fields();}}
void set_asm_binding(const std::optional<std::string> &target){auto a=asm_animation();auto doc=current_bindings();if(a&&doc)doc->set(a->label,target,binding_ui.error);}
void request_bindings_quit(){auto &u=binding_ui;auto doc=dirty_bindings();if(!doc)return;strike_phases.open=false;strike_phases.playing=false;asm_ui.open=false;asm_ui.player.playing=false;strikes.open=false;strikes.saving=false;strikes.guard=0;strikes.pending_source.reset();u.selected=bindings_key(doc->path());u.guard=1;u.open=false;u.saving=false;u.return_asm=false;u.pending_reload.reset();}
void bindings_save_review(){auto &u=binding_ui;u.valid=false;u.save_error.clear();auto doc=selected_bindings();if(!doc)return;u.valid=doc->review_save(u.review,u.save_error);if(!u.valid)return;
    std::vector<std::string> paths;for(auto &t:tabs){paths.push_back(t->art->path());if(t->project)paths.push_back(t->project->path());}if(asm_ui.source)paths.push_back(asm_ui.source->path);if(strikes.source)paths.push_back(strikes.source->path());for(auto &p:u.documents)paths.push_back(p.second->source());
    std::error_code ec;for(auto &p:paths)if(!p.empty()&&fs::equivalent(fs::u8path(u.review.path),fs::u8path(p),ec)){u.valid=false;u.save_error="The association destination aliases open artwork, a project or ASM source.";return;}
}
void request_bindings_save(){auto &u=binding_ui;u.saving=true;asm_ui.player.playing=false;bindings_save_review();}
void bindings_reload(){auto &u=binding_ui;auto doc=selected_bindings();if(!doc)return;auto next=StrikeBindings::open_for(doc->source(),u.error);if(!next)return;
    if(doc->dirty()){u.pending_reload=std::move(next);u.guard=2;}else{u.documents[u.selected]=std::move(next);sync_binding_fields();}}
void finish_bindings_guard(){auto &u=binding_ui;int reason=u.guard;u.guard=0;u.saving=false;
    if(reason==1){u.open=false;action(Action::Quit);}else if(reason==2){if(u.pending_reload){u.documents[u.selected]=std::move(u.pending_reload);}sync_binding_fields();u.open=true;}
}
void bindings_guard_dialog(){auto &u=binding_ui;auto doc=selected_bindings();ImGui::OpenPopup("Save strike associations?");ImGui::SetNextWindowSizeConstraints(ImVec2(510,0),ImVec2(700,700));
    if(!ImGui::BeginPopupModal("Save strike associations?",nullptr,ImGuiWindowFlags_AlwaysAutoResize))return;
    ImGui::TextWrapped("Save changed associations to %s? These are separate from IMG artwork and strike ASM edits.",doc?doc->path().c_str():"the sidecar");
    if(ImGui::Button("Review save",ImVec2(120,0))){request_bindings_save();ImGui::CloseCurrentPopup();}u.guard_save_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();
    if(ImGui::Button("Discard associations",ImVec2(170,0))){if(doc)doc->discard();finish_bindings_guard();ImGui::CloseCurrentPopup();}u.guard_discard_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();
    if(ImGui::Button("Keep editing",ImVec2(120,0))){u.guard=0;u.pending_reload.reset();u.open=true;ImGui::CloseCurrentPopup();}u.guard_cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndPopup();
}
void bindings_write_dialog(){auto &u=binding_ui;auto doc=selected_bindings();ImGui::OpenPopup("Review association save");auto display=ImGui::GetIO().DisplaySize;ImGui::SetNextWindowSize(ImVec2(std::min(880.f,display.x-30),std::min(700.f,display.y-40)),ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal("Review association save",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    ImGui::TextWrapped("Write only the association sidecar beside its strike source. IMG files and ASM source files are unchanged.");if(doc)ImGui::TextWrapped("Destination: %s",doc->path().c_str());
    if(ImGui::Button("Refresh review"))bindings_save_review();ImGui::SameLine();ImGui::TextDisabled(u.valid?(u.review.existed?"Replace reviewed sidecar":"Create sidecar"):"Review required");
    float height=std::max(80.f,(ImGui::GetContentRegionAvail().y-85)*.5f);ImGui::TextDisabled("Original");ImGui::BeginChild("bindings-before",ImVec2(0,height),ImGuiChildFlags_Border);if(doc)ImGui::TextUnformatted(doc->saved_bytes().empty()?"(No sidecar content)":doc->saved_bytes().c_str());ImGui::EndChild();
    ImGui::TextDisabled("After save");ImGui::BeginChild("bindings-after",ImVec2(0,height),ImGuiChildFlags_Border);if(doc)ImGui::TextUnformatted(doc->bytes().c_str());ImGui::EndChild();if(!u.save_error.empty())ImGui::TextWrapped("%s",u.save_error.c_str());
    ImGui::BeginDisabled(!u.valid||!doc||u.review.generation!=doc->generation());if(ImGui::Button("Write associations",ImVec2(160,0))&&doc){auto reviewed=u.review;bindings_save_review();if(u.valid&&doc->save(reviewed,u.save_error)){status="Saved strike associations: "+doc->path();u.saving=false;finish_bindings_guard();ImGui::CloseCurrentPopup();}}u.write_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Cancel",ImVec2(100,0))){u.saving=false;u.guard=0;u.pending_reload.reset();u.open=true;ImGui::CloseCurrentPopup();}u.cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndPopup();
}
void bindings_editor(){auto &u=binding_ui;ImGui::OpenPopup("Strike associations");auto display=ImGui::GetIO().DisplaySize;ImGui::SetNextWindowSize(ImVec2(std::min(1040.f,display.x-30),std::min(740.f,display.y-40)),ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal("Strike associations",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    ImGui::TextWrapped("Remember which strike belongs to each animation. Associations guide the preview; they do not control game timing.");auto doc=selected_bindings();ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##binding-document",doc?doc->path().c_str():"Load a strike ASM source first")){
        for(auto &p:u.documents){auto name=p.second->path()+(p.second->dirty()?" *":"");if(ImGui::Selectable(name.c_str(),p.first==u.selected)){u.selected=p.first;u.animation[0]=u.strike[0]=0;u.error.clear();}}ImGui::EndCombo();}doc=selected_bindings();
    if(!doc){ImGui::TextWrapped("Load MKSTK.ASM using Tools > MK2 strike tables, then open this workspace.");if(ImGui::Button("Retry sidecar load")){u.failed.clear();if(auto current=current_bindings())u.selected=bindings_key(current->path());}}
    if(doc){ImGui::TextWrapped("Strike source: %s",doc->source().c_str());ImGui::TextDisabled(doc->dirty()?"Unsaved associations | separate history":"Saved associations | separate history");for(auto &warning:doc->warnings())ImGui::TextWrapped("%s",warning.c_str());
        float height=std::max(90.f,ImGui::GetContentRegionAvail().y-180);ImGui::BeginChild("binding-list",ImVec2(0,height),ImGuiChildFlags_Border);if(doc->bindings().empty())ImGui::TextWrapped("No explicit associations. Unique name suggestions remain available.");
        if(ImGui::BeginTable("binding-pairs",2,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_SizingStretchProp)){ImGui::TableSetupColumn("Animation");ImGui::TableSetupColumn("Strike");ImGui::TableHeadersRow();for(auto &pair:doc->bindings()){ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::PushID(pair.anim.c_str());if(ImGui::Selectable(pair.anim.c_str(),std::string(u.animation)==pair.anim,ImGuiSelectableFlags_SpanAllColumns)){snprintf(u.animation,sizeof u.animation,"%s",pair.anim.c_str());snprintf(u.strike,sizeof u.strike,"%s",pair.strike.c_str());}ImGui::TableSetColumnIndex(1);ImGui::TextUnformatted(pair.strike=="-"?"None (disable suggestion)":pair.strike.c_str());ImGui::PopID();}ImGui::EndTable();}ImGui::EndChild();
        ImGui::TextDisabled("Assembly labels | '-' means none | remove an entry to restore name suggestions");ImGui::SetNextItemWidth(250);ImGui::InputTextWithHint("##binding-anim","Animation label",u.animation,sizeof u.animation);ImGui::SameLine();ImGui::SetNextItemWidth(250);ImGui::InputTextWithHint("##binding-strike","Strike label or -",u.strike,sizeof u.strike);ImGui::SameLine();
        if(ImGui::Button("Set association"))doc->set(u.animation,std::string(u.strike),u.error);u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();if(ImGui::Button("Use suggestion"))doc->set(u.animation,{},u.error);
        ImGui::TextWrapped("Entries for other characters are retained. Unusual lines stay intact and may require correction outside Studio.");
        ImGui::BeginDisabled(!doc->can_undo());if(ImGui::Button("Undo associations")){doc->undo();sync_binding_fields();}u.undo_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(!doc->can_redo());if(ImGui::Button("Redo associations")){doc->redo();sync_binding_fields();}u.redo_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
        ImGui::BeginDisabled(!doc->dirty());if(ImGui::Button("Save associations..."))request_bindings_save();u.save_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Reload from disk..."))bindings_reload();u.reload_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    }if(!u.error.empty())ImGui::TextWrapped("%s",u.error.c_str());if(ImGui::Button(u.return_asm?"Back to animation":"Done",ImVec2(150,0))){u.open=false;if(u.return_asm){u.return_asm=false;show_asm();}ImGui::CloseCurrentPopup();}u.done_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    doc=selected_bindings();if(doc&&!ImGui::GetIO().WantTextInput){auto route=ImGuiInputFlags_RouteGlobal;if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_Z,route)){doc->undo();sync_binding_fields();}if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_Y,route)){doc->redo();sync_binding_fields();}if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_S,route)&&doc->dirty())request_bindings_save();}ImGui::EndPopup();
}
bool bindings_dialogs(){auto &u=binding_ui;if(u.saving){bindings_write_dialog();return true;}if(u.guard){bindings_guard_dialog();return true;}if(u.open){bindings_editor();return true;}return false;}
