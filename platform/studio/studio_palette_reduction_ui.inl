void refresh_palette_reduction(Tab &t){
    auto &u=palettes;if(t.art->generation()!=u.usage_generation){u.usage_valid=false;u.usage_error="The library changed. Cancel and reopen the downscale review.";return;}
    if(!u.reduce_dirty)return;u.reduce_dirty=false;u.reduce_allow=false;u.error.clear();u.usage_valid=plan_palette_reduction(*t.art,u.reduce_options,u.reduce_plan,u.usage_error);++u.usage_revision;
    if(u.usage_valid&&!u.reduce_plan.preview.preview_sprites.empty()&&std::find(u.reduce_plan.preview.preview_sprites.begin(),u.reduce_plan.preview.preview_sprites.end(),u.preview_sprite)==u.reduce_plan.preview.preview_sprites.end())u.preview_sprite=u.reduce_plan.preview.preview_sprites.front();
}
void start_palette_reduction(Tab &t){
    if(palette_pending())return;start_palette_usage(t);auto &u=palettes;u.reduce_mode=true;u.reduce_plan={};u.reduce_options={u.index,std::clamp((int)t.art->palette(u.index)->bitspix-1,4,7),t.ppp};u.reduce_dirty=true;refresh_palette_reduction(t);
}
void start_palette_blood(Tab &t){
    if(palette_pending())return;start_palette_usage(t);auto &u=palettes;u.reduce_mode=true;u.reduce_plan={};auto pal=t.art->palette(u.index);u.reduce_options={u.index,6,t.ppp};u.reduce_options.reserve_tail=std::min(10,std::max(1,(int)pal->numc-2));u.reduce_dirty=true;refresh_palette_reduction(t);
}
void palette_blood_controls(Tab &t){
    auto &u=palettes;auto &o=u.reduce_options;auto pal=t.art->palette(u.index);
    ImGui::TextUnformatted("Make room for blood");ImGui::TextWrapped("%s / all primary sprites",u.name);
    ImGui::TextWrapped("Condense character colors into the front of this palette. Reserve the last slots for richer blood detail.");
    ImGui::SetNextItemWidth(-1);if(ImGui::InputInt("##blood-count",&o.reserve_tail)){o.reserve_tail=std::clamp(o.reserve_tail,1,std::max(1,(int)pal->numc-2));u.reduce_dirty=true;}u.blood_count_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::TextDisabled("Blood colors (default 10)");
    const char *fills[]={"New editable red ramp","New blank slots","Keep existing blood tail"};ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##blood-fill",fills[(int)o.blood_fill])){for(int i=0;i<3;++i){if(ImGui::Selectable(fills[i],(int)o.blood_fill==i)){o.blood_fill=(BloodLaneFill)i;u.reduce_dirty=true;}u.blood_fill_choices[i]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;}ImGui::EndCombo();}u.blood_fill_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::TextWrapped(o.blood_fill==BloodLaneFill::KeepExisting?"Existing tail colors and pixels using them stay exact. Only character colors below the lane are condensed.":"Existing artwork is remapped below the blood lane, leaving every reserved slot unused and ready to paint.");
    refresh_palette_reduction(t);if(!u.usage_valid){ImGui::TextWrapped("%s",u.usage_error.c_str());return;}auto &p=u.reduce_plan;int first=pal->numc-o.reserve_tail;
    ImGui::SeparatorText("Reviewed result");ImGui::Text("Blood slots: %d - %d (%d colors)",first,pal->numc-1,o.reserve_tail);ImGui::Text("Character: up to %d opaque colors",first-1);ImGui::TextDisabled("Index 0 stays transparent");ImGui::Text("%d slots / %d bpp retained",pal->numc,pal->bitspix);ImGui::Text("%zu sprites / %llu remapped pixels",p.preview.primary_sprites.size(),(unsigned long long)p.preview.index_changes);
    ImGui::Text("%d colors approximated",p.approximate_colors);ImGui::Text("%llu pixels change color",(unsigned long long)p.color_changes);
    if(p.approximate_colors){ImGui::TextWrapped("Inspect before/after artwork. Fewer character colors can lose shading detail.");ImGui::Checkbox("Allow approximate colors",&u.reduce_allow);u.reduce_approximate_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;}else ImGui::TextDisabled("Exact character colors preserved");
    ImGui::TextWrapped("Palette size and declared bit depth stay fixed. Sprite names, anchors, transparency and original IMG format stay intact.");
}
void palette_reduction_controls(Tab &t){
    if(palettes.reduce_options.reserve_tail){palette_blood_controls(t);return;}
    auto &u=palettes;ImGui::TextUnformatted("Downscale palette");ImGui::TextWrapped("%s / all primary sprites",u.name);ImGui::TextWrapped("Choose a color limit. Used colors pack exactly when they fit; otherwise frequent pixel colors guide a weighted merge.");
    const char *labels[]={"4 bpp / up to 16 colors","5 bpp / up to 32 colors","6 bpp / up to 64 colors","7 bpp / up to 128 colors","8 bpp / up to 256 colors"};
    ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##reduce-depth",labels[u.reduce_options.depth-4])){for(int i=0;i<5;++i){if(ImGui::Selectable(labels[i],u.reduce_options.depth==i+4)){u.reduce_options.depth=i+4;u.reduce_dirty=true;}u.reduce_choices[i]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;}ImGui::EndCombo();}u.reduce_depth_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SetNextItemWidth(-1);if(ImGui::SliderInt("##reduce-ppp",&u.reduce_options.ppp,0,8,"Packing PPP: %d (0 = auto)"))u.reduce_dirty=true;refresh_palette_reduction(t);
    if(!u.usage_valid){ImGui::TextWrapped("%s",u.usage_error.c_str());return;}auto &p=u.reduce_plan;auto &v=p.preview;
    ImGui::SeparatorText("Downscale impact");ImGui::Text("%zu -> %zu colors",v.original.size(),v.words.size());ImGui::Text("%d active opaque slots",p.active_colors);ImGui::Text("%zu sprites / %llu remapped pixels",v.primary_sprites.size(),(unsigned long long)v.index_changes);ImGui::Text("%d colors approximated",p.approximate_colors);ImGui::Text("%llu pixels change color",(unsigned long long)p.color_changes);ImGui::TextDisabled("Method: %s",p.quantized?"Pixel-weighted merge":v.removed?"Exact packing":"Keep original colors");
    ImGui::TextDisabled("Declared depth: %d -> %d bpp",t.art->palette(u.index)->bitspix,u.reduce_options.depth);if(v.ppp_changed)ImGui::TextColored(ImVec4(1,.72f,.38f,1),"PPP %d fallback changes",u.reduce_options.ppp);
    if(p.approximate_colors){ImGui::TextWrapped("This limit changes some colors. Inspect the sprites before allowing approximation.");ImGui::Checkbox("Allow approximate colors",&u.reduce_allow);u.reduce_approximate_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;}else ImGui::TextDisabled("Exact colors / transparency preserved");
    ImGui::TextWrapped("Index 0 stays transparent; opaque pixels never map to zero. The palette name, other metadata and sprite assignments stay fixed.");
}
void palette_reduction_footer(Tab &t){
    auto &u=palettes;bool blood=u.reduce_options.reserve_tail!=0;ImGui::TextWrapped("%s",!u.error.empty()?u.error.c_str():!u.usage_error.empty()?u.usage_error.c_str():blood?"Preview only. Apply reserves the lane in one Undo step; Cancel leaves the IMG unchanged.":"Preview only. Apply downscales in one Undo step; Cancel leaves the IMG unchanged.");
    ImGui::BeginDisabled(!u.usage_valid||!u.reduce_plan.changed||(u.reduce_plan.approximate_colors&&!u.reduce_allow));if(ImGui::Button(blood?"Apply blood lane":"Apply downscale"))u.command=PaletteCommand::Reduce;u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button(blood?"Cancel":"Cancel downscale")){u.usage_mode=false;u.error.clear();}u.cancel_reduce_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();if(ImGui::Button("Close")){u.open=u.usage_mode=false;u.command=PaletteCommand::None;ImGui::CloseCurrentPopup();}
}
