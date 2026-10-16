void copy_swatch_colors(Tab &t,int highlighted,bool only_one){
    std::array<bool,256> selected{};if(!only_one)selected=palettes.selected;
    if(t.art->copy_palette_swatches(palettes.index,selected,highlighted,swatch_clipboard,palettes.error))status="Copied "+std::to_string(swatch_clipboard.slots.size())+" color(s) with their indices. Whole-palette and pixel clipboards are unchanged.";
}
void refresh_swatch_review(Tab &t){
    auto &u=palettes;
    if(t.art->generation()!=u.swatch_generation){u.swatch_valid=false;u.swatch_error="The library changed. Cancel and reopen this color paste review.";return;}
    u.swatch_valid=plan_palette_swatches(*t.art,u.index,u.swatch_source,(SwatchPaste)u.swatch_kind,u.swatch_slot,t.ppp,u.swatch_plan,u.swatch_error);
}
void start_swatch_review(Tab &t,SwatchPaste mode,int slot){
    auto &u=palettes;if(palette_pending()||u.merge_mode||u.usage_mode||u.ramp_mode||u.swatch_mode)return;
    finish();u.swatch_source=swatch_clipboard;u.swatch_generation=t.art->generation();u.swatch_kind=(int)mode;u.swatch_slot=slot;u.swatch_mode=true;u.swatch_plan={};u.error.clear();u.swatch_error.clear();refresh_swatch_review(t);
}
void swatch_copy_menu(Tab &t,int slot){
    auto &u=palettes;ImGui::TextDisabled("Swatch #%d",slot);
    if(ImGui::MenuItem("Copy this color"))copy_swatch_colors(t,slot,true);u.copy_one_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    int count=0;for(size_t i=0;i<u.baseline.size();++i)count+=u.selected[i];if(!count)count=1;
    if(ImGui::MenuItem(("Copy selected colors ("+std::to_string(count)+")").c_str()))copy_swatch_colors(t,slot,false);u.copy_swatches_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::Separator();bool any=!swatch_clipboard.slots.empty();
    ImGui::TextDisabled("%zu copied colors",swatch_clipboard.slots.size());if(any)ImGui::TextDisabled("From %s",swatch_clipboard.source.c_str());
    if(ImGui::MenuItem("Paste copied color here",nullptr,false,swatch_clipboard.slots.size()==1&&slot>0))start_swatch_review(t,SwatchPaste::AtSlot,slot);u.paste_one_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(ImGui::MenuItem("Paste at original indices...",nullptr,false,any))start_swatch_review(t,SwatchPaste::SameIndices,slot);u.paste_swatches_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(ImGui::MenuItem("Append copied colors...",nullptr,false,any))start_swatch_review(t,SwatchPaste::Append,slot);u.append_swatches_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::Separator();if(ImGui::MenuItem("Clear copied colors",nullptr,false,any)){swatch_clipboard={};status="Color clipboard cleared. Other clipboards are unchanged.";}u.clear_swatches_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
}
void swatch_review_controls(Tab &t){
    auto &u=palettes;ImGui::TextUnformatted("Paste palette colors");ImGui::TextWrapped("%zu copied from %s",u.swatch_source.slots.size(),u.swatch_source.source.c_str());
    ImGui::TextDisabled("Destination: %s",u.name);
    bool changed=ImGui::RadioButton("Original indices",&u.swatch_kind,(int)SwatchPaste::SameIndices);u.swatch_same_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    changed|=ImGui::RadioButton("Append after last color",&u.swatch_kind,(int)SwatchPaste::Append);u.swatch_append_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(u.swatch_source.slots.size()==1){changed|=ImGui::RadioButton("One opaque slot",&u.swatch_kind,(int)SwatchPaste::AtSlot);if(u.swatch_kind==(int)SwatchPaste::AtSlot){ImGui::SetNextItemWidth(-1);changed|=ImGui::SliderInt("##swatch-dest",&u.swatch_slot,1,std::max(1,(int)u.baseline.size()-1),"Slot #%d");}}
    if(changed||t.art->generation()!=u.swatch_generation)refresh_swatch_review(t);
    if(!u.swatch_valid){ImGui::TextWrapped("%s",u.swatch_error.c_str());return;}
    auto &p=u.swatch_plan;ImGui::SeparatorText("Color paste impact");ImGui::Text("%zu -> %zu colors (+%d)",p.before.size(),p.words.size(),p.growth);ImGui::Text("%d slots change",p.changed);ImGui::TextWrapped("Shared by %d primary sprites / %d alternate references",p.primary,p.alternate);
    ImGui::TextDisabled("Declared depth: %d -> %d bpp",p.fields.bitspix,p.depth);
    if(p.ppp_images)ImGui::TextColored(ImVec4(1,.72f,.38f,1),"%d sprites cross the PPP %d limit",p.ppp_images,p.ppp);
    if(p.skipped_zero)ImGui::TextWrapped("Copied index 0 is skipped. Destination transparency stays unchanged.");
    if(p.growth&&p.mode==SwatchPaste::SameIndices)ImGui::TextWrapped("New gaps are filled with black. Existing pixel indices stay in place.");
    ImGui::TextWrapped("Original-index paste recolors users of those slots. Append keeps existing colors. No sprite pixels or palette assignments are remapped.");
    ImGui::BeginChild("swatch-mappings",ImVec2(0,135),ImGuiChildFlags_Border);
    for(size_t i=0;i<p.clipboard.slots.size();++i){auto &c=p.clipboard.slots[i];int d=p.destinations[i];if(d<0)ImGui::Text("#%d -> skipped (transparency)",c.index);else{unsigned char raw[]={(unsigned char)c.word,(unsigned char)(c.word>>8)},r,g,b;pal_word_to_rgb8(raw,&r,&g,&b);ImGui::PushID((int)i);ImGui::ColorButton("##copied",ImVec4(r/255.f,g/255.f,b/255.f,1),ImGuiColorEditFlags_NoTooltip,ImVec2(16,16));ImGui::SameLine();ImGui::Text("#%d -> #%d",c.index,d);ImGui::PopID();}}
    ImGui::EndChild();
}
void swatch_review_footer(Tab &t){
    auto &u=palettes;ImGui::TextWrapped("%s",!u.error.empty()?u.error.c_str():!u.swatch_valid?u.swatch_error.c_str():"Preview only. Apply records one Undo step; Save IMG separately.");
    ImGui::BeginDisabled(!u.swatch_valid||(!u.swatch_plan.changed&&!u.swatch_plan.growth));if(ImGui::Button("Apply color paste"))u.command=PaletteCommand::Swatches;u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();
    ImGui::SameLine();if(ImGui::Button("Cancel color paste")){u.swatch_mode=false;u.error.clear();}u.cancel_swatches_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SameLine();if(ImGui::Button("Close")){u.open=u.swatch_mode=false;u.command=PaletteCommand::None;ImGui::CloseCurrentPopup();}u.close_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
}
