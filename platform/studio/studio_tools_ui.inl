bool pixel_preview()const{return layer_drag.active||floating.active||paint_line.active||variant.active;}
void tool_header(Tab &t){
    auto &spec=tool_specs[(int)tool];ImGui::TextUnformatted(spec.name);ImGui::SameLine();ImGui::TextDisabled("%s",spec.key);ImGui::SameLine();
    if(ImGui::Button("Fit"))action(Action::Fit);if(ImGui::IsItemHovered())ImGui::SetTooltip("Fit artwork (Ctrl+0)");
    if(layer_drag.active)ImGui::TextColored(accent_surface(1),"Moving overlay / Esc cancels");
    else if(variant.active)ImGui::TextColored(accent_surface(1),variant.dragging?"Release to review / Esc cancels":"Enter applies / Esc cancels");
    else if(paint_line.active)ImGui::TextColored(accent_surface(1),"Release applies / Esc cancels");
    else if(floating.active)ImGui::TextColored(accent_surface(1),"Enter applies / Esc cancels");
    else if(paste_mode)ImGui::TextColored(accent_surface(1),"Click to paste / Esc cancels");
    else if(layer_move_selected(t))ImGui::TextDisabled(layer_move_target(t)?"Move overlay / Shift locks axis":"Overlay hidden / show to move");
    else if(t.flip)ImGui::TextDisabled("Flipped preview / pixel editing paused");
    else ImGui::TextDisabled("Wheel zooms / middle-drag pans");
}
void wheel_write(Tab &t){
    auto &w=t.wheel;auto pal=t.art->palette(w.palette);if(!pal||w.slot<=0||w.slot>=pal->numc)return;
    auto value=rgb_to_word15((unsigned char)std::lround(std::clamp(w.rgb[0],0.f,1.f)*255),(unsigned char)std::lround(std::clamp(w.rgb[1],0.f,1.f)*255),(unsigned char)std::lround(std::clamp(w.rgb[2],0.f,1.f)*255));
    if((palette_word_at((unsigned char *)pal->data_p,w.slot)&0x7fff)==value)return;
    t.art->color(w.palette,w.slot,value);w.editing=true;w.serial=t.art->generation();t.alignment.player.playing=false;t.embedded.playing=false;t.scene.playing=false;
}
void color_wheel(Tab &t){
    auto im=t.art->sprite(t.selected);auto pal=t.art->palette(im->palnum);t.color=std::clamp(t.color,0,(int)pal->numc-1);auto &w=t.wheel;
    if(w.palette!=im->palnum||w.slot!=t.color||(!w.editing&&w.serial!=t.art->generation())){
        if(w.editing)t.art->commit();w.editing=false;w.palette=im->palnum;w.slot=t.color;w.serial=t.art->generation();unsigned char r,g,b;pal_word_to_rgb8((unsigned char *)pal->data_p+t.color*2,&r,&g,&b);w.rgb[0]=r/255.f;w.rgb[1]=g/255.f;w.rgb[2]=b/255.f;
    }
    if(t.color)w.last_opaque=t.color;
    bool blocked=pixel_preview();ImGui::BeginDisabled(blocked);
    if(ImGui::ColorButton("##active-swatch",ImVec4(w.rgb[0],w.rgb[1],w.rgb[2],t.color?1.f:0.f),ImGuiColorEditFlags_AlphaPreview|ImGuiColorEditFlags_NoTooltip,ImVec2(32,26))&&t.color>0)ImGui::OpenPopup("Color values");
    if(ImGui::IsItemHovered()){auto usage=palette_usage(*t.art,im->palnum);ImGui::SetTooltip("Palette %s / index %d\nShared by %d primary sprites / %d alternate references.\nWheel edits this palette entry; index 0 stays transparent.",pal->n_s,t.color,usage.primary,usage.alternate);}
    ImGui::SameLine();ImGui::Text("#%d",t.color);ImGui::SameLine(ImGui::GetWindowContentRegionMax().x-24);
    ImGui::BeginDisabled(pal->numc<2);if(ImGui::SmallButton("0")){finish();t.color=t.color?0:std::clamp(w.last_opaque,1,std::max(1,(int)pal->numc-1));}tools_ui.transparent_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Toggle the painting color between transparency (#0) and the last opaque index.");
    // The compact picker stays visible; exact values live in its optional pop-out.
    ImGui::BeginDisabled(t.color==0);float width=ImGui::GetContentRegionAvail().x;float wheel=std::min(width,std::max(100.f,ImGui::GetContentRegionAvail().y-22));
    tools_ui.wheel_origin=ImGui::GetCursorScreenPos();tools_ui.wheel_size=wheel;ImGui::SetNextItemWidth(wheel);
    if(ImGui::ColorPicker3("##active-color-wheel",w.rgb,ImGuiColorEditFlags_PickerHueWheel|ImGuiColorEditFlags_InputRGB|ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoLabel|ImGuiColorEditFlags_NoSidePreview|ImGuiColorEditFlags_NoSmallPreview|ImGuiColorEditFlags_NoOptions))wheel_write(t);
    ImGui::TextDisabled("RGB555");ImGui::SameLine();if(ImGui::SmallButton("Edit..."))ImGui::OpenPopup("Color values");tools_ui.detail_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(ImGui::BeginPopup("Color values")){
        ImGui::Text("%s / index %d",pal->n_s,t.color);ImGui::SetNextItemWidth(220);
        if(ImGui::ColorPicker3("##color-values",w.rgb,ImGuiColorEditFlags_PickerHueWheel|ImGuiColorEditFlags_InputRGB|ImGuiColorEditFlags_DisplayRGB|ImGuiColorEditFlags_DisplayHSV|ImGuiColorEditFlags_DisplayHex|ImGuiColorEditFlags_NoLabel|ImGuiColorEditFlags_NoSidePreview|ImGuiColorEditFlags_NoOptions))wheel_write(t);
        ImGui::TextWrapped("Edits affect every sprite using this palette entry.");ImGui::EndPopup();
    }
    ImGui::EndDisabled();ImGui::EndDisabled();
    if(w.editing&&!ImGui::IsAnyItemActive()){t.art->commit();w.editing=false;w.serial=0;}
}
void tools_rail(Tab &t,float height){
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(10,10));ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(4,4));
    ImGui::BeginChild("tools-rail",ImVec2(0,height),ImGuiChildFlags_Border,panel_flags);ImGui::TextDisabled("TOOLS");
    bool unavailable=t.alignment.enabled||t.embedded.enabled||t.flip||pixel_preview();ImGui::BeginDisabled(unavailable);
    const int order[]={0,6,7,8,1,9,2,11,3,10,12,13,14,15,16,17,4,5};float size=(ImGui::GetContentRegionAvail().x-4)*.5f;float row=height<590?30.f:34.f;
    for(int at=0;at<18;++at){int i=order[at];auto &spec=tool_specs[i];if(at%2)ImGui::SameLine(0,4);ImGui::PushID(i);bool selected=tool==(Tool)i;
        ImGui::PushStyleColor(ImGuiCol_Button,selected?accent_surface(.45f):ImVec4(.13f,.15f,.18f,1));unsigned int codepoint=0;ImTextCharFromUtf8(&codepoint,spec.glyph,nullptr);bool glyph=tool_font&&tool_font->FindGlyphNoFallback((ImWchar)codepoint);const char *label=i==9?"##line":glyph?spec.glyph:spec.fallback;if(glyph)ImGui::PushFont(tool_font);
        if(ImGui::Button(label,ImVec2(size,row))){finish();tool=(Tool)i;paste_mode=false;}if(glyph)ImGui::PopFont();ImGui::PopStyleColor();tools_ui.points[i]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
        if(i==9){auto c=tools_ui.points[i];auto dl=ImGui::GetWindowDrawList();auto color=ImGui::GetColorU32(ImGuiCol_Text);dl->AddLine(c+ImVec2(-8,7),c+ImVec2(8,-7),color,1.6f);dl->AddCircleFilled(c+ImVec2(-8,7),1.8f,color);dl->AddCircleFilled(c+ImVec2(8,-7),1.8f,color);}
        if(i==11){auto p=ImGui::GetItemRectMax()-ImVec2(9,8);auto dl=ImGui::GetWindowDrawList();dl->AddLine(p-ImVec2(3,0),p+ImVec2(3,0),ImGui::GetColorU32(ImGuiCol_Text),1.2f);dl->AddLine(p-ImVec2(0,3),p+ImVec2(0,3),ImGui::GetColorU32(ImGuiCol_Text),1.2f);}
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip(unavailable?"%s (%s)\n%s\nSwitch to Artwork and close previews to use pixel tools.":"%s (%s)\n%s",spec.name,spec.key,spec.tip);ImGui::PopID();
    }
    ImGui::EndDisabled();ImGui::Spacing();ImGui::Separator();ImGui::Spacing();color_wheel(t);ImGui::EndChild();ImGui::PopStyleVar(2);
}
