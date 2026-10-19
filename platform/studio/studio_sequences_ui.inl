// Embedded records belong to the IMG document and use its undo/save boundary.
const EmbeddedRecord *embedded_record(const Tab &t){auto &e=t.embedded;return e.record>=0&&e.record<(int)e.records.size()?&e.records[e.record]:nullptr;}
void refresh_embedded(Tab &t){
    auto &e=t.embedded;if(!e.enabled||e.seen==t.art->generation())return;
    e.seen=t.art->generation();if(!read_embedded(*t.art,e.records,e.error)){e.playing=false;e.record=-1;return;}
    if(!embedded_record(t)||embedded_record(t)->script!=e.scripts){e.record=-1;for(int r=0;r<(int)e.records.size();++r)if(e.records[r].script==e.scripts){e.record=r;break;}}
    if(auto r=embedded_record(t)){e.entry=std::clamp(e.entry,0,std::max(0,(int)r->entries.size()-1));e.tick=std::clamp(e.tick,0.,(double)embedded_duration(*r)-1);}
}
void embedded_floor(Tab &t){auto r=embedded_record(t);if(!r||r->script||r->entries.empty())return;auto &v=r->entries.front().values;if(auto im=t.art->sprite(v.index))t.embedded.origin_y=t.embedded.floor-(im->h-(short)im->aniy)+v.dy;}
void embedded_workspace(Tab &t){finish();t.alignment.player.playing=false;t.scene.playing=false;t.alignment.enabled=false;t.embedded.enabled=true;t.embedded.playing=false;paste_mode=false;refresh_embedded(t);embedded_floor(t);}
void embedded_seek(Tab &t,int entry){finish();auto &e=t.embedded;e.playing=false;auto r=embedded_record(t);if(!r||r->entries.empty())return;e.entry=std::clamp(entry,0,(int)r->entries.size()-1);e.tick=0;for(int i=0;i<e.entry;++i)e.tick+=std::max(1,r->entries[i].values.ticks);if(!r->script&&t.art->sprite(r->entries[e.entry].values.index))select(r->entries[e.entry].values.index);}
void embedded_select_record(Tab &t,int record){finish();auto &e=t.embedded;if(record<0||record>=(int)e.records.size())return;e.record=record;e.scripts=e.records[record].script;e.entry=0;e.tick=0;e.playing=false;e.zoom=0;e.pan={0,0};e.target=e.scripts?0:t.selected;embedded_floor(t);}
void embedded_step(Tab &t,int delta){auto r=embedded_record(t);if(r&&!r->entries.empty())embedded_seek(t,(t.embedded.entry+delta+(int)r->entries.size())%(int)r->entries.size());}
void embedded_toggle_play(Tab &t){finish();auto r=embedded_record(t);if(!r||r->script||r->entries.empty())return;auto &e=t.embedded;if(!e.playing&&e.tick>=embedded_duration(*r)-1)e.tick=0;e.playing=!e.playing;}
void embedded_advance(Tab &t,double seconds){auto &e=t.embedded;auto r=embedded_record(t);if(!e.playing||!r||r->script||r->entries.empty())return;e.tick+=seconds*mk2_tick_hz;int duration=embedded_duration(*r);if(e.tick>=duration){if(e.loop)e.tick=std::fmod(e.tick,duration);else{e.tick=duration-1;e.playing=false;}}e.entry=std::max(0,embedded_frame_at(*r,(int)e.tick));}
void embedded_library(Tab &t){
    auto &e=t.embedded;ImGui::TextUnformatted("IMG animations");ImGui::TextDisabled("%u sequences / %u scripts",t.art->data.seqcnt,t.art->data.scrcnt);
    bool scripts=e.scripts;bool changed=ImGui::RadioButton("Sequences",!scripts);ImGui::SameLine();if(ImGui::RadioButton("Scripts",scripts)){scripts=true;changed=true;}else if(changed)scripts=false;
    if(changed){finish();e.scripts=scripts;e.record=-1;e.seen=0;e.playing=false;refresh_embedded(t);embedded_floor(t);}
    e.filter.Draw("##animation-filter",-1);ImGui::Separator();
    if(!e.error.empty()){ImGui::TextWrapped("%s",e.error.c_str());return;}
    bool any=false;for(int i=0;i<(int)e.records.size();++i){auto &r=e.records[i];if(r.script!=e.scripts||!e.filter.PassFilter(r.name.c_str()))continue;any=true;ImGui::PushID(i);
        std::string label=r.name.empty()?"(unnamed)":r.name;if(ImGui::Selectable(label.c_str(),e.record==i))embedded_select_record(t,i);ImGui::TextDisabled("%zu %s",r.entries.size(),r.script?"calls":"frames");ImGui::PopID();}
    if(!any)ImGui::TextWrapped("No matching animations. Create one above the canvas.");
}
void embedded_controls(Tab &t){
    auto &e=t.embedded;ImGui::TextUnformatted("Embedded IMG animations");
    ImGui::BeginDisabled(!e.error.empty());if(ImGui::Button("New sequence"))e.command=EmbeddedCommand::NewSequence;ImGui::SameLine();if(ImGui::Button("New script"))e.command=EmbeddedCommand::NewScript;ImGui::EndDisabled();
    auto r=embedded_record(t);if(r)ImGui::TextDisabled("%s | %s",r->name.c_str(),r->script?"sequence call list":"stored frame order and timing");else ImGui::TextDisabled("Edits save with this IMG library.");
}
void embedded_canvas(Tab &t,float height){
    auto &e=t.embedded;ImGui::BeginChild("embedded-canvas",ImVec2(0,height),ImGuiChildFlags_Border,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    auto available=ImGui::GetContentRegionAvail();auto pos=ImGui::GetCursorScreenPos();auto dl=ImGui::GetWindowDrawList();auto &io=ImGui::GetIO();
    float fit=std::max(.1f,std::min((available.x-20)/400,(available.y-20)/254));float z=e.zoom>0?e.zoom:fit;
    ImVec2 stage=pos+(available-ImVec2(400*z,254*z))*.5f+e.pan;
    ImGui::InvisibleButton("##embedded-stage",available,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle|ImGuiButtonFlags_MouseButtonRight);
    if(ImGui::IsItemHovered()){if(io.MouseWheel!=0)e.zoom=std::clamp(z*std::pow(1.15f,io.MouseWheel),.1f,32.f);if(ImGui::IsMouseDragging(ImGuiMouseButton_Middle)||ImGui::IsMouseDragging(ImGuiMouseButton_Right))e.pan+=io.MouseDelta;}
    dl->PushClipRect(pos,pos+available,true);dl->AddRectFilled(stage,stage+ImVec2(400*z,254*z),IM_COL32(28,32,40,255));dl->AddRect(stage,stage+ImVec2(400*z,254*z),IM_COL32(78,83,95,255));
    dl->PushClipRect(ImMax(pos,stage),ImMin(pos+available,stage+ImVec2(400*z,254*z)),true);draw_stage(t,dl,stage,z);dl->PopClipRect();
    dl->AddLine(stage+ImVec2(0,e.floor*z),stage+ImVec2(400*z,e.floor*z),IM_COL32(100,106,120,180));
    auto r=embedded_record(t);if(r&&!r->script&&!r->entries.empty()){
        auto &v=r->entries[e.entry].values;if(auto im=t.art->sprite(v.index)){float x=(float)e.origin_x-anipoint_effective((short)im->anix+v.dx,im->w,t.flip,t.convention);float y=(float)e.origin_y-((short)im->aniy+v.dy);
            if(auto tex=texture(t,v.index))dl->AddImage((ImTextureID)(intptr_t)tex,stage+ImVec2(x*z,y*z),stage+ImVec2((x+im->w)*z,(y+im->h)*z),ImVec2(t.flip?1.f:0.f,0),ImVec2(t.flip?0.f:1.f,1));
        }else dl->AddText(pos+ImVec2(12,12),IM_COL32(255,140,140,255),"Missing sprite reference - choose a target in the inspector.");
    }else dl->AddText(pos+ImVec2(12,12),IM_COL32(190,194,204,255),r&&r->script?"Open a called sequence to preview its frames.":"Add a sprite frame to start this sequence.");
    ImVec2 anchor=stage+ImVec2(e.origin_x*z,e.origin_y*z);dl->AddLine(anchor-ImVec2(8,0),anchor+ImVec2(8,0),IM_COL32(239,193,94,255));dl->AddLine(anchor-ImVec2(0,8),anchor+ImVec2(0,8),IM_COL32(239,193,94,255));dl->PopClipRect();ImGui::EndChild();
}
void embedded_timeline(Tab &t,float height){
    auto &e=t.embedded;auto r=embedded_record(t);ImGui::BeginChild("embedded-timeline",ImVec2(0,std::max(1.f,height)),ImGuiChildFlags_Border);
    bool frames=r&&!r->script&&!r->entries.empty();ImGui::BeginDisabled(!frames);if(ImGui::Button(e.playing?"Pause":"Play"))embedded_toggle_play(t);ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("<"))embedded_step(t,-1);ImGui::SameLine();if(ImGui::Button(">"))embedded_step(t,1);ImGui::SameLine();ImGui::Checkbox("Loop",&e.loop);
    if(frames){int tick=(int)e.tick;ImGui::SetNextItemWidth(-1);if(ImGui::SliderInt("##embedded-tick",&tick,0,embedded_duration(*r)-1,"Tick %d")){finish();e.tick=tick;e.playing=false;e.entry=std::max(0,embedded_frame_at(*r,tick));}}
    ImGui::BeginDisabled(!r);if(ImGui::Button(r&&r->script?"Add call":"Add frame"))e.command=EmbeddedCommand::Append;e.append_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();
    ImGui::BeginDisabled(!r||r->entries.empty());if(ImGui::Button("Duplicate"))e.command=EmbeddedCommand::Duplicate;e.duplicate_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();
    ImGui::BeginDisabled(e.entry<=0);if(ImGui::Button("Move <"))e.command=EmbeddedCommand::Left;ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(!r||e.entry+1>=(int)r->entries.size());if(ImGui::Button("Move >"))e.command=EmbeddedCommand::Right;ImGui::EndDisabled();
    if(ImGui::Button("Delete entry"))e.command=EmbeddedCommand::Delete;ImGui::EndDisabled();ImGui::EndDisabled();
    ImGui::BeginChild("embedded-entries",ImVec2(0,0),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar);
    if(r)for(int i=0;i<(int)r->entries.size();++i){if(i)ImGui::SameLine();ImGui::PushID(i);auto &v=r->entries[i].values;std::string name;
        if(r->script){name=v.index>=0&&v.index<(int)t.art->data.seqcnt?e.records[v.index].name:"Missing sequence";}else{auto im=t.art->sprite(v.index);name=im?im->n_s:"Missing sprite";}
        ImVec2 a=ImGui::GetCursorScreenPos(),size(105,90);bool clicked=ImGui::InvisibleButton("##entry",size);auto dl=ImGui::GetWindowDrawList();dl->AddRectFilled(a,a+size,ImGui::GetColorU32(i==e.entry?accent_surface(.5f):ImGui::IsItemHovered()?ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered):ImGui::GetStyleColorVec4(ImGuiCol_Button)),4);
        dl->PushClipRect(a,a+size,true);if(!r->script){if(auto im=t.art->sprite(v.index)){float scale=std::min(78.f/im->w,38.f/im->h);ImVec2 thumb(im->w*scale,im->h*scale),p=a+ImVec2((size.x-thumb.x)*.5f,5);if(auto tex=texture(t,v.index))dl->AddImage((ImTextureID)(intptr_t)tex,p,p+thumb);}}
        else dl->AddText(a+ImVec2(34,17),IM_COL32(210,213,220,255),"CALL");
        std::string label=std::to_string(i+1)+" "+name,hold=std::to_string(v.ticks)+" ticks";dl->AddText(a+ImVec2(6,49),IM_COL32(220,222,229,255),label.c_str());dl->AddText(a+ImVec2(6,69),IM_COL32(175,181,195,255),hold.c_str());dl->PopClipRect();
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s\n%d stored ticks\nOffset: %d, %d",name.c_str(),v.ticks,v.dx,v.dy);if(clicked)embedded_seek(t,i);ImGui::PopID();}
    ImGui::EndChild();ImGui::EndChild();
}
void embedded_inspector(Tab &t){
    auto &e=t.embedded;auto r=embedded_record(t);ImGui::TextUnformatted("Animation properties");ImGui::TextWrapped("Changes here are stored in the IMG. Preview position and facing are view settings.");ImGui::Separator();if(!r)return;
    char name[17]={};memcpy(name,r->name.data(),std::min<size_t>(16,r->name.size()));if(e.rename_focus){ImGui::SetKeyboardFocusHere();e.rename_focus=false;}ImGui::SetNextItemWidth(-1);
    if(ImGui::InputText("##embedded-name",name,sizeof(name))&&name[0]){e.playing=false;if(!t.art->embedded_rename(e.record,name,error))return;}if(ImGui::IsItemDeactivatedAfterEdit())t.art->commit();ImGui::TextDisabled("Name (up to 16 ASCII characters)");
    if(ImGui::Button("Edit stored metadata..."))show_embedded_metadata(t);e.metadata_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    bool entry=!r->entries.empty();EmbeddedEntryValues v=entry?r->entries[e.entry].values:EmbeddedEntryValues{};
    int count=r->script?(int)t.art->data.seqcnt:t.art->count();int target=entry?v.index:e.target;
    auto target_name=[&](int index)->std::string{if(index<0||index>=count)return "Choose a target";return r->script?e.records[index].name:t.art->sprite(index)->n_s;};
    ImGui::SetNextItemWidth(-1);bool changed=false;if(ImGui::BeginCombo("##embedded-target",target_name(target).c_str())){for(int i=0;i<count;++i){ImGui::PushID(i);if(ImGui::Selectable(target_name(i).c_str(),i==target)){target=i;changed=entry;e.target=i;}ImGui::PopID();}ImGui::EndCombo();}ImGui::TextDisabled(r->script?"Called sequence":"Frame sprite");
    if(entry){v.index=target;e.target=target;ImGui::SetNextItemWidth(-1);if(ImGui::InputInt("##embedded-hold",&v.ticks)){v.ticks=std::clamp(v.ticks,0,255);changed=true;}ImGui::TextDisabled("Stored ticks (0–255)");ImGui::SetItemTooltip("A stored zero is preserved. Preview displays it for one tick; clock is 54.7068 Hz.");
        int offset[]={v.dx,v.dy};ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##embedded-offset",offset)){v.dx=std::clamp(offset[0],-32768,32767);v.dy=std::clamp(offset[1],-32768,32767);changed=true;}ImGui::TextDisabled(r->script?"Stored entry delta X / Y":"Animation-point delta X / Y");
        if(changed){e.playing=false;if(!t.art->embedded_edit_entry(e.record,e.entry,v,error))return;}
        if(r->script){if(ImGui::Button("Open sequence")&&target>=0&&target<count)embedded_select_record(t,target);e.open_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::TextWrapped("This is the stored call list. Script runtime execution is not implemented.");}
    }else ImGui::TextWrapped(count?"Select a target, then add the first entry.":"Create a sequence before adding script calls.");
    if(!r->script){ImGui::Separator();ImGui::Checkbox("Flip preview",&t.flip);int convention=(int)t.convention;const char *mirrors[]={"ani2 (multipart)","ganiof (single part)"};ImGui::SetNextItemWidth(-1);if(ImGui::Combo("##embedded-mirror",&convention,mirrors,2))t.convention=(MirrorConvention)convention;ImGui::TextDisabled("Mirror math");if(ImGui::Button("Fit stage")){e.zoom=0;e.pan={0,0};}ImGui::SameLine();if(ImGui::Button("Feet to floor"))embedded_floor(t);
        ImGui::BeginDisabled(!entry);if(ImGui::Button("Create scene snapshot"))e.command=EmbeddedCommand::Scene;ImGui::EndDisabled();ImGui::TextWrapped("A snapshot copies this sequence into a separate scene for staging and composition.");}
    if(ImGui::CollapsingHeader("Preserved metadata")){ImGui::Text("Flags: 0x%04X",r->flags);ImGui::Text("Start: %d, %d",r->start_x,r->start_y);ImGui::TextWrapped("Header fields, damage references and entry spare bytes stay attached to their records.");}
}
void apply_embedded_command(Tab &t){
    auto &e=t.embedded;auto command=e.command;e.command=EmbeddedCommand::None;if(command==EmbeddedCommand::None)return;finish();e.playing=false;bool ok=false;
    if(command==EmbeddedCommand::NewSequence||command==EmbeddedCommand::NewScript){bool script=command==EmbeddedCommand::NewScript;std::string base=script?"NEWSCR":"NEWSEQ",name;int suffix=1;do{name=base+std::to_string(suffix++);}while(std::any_of(e.records.begin(),e.records.end(),[&](const EmbeddedRecord &r){return r.name==name;}));int index=t.art->embedded_add_record(script,name,error);ok=index>=0;if(ok){e.scripts=script;e.record=index;e.entry=0;e.tick=0;e.target=script?0:t.selected;}}
    else if(command==EmbeddedCommand::Scene){auto snapshot=embedded_scene(*t.art,t.uid,e.record,error,t.flip,t.convention);if(snapshot){snapshot->state.origin_x=e.origin_x;snapshot->state.origin_y=e.origin_y;snapshot->state.floor=e.floor;drop_pending_scene();pending_project=std::move(snapshot);pending_scene_owner=t.uid;pending_scene_new=true;install_scene();}return;}
    else if(command==EmbeddedCommand::Append){ok=t.art->embedded_append(e.record,e.target,error);if(ok)++e.entry;}
    else if(command==EmbeddedCommand::Duplicate){ok=t.art->embedded_duplicate(e.record,e.entry,error);if(ok)++e.entry;}
    else if(command==EmbeddedCommand::Delete)ok=t.art->embedded_delete(e.record,e.entry,error);
    else {int destination=e.entry+(command==EmbeddedCommand::Left?-1:1);ok=t.art->embedded_move(e.record,e.entry,destination,error);if(ok)e.entry=destination;}
    if(ok){e.seen=0;refresh_embedded(t);embedded_seek(t,e.entry);status="Embedded animation updated. Save IMG to keep these changes.";}
}
