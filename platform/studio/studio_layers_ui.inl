struct LayerDragUi {
    bool active=false,dirty=false;
    uint64_t doc=0,sprite=0,generation=0;
    int start_x=0,start_y=0,x=0,y=0;
    ImVec2 mouse;float zoom=1;
    SDL_Texture *texture=nullptr;
} layer_drag;
ImVec2 layer_visible_point,layer_flip_point,layer_flatten_point,layer_remove_point,layer_paste_point,layer_right_point,layer_target_point,layer_base_point;
bool layer_move_selected(const Tab &t)const{
    auto im=t.art->sprite(t.selected);auto l=im?(SpriteLayer *)im->layer_p:nullptr;
    return t.move_overlay&&tool==Tool::Move&&l&&!t.flip&&!t.alignment.enabled&&!t.embedded.enabled&&!paste_mode&&!floating.active&&!paint_line.active&&!variant.active;
}
bool layer_move_target(const Tab &t)const{return layer_move_selected(t)&&img_layer(t.art->sprite(t.selected))->visible;}
void clear_layer_drag(){if(layer_drag.texture)SDL_DestroyTexture(layer_drag.texture);layer_drag=LayerDragUi{};}
void cancel_layer_drag(){
    if(!layer_drag.active)return;
    layer_drag.active=false;status="Cancelled overlay move. Artwork is unchanged.";
    // Canvas draw data may still refer to this texture. Retire next frame.
}
void validate_layer_drag(){
    auto &u=layer_drag;if(!u.active){if(u.texture)clear_layer_drag();return;}
    auto t=tab();
    if(!t||t->uid!=u.doc||t->selected_id!=u.sprite||t->art->generation()!=u.generation||!layer_move_target(*t)||ImGui::GetIO().AppFocusLost||ImGui::GetIO().WantTextInput||!ImGui::IsMousePosValid()||help||!error.empty()||path_dialog||edit_dialog!=EditDialog::None||ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel)){
        cancel_layer_drag();clear_layer_drag();
    }
}
void refresh_layer_drag(Tab &t){
    auto &u=layer_drag;if(!u.active||!u.dirty)return;u.dirty=false;
    auto im=t.art->sprite(t.selected);auto l=img_layer(im);checked_layer_bytes(*im);auto pal=t.art->palette(im->palnum);
    std::vector<unsigned char> rgba((size_t)im->w*im->h*4);
    for(int y=0;y<im->h;++y)for(int x=0;x<im->w;++x){
        int ci=((const unsigned char *)im->data_p)[(size_t)y*((im->w+3)&~3)+x];int sx=x-u.x,sy=y-u.y;
        if(sx>=0&&sy>=0&&sx<l->w&&sy<l->h){auto v=layer_pixels(l)[(size_t)sy*l->stride+sx];if(v)ci=v;}
        auto p=rgba.data()+((size_t)y*im->w+x)*4;
        if(ci<pal->numc)pal_word_to_rgb8((const unsigned char *)pal->data_p+ci*2,p,p+1,p+2);else{p[0]=p[2]=255;p[1]=0;}p[3]=ci?255:0;
    }
    if(!u.texture){u.texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,im->w,im->h);if(u.texture){SDL_SetTextureBlendMode(u.texture,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(u.texture,SDL_ScaleModeNearest);}}
    if(!u.texture||SDL_UpdateTexture(u.texture,nullptr,rgba.data(),im->w*4)!=0){cancel_layer_drag();error="Could not render the overlay move preview. Artwork was left unchanged.";}
}
void layer_drag_input(Tab &t,ImVec2 origin,float zoom,bool hovered){
    if(!layer_move_target(t)||!ImGui::IsMousePosValid())return;
    auto &u=layer_drag;auto &io=ImGui::GetIO();auto im=t.art->sprite(t.selected);auto l=img_layer(im);
    int x=(int)std::floor((io.MousePos.x-origin.x)/zoom),y=(int)std::floor((io.MousePos.y-origin.y)/zoom);
    bool hit=hovered&&x>=0&&y>=0&&x<im->w&&y<im->h&&x>=l->x&&y>=l->y&&x<l->x+l->w&&y<l->y+l->h;
    if(hit||u.active)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if(hit&&!u.active&&!io.KeyCtrl&&!io.KeyAlt&&!io.KeySuper&&ImGui::IsMouseClicked(0)){
        finish();clear_layer_drag();u.active=u.dirty=true;u.doc=t.uid;u.sprite=t.selected_id;u.generation=t.art->generation();u.x=u.start_x=l->x;u.y=u.start_y=l->y;u.mouse=io.MousePos;u.zoom=zoom;
        status="Drag overlay; release applies one edit. Shift locks an axis; Escape or Undo cancels.";
    }
    if(!u.active)return;
    if(ImGui::IsMouseDown(0)||ImGui::IsMouseReleased(0)){
        int dx=(int)std::lround(std::clamp((io.MousePos.x-u.mouse.x)/u.zoom,-65535.f,65535.f));
        int dy=(int)std::lround(std::clamp((io.MousePos.y-u.mouse.y)/u.zoom,-65535.f,65535.f));
        if(io.KeyShift){if(std::abs(dx)>=std::abs(dy))dy=0;else dx=0;}
        int nx=std::clamp(u.start_x+dx,-32768,32767),ny=std::clamp(u.start_y+dy,-32768,32767);
        u.dirty|=u.x!=nx||u.y!=ny;u.x=nx;u.y=ny;
    }
    if(ImGui::IsMouseReleased(0)){
        bool moved=u.x!=u.start_x||u.y!=u.start_y;
        if(t.art->edit_layer(t.selected,u.x,u.y,true,false,false,error))status=moved?"Moved overlay. Undo restores its previous position.":"Overlay position unchanged.";
        u.active=false;return;
    }
    // A lost release must never turn a later unrelated click into a move.
    if(!ImGui::IsMouseDown(0)){cancel_layer_drag();return;}
    refresh_layer_drag(t);
}
bool layer_move_keys(Tab &t){
    if(!layer_move_target(t))return false;
    auto &io=ImGui::GetIO();if(io.KeyCtrl||io.KeyAlt||io.KeySuper)return false;
    const ImGuiKey keys[]={ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_UpArrow,ImGuiKey_DownArrow};const int dx[]={-1,1,0,0},dy[]={0,0,-1,1};
    for(int i=0;i<4;++i)if(ImGui::IsKeyPressed(keys[i])){
        auto l=img_layer(t.art->sprite(t.selected));int step=io.KeyShift?10:1;int x=std::clamp(l->x+dx[i]*step,-32768,32767),y=std::clamp(l->y+dy[i]*step,-32768,32767);
        finish();if(t.art->edit_layer(t.selected,x,y,true,false,false,error))status="Nudged overlay. Undo restores its previous position.";return true;
    }return false;
}
void drop_paste_layer(){
    auto t=tab();if(!t||t->alignment.enabled||t->embedded.enabled||t->flip)return;
    bool ok=false;
    if(floating.active&&floating.stage.clipboard){
        ok=t->art->paste_layer(floating.stage,error);
        if(ok){t->selection=floating.prior_selection;floating.active=false;floating.dragging=false;}
    }else if(paste_mode){finish();ok=t->art->paste_layer(t->selected,clipboard,paste_x,paste_y,error);}
    if(ok){paste_mode=false;t->move_overlay=true;tool=Tool::Move;status="Created overlay. Drag it on the canvas or adjust it in the inspector. Save retains the original IMG format.";}
}
void layer_inspector(Tab &t){
    auto im=t.art->sprite(t.selected);auto l=img_layer(im);if(!l)return;
    ImGui::Spacing();ImGui::SeparatorText("Sprite overlay");
    ImGui::TextDisabled("%d x %d  |  one overlay per sprite",l->w,l->h);
    ImGui::TextWrapped("Save includes the visible overlay. Layers stay editable only during this session; IMG stores flattened pixels.");
    bool visible=l->visible!=0;int x=l->x,y=l->y;bool fh=false,fv=false,changed=false;
    changed|=ImGui::Checkbox("Show overlay",&visible);layer_visible_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::TextDisabled("Move target (Shift+V)");
    if(ImGui::RadioButton("Overlay",t.move_overlay)){finish();t.move_overlay=true;tool=Tool::Move;}layer_target_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SameLine();if(ImGui::RadioButton("Base selection",!t.move_overlay)){finish();t.move_overlay=false;tool=Tool::Move;}layer_base_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(layer_move_target(t))ImGui::TextWrapped("Drag overlay; release applies. Shift locks an axis. Arrows nudge 1 pixel; Shift + arrows 10.");
    ImGui::TextDisabled("Position X / Y");int xy[]={x,y};ImGui::SetNextItemWidth(-1);
    if(ImGui::InputInt2("##overlay-pos",xy)){x=std::clamp(xy[0],-32768,32767);y=std::clamp(xy[1],-32768,32767);changed=true;}
    auto nudge=[&](const char *label,int dx,int dy){if(ImGui::SmallButton(label)){int step=ImGui::GetIO().KeyShift?10:1;x=std::clamp(x+dx*step,-32768,32767);y=std::clamp(y+dy*step,-32768,32767);changed=true;}};
    nudge("Left",-1,0);ImGui::SameLine();nudge("Right",1,0);layer_right_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();nudge("Up",0,-1);ImGui::SameLine();nudge("Down",0,1);
    ImGui::SetItemTooltip("Move by 1 pixel; hold Shift for 10. Off-canvas pixels are clipped on save.");
    fh=ImGui::Button("Flip overlay H");layer_flip_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();fv=ImGui::Button("Flip V##overlay");
    if(changed||fh||fv){finish();t.art->edit_layer(t.selected,x,y,visible,fh,fv,error);}
    ImGui::BeginDisabled(!visible);bool flatten=ImGui::Button("Flatten overlay");layer_flatten_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();
    ImGui::SameLine();bool remove=ImGui::Button("Remove##overlay");layer_remove_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::TextWrapped("Index 0 is transparent. Paint and pixel Copy edit the base beneath this overlay. Flatten before geometry or palette remapping.");
    if(!visible)ImGui::TextColored(accent_surface(1),"Hidden overlay is excluded from saved IMG.");
    if(flatten){finish();if(t.art->flatten_layer(t.selected,error))status="Flattened the overlay into base pixels. Undo restores the separate overlay.";}
    if(remove){finish();if(t.art->remove_layer(t.selected,error))status="Removed the overlay. Undo restores it.";}
}
