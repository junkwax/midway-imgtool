void clear_floating(){if(floating.texture)SDL_DestroyTexture(floating.texture);floating=FloatingUi{};}
void cancel_floating(){
    auto &u=floating;if(!u.active)return;auto t=find(u.doc);
    if(t&&t->selected_id==u.stage.sprite){auto im=t->art->sprite(t->selected);if(im&&u.stage.source.fits(im->w,im->h))t->selection=u.stage.clipboard?u.prior_selection:u.stage.source;}
    u.active=false;u.dragging=false;status="Cancelled selection preview. Artwork is unchanged.";
    // Texture retirement waits until the next frame: inspector buttons follow canvas drawing.
}
void validate_floating(){
    auto &u=floating;if(!u.active){if(u.texture||u.stage.owner)clear_floating();return;}
    auto t=tab();if(!t||t->uid!=u.doc||t->selected_id!=u.stage.sprite||t->art->generation()!=u.stage.generation||t->flip||t->alignment.enabled||t->embedded.enabled){cancel_floating();status="Selection preview closed because its source or workspace changed.";clear_floating();}
}
void begin_floating(Tab &t){
    finish();synchronize(t);clear_floating();auto &u=floating;
    if(!begin_selection_transform(*t.art,t.selected,t.selection,u.stage,error))return;
    u.doc=t.uid;u.active=true;u.dirty=true;paste_mode=false;tool=Tool::Move;
    status="Drag or transform the preview. Enter applies; Escape cancels.";
}
void begin_clipboard_floating(Tab &t){
    finish();synchronize(t);clear_floating();auto &u=floating;
    if(!begin_clipboard_transform(*t.art,t.selected,clipboard,u.stage,error))return;
    u.doc=t.uid;u.prior_selection=t.selection;u.stage.x=std::clamp(paste_x,0,u.stage.canvas_w-u.stage.w);u.stage.y=std::clamp(paste_y,0,u.stage.canvas_h-u.stage.h);u.active=u.dirty=true;paste_mode=false;tool=Tool::Move;
    status="Transform pasted pixels. Enter applies; Escape cancels without changing the sprite.";
}
void refresh_floating(){
    auto &u=floating;if(!u.active||!u.dirty)return;u.dirty=false;u.valid=preview_selection_transform(u.stage,u.preview,u.error);
    if(u.texture)SDL_DestroyTexture(u.texture);u.texture=nullptr;if(!u.valid)return;
    auto &s=u.stage;auto t=find(u.doc);auto im=t?t->art->sprite(t->art->sprite_index(s.sprite)):nullptr;if(!im){u.valid=false;return;}checked_layer_bytes(*im);std::vector<unsigned char> rgba(u.preview.pixels.size()*4);
    for(size_t i=0;i<u.preview.pixels.size();++i){int c=overlay_pixel(*im,(int)(i%s.canvas_w),(int)(i/s.canvas_w),u.preview.pixels[i]);auto p=rgba.data()+i*4;unsigned short word=s.clip.colors[c];unsigned char bytes[]={(unsigned char)word,(unsigned char)(word>>8)};pal_word_to_rgb8(bytes,p,p+1,p+2);p[3]=c?255:0;}
    u.texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,s.canvas_w,s.canvas_h);
    if(!u.texture){u.valid=false;u.error="Could not create the selection preview texture.";return;}
    SDL_SetTextureBlendMode(u.texture,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(u.texture,SDL_ScaleModeNearest);SDL_UpdateTexture(u.texture,nullptr,rgba.data(),s.canvas_w*4);
}
void apply_floating(){
    auto &u=floating;if(!u.active||u.dirty||!u.valid)return;auto t=find(u.doc);if(!t)return;
    if(!t->art->apply_selection_transform(u.stage,u.error)){u.valid=false;return;}
    t->selection=u.preview.selection;u.active=false;u.dragging=false;status="Applied selected pixels. Undo restores the complete edit.";
}
void duplicate_floating(){
    auto &u=floating;u.stage.copy=true;apply_floating();if(!u.active)if(auto t=tab()){begin_floating(*t);floating.stage.copy=true;}
}
void floating_shortcuts(ImGuiInputFlags route){
    auto &u=floating;if(u.stage.clipboard&&ImGui::Shortcut(ImGuiKey_L,route)){drop_paste_layer();return;}if(ImGui::IsKeyPressed(ImGuiKey_Escape)){cancel_floating();return;}
    if(ImGui::Shortcut(ImGuiKey_Enter,route)||ImGui::Shortcut(ImGuiKey_KeypadEnter,route)||ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_E,route)||ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_T,route)){apply_floating();return;}
    if(ImGui::Shortcut(ImGuiKey_H,route)){u.stage.flip_h=!u.stage.flip_h;u.dirty=true;}
    if(ImGui::Shortcut(ImGuiKey_V,route)){u.stage.flip_v=!u.stage.flip_v;u.dirty=true;}
    if(ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_J,route)){duplicate_floating();return;}
    const ImGuiKey arrows[]={ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_UpArrow,ImGuiKey_DownArrow};const int dx[]={-1,1,0,0},dy[]={0,0,-1,1};
    for(int i=0;i<4;++i)for(int fast=0;fast<2;++fast)if(ImGui::Shortcut((fast?ImGuiMod_Shift:0)|arrows[i],route)){u.stage.x=std::clamp(u.stage.x+dx[i]*(fast?10:1),0,std::max(0,u.stage.canvas_w-u.stage.w));u.stage.y=std::clamp(u.stage.y+dy[i]*(fast?10:1),0,std::max(0,u.stage.canvas_h-u.stage.h));u.dirty=true;}
}
void floating_input(Tab &t,ImVec2 origin,float z,bool hovered){
    auto &io=ImGui::GetIO();int x=(int)std::floor((io.MousePos.x-origin.x)/z),y=(int)std::floor((io.MousePos.y-origin.y)/z);
    bool started=false;if(!floating.active&&tool==Tool::Move&&!t.flip&&!paste_mode&&hovered&&ImGui::IsMouseClicked(0)&&t.selection.contains(x,y)){begin_floating(t);started=floating.active;}
    auto &u=floating;if(!u.active)return;auto &s=u.stage;int handle=-1;
    if(!started&&s.w*z>=24&&s.h*z>=24){ImVec2 corners[]={origin+ImVec2(s.x*z,s.y*z),origin+ImVec2((s.x+s.w)*z,s.y*z),origin+ImVec2(s.x*z,(s.y+s.h)*z),origin+ImVec2((s.x+s.w)*z,(s.y+s.h)*z)};for(int i=0;i<4;++i)if(std::abs(io.MousePos.x-corners[i].x)<8&&std::abs(io.MousePos.y-corners[i].y)<8)handle=i;}
    bool inside=started||(u.valid&&u.preview.selection.contains(x,y));
    if(hovered&&(inside||handle>=0)){
        ImGui::SetMouseCursor(handle<0?ImGuiMouseCursor_ResizeAll:handle==0||handle==3?ImGuiMouseCursor_ResizeNWSE:ImGuiMouseCursor_ResizeNESW);
        if(ImGui::IsMouseClicked(0)){u.dragging=true;u.drag_kind=handle;u.drag_mouse=io.MousePos;u.drag_x=s.x;u.drag_y=s.y;u.drag_w=s.w;u.drag_h=s.h;}
    }
    if(u.dragging&&ImGui::IsMouseDown(0)){
        int dx=(int)std::lround(std::clamp((io.MousePos.x-u.drag_mouse.x)/z,-8192.f,8192.f)),dy=(int)std::lround(std::clamp((io.MousePos.y-u.drag_mouse.y)/z,-8192.f,8192.f));
        if(u.drag_kind<0){s.x=std::clamp(u.drag_x+dx,0,std::max(0,s.canvas_w-s.w));s.y=std::clamp(u.drag_y+dy,0,std::max(0,s.canvas_h-s.h));}
        else {
            bool left=u.drag_kind==0||u.drag_kind==2,top=u.drag_kind<2;int maxw=left?u.drag_x+u.drag_w:s.canvas_w-u.drag_x,maxh=top?u.drag_y+u.drag_h:s.canvas_h-u.drag_y;
            int w=std::clamp(u.drag_w+(left?-dx:dx),1,std::max(1,maxw)),h=std::clamp(u.drag_h+(top?-dy:dy),1,std::max(1,maxh));
            if(u.ratio){double ratio=(double)u.drag_w/u.drag_h;if(std::abs(dx)/(double)u.drag_w>=std::abs(dy)/(double)u.drag_h)h=std::max(1,(int)std::lround(w/ratio));else w=std::max(1,(int)std::lround(h*ratio));if(w>maxw){w=maxw;h=std::max(1,(int)std::lround(w/ratio));}if(h>maxh){h=maxh;w=std::max(1,(int)std::lround(h*ratio));}}
            s.w=std::clamp(w,1,std::max(1,maxw));s.h=std::clamp(h,1,std::max(1,maxh));s.x=left?u.drag_x+u.drag_w-s.w:u.drag_x;s.y=top?u.drag_y+u.drag_h-s.h:u.drag_y;
        }u.dirty=true;
    }
    if(ImGui::IsMouseReleased(0))u.dragging=false;refresh_floating();
}
void draw_floating(ImDrawList *dl,ImVec2 origin,float z,ImVec2 a,ImVec2 b){
    auto &u=floating;auto &s=u.stage;auto color=u.valid?IM_COL32(239,193,94,255):IM_COL32(235,85,99,255);
    if(u.valid)draw_selection(u.preview.selection,dl,origin,z,a,b);dl->AddRect(origin+ImVec2(s.x*z,s.y*z),origin+ImVec2((s.x+s.w)*z,(s.y+s.h)*z),color,0,0,2);
    if(s.w*z>=24&&s.h*z>=24){const int x[]={s.x,s.x+s.w,s.x,s.x+s.w},y[]={s.y,s.y,s.y+s.h,s.y+s.h};for(int i=0;i<4;++i){auto p=origin+ImVec2(x[i]*z,y[i]*z);dl->AddRectFilled(p-ImVec2(3,3),p+ImVec2(3,3),color);}}
}
void floating_inspector(Tab &t){
    auto &u=floating;auto &s=u.stage;ImGui::Spacing();ImGui::SeparatorText("Selection preview");ImGui::TextWrapped("Drag to move; drag corners to resize. Artwork changes when you apply.");
    ImGui::BeginDisabled(!u.valid||u.dirty||u.dragging);bool apply=ImGui::Button("Apply",ImVec2(100,0));u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();bool cancel=ImGui::Button("Cancel",ImVec2(100,0));u.cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(s.clipboard){if(ImGui::Button("Keep as overlay (L)"))drop_paste_layer();layer_paste_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::TextWrapped("Overlay keeps index 0 transparent; it does not clear base pixels.");}
    if(!u.error.empty())ImGui::TextWrapped("%s",u.error.c_str());
    int xy[]={s.x,s.y};ImGui::TextUnformatted("Position X / Y");ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##floating-pos",xy)){s.x=std::clamp(xy[0],-8192,8192);s.y=std::clamp(xy[1],-8192,8192);u.dirty=true;}
    int w=s.w,h=s.h;double ratio=(double)s.w/s.h;
    ImGui::SetNextItemWidth(110);if(ImGui::InputInt("Width",&w,0,0)){s.w=std::clamp(w,1,8192);if(u.ratio)s.h=std::clamp((int)std::lround(s.w/ratio),1,8192);u.dirty=true;}
    ImGui::SetNextItemWidth(110);if(ImGui::InputInt("Height",&h,0,0)){s.h=std::clamp(h,1,8192);if(u.ratio)s.w=std::clamp((int)std::lround(s.h*ratio),1,8192);u.dirty=true;}
    ImGui::Checkbox("Keep proportions",&u.ratio);ImGui::BeginDisabled(s.clipboard);u.dirty|=ImGui::Checkbox("Duplicate instead of move",&s.copy);ImGui::EndDisabled();u.copy_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(ImGui::Button("Flip H")){s.flip_h=!s.flip_h;u.dirty=true;}u.flip_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();if(ImGui::Button("Flip V")){s.flip_v=!s.flip_v;u.dirty=true;}
    if(ImGui::Button("Rotate 90")){s.rotation=(s.rotation+1)%4;std::swap(s.w,s.h);std::swap(s.flip_h,s.flip_v);u.dirty=true;}u.rotate_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::TextDisabled("Rotation: %d degrees",s.rotation*90);ImGui::TextWrapped("Nearest pixels retain palette indices.");if(ImGui::IsItemHovered())ImGui::SetTooltip("Transparent selected pixels clear the destination. Unselected holes leave it intact.");
    if(ImGui::Button("Reset transform")){s.x=s.source.x;s.y=s.source.y;s.w=s.source.w;s.h=s.source.h;s.rotation=0;s.flip_h=s.flip_v=false;u.dirty=true;}
    if(ImGui::Button("Fit selection to canvas")){double scale=std::min(1.,std::min((double)s.canvas_w/s.w,(double)s.canvas_h/s.h));s.w=std::max(1,(int)std::floor(s.w*scale));s.h=std::max(1,(int)std::floor(s.h*scale));s.x=std::clamp(s.x,0,s.canvas_w-s.w);s.y=std::clamp(s.y,0,s.canvas_h-s.h);u.dirty=true;}
    ImGui::TextWrapped("Enter applies / Escape cancels. Arrows move 1 pixel; Shift + arrows move 10. Anchors stay in place.");
    if(apply)apply_floating();if(cancel)cancel_floating();
}
