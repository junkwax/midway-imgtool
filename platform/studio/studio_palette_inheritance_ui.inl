int inherit_palette_source(const Tab &t){int source=-1;for(int index:t.palette_marks){if(index==palettes.index)continue;if(source>=0)return -1;source=index;}return source;}
void refresh_palette_inheritance(Tab &t){
    auto &u=palettes;if(t.art->generation()!=u.inherit_generation){u.inherit_valid=false;u.inherit_error="The library changed. Cancel and reopen the inheritance review.";return;}
    if(!u.inherit_dirty)return;u.inherit_dirty=false;u.inherit_allow=false;u.error.clear();u.inherit_valid=plan_palette_inheritance(*t.art,u.inherit_options,u.inherit_plan,u.inherit_error);++u.inherit_revision;
    if(u.inherit_valid&&!u.inherit_plan.sprites.empty()&&std::none_of(u.inherit_plan.sprites.begin(),u.inherit_plan.sprites.end(),[&](const PaletteCompactionSprite &s){return s.index==u.preview_sprite;}))u.preview_sprite=u.inherit_plan.sprites.front().index;
}
void start_palette_inheritance(Tab &t){
    auto &u=palettes;int source=inherit_palette_source(t);if(source<0||palette_pending())return;
    u.merge_mode=u.usage_mode=u.ramp_mode=u.swatch_mode=false;u.inherit_mode=true;u.inherit_generation=t.art->generation();u.inherit_options={u.index,source,t.ppp,false};u.inherit_plan={};u.inherit_dirty=true;refresh_palette_inheritance(t);
}
void palette_inheritance_controls(Tab &t){
    auto &u=palettes;refresh_palette_inheritance(t);ImGui::TextUnformatted("Inherit palette colors");
    auto target=t.art->palette(u.inherit_options.target),source=t.art->palette(u.inherit_options.source);if(!target||!source)return;
    ImGui::TextWrapped("From: %s",palette_name(*source).c_str());ImGui::TextWrapped("Into: %s",palette_name(*target).c_str());ImGui::TextWrapped("Keeps the destination name and sprite assignments. Its colors, declared depth and padding follow the marked source.");
    if(ImGui::Checkbox("Perceptual color matching",&u.inherit_options.perceptual))u.inherit_dirty=true;u.inherit_perceptual_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SetItemTooltip("Off matches the original tool's RGB distance. On weights green more strongly. Changing matching resets approximation approval.");
    ImGui::SetNextItemWidth(-1);if(ImGui::SliderInt("##inherit-ppp",&u.inherit_options.ppp,0,8,"Packing PPP: %d (0 = auto)"))u.inherit_dirty=true;refresh_palette_inheritance(t);
    if(!u.inherit_valid){ImGui::TextWrapped("%s",u.inherit_error.c_str());return;}auto &p=u.inherit_plan;
    ImGui::SeparatorText("Inheritance impact");ImGui::Text("%zu -> %zu colors",p.original.size(),p.words.size());ImGui::Text("%zu sprites / %llu remapped pixels",p.sprites.size(),(unsigned long long)p.index_changes);ImGui::Text("%d used colors approximated",p.approximate_colors);ImGui::Text("%llu pixels change color",(unsigned long long)p.color_changes);ImGui::TextDisabled("Declared depth: %d -> %d bpp",target->bitspix,source->bitspix);
    if(p.ppp_changed)ImGui::TextColored(ImVec4(1,.72f,.38f,1),"PPP %d fallback changes",p.options.ppp);
    if(p.color_changes){ImGui::TextWrapped("Some used colors are absent from the marked palette. Review the result before allowing approximation.");ImGui::Checkbox("Allow approximate colors",&u.inherit_allow);u.inherit_approximate_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;}else ImGui::TextDisabled("Used colors match exactly.");
    ImGui::TextWrapped("Index 0 stays transparent; opaque pixels always map to an opaque slot. The marked palette and its sprites stay unchanged.");
}
void palette_inheritance_preview(Tab &t){
    auto &u=palettes;if(!u.inherit_valid)return;auto &p=u.inherit_plan;
    ImGui::TextUnformatted("Before / inherited colors");if(!p.sprites.empty()){
        ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##inherit-sprite",t.art->sprite(u.preview_sprite)->n_s)){for(const auto &s:p.sprites){ImGui::PushID(s.index);if(ImGui::Selectable(t.art->sprite(s.index)->n_s,s.index==u.preview_sprite))u.preview_sprite=s.index;ImGui::PopID();}ImGui::EndCombo();}
        auto im=t.art->sprite(u.preview_sprite);if(!u.tex_inherit||u.tex_inherit_revision!=u.inherit_revision||u.tex_sprite!=u.preview_sprite||u.tex_doc!=t.uid){
            if(u.tex_w!=im->w||u.tex_h!=im->h||!u.preview||!u.original){if(u.preview)SDL_DestroyTexture(u.preview);if(u.original)SDL_DestroyTexture(u.original);u.preview=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,im->w,im->h);u.original=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,im->w,im->h);u.tex_w=im->w;u.tex_h=im->h;}
            auto upload=[&](SDL_Texture *tex,bool after){auto rgba=render_palette_inheritance(*t.art,p,u.preview_sprite,after);if(tex&&!rgba.empty()){SDL_SetTextureBlendMode(tex,SDL_BLENDMODE_BLEND);SDL_UpdateTexture(tex,nullptr,rgba.data(),im->w*4);}};upload(u.original,false);upload(u.preview,true);u.tex_inherit=true;u.tex_inherit_revision=u.inherit_revision;u.tex_sprite=u.preview_sprite;u.tex_doc=t.uid;u.tex_usage=u.tex_merge=false;u.tex_words.clear();u.tex_baseline.clear();
        }
        float box=(ImGui::GetContentRegionAvail().x-8)*.5f;auto draw=[&](const char *label,SDL_Texture *tex){ImGui::BeginGroup();ImGui::TextDisabled("%s",label);auto a=ImGui::GetCursorScreenPos();ImVec2 size(box,200);ImGui::InvisibleButton(label,size);auto dl=ImGui::GetWindowDrawList();dl->PushClipRect(a,a+size,true);for(float y=0;y<size.y;y+=12)for(float x=0;x<size.x;x+=12)dl->AddRectFilled(a+ImVec2(x,y),a+ImVec2(std::min(x+12,size.x),std::min(y+12,size.y)),(((int)x/12+(int)y/12)&1)?IM_COL32(41,44,51,255):IM_COL32(33,36,43,255));if(tex){float z=std::min((size.x-14)/im->w,(size.y-14)/im->h);ImVec2 sprite(im->w*z,im->h*z),pos=a+(size-sprite)*.5f;dl->AddImage((ImTextureID)(intptr_t)tex,pos,pos+sprite);}dl->PopClipRect();ImGui::EndGroup();};draw("Before",u.original);ImGui::SameLine();draw("After",u.preview);
    }else ImGui::TextWrapped("No sprites use this palette. Only its colors, count, depth and padding will change.");
    ImGui::SeparatorText("Inherited palette");ImGui::BeginChild("inherited-swatches",ImVec2(0,110),ImGuiChildFlags_Border);int columns=std::max(1,(int)((ImGui::GetContentRegionAvail().x+3)/23));for(int i=0;i<(int)p.words.size();++i){if(i%columns)ImGui::SameLine(0,3);auto w=p.words[i];unsigned char raw[]={(unsigned char)w,(unsigned char)(w>>8)},r,g,b;pal_word_to_rgb8(raw,&r,&g,&b);ImGui::PushID(i);ImGui::ColorButton("##inherited",ImVec4(r/255.f,g/255.f,b/255.f,i?1.f:0.f),ImGuiColorEditFlags_NoTooltip,ImVec2(20,20));if(ImGui::IsItemHovered())ImGui::SetTooltip("Index %d / RGB %d, %d, %d",i,r,g,b);ImGui::PopID();}ImGui::EndChild();
    ImGui::TextWrapped("Color-count and depth changes can affect LOAD2 and exports. Verify packing before building game data.");
}
void palette_inheritance_footer(Tab &t){
    auto &u=palettes;ImGui::TextWrapped("%s",!u.error.empty()?u.error.c_str():!u.inherit_error.empty()?u.inherit_error.c_str():"Preview only. Apply inherits colors in one Undo step; Cancel leaves the IMG unchanged.");
    ImGui::BeginDisabled(!u.inherit_valid||!u.inherit_plan.changed||(u.inherit_plan.color_changes&&!u.inherit_allow));if(ImGui::Button("Apply inherited colors"))u.command=PaletteCommand::Inherit;u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Cancel inheritance")){u.inherit_mode=false;u.error.clear();}u.cancel_inherit_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();if(ImGui::Button("Close")){u.open=u.inherit_mode=false;u.command=PaletteCommand::None;ImGui::CloseCurrentPopup();}
}
