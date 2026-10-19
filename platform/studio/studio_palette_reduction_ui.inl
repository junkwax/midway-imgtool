void refresh_palette_reduction(Tab &t){
    auto &u=palettes;if(t.art->generation()!=u.usage_generation){u.usage_valid=false;u.usage_error="The library changed. Cancel and reopen the downscale review.";return;}
    if(!u.reduce_dirty)return;u.reduce_dirty=false;u.reduce_allow=false;u.error.clear();u.usage_valid=plan_palette_reduction(*t.art,u.reduce_options,u.reduce_plan,u.usage_error);++u.usage_revision;
    if(u.usage_valid&&!u.reduce_plan.preview.preview_sprites.empty()&&std::find(u.reduce_plan.preview.preview_sprites.begin(),u.reduce_plan.preview.preview_sprites.end(),u.preview_sprite)==u.reduce_plan.preview.preview_sprites.end())u.preview_sprite=u.reduce_plan.preview.preview_sprites.front();
}
void start_palette_reduction(Tab &t){
    if(palette_pending())return;start_palette_usage(t);auto &u=palettes;u.reduce_mode=true;u.reduce_plan={};u.reduce_options={u.index,std::clamp((int)t.art->palette(u.index)->bitspix-1,4,7),t.ppp};u.reduce_dirty=true;refresh_palette_reduction(t);
}
void palette_reduction_controls(Tab &t){
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
    auto &u=palettes;ImGui::TextWrapped("%s",!u.error.empty()?u.error.c_str():!u.usage_error.empty()?u.usage_error.c_str():"Preview only. Apply downscales in one Undo step; Cancel leaves the IMG unchanged.");
    ImGui::BeginDisabled(!u.usage_valid||!u.reduce_plan.changed||(u.reduce_plan.approximate_colors&&!u.reduce_allow));if(ImGui::Button("Apply downscale"))u.command=PaletteCommand::Reduce;u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Cancel downscale")){u.usage_mode=false;u.error.clear();}u.cancel_reduce_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();if(ImGui::Button("Close")){u.open=u.usage_mode=false;u.command=PaletteCommand::None;ImGui::CloseCurrentPopup();}
}
