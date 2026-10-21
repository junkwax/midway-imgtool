void show_asm(){finish();asm_ui.open=true;asm_ui.reaction_mode=false;asm_ui.player.playing=false;strikes.open=false;
    if(auto t=tab()){asm_ui.preferred=t->uid;t->alignment.player.playing=false;t->embedded.playing=false;t->scene.playing=false;}}
const AsmSourceAnimation *asm_animation(){auto &u=asm_ui;if(!u.source||u.selected<0||u.selected>=(int)u.source->animations.size())return nullptr;return &u.source->animations[u.selected];}
const AsmSourceFrame *asm_frame(){auto a=asm_animation();if(!a||a->frames.empty())return nullptr;asm_ui.player.frame=std::clamp(asm_ui.player.frame,0,(int)a->frames.size()-1);return &a->frames[asm_ui.player.frame];}
void asm_select(int index){auto &u=asm_ui;if(!u.source)return;u.selected=std::clamp(index,0,(int)u.source->animations.size()-1);u.player.playing=false;u.player.seek(0,(int)u.source->animations[u.selected].frames.size());u.zoom=0;u.pan={};u.cache_anim=-1;u.reactions.cache_animation=-1;u.reactions.selected=-1;u.assets.selected=-1;u.assets.cache_animation=-1;}
void install_asm(std::unique_ptr<AsmSource> source){
    auto &u=asm_ui;auto catalogue=source?classify_asm_reactions(*source):std::vector<ReactionAnimation>{};
    auto visible=filter_asm_reactions(catalogue,u.reactions.family,u.reactions.show_all,u.reactions.search);
    int selected=u.reaction_mode&&!visible.empty()?catalogue[visible.front()].animation:0;
    // Prepare directory/filter storage before replacing the current source.
    u.reactions.catalogue=std::move(catalogue);u.reactions.visible=std::move(visible);
    u.scene_review=AsmUi::SceneReview{};u.assets=AsmAssetsUi{};u.reactions.anchors={};u.reactions.error.clear();u.source=std::move(source);u.error.clear();
    if(u.source){snprintf(u.path,sizeof u.path,"%s",u.source->path.c_str());asm_select(selected);}
}
bool load_asm(const std::string &path){try{auto source=AsmSource::open(path,asm_ui.error);if(!source)return false;install_asm(std::move(source));return true;}catch(const std::bad_alloc &){asm_ui.error="Not enough memory to load animation source. The previous source was kept.";return false;}}
void asm_load_controls(){auto &u=asm_ui;ImGui::SetNextItemWidth(std::max(80.f,ImGui::GetContentRegionAvail().x-150));ImGui::InputText("##asm-source-path",u.path,sizeof u.path);ImGui::SameLine();
#ifdef _WIN32
    if(ImGui::Button("Browse...")){auto path=native_file(window,false,u.path,false,false,false,false,false,false,false,5);if(!path.empty())load_asm(path);}ImGui::SameLine();
#endif
    if(ImGui::Button("Load ASM"))load_asm(u.path);if(!u.error.empty())ImGui::TextWrapped("%s",u.error.c_str());
}
void asm_resolve(){auto &u=asm_ui;auto f=asm_frame();if(!f){u.pieces.clear();return;}std::vector<std::pair<uint64_t,uint64_t>> signature;std::vector<AsmLibrary> libs;
    for(auto &t:tabs){signature.push_back({t->uid,t->art->generation()});libs.push_back({t->uid,t->art.get()});}
    if(u.cache_anim==u.selected&&u.cache_frame==u.player.frame&&u.cache_preferred==u.preferred&&signature==u.cache_libraries)return;
    u.pieces=resolve_asm_frame(*f,libs,u.preferred,u.assets.choices);u.cache_anim=u.selected;u.cache_frame=u.player.frame;u.cache_preferred=u.preferred;u.cache_libraries=std::move(signature);
}
void asm_library_picker(){auto &u=asm_ui;auto preferred=find(u.preferred);ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##asm-preferred-library",preferred?preferred->name().c_str():"All open libraries (unique names)")){
    if(ImGui::Selectable("All open libraries (unique names)",!preferred))u.preferred=0;for(auto &t:tabs){ImGui::PushID((int)t->uid);if(ImGui::Selectable(t->name().c_str(),t->uid==u.preferred))u.preferred=t->uid;ImGui::PopID();}ImGui::EndCombo();}
    ImGui::TextDisabled("Preferred library resolves duplicate names");}
void asm_floor(){auto &u=asm_ui;auto f=asm_frame();if(!f)return;asm_resolve();int bottom=-32768;for(auto &piece:u.pieces)if(auto t=find(piece.doc)){auto im=t->art->sprite(t->art->sprite_index(piece.sprite));if(im){auto p=place_asm_piece(*im,*f,0,0,u.facing,t->convention);bottom=std::max(bottom,p.y+p.h);}}if(bottom!=-32768)u.origin_y=std::clamp(230-bottom,-4096,4096);}
void asm_transport(){auto &u=asm_ui;auto a=asm_animation();int count=a?(int)a->frames.size():0;ImGui::BeginDisabled(count==0);
    if(ImGui::Button("<##asm-prev"))u.player.step(-1,count);u.previous_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();
    if(ImGui::Button(u.player.playing?"Pause##asm-play":"Play##asm-play"))u.player.playing=!u.player.playing;u.play_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();
    if(ImGui::Button(">##asm-next"))u.player.step(1,count);u.next_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();ImGui::Checkbox("Loop",&u.player.loop);ImGui::EndDisabled();
    int frame=u.player.frame+1;ImGui::SetNextItemWidth(-1);if(ImGui::SliderInt("##asm-frame",&frame,1,std::max(1,count),"Frame %d")){u.player.playing=false;u.player.seek(frame-1,count);}
    if(count&&u.player.playing)u.player.advance(ImGui::GetIO().DeltaTime,std::vector<int>(count,u.hold),mk2_tick_hz);
}
int asm_strike_record(){auto a=asm_animation();if(!a||!strikes.source)return -1;auto doc=current_bindings();if(!doc)return -1;auto bound=doc->lookup(a->label);
    if(bound){if(*bound=="-")return -1;for(int i=0;i<(int)strikes.source->records().size();++i){auto &r=strikes.source->records()[i];auto left=r.label,right=*bound;for(auto &c:left)c=(char)std::tolower((unsigned char)c);for(auto &c:right)c=(char)std::tolower((unsigned char)c);if(left==right&&r.editable)return i;}return -1;}
    std::vector<std::string> labels;std::vector<int> indices;for(int i=0;i<(int)strikes.source->records().size();++i){auto &r=strikes.source->records()[i];if(r.editable){labels.push_back(r.label);indices.push_back(i);}}
    int match=strike_bind::MatchStrikeForAnim(a->label,labels);return match>=0?indices[match]:-1;
}
void asm_binding_picker(){auto a=asm_animation();if(!a||!strikes.source)return;auto &u=asm_ui;auto doc=current_bindings();auto bound=doc?doc->lookup(a->label):std::optional<std::string>{};int record=asm_strike_record();std::string caption=record>=0?strikes.source->records()[record].label:bound&&*bound!="-"?"Unavailable: "+*bound:"No strike association";
    ImGui::BeginDisabled(!doc);ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##asm-strike-binding",caption.c_str())){
        if(ImGui::Selectable("Suggest from names",!bound))set_asm_binding({});
        if(ImGui::Selectable("None",bound&&*bound=="-"))set_asm_binding(std::string("-"));
        for(auto &r:strikes.source->records())if(r.editable){ImGui::PushID(r.label.c_str());if(ImGui::Selectable(r.label.c_str(),record>=0&&r.label==strikes.source->records()[record].label))set_asm_binding(r.label);ImGui::PopID();}ImGui::EndCombo();}ImGui::EndDisabled();
    u.bind_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::TextDisabled(bound?(doc&&doc->dirty()?"Unsaved association":"Saved association"):"Suggested from names");ImGui::Checkbox("Show strike overlay",&u.overlay);
    if(ImGui::Button("Manage / save associations...")){show_bindings();ImGui::CloseCurrentPopup();}if(!binding_ui.error.empty())ImGui::TextWrapped("%s",binding_ui.error.c_str());
    ImGui::BeginDisabled(record<0);if(ImGui::Button("Edit associated strike...")){asm_ui.open=false;asm_ui.player.playing=false;show_strikes();strikes.animation=true;strike_select(record);ImGui::CloseCurrentPopup();}ImGui::EndDisabled();
}
void draw_asm_frame(ImDrawList *dl,ImVec2 start,float z,int x,int y,bool facing){auto f=asm_frame();if(!f)return;asm_resolve();
    for(auto &piece:asm_ui.pieces)if(auto t=find(piece.doc)){int i=t->art->sprite_index(piece.sprite);auto im=t->art->sprite(i);if(!im)continue;auto p=place_asm_piece(*im,*f,x,y,facing,t->convention);
        if(auto tex=texture(*t,i))dl->AddImage((ImTextureID)(intptr_t)tex,start+ImVec2(p.x*z,p.y*z),start+ImVec2((p.x+p.w)*z,(p.y+p.h)*z),ImVec2(facing!=f->flip?1.f:0.f,f->flip_y?1.f:0.f),ImVec2(facing!=f->flip?0.f:1.f,f->flip_y?0.f:1.f));}
}
void asm_canvas(float height=0){auto &u=asm_ui;auto pos=ImGui::GetCursorScreenPos(),avail=ImMax(ImGui::GetContentRegionAvail(),ImVec2(1,1));if(height>0)avail.y=std::min(avail.y,height);ImGui::InvisibleButton("##asm-canvas",avail,ImGuiButtonFlags_MouseButtonMiddle);bool hovered=ImGui::IsItemHovered();auto &io=ImGui::GetIO();
    float z=u.zoom>0?u.zoom:std::max(.25f,std::min((avail.x-20)/400,(avail.y-30)/254));if(hovered&&io.MouseWheel){u.zoom=std::clamp(z*std::pow(1.2f,io.MouseWheel),.25f,32.f);z=u.zoom;}if(hovered&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle))u.pan+=io.MouseDelta;
    ImVec2 start=pos+(avail-ImVec2(400*z,254*z))*.5f+u.pan,end=start+ImVec2(400*z,254*z);auto dl=ImGui::GetWindowDrawList();dl->PushClipRect(pos,pos+avail,true);dl->AddRectFilled(pos,pos+avail,IM_COL32(18,21,27,255));dl->AddRectFilled(start,end,IM_COL32(28,32,40,255));dl->PushClipRect(ImMax(pos,start),ImMin(pos+avail,end),true);
    if(auto t=find(u.preferred))draw_stage(*t,dl,start,z);dl->AddLine(start+ImVec2(0,230*z),start+ImVec2(400*z,230*z),IM_COL32(72,78,88,255));draw_asm_frame(dl,start,z,u.origin_x,u.origin_y,u.facing);auto origin=start+ImVec2(u.origin_x*z,u.origin_y*z);dl->AddLine(origin-ImVec2(8,0),origin+ImVec2(8,0),IM_COL32(239,193,94,255));dl->AddLine(origin-ImVec2(0,8),origin+ImVec2(0,8),IM_COL32(239,193,94,255));
    auto f=asm_frame();int record=asm_strike_record();if(!u.reaction_mode&&f&&u.overlay&&record>=0){StrikeRect box;if(strike_rect(strikes.source->records()[record].fields,u.origin_x+(u.facing?-f->dx:f->dx),u.origin_y+f->dy,u.facing!=f->flip,box)){
        auto a=start+ImVec2(box.x*z,box.y*z),b=a+ImVec2(box.w*z,box.h*z);dl->AddRectFilled(a,b,IM_COL32(225,67,89,28));dl->AddRect(a,b,IM_COL32(239,78,103,255),0,0,2);}}
    dl->PopClipRect();dl->AddRect(start,end,IM_COL32(90,96,106,255));dl->PopClipRect();
}
void asm_dialog(){auto &u=asm_ui;if(!u.open)return;ImGui::OpenPopup("ASM animations / Reactions");auto display=ImGui::GetIO().DisplaySize;ImGui::SetNextWindowSize(ImVec2(std::min(1120.f,display.x-30),std::min(790.f,display.y-40)),ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal("ASM animations / Reactions",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    asm_load_controls();if(ImGui::RadioButton("All animations",!u.reaction_mode)){u.reaction_mode=false;u.player.playing=false;}ImGui::SameLine();if(ImGui::RadioButton("Reactions",u.reaction_mode)){u.reaction_mode=true;u.player.playing=false;reaction_choose_visible();}u.reactions.mode_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(u.source)ImGui::TextWrapped("%s",u.source->path.c_str());ImGui::TextDisabled("Source inspection | timing is a preview override; game routines are not executed");
    float height=std::max(160.f,ImGui::GetContentRegionAvail().y-42),left=std::clamp(ImGui::GetContentRegionAvail().x*.32f,260.f,350.f);
    ImGui::BeginChild("asm-controls",ImVec2(left,height),ImGuiChildFlags_Border);if(u.reaction_mode)reaction_list(height);else{ImGui::SetNextItemWidth(-1);if(ImGui::InputTextWithHint("##asm-filter","Find animation...",u.filter.InputBuf,sizeof u.filter.InputBuf))u.filter.Build();
    ImGui::BeginChild("asm-list",ImVec2(0,std::max(80.f,height*.29f)),ImGuiChildFlags_Border);if(u.source)for(int i=0;i<(int)u.source->animations.size();++i){auto &a=u.source->animations[i];if(!u.filter.PassFilter(a.label.c_str())&&!u.filter.PassFilter(a.name.c_str()))continue;ImGui::PushID(i);if(ImGui::Selectable(a.label.c_str(),i==u.selected))asm_select(i);ImGui::PopID();}ImGui::EndChild();}
    auto a=asm_animation();if(a){ImGui::TextWrapped("%s",a->name.c_str());if(u.reaction_mode)reaction_description();asm_transport();asm_library_picker();if(ImGui::Button("Open IMG library...")){auto path=native_file(window,false,{},false,false,false,false,false,false,false,6);if(!path.empty()){auto extension=fs::u8path(path).extension().u8string();for(auto &c:extension)c=(char)std::toupper((unsigned char)c);if(extension==".IMG")open(path);else u.error="Choose an IMG sprite library.";}}ImGui::SameLine();if(ImGui::Button("Fit")){u.zoom=0;u.pan={};}
        if(ImGui::Button("Feet to floor"))asm_floor();u.floor_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;if(!u.reaction_mode){ImGui::SameLine();ImGui::Checkbox("Facing left",&u.facing);u.reactions.facing_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;}ImGui::SetNextItemWidth(-1);ImGui::SliderInt("##asm-hold",&u.hold,1,20,"Preview hold: %d ticks");
        if(!u.reaction_mode){asm_binding_picker();if(!strikes.source&&ImGui::Button("Load strike tables...")){u.open=false;u.player.playing=false;show_strikes();strikes.animation=true;ImGui::CloseCurrentPopup();}
        ImGui::TextWrapped("Strike overlays show an association, not active-frame timing. Save associations to remember them between sessions.");}else{ImGui::TextWrapped("Reaction groups are suggestions from source names and comments. Show other animations to inspect a missed reaction. Timing is a preview override.");}
        if(a->partial)ImGui::TextColored(ImVec4(1,.65f,.4f,1),"Partial preview - runtime behavior omitted");for(auto &notice:a->notices)ImGui::TextWrapped("%s",notice.c_str());
    }ImGui::EndChild();ImGui::SameLine();ImGui::BeginChild("asm-view",ImVec2(0,height),ImGuiChildFlags_Border);
    int origin[]={u.origin_x,u.origin_y};ImGui::SetNextItemWidth(180);if(ImGui::InputInt2("Object origin",origin)){u.origin_x=std::clamp(origin[0],-4096,4096);u.origin_y=std::clamp(origin[1],-4096,4096);}
    if(u.reaction_mode){ImGui::SameLine();ImGui::Checkbox("Facing left",&u.facing);u.reactions.facing_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;}
    auto f=asm_frame();if(f){ImGui::TextWrapped("%s | segment %d | source line %d",f->name.c_str(),f->segment+1,f->line);if(!f->source.empty()){ImGui::TextWrapped("Source: %s",fs::u8path(f->source).filename().u8string().c_str());ImGui::SetItemTooltip("%s / line %d",f->source.c_str(),f->line);}ImGui::TextDisabled("Offset %d, %d%s",f->dx,f->dy,f->no_sleep?" | no-sleep frame (steppable preview)":"");}
    asm_resolve();int missing=0,ambiguous=0;for(auto &p:u.pieces)if(!p.sprite){++missing;ambiguous+=p.ambiguous;}
    if(missing){ImGui::TextColored(ImVec4(1,.65f,.4f,1),"%d unresolved pieces (%d ambiguous)",missing,ambiguous);if(!u.reaction_mode)for(auto &p:u.pieces)if(!p.sprite)ImGui::TextDisabled("%s%s",p.symbol.c_str(),p.ambiguous?" - review artwork to choose a match":p.stale?" - chosen sprite unavailable; review artwork":" - open its IMG library");}
    if(u.reaction_mode){asm_canvas(std::max(110.f,height*(height<550?.25f:.37f)));reaction_anchor_table();}else asm_canvas();ImGui::EndChild();if(ImGui::Button("Done",ImVec2(110,0))){u.open=false;u.player.playing=false;ImGui::CloseCurrentPopup();}u.close_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(fatality.return_preview&&fatality.source){ImGui::SameLine();if(ImGui::Button("Back to fatality source")){show_fatality();ImGui::CloseCurrentPopup();}fatality.back_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;}
    if(!u.assets.open&&!u.scene_review.open&&!ImGui::GetIO().WantTextInput){if(ImGui::IsKeyPressed(ImGuiKey_LeftArrow))u.player.step(-1,a?(int)a->frames.size():0);if(ImGui::IsKeyPressed(ImGuiKey_RightArrow))u.player.step(1,a?(int)a->frames.size():0);if(ImGui::IsKeyPressed(ImGuiKey_Space)&&a&&!a->frames.empty())u.player.playing=!u.player.playing;}
    ImGui::SameLine();if(ImGui::Button("Review artwork...")){u.assets.open=true;u.player.playing=false;}u.assets.show_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SameLine();ImGui::BeginDisabled(!a||a->frames.empty()||tabs.empty());if(ImGui::Button("Create scene..."))open_asm_scene();u.scene_review.show_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();
    if(u.assets.open)asm_assets_dialog();
    if(u.scene_review.open)asm_scene_dialog();
    if(!u.open)ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
