void clear_variant(){if(variant.texture)SDL_DestroyTexture(variant.texture);variant=VariantUi{};}
void cancel_variant(){variant.active=false;variant.dragging=false;status="Cancelled variant preview. Artwork and palettes are unchanged.";}
void validate_variant(){
    auto &u=variant;if(!u.active){if(u.texture||u.source.owner)clear_variant();return;}auto t=tab();
    if(!t||t->uid!=u.doc||t->selected_id!=u.source.sprite||t->art->generation()!=u.source.generation||t->flip||t->alignment.enabled||t->embedded.enabled){cancel_variant();status="Variant preview closed because its source or workspace changed.";clear_variant();}
}
void refresh_variant(){
    auto &u=variant;if(!u.active||!u.dirty)return;u.dirty=false;u.valid=false;u.error.clear();
    if(!u.dragging)u.valid=plan_variant(u.source,u.coverage,u.plan,u.error);
    auto &s=u.source;auto &c=s.original;std::vector<unsigned char> rgba(c.pixels.size()*4);u.preview_palette=std::clamp(u.preview_palette,0,(int)s.palettes.size()-1);
    auto &words=!u.show_original&&u.valid?u.plan.palettes[u.preview_palette]:s.palettes[u.preview_palette];
    for(size_t i=0;i<c.pixels.size();++i){int index=!u.show_original&&u.valid?u.plan.pixels[i]:c.pixels[i];auto word=index<(int)words.size()?words[index]:0;
        if(!u.show_original&&!u.valid&&u.preview_palette==s.palette&&u.coverage[i]&&index)word=s.palettes[s.palette][s.color];unsigned char bytes[]={(unsigned char)word,(unsigned char)(word>>8)};auto p=rgba.data()+i*4;pal_word_to_rgb8(bytes,p,p+1,p+2);p[3]=index?255:0;}
    if(u.texture)SDL_DestroyTexture(u.texture);u.texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,c.w,c.h);
    if(!u.texture){u.valid=false;u.error="Could not create the variant preview texture.";return;}SDL_SetTextureBlendMode(u.texture,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(u.texture,SDL_ScaleModeNearest);SDL_UpdateTexture(u.texture,nullptr,rgba.data(),c.w*4);
}
void begin_variant(Tab &t,bool selected){
    finish();clear_variant();auto &u=variant;if(!capture_variant_source(*t.art,t.selected,t.color,t.ppp,t.selection,u.source,error))return;
    u.doc=t.uid;u.active=true;u.dragging=!selected;u.preview_palette=u.source.palette;u.brush=paint_brush(t);u.brush.color=1;u.brush.source=-1;u.coverage.resize(u.source.original.pixels.size(),0);paste_mode=false;
    if(selected){auto &c=u.source.original;for(int y=0;y<c.h;++y)for(int x=0;x<c.w;++x)if(u.source.selection.contains(x,y))u.coverage[(size_t)y*c.w+x]=1;refresh_variant();}
    status=selected?"Review variant pixels and palette changes, then Apply or Cancel.":"Paint the variant preview. Release to review; Escape cancels.";
}
void apply_variant(){
    auto &u=variant;if(!u.active||u.dragging||u.dirty||!u.valid)return;auto t=find(u.doc);if(!t)return;
    if(!t->art->apply_variant(u.source,u.coverage,u.plan,u.error)){u.valid=false;return;}u.active=false;status="Applied palette variant in one undo step.";
}
void variant_input(Tab &t,ImVec2 origin,float z,bool hovered){
    auto &u=variant;auto &io=ImGui::GetIO();auto im=t.art->sprite(t.selected);int x=(int)std::floor((io.MousePos.x-origin.x)/z),y=(int)std::floor((io.MousePos.y-origin.y)/z);bool started=false;
    if(!u.active&&tool==Tool::Variant&&!floating.active&&!paint_line.active&&!paste_mode&&!t.flip&&hovered&&x>=0&&y>=0&&x<im->w&&y<im->h&&ImGui::IsMouseClicked(0)){begin_variant(t,false);started=u.active;u.last_x=x;u.last_y=y;}
    if(!u.active)return;if(u.dragging&&(ImGui::IsMouseDown(0)||ImGui::IsMouseReleased(0))){int nx=std::clamp(x,-32,(int)im->w+32),ny=std::clamp(y,-32,(int)im->h+32);
        if(started||nx!=u.last_x||ny!=u.last_y){unsigned short colors[]={0,0};PAL pal{};pal.numc=2;pal.data_p=colors;if(paint_line_pixels(u.coverage.data(),im->w,im->h,im->w,pal,u.last_x,u.last_y,nx,ny,u.brush,&u.source.selection,u.error)>0)u.dirty=true;u.last_x=nx;u.last_y=ny;}
        if(ImGui::IsMouseReleased(0)){u.dragging=false;u.dirty=true;status="Review variant pixels and palette changes, then Apply or Cancel.";}}
    refresh_variant();
}
void variant_controls(Tab &t){
    ImGui::TextWrapped("Recolor painted pixels in this palette while preserving their appearance under other palettes.");
    ImGui::BeginDisabled(!t.selection.valid||t.color==0||t.flip||paste_mode);if(ImGui::Button("Preview selected pixels"))begin_variant(t,true);variant.selection_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();
    if(t.color==0)ImGui::TextColored(accent_surface(1),"Choose an opaque target color.");
    paint_brush_controls(t);ImGui::TextWrapped("Drag to paint a preview, then Apply or Cancel. Transparent pixels stay transparent.");
}
void variant_inspector(Tab &t){
    auto &u=variant;auto &s=u.source;ImGui::SeparatorText("Palette variant preview");
    ImGui::BeginDisabled(!u.valid||u.dirty||u.dragging);bool apply=ImGui::Button("Apply",ImVec2(100,0));u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();bool cancel=ImGui::Button("Cancel",ImVec2(100,0));u.cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(u.dragging)ImGui::TextWrapped("Release to review. The IMG is unchanged.");
    else if(u.valid){auto &p=u.plan;ImGui::Text("%d pixels change",p.changed);ImGui::Text("%d unused slots / %d reused",p.new_slots,p.reused_slots);ImGui::Text("%d palettes change / %d grow",(int)p.changed_palettes.size(),(int)p.grown_palettes.size());ImGui::TextDisabled("%d transparent pixels skipped",p.transparent);
        if(!p.ppp_breaks.empty())ImGui::TextColored(accent_surface(1),"LOAD2: %d palettes cross PPP %d",(int)p.ppp_breaks.size(),s.ppp);
        if(!p.ppp_breaks.empty())ImGui::TextWrapped("Their color counts exceed the packing limit. LOAD2 may change sprite addresses.");
    }
    if(!u.error.empty())ImGui::TextWrapped("%s",u.error.c_str());
    bool redraw=ImGui::Checkbox("Show original",&u.show_original);u.original_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##variant-preview-palette",t.art->palette(u.preview_palette)->n_s)){for(int i=0;i<(int)s.palettes.size();++i)if(ImGui::Selectable(t.art->palette(i)->n_s,u.preview_palette==i)){u.preview_palette=i;redraw=true;}ImGui::EndCombo();}
    ImGui::TextWrapped("Preview another palette to check the original appearance. Unused color entries may be repurposed; existing painted colors stay intact.");ImGui::TextDisabled("Enter applies / Escape cancels");
    if(redraw)u.dirty=true; // Rebuild next frame; canvas has already queued this texture.
    if(apply)apply_variant();if(cancel)cancel_variant();
}
