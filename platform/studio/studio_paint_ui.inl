PaintBrush paint_brush(const Tab &t)const{
    PaintBrush brush;brush.color=tool==Tool::Erase?0:t.color;brush.radius=t.radius;brush.shape=t.brush_shape;brush.match_colors=t.match_colors;brush.tolerance=t.paint_tolerance;return brush;
}
void paint_match_controls(Tab &t){
    if(tool!=Tool::Replace)ImGui::TextWrapped(tool==Tool::SmartErase?"Click the color to erase. Scope stays inside the selection.":"Click a pixel to fill its matching colors with the selected palette index.");
    ImGui::Checkbox("Match palette colors",&t.match_colors);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Off: match the exact palette index. On: match RGB555 colors, including duplicate colors in different slots. Transparency never matches opaque black.");
    ImGui::BeginDisabled(!t.match_colors);ImGui::SetNextItemWidth(-1);ImGui::SliderInt("##paint-tolerance",&t.paint_tolerance,0,255,"Tolerance: %d");ImGui::EndDisabled();
    if(tool!=Tool::Replace)ImGui::Checkbox("Connected pixels only",&t.fill_connected);
    if(tool!=Tool::Replace)ImGui::TextWrapped("Disconnected selection areas stay separate during connected fill.");
    if(tool==Tool::SmartErase){ImGui::Checkbox("Clean erased edges",&t.erase_cleanup);tools_ui.cleanup_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;if(ImGui::IsItemHovered())ImGui::SetTooltip("Recolors surviving edge pixels from safe neighboring colors in the same undo step. Does not erase additional pixels. Off gives a plain matching erase.");}
}
void paint_brush_controls(Tab &t){
    if(tool==Tool::Replace){ImGui::Checkbox("All matching pixels",&t.replace_all);ImGui::TextWrapped(t.replace_all?"Click the color to replace throughout the sprite or selection.":"Drag to replace only the color picked at the stroke's start.");if(stroke)ImGui::TextDisabled("Source index: %d",stroke_brush.source);}
    ImGui::BeginDisabled(tool==Tool::Replace&&t.replace_all);ImGui::SetNextItemWidth(-1);ImGui::SliderInt("##radius",&t.radius,1,16,"Radius: %d px");const char *shapes[]={"Round","Square"};int shape=(int)t.brush_shape;ImGui::SetNextItemWidth(-1);if(ImGui::Combo("##brush-shape",&shape,shapes,2))t.brush_shape=(BrushShape)shape;ImGui::EndDisabled();
    ImGui::TextDisabled("[ / ] changes brush size");
    if(tool==Tool::Line)ImGui::TextWrapped("Drag to preview a line. Shift snaps horizontal, vertical or diagonal. Release applies; Escape cancels.");
    if(tool==Tool::Replace)paint_match_controls(t);
}
void clear_paint_line(){if(paint_line.texture)SDL_DestroyTexture(paint_line.texture);paint_line=PaintLineUi{};}
void cancel_paint_line(){paint_line.active=false;status="Cancelled line preview. Artwork is unchanged.";}
void validate_paint_line(){
    auto &u=paint_line;if(!u.active){if(u.texture||u.review.owner)clear_paint_line();return;}auto t=tab();
    if(!t||t->uid!=u.doc||t->selected_id!=u.review.sprite||t->art->generation()!=u.review.generation||t->flip||t->alignment.enabled||t->embedded.enabled){cancel_paint_line();status="Line preview closed because its source or workspace changed.";clear_paint_line();}
}
void refresh_paint_line(){
    auto &u=paint_line;if(!u.active||!u.dirty)return;u.dirty=false;u.valid=preview_paint_line(u.review,u.pixels,u.changed,u.error);if(u.texture)SDL_DestroyTexture(u.texture);u.texture=nullptr;if(!u.valid)return;
    const auto &c=u.review.original;std::vector<unsigned char> rgba(u.pixels.size()*4);
    auto t=find(u.doc);auto im=t?t->art->sprite(t->art->sprite_index(u.review.sprite)):nullptr;if(!im){u.valid=false;return;}checked_layer_bytes(*im);
    for(size_t i=0;i<u.pixels.size();++i){int slot=overlay_pixel(*im,(int)(i%c.w),(int)(i/c.w),u.pixels[i]);unsigned short color=c.colors[slot];unsigned char words[]={(unsigned char)color,(unsigned char)(color>>8)};auto p=rgba.data()+i*4;pal_word_to_rgb8(words,p,p+1,p+2);p[3]=slot?255:0;}
    u.texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,c.w,c.h);if(!u.texture){u.valid=false;u.error="Could not create the line preview texture.";return;}
    SDL_SetTextureBlendMode(u.texture,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(u.texture,SDL_ScaleModeNearest);SDL_UpdateTexture(u.texture,nullptr,rgba.data(),c.w*4);
}
void paint_line_input(Tab &t,ImVec2 origin,float z,bool hovered){
    auto &u=paint_line;const auto &io=ImGui::GetIO();auto im=t.art->sprite(t.selected);int x=(int)std::floor((io.MousePos.x-origin.x)/z),y=(int)std::floor((io.MousePos.y-origin.y)/z);
    if(!u.active&&tool==Tool::Line&&!floating.active&&!paste_mode&&!t.flip&&hovered&&x>=0&&y>=0&&x<im->w&&y<im->h&&ImGui::IsMouseClicked(0)){
        finish();clear_paint_line();if(!begin_paint_line(*t.art,t.selected,x,y,paint_brush(t),t.selection,u.review,error))return;u.active=true;u.doc=t.uid;u.dirty=true;status="Line preview: release applies; Escape cancels.";
    }
    if(!u.active)return;
    if(ImGui::IsMouseDown(0)||ImGui::IsMouseReleased(0)){
        int nx=std::clamp(x,-32,(int)im->w+32),ny=std::clamp(y,-32,(int)im->h+32);
        if(io.KeyShift){int dx=nx-u.review.x0,dy=ny-u.review.y0,ax=std::abs(dx),ay=std::abs(dy);if(ay<=ax*.414213562)ny=u.review.y0;else if(ax<=ay*.414213562)nx=u.review.x0;else{int length=std::max(ax,ay);nx=u.review.x0+(dx<0?-length:length);ny=u.review.y0+(dy<0?-length:length);}}
        if(nx!=u.review.x1||ny!=u.review.y1){u.review.x1=nx;u.review.y1=ny;u.dirty=true;}
    }refresh_paint_line();
    if(ImGui::IsMouseReleased(0)){if(u.valid){if(!t.art->apply_paint_line(u.review,error))status="Could not apply the line preview.";else status="Drew "+std::to_string(u.changed)+" pixels in one undo step.";}else error=u.error;u.active=false;}
}
