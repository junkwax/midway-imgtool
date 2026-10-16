void strike_select(int record){auto &u=strikes;u.valid=false;u.dragging=false;u.dirty_preview=false;u.error.clear();if(!u.source)return;
    u.record=std::clamp(record,0,(int)u.source->records().size()-1);auto &r=u.source->records()[u.record];for(int i=0;i<8;++i)snprintf(u.operands[i],sizeof u.operands[i],"%s",r.fields[i].raw.c_str());
}
void show_strikes(){finish();asm_ui.open=false;asm_ui.player.playing=false;auto &u=strikes;u.open=true;if(auto t=tab()){u.preview_doc=t->uid;u.preview_sprite=t->selected_id;t->alignment.player.playing=false;t->embedded.playing=false;t->scene.playing=false;}
    if(!u.source&&!u.path[0]){auto candidate=fs::path("../mk2-main/src/MKSTK.ASM");if(fs::exists(candidate))snprintf(u.path,sizeof u.path,"%s",fs::absolute(candidate).u8string().c_str());}}
void install_strike_source(){strike_phases.owner=nullptr;strike_phases.generation=0;auto &u=strikes;u.source=std::move(u.pending_source);u.character=-1;u.guard=0;u.saving=false;u.open=true;if(u.source){snprintf(u.path,sizeof u.path,"%s",u.source->path().c_str());int selected=0;for(int i=0;i<(int)u.source->records().size();++i){StrikeRect box;auto &r=u.source->records()[i];if(r.editable&&strike_rect(r.fields,0,0,false,box)&&box.w>0&&box.h>0){selected=i;break;}}strike_select(selected);}}
void load_strikes(const std::string &path){auto &u=strikes;auto next=StrikeSource::open(path,u.error);if(!next)return;u.pending_source=std::move(next);
    if(u.source&&u.source->dirty())u.guard=2;else install_strike_source();}
void rebuild_strike_preview(){auto &u=strikes;if(!u.source)return;std::array<std::string,8> values;for(int i=0;i<8;++i)values[i]=u.operands[i];
    u.valid=u.source->review(u.record,values,u.patch,u.error);u.dirty_preview=false;
}
void strike_set(int field,int64_t value){auto &u=strikes;if(!u.source||u.record<0||u.record>=(int)u.source->records().size())return;
    auto raw=strike_literal(u.source->records()[u.record].fields[field].raw,field,value);snprintf(u.operands[field],sizeof u.operands[field],"%s",raw.c_str());u.dirty_preview=true;}
void strike_save_review(){auto &u=strikes;u.save_valid=false;u.save_error.clear();if(!u.source)return;
    u.save_valid=u.source->review_save(u.save_path,u.write_review,u.save_error);if(!u.save_valid)return;
    std::error_code ec;auto dest=fs::u8path(u.write_review.path);for(auto &t:tabs){std::vector<std::string> protected_paths={t->art->path()};if(t->project)protected_paths.push_back(t->project->path());
        for(auto &path:protected_paths)if(!path.empty()&&fs::equivalent(dest,fs::u8path(path),ec)){u.save_valid=false;u.save_error="Strike source cannot replace open artwork or a Studio project.";return;}}
    std::istringstream before(u.source->saved_bytes()),after(u.write_review.bytes);std::string a,b;u.diff.clear();int line=0,count=0;
    while(std::getline(before,a)||!before.eof()){if(!std::getline(after,b))b.clear();++line;if(a!=b){u.diff+="Line "+std::to_string(line)+"\n- "+a+"\n+ "+b+"\n\n";if(++count>=200){u.diff+="Further changed lines omitted from this view.\n";break;}}}
    if(u.diff.empty())u.diff="No source text changes. This writes a copy of the loaded source.";
}
void request_strike_save(bool as){auto &u=strikes;if(!u.source)return;std::string path=u.source->path();
#ifdef _WIN32
    if(as){path=native_file(window,true,path,false,false,false,false,false,false,false,4);if(path.empty())return;}
#else
    (void)as;
#endif
    snprintf(u.save_path,sizeof u.save_path,"%s",path.c_str());u.saving=true;strike_save_review();
}
void complete_strike_guard(){auto &u=strikes;int reason=u.guard;u.guard=0;u.saving=false;
    if(reason==2)install_strike_source();else if(reason==1)u.open=false;else if(reason==3){u.open=false;action(Action::Quit);}}
void strike_guard_dialog(){auto &u=strikes;ImGui::OpenPopup("Save strike source changes?");ImGui::SetNextWindowSizeConstraints(ImVec2(500,0),ImVec2(680,700));
    if(!ImGui::BeginPopupModal("Save strike source changes?",nullptr,ImGuiWindowFlags_AlwaysAutoResize))return;
    ImGui::TextWrapped("Save changes to %s? These belong to the ASM source and are independent of the IMG.",u.source?u.source->path().c_str():"the strike source");
    if(ImGui::Button("Review save",ImVec2(120,0))){request_strike_save(false);ImGui::CloseCurrentPopup();}u.guard_save_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SameLine();if(ImGui::Button("Discard source edits",ImVec2(160,0))){u.source.reset();complete_strike_guard();ImGui::CloseCurrentPopup();}u.guard_discard_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SameLine();if(ImGui::Button("Keep editing",ImVec2(120,0))){u.guard=0;u.pending_source.reset();u.open=true;ImGui::CloseCurrentPopup();}u.guard_cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndPopup();
}
void strike_write_dialog(){auto &u=strikes;ImGui::OpenPopup("Review strike source save");auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(850.f,display.x-30),std::min(680.f,display.y-40)),ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal("Review strike source save",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    ImGui::TextWrapped("Write the reviewed source changes to this ASM file. IMG artwork is not saved or modified here.");
    ImGui::SetNextItemWidth(-1);if(ImGui::InputText("##source-output",u.save_path,sizeof u.save_path))u.save_valid=false;
    if(ImGui::Button("Rebuild save review"))strike_save_review();ImGui::SameLine();ImGui::TextDisabled(u.save_valid?(u.write_review.existed?"Replace existing file":"Create new file"):"Review required");
    ImGui::BeginChild("strike-source-diff",ImVec2(0,std::max(100.f,ImGui::GetContentRegionAvail().y-80)),ImGuiChildFlags_Border);ImGui::TextUnformatted(u.diff.c_str());ImGui::EndChild();
    if(!u.save_error.empty())ImGui::TextWrapped("%s",u.save_error.c_str());bool current=u.save_valid&&u.source&&u.write_review.generation==u.source->generation();
    ImGui::BeginDisabled(!current);bool write_clicked=ImGui::Button("Write source",ImVec2(120,0));if(write_clicked&&u.source){if(u.source->save(u.write_review,u.save_error)){status="Saved strike source: "+u.source->path();u.saving=false;complete_strike_guard();ImGui::CloseCurrentPopup();}}
    u.write_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Cancel",ImVec2(100,0))){u.saving=false;u.guard=0;u.pending_source.reset();u.open=true;ImGui::CloseCurrentPopup();}u.cancel_save_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndPopup();
}
void strike_preview_fields(std::array<StrikeOperand,8> &fields){auto &u=strikes;if(!u.source)return;fields=u.source->records()[u.record].fields;
    for(int i=0;i<8;++i){fields[i].raw=u.operands[i];fields[i].numeric=strike_numeric(fields[i].raw,i,fields[i].value);}}
void strike_canvas(){auto &u=strikes;auto &io=ImGui::GetIO();auto pos=ImGui::GetCursorScreenPos(),avail=ImMax(ImGui::GetContentRegionAvail(),ImVec2(1,1));
    ImGui::InvisibleButton("##strike-canvas",avail,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);bool hovered=ImGui::IsItemHovered();
    float z=u.zoom>0?u.zoom:std::max(.25f,std::min((avail.x-20)/400,(avail.y-40)/254));if(hovered&&io.MouseWheel){u.zoom=std::clamp(z*std::pow(1.2f,io.MouseWheel),.25f,32.f);z=u.zoom;}
    if(hovered&&!u.dragging&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle))u.pan+=io.MouseDelta;
    ImVec2 start=pos+(avail-ImVec2(400*z,254*z))*.5f+u.pan,end=start+ImVec2(400*z,254*z);u.preview_origin=start;u.preview_zoom=z;auto dl=ImGui::GetWindowDrawList();
    dl->PushClipRect(pos,pos+avail,true);dl->AddRectFilled(pos,pos+avail,IM_COL32(18,21,27,255));dl->AddRectFilled(start,end,IM_COL32(28,32,40,255));dl->PushClipRect(ImMax(pos,start),ImMin(pos+avail,end),true);
    auto frame=u.animation?asm_frame():nullptr;int ox=u.origin_x+(frame?(u.flip?-frame->dx:frame->dx):0),oy=u.origin_y+(frame?frame->dy:0);bool facing=frame?u.flip!=frame->flip:u.flip;
    if(u.animation){if(auto t=find(asm_ui.preferred))draw_stage(*t,dl,start,z);draw_asm_frame(dl,start,z,u.origin_x,u.origin_y,u.flip);}
    else if(auto t=find(u.preview_doc)){draw_stage(*t,dl,start,z);int index=t->art->sprite_index(u.preview_sprite);if(index>=0)draw_world_pieces(*t,{index},dl,start,z,u.origin_x,u.origin_y,u.flip,t->convention,IM_COL32(255,255,255,255));}
    auto origin=start+ImVec2(u.origin_x*z,u.origin_y*z);dl->AddLine(origin-ImVec2(8,0),origin+ImVec2(8,0),IM_COL32(239,193,94,255));dl->AddLine(origin-ImVec2(0,8),origin+ImVec2(0,8),IM_COL32(239,193,94,255));
    std::array<StrikeOperand,8> fields;strike_preview_fields(fields);StrikeRect box;bool geometry=u.source&&strike_rect(fields,ox,oy,facing,box);
    if(geometry){auto a=start+ImVec2(box.x*z,box.y*z),b=a+ImVec2(box.w*z,box.h*z);dl->AddRectFilled(a,b,IM_COL32(225,67,89,28));dl->AddRect(a,b,IM_COL32(239,78,103,255),0,0,2);
        u.corner_points[0]=a;u.corner_points[1]=ImVec2(b.x,a.y);u.corner_points[2]=b;u.corner_points[3]=ImVec2(a.x,b.y);
        for(auto corner:u.corner_points)dl->AddCircleFilled(corner,4,IM_COL32(239,193,94,255));
        if(hovered&&u.valid&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)){for(int c=0;c<4;++c){auto delta=io.MousePos-u.corner_points[c];if(delta.x*delta.x+delta.y*delta.y<=64){u.dragging=true;u.drag_kind=c;u.drag_rect[0]=box.x;u.drag_rect[1]=box.y;u.drag_rect[2]=box.w;u.drag_rect[3]=box.h;u.drag_mouse=io.MousePos;break;}}}
    }
    dl->PopClipRect();dl->AddRect(start,end,IM_COL32(90,96,106,255));dl->AddText(pos+ImVec2(10,6),IM_COL32(180,186,197,255),geometry?"STRIKE PREVIEW | Object-origin coordinates":"Symbolic or invalid geometry cannot be previewed");dl->PopClipRect();
    if(u.dragging&&ImGui::IsMouseDown(ImGuiMouseButton_Left)){int dx=(int)std::round((io.MousePos.x-u.drag_mouse.x)/z),dy=(int)std::round((io.MousePos.y-u.drag_mouse.y)/z);
        int left=u.drag_rect[0],top=u.drag_rect[1],right=left+u.drag_rect[2],bottom=top+u.drag_rect[3];if(u.drag_kind==0||u.drag_kind==3)left=std::min(right,left+dx);else right=std::max(left,right+dx);if(u.drag_kind<2)top=std::min(bottom,top+dy);else bottom=std::max(top,bottom+dy);
        int w=std::clamp(right-left,0,32767),h=std::clamp(bottom-top,0,32767),hx=std::clamp(facing?ox-left:left+w-ox,-32768,32767),hy=std::clamp(top-oy,-32768,32767);
        strike_set(0,hx);strike_set(1,hy);strike_set(2,w);strike_set(3,h);}
    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left))u.dragging=false;
}
void strikes_dialog(){auto &u=strikes;if(u.saving){strike_write_dialog();return;}if(u.guard){strike_guard_dialog();return;}if(u.open)ImGui::OpenPopup("MK2 strike tables");auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(1250.f,display.x-30),std::min(820.f,display.y-40)),ImGuiCond_Appearing);ImGui::SetNextWindowSizeConstraints(ImVec2(950,560),display-ImVec2(20,20));
    if(!ImGui::BeginPopupModal("MK2 strike tables",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    ImGui::TextWrapped("Edit MKSTK.ASM strike records. Preview a sprite or ASM animation; source edits have their own history.");
    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##strike-path","Path to MKSTK.ASM",u.path,sizeof u.path);
    if(ImGui::Button("Browse...")){
#ifdef _WIN32
        auto path=native_file(window,false,u.path,false,false,false,false,false,false,false,4);if(!path.empty())snprintf(u.path,sizeof u.path,"%s",path.c_str());
#endif
    }ImGui::SameLine();if(ImGui::Button("Load source"))load_strikes(u.path);u.load_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SameLine();if(u.source)ImGui::TextDisabled("%zu records | %s",u.source->records().size(),u.source->dirty()?"Unsaved ASM changes":"Source saved");
    if(u.source)ImGui::TextWrapped("Editing: %s",u.source->path().c_str());
    if(u.source){if(ImGui::Button("Inspect phase countdown...")){show_strike_phases();ImGui::CloseCurrentPopup();}}
    if(u.source&&u.dirty_preview)rebuild_strike_preview();
    float height=std::max(150.f,ImGui::GetContentRegionAvail().y-53);
    ImGui::BeginChild("strike-records",ImVec2(190,height),ImGuiChildFlags_Border);
    if(u.source){ImGui::SetNextItemWidth(-1);if(ImGui::InputTextWithHint("##strike-filter","Filter strikes...",u.filter.InputBuf,sizeof u.filter.InputBuf))u.filter.Build();auto &chars=u.source->characters();const char *name=u.character>=0&&u.character<(int)chars.size()?chars[u.character].name.c_str():"All records";
        ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##strike-character",name)){if(ImGui::Selectable("All records",u.character<0))u.character=-1;for(int i=0;i<(int)chars.size();++i)if(ImGui::Selectable(chars[i].name.c_str(),u.character==i))u.character=i;ImGui::EndCombo();}
        std::vector<int> visible;for(int i=0;i<(int)u.source->records().size();++i){auto &r=u.source->records()[i];if(!u.filter.PassFilter(r.label.c_str()))continue;
            if(u.character>=0&&u.character<(int)chars.size()&&std::find(chars[u.character].moves.begin(),chars[u.character].moves.end(),r.label)==chars[u.character].moves.end())continue;visible.push_back(i);}
        ImGuiListClipper clip;clip.Begin((int)visible.size());while(clip.Step())for(int v=clip.DisplayStart;v<clip.DisplayEnd;++v){int i=visible[v];ImGui::PushID(i);auto &r=u.source->records()[i];if(ImGui::Selectable(r.label.c_str(),u.record==i))strike_select(i);ImGui::PopID();}}
    else ImGui::TextWrapped("Choose the game's strike-table ASM source, then Load source.");ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("strike-fields",ImVec2(250,height),ImGuiChildFlags_Border);
    if(u.source){auto &r=u.source->records()[u.record];ImGui::TextWrapped("%s | line %d",r.label.c_str(),r.line);if(!r.editable)ImGui::TextWrapped("%s",r.error.c_str());
        ImGui::BeginDisabled(!r.editable);const char *geometry[]={"X offset","Y offset","Box width","Box height"};
        for(int i=0;i<4;++i){int64_t n=0;bool numeric=strike_numeric(u.operands[i],i,n);int value=numeric?(short)n:0;ImGui::SetNextItemWidth(130);if(ImGui::InputInt(geometry[i],&value))strike_set(i,std::clamp(value,i<2?-32768:0,32767));if(!numeric)ImGui::TextDisabled("Stored expression: %s",u.operands[i]);}
        for(int i:{4,7}){ImGui::TextUnformatted(i==4?"Strike routine":"Sound flags");ImGui::SetNextItemWidth(-1);if(ImGui::InputText(i==4?"##strike-routine":"##strike-sound",u.operands[i],sizeof u.operands[i]))u.dirty_preview=true;}
        int64_t damage=0;bool damage_numeric=strike_numeric(u.operands[5],5,damage);int hit=mk2::damage_hit((int)damage),block=mk2::damage_block((int)damage);ImGui::SetNextItemWidth(130);
        if(ImGui::InputInt("Hit damage",&hit))strike_set(5,mk2::pack_damage(std::clamp(hit,0,255),block));ImGui::SetNextItemWidth(130);if(ImGui::InputInt("Block damage",&block))strike_set(5,mk2::pack_damage(hit,std::clamp(block,0,255)));if(!damage_numeric)ImGui::TextDisabled("Stored damage: %s",u.operands[5]);
        int64_t score=0;strike_numeric(u.operands[6],6,score);uint32_t shown_score=(uint32_t)score;ImGui::SetNextItemWidth(130);if(ImGui::InputScalar("Score",ImGuiDataType_U32,&shown_score))strike_set(6,shown_score);ImGui::EndDisabled();
        if(r.aliases.size()>1){ImGui::TextWrapped("Shared data: edits affect %zu labels.",r.aliases.size());for(auto &alias:r.aliases)ImGui::TextDisabled("%s",alias.c_str());}
        if(u.dirty_preview)rebuild_strike_preview();if(!u.valid&&r.editable)rebuild_strike_preview();if(!u.error.empty())ImGui::TextWrapped("%s",u.error.c_str());
        ImGui::TextWrapped("Changes stay in preview until Apply record. Source saves review exact changed lines.");
    }else if(!u.error.empty())ImGui::TextWrapped("%s",u.error.c_str());ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("strike-view",ImVec2(0,height),ImGuiChildFlags_Border);
    if(ImGui::Button("Animation source...")){show_asm();ImGui::CloseCurrentPopup();}ImGui::SameLine();ImGui::BeginDisabled(!asm_ui.source);ImGui::Checkbox("Animate",&u.animation);ImGui::EndDisabled();
    if(u.animation&&asm_ui.source){auto a=asm_animation();ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##strike-animation",a?a->label.c_str():"Choose animation")){for(int i=0;i<(int)asm_ui.source->animations.size();++i){auto &anim=asm_ui.source->animations[i];if(ImGui::Selectable(anim.label.c_str(),i==asm_ui.selected))asm_select(i);}ImGui::EndCombo();}
        if(u.source&&ImGui::Button("Suggest animation from strike")){std::vector<std::string> labels;for(auto &anim:asm_ui.source->animations)labels.push_back(anim.label);int match=strike_bind::MatchAnimForStrike(u.source->records()[u.record].label,labels);if(match>=0)asm_select(match);else status="No unique animation name match. Choose a preview animation.";}
        if(u.dragging)asm_ui.player.playing=false;asm_transport();asm_library_picker();asm_resolve();int missing=0;for(auto &p:asm_ui.pieces)missing+=!p.sprite;if(missing)ImGui::TextWrapped("%d unresolved pieces. Open their IMG libraries in the animation viewer.",missing);
        if(a&&a->partial)ImGui::TextWrapped("Partial animation preview; see source viewer for omitted runtime behavior.");
    }
    if(!u.animation){
    auto preview=find(u.preview_doc);ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##strike-preview-library",preview?preview->name().c_str():"No sprite preview")){
        if(ImGui::Selectable("None",!preview)){u.preview_doc=0;u.preview_sprite=0;}for(auto &t:tabs){ImGui::PushID((int)t->uid);if(ImGui::Selectable(t->name().c_str(),t->uid==u.preview_doc)){u.preview_doc=t->uid;u.preview_sprite=t->selected_id;}ImGui::PopID();}ImGui::EndCombo();}
    preview=find(u.preview_doc);if(preview){int index=preview->art->sprite_index(u.preview_sprite);auto im=preview->art->sprite(index);ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##strike-preview-sprite",im?im->n_s:"Missing sprite")){for(int i=0;i<preview->art->count();++i){ImGui::PushID(i);if(ImGui::Selectable(preview->art->sprite(i)->n_s,i==index))u.preview_sprite=preview->art->sprite_id(i);ImGui::PopID();}ImGui::EndCombo();}}
    }
    ImGui::Checkbox("Facing left",&u.flip);ImGui::SameLine();if(ImGui::Button("Fit")){u.zoom=0;u.pan={};}int origin[]={u.origin_x,u.origin_y};ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##strike-object-origin",origin)){u.origin_x=std::clamp(origin[0],-4096,4096);u.origin_y=std::clamp(origin[1],-4096,4096);}ImGui::TextDisabled("Object origin X / Y | overlay does not imply active timing");
    if(u.source)strike_canvas();ImGui::EndChild();
    bool apply=u.valid&&u.patch.changed&&u.source;ImGui::BeginDisabled(!apply);if(ImGui::Button("Apply record")&&u.source){if(u.source->apply(u.patch,u.error)){int record=u.record;strike_select(record);status="Updated strike source in memory. IMG artwork unchanged.";}}u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!u.source||!u.source->can_undo());bool undo=ImGui::Button("Undo source");u.undo_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(!u.source||!u.source->can_redo());bool redo=ImGui::Button("Redo source");u.redo_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!u.source);if(ImGui::Button("Save source..."))request_strike_save(false);u.save_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();if(ImGui::Button("Save source As..."))request_strike_save(true);ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Done")){if(u.source&&u.source->dirty())u.guard=1;else{u.open=false;ImGui::CloseCurrentPopup();}}u.close_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(!ImGui::GetIO().WantTextInput&&u.source){const auto route=ImGuiInputFlags_RouteGlobal;undo|=ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_Z,route);redo|=(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_Y,route)||ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiMod_Shift|ImGuiKey_Z,route));if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_S,route))request_strike_save(false);if(ImGui::IsKeyPressed(ImGuiKey_Escape))strike_select(u.record);}
    if(undo&&u.source&&u.source->can_undo()){u.source->undo();strike_select(u.record);}else if(redo&&u.source&&u.source->can_redo()){u.source->redo();strike_select(u.record);}ImGui::EndPopup();
}
