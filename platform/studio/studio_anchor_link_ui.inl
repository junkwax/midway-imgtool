void reset_anchor_link(){auto &u=anchor_link;u.valid=false;u.reference_picked=false;u.target_picked=false;u.dragging=false;u.error.clear();u.picking=0;}
void show_anchor_link(Tab &t){
    finish();t.alignment.player.playing=false;t.scene.playing=false;t.embedded.playing=false;
    anchor_link=AnchorLinkUi{};auto &u=anchor_link;u.open=true;u.owner=t.uid;u.reference_doc=u.target_doc=t.uid;
    u.reference_id=t.selected_id;u.target_id=t.art->sprite_id((t.selected+1)%t.art->count());u.options.convention=t.convention;
    u.origin_x=t.alignment.origin_x;u.origin_y=t.alignment.origin_y;
    if(!t.alignment.enabled)if(auto im=t.art->sprite(t.selected))u.origin_y=t.alignment.floor_y-(im->h-(short)im->aniy);
}
void rebuild_anchor_link(){auto &u=anchor_link;u.valid=false;auto ref=find(u.reference_doc),dst=find(u.target_doc);
    if(!ref||!dst){u.error="A chosen library is closed. Select its replacement.";return;}
    if(u.reference_picked&&u.target_picked)u.valid=plan_anchor_link(*ref->art,u.reference_id,*dst->art,u.target_id,u.options,u.plan,u.error);
}
void link_picker(const char *label,uint64_t &doc,uint64_t &sprite){
    ImGui::PushID(label);ImGui::SeparatorText(label);auto t=find(doc);
    ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##library",t?t->name().c_str():"Choose library")){
        for(auto &candidate:tabs){ImGui::PushID((int)candidate->uid);if(ImGui::Selectable(candidate->name().c_str(),candidate->uid==doc)){doc=candidate->uid;sprite=candidate->selected_id;reset_anchor_link();}ImGui::PopID();}ImGui::EndCombo();}
    t=find(doc);int index=t?t->art->sprite_index(sprite):-1;auto im=t?t->art->sprite(index):nullptr;
    ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##sprite",im?im->n_s:"Choose sprite")){
        if(t)for(int i=0;i<t->art->count();++i){ImGui::PushID(i);if(ImGui::Selectable(t->art->sprite(i)->n_s,i==index)){sprite=t->art->sprite_id(i);reset_anchor_link();}ImGui::PopID();}ImGui::EndCombo();}
    if(im)ImGui::TextDisabled("%d x %d | anchor %d, %d",im->w,im->h,(short)im->anix,(short)im->aniy);ImGui::PopID();
}
void anchor_link_canvas(){
    auto &u=anchor_link;auto ref=find(u.reference_doc),dst=find(u.target_doc);int ri=ref?ref->art->sprite_index(u.reference_id):-1,ti=dst?dst->art->sprite_index(u.target_id):-1;
    auto r=ref?ref->art->sprite(ri):nullptr,t=dst?dst->art->sprite(ti):nullptr;auto &io=ImGui::GetIO();
    auto available=ImGui::GetContentRegionAvail(),pos=ImGui::GetCursorScreenPos();available=ImMax(available,ImVec2(1,1));
    ImGui::InvisibleButton("##link-canvas",available,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);bool hovered=ImGui::IsItemHovered();
    float z=u.zoom>0?u.zoom:std::max(.25f,std::min((available.x-20)/400,(available.y-40)/254));
    if(hovered&&io.MouseWheel){u.zoom=std::clamp(z*std::pow(1.2f,io.MouseWheel),.25f,32.f);z=u.zoom;}
    if(hovered&&!u.dragging&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle))u.pan+=io.MouseDelta;
    ImVec2 stage=pos+(available-ImVec2(400*z,254*z))*.5f+u.pan,end=stage+ImVec2(400*z,254*z);u.stage_origin=stage;u.stage_zoom=z;
    auto dl=ImGui::GetWindowDrawList();dl->PushClipRect(pos,pos+available,true);dl->AddRectFilled(pos,pos+available,IM_COL32(18,21,27,255));dl->AddRectFilled(stage,end,IM_COL32(28,32,40,255));
    dl->PushClipRect(ImMax(pos,stage),ImMin(pos+available,end),true);if(auto owner=find(u.owner))draw_stage(*owner,dl,stage,z);
    for(int x=0;x<=400;x+=50)dl->AddLine(stage+ImVec2(x*z,0),stage+ImVec2(x*z,254*z),IM_COL32(60,66,77,110));
    auto draw=[&](Tab &doc,int index,const SpritePlacement &p,bool flip,int alpha){if(auto tex=texture(doc,index))dl->AddImage((ImTextureID)(intptr_t)tex,stage+ImVec2(p.x*z,p.y*z),stage+ImVec2((p.x+p.w)*z,(p.y+p.h)*z),ImVec2(flip?1.f:0.f,0),ImVec2(flip?0.f:1.f,1),IM_COL32(255,255,255,alpha));};
    SpritePlacement rp{},tp{},preview{};
    if(r){rp=place_sprite(*r,u.origin_x,u.origin_y,u.options.reference_flip,u.options.convention);draw(*ref,ri,rp,u.options.reference_flip,u.alpha);}
    if(t){tp=place_sprite(*t,u.origin_x+u.options.offset_x,u.origin_y+u.options.offset_y,u.options.target_flip,u.options.convention);preview=tp;
        if(u.valid){IMG adjusted=*t;adjusted.anix=(unsigned short)u.plan.x;adjusted.aniy=(unsigned short)u.plan.y;preview=place_sprite(adjusted,u.origin_x+u.options.offset_x,u.origin_y+u.options.offset_y,u.options.target_flip,u.options.convention);draw(*dst,ti,tp,u.options.target_flip,65);}
        draw(*dst,ti,preview,u.options.target_flip,255);dl->AddRect(stage+ImVec2(preview.x*z,preview.y*z),stage+ImVec2((preview.x+preview.w)*z,(preview.y+preview.h)*z),IM_COL32(228,75,91,200));}
    auto marker=[&](const IMG &im,const SpritePlacement &p,int x,int y,bool flip,ImU32 color){ImVec2 at=stage+ImVec2((p.x+(flip?im.w-1-x:x)+.5f)*z,(p.y+y+.5f)*z);dl->AddCircle(at,5,color,16,2);return at;};
    ImVec2 ra{},ta{};if(r&&u.reference_picked)ra=marker(*r,rp,u.options.reference_x,u.options.reference_y,u.options.reference_flip,IM_COL32(239,193,94,255));
    if(t&&u.target_picked)ta=marker(*t,preview,u.options.target_x,u.options.target_y,u.options.target_flip,IM_COL32(255,106,119,255));
    if(u.reference_picked&&u.target_picked)dl->AddLine(ra,ta,IM_COL32(239,193,94,190),1.5f);if(u.dragging)dl->AddLine(ra,io.MousePos,IM_COL32(239,193,94,255),2);
    // Deterministic opaque feature locations for hidden UI interaction checks.
    auto probe=[&](const IMG &im,const SpritePlacement &p,bool flip){for(int y=0;y<im.h;++y)for(int x=0;x<im.w;++x)if(((unsigned char *)im.data_p)[y*((im.w+3)&~3)+x])return stage+ImVec2((p.x+(flip?im.w-1-x:x)+.5f)*z,(p.y+y+.5f)*z);return stage;};
    if(r)u.reference_point=probe(*r,rp,u.options.reference_flip);if(t)u.target_point=probe(*t,tp,u.options.target_flip);
    dl->PopClipRect();dl->AddRect(stage,end,IM_COL32(90,96,106,255));dl->AddText(pos+ImVec2(10,8),IM_COL32(180,186,197,255),u.valid?"ALIGNED PREVIEW | Apply to keep the target anchor":"PICK MATCHING FEATURES | Reference, then target");dl->PopClipRect();
    auto pick=[&](const IMG *im,const SpritePlacement &p,bool flip,int &x,int &y){if(!im)return false;int px=(int)std::floor((io.MousePos.x-stage.x)/z)-p.x,py=(int)std::floor((io.MousePos.y-stage.y)/z)-p.y;
        if(px<0||py<0||px>=im->w||py>=im->h)return false;if(flip)px=im->w-1-px;
        if(!((unsigned char *)im->data_p)[py*((im->w+3)&~3)+px])return false;x=px;y=py;return true;};
    if(hovered&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        if(u.picking==0){if(pick(r,rp,u.options.reference_flip,u.options.reference_x,u.options.reference_y)){u.reference_picked=true;u.target_picked=false;u.valid=false;u.error.clear();u.picking=1;u.dragging=true;}}
        else if(pick(t,preview,u.options.target_flip,u.options.target_x,u.options.target_y)){u.target_picked=true;rebuild_anchor_link();}
    }
    if(u.dragging&&ImGui::IsMouseReleased(ImGuiMouseButton_Left)){u.dragging=false;
        // A click chooses Reference; a drag can also choose Target in one gesture.
        if(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x*ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x+ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y*ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y>16&&hovered&&pick(t,tp,u.options.target_flip,u.options.target_x,u.options.target_y)){u.target_picked=true;rebuild_anchor_link();}}
}
void anchor_link_dialog(){
    auto &u=anchor_link;if(u.open)ImGui::OpenPopup("Link animation anchors");
    ImVec2 display=ImGui::GetIO().DisplaySize;ImGui::SetNextWindowSize(ImVec2(std::min(1100.f,display.x-30),std::min(740.f,display.y-40)),ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(900,500),display-ImVec2(20,20));
    if(!ImGui::BeginPopupModal("Link animation anchors",nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    if(u.valid){auto ref=find(u.reference_doc),dst=find(u.target_doc);if(!ref||!dst||ref->art->generation()!=u.plan.reference_generation||dst->art->generation()!=u.plan.target_generation){reset_anchor_link();u.error="The reference or target changed. Pick the features again.";}}
    ImGui::TextWrapped("Pick the same feature on both sprites, or drag from Reference to Target. Review the placement before applying the target's anchor.");
    float height=ImGui::GetContentRegionAvail().y-45;
    ImGui::BeginChild("link-settings",ImVec2(270,height),ImGuiChildFlags_Border);
    link_picker("Reference",u.reference_doc,u.reference_id);bool changed=ImGui::Checkbox("Flip reference",&u.options.reference_flip);
    link_picker("Target",u.target_doc,u.target_id);changed|=ImGui::Checkbox("Flip target",&u.options.target_flip);
    int xy[]={u.options.offset_x,u.options.offset_y};ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##link-offset",xy)){u.options.offset_x=std::clamp(xy[0],-4096,4096);u.options.offset_y=std::clamp(xy[1],-4096,4096);changed=true;}ImGui::TextDisabled("Target staging offset X / Y");
    int convention=(int)u.options.convention;const char *conventions[]={"Multipart (ani2)","Single part (ganiof)"};ImGui::SetNextItemWidth(-1);if(ImGui::Combo("##link-convention",&convention,conventions,2)){u.options.convention=(MirrorConvention)convention;changed=true;}
    if(changed)rebuild_anchor_link();ImGui::SetNextItemWidth(-1);ImGui::SliderInt("##reference-alpha",&u.alpha,25,255,"Reference opacity: %d");
    if(ImGui::RadioButton("Pick reference feature",u.picking==0))u.picking=0;if(ImGui::RadioButton("Pick target feature",u.picking==1))u.picking=1;
    if(ImGui::Button("Reset points"))reset_anchor_link();u.reset_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();if(ImGui::Button("Fit")){u.zoom=0;u.pan={};}
    auto ref=find(u.reference_doc),dst=find(u.target_doc);
    if(u.reference_picked)ImGui::Text("Reference feature: %d, %d",u.options.reference_x,u.options.reference_y);if(u.target_picked)ImGui::Text("Target feature: %d, %d",u.options.target_x,u.options.target_y);
    if(u.valid){ImGui::Separator();ImGui::Text("Anchor: %d, %d -> %d, %d",u.plan.old_x,u.plan.old_y,u.plan.x,u.plan.y);ImGui::TextDisabled(u.plan.changed?"Preview only until applied":"Already aligned");}
    if(!u.error.empty())ImGui::TextWrapped("%s",u.error.c_str());
    if(dst){ImGui::Spacing();ImGui::TextWrapped("Anchor edits belong to %s. Reference pixels and anchors stay unchanged.",dst->name().c_str());
    }
    ImGui::EndChild();ImGui::SameLine();ImGui::BeginChild("link-preview",ImVec2(0,height),ImGuiChildFlags_Border,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);anchor_link_canvas();ImGui::EndChild();
    ImGui::BeginDisabled(!u.valid||!u.plan.changed||!ref||!dst);if(ImGui::Button("Apply target anchor",ImVec2(165,0))&&ref&&dst){if(apply_anchor_link(*dst->art,*ref->art,u.plan,u.error)){status="Linked target anchor in "+dst->name()+". Save that IMG to keep it.";reset_anchor_link();}}
    u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Done",ImVec2(100,0))){u.open=false;reset_anchor_link();ImGui::CloseCurrentPopup();}u.close_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(dst){ImGui::SameLine();
        ImGui::BeginDisabled(!dst->art->can_undo());bool undo=ImGui::Button("Undo target");u.undo_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();
        ImGui::BeginDisabled(!dst->art->can_redo());bool redo=ImGui::Button("Redo target");u.redo_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();
        if(!ImGui::GetIO().WantTextInput){const auto route=ImGuiInputFlags_RouteGlobal;undo|=ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_Z,route);redo|=ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_Y,route);}
        if(undo&&dst->art->can_undo()){dst->art->undo();reset_anchor_link();}else if(redo&&dst->art->can_redo()){dst->art->redo();reset_anchor_link();}
        ImGui::SameLine();if(ImGui::Button("Save target IMG")){
#ifdef _WIN32
            save(*dst,false);
#else
            u.error="Close this workspace, switch to the target library, then Save IMG.";
#endif
        }
    }
    if(!ImGui::GetIO().WantTextInput&&ImGui::IsKeyPressed(ImGuiKey_Escape))reset_anchor_link();ImGui::EndPopup();
}
