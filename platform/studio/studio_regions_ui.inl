void clear_region_preview(){if(regions.preview)SDL_DestroyTexture(regions.preview);regions.preview=nullptr;}
void region_name(){auto &u=regions;if(u.selected>=0&&u.selected<(int)u.requests.size())snprintf(u.name,sizeof u.name,"%s",u.requests[u.selected].name.c_str());}
void request_regions(bool body){
    auto t=tab();if(!t)return;finish();synchronize(*t);paste_mode=false;clear_region_preview();regions=RegionUi{};auto &u=regions;
    u.doc=t->uid;u.source=t->selected_id;auto im=t->art->sprite(t->selected);
    if(body)u.requests=propose_body_regions(*im,u.fallback);
    else u.requests.push_back({"Selection",std::string(im->n_s).substr(0,10)+"_PART",t->selection,true});
    if(u.requests.empty()){error="This sprite is too small or empty for suggested body parts. Select a region to extract it manually.";return;}
    u.open=true;region_name();rebuild_regions();
}
void rebuild_regions(){
    auto &u=regions;auto t=find(u.doc);u.valid=false;u.error.clear();u.dirty=false;
    if(!t){u.error="The source library is closed.";return;}int index=t->art->sprite_index(u.source);
    u.valid=build_region_plan(*t->art,index,u.requests,u.trim,u.cut,u.plan,u.error);clear_region_preview();if(!u.valid)return;
    auto im=t->art->sprite(index);auto pal=t->art->palette(im->palnum);int w=im->w,h=im->h,stride=(w+3)&~3;
    std::vector<unsigned char> pixels((size_t)w*h);
    if(u.view==1){for(const auto &r:u.requests)if(r.enabled)for(int y=r.selection.y;y<r.selection.y+r.selection.h;++y)for(int x=r.selection.x;x<r.selection.x+r.selection.w;++x)if(r.selection.contains(x,y))pixels[y*w+x]=((unsigned char *)im->data_p)[y*stride+x];}
    else {for(int y=0;y<h;++y)memcpy(pixels.data()+(size_t)y*w,(unsigned char *)im->data_p+y*stride,w);if(u.cut)for(const auto &r:u.requests)if(r.enabled)for(int y=r.selection.y;y<r.selection.y+r.selection.h;++y)for(int x=r.selection.x;x<r.selection.x+r.selection.w;++x)if(r.selection.contains(x,y))pixels[y*w+x]=0;}
    std::vector<unsigned char> rgba((size_t)w*h*4);for(size_t i=0;i<pixels.size();++i){int c=pixels[i];auto p=rgba.data()+i*4;if(c<pal->numc)pal_word_to_rgb8((unsigned char *)pal->data_p+c*2,p,p+1,p+2);p[3]=c?255:0;}
    u.preview=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,w,h);if(u.preview){SDL_SetTextureBlendMode(u.preview,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(u.preview,SDL_ScaleModeNearest);SDL_UpdateTexture(u.preview,nullptr,rgba.data(),w*4);}
}
void region_canvas(Tab &t,int source,float height){
    auto &u=regions;auto im=t.art->sprite(source);ImGui::BeginChild("region-preview",ImVec2(0,height),ImGuiChildFlags_Border,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    const char *views[]={"Source and regions","Extracted pixels","Remaining source"};ImGui::SetNextItemWidth(195);if(ImGui::Combo("##region-view",&u.view,views,3))u.dirty=true;ImGui::SameLine();if(ImGui::Button("Fit")){u.zoom=0;u.pan={};}
    auto pos=ImGui::GetCursorScreenPos(),space=ImMax(ImGui::GetContentRegionAvail(),ImVec2(10,10));ImGui::InvisibleButton("##region-surface",space,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered=ImGui::IsItemHovered();auto &io=ImGui::GetIO();float fit=std::max(.1f,std::min((space.x-40)/im->w,(space.y-40)/im->h)),z=u.zoom>0?u.zoom:std::min(12.f,fit);
    if(hovered&&io.MouseWheel){u.zoom=std::clamp(z*(io.MouseWheel>0?1.25f:.8f),.1f,48.f);z=u.zoom;}if(hovered&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle))u.pan+=io.MouseDelta;
    auto origin=pos+(space-ImVec2(im->w*z,im->h*z))*.5f+u.pan;u.preview_origin=origin;u.preview_zoom=z;auto dl=ImGui::GetWindowDrawList();dl->PushClipRect(pos,pos+space,true);dl->AddRectFilled(pos,pos+space,IM_COL32(18,21,27,255));
    auto a=ImMax(pos,origin),b=ImMin(pos+space,origin+ImVec2(im->w*z,im->h*z));for(float y=a.y;y<b.y;y+=12)for(float x=a.x;x<b.x;x+=12)dl->AddRectFilled(ImVec2(x,y),ImMin(ImVec2(x+12,y+12),b),(((int)((x-origin.x)/12)+(int)((y-origin.y)/12))&1)?IM_COL32(41,44,51,255):IM_COL32(33,36,43,255));
    SDL_Texture *tex=u.view==0?texture(t,source):(u.valid?u.preview:nullptr);if(tex)dl->AddImage((ImTextureID)(intptr_t)tex,origin,origin+ImVec2(im->w*z,im->h*z));
    if(u.view==0){
        for(int i=0;i<(int)u.requests.size();++i)if((u.requests[i].enabled||i==u.selected)&&u.requests[i].selection.valid){auto &s=u.requests[i].selection;auto color=i==u.selected?IM_COL32(239,193,94,255):IM_COL32(228,96,108,190);dl->AddRect(origin+ImVec2(s.x*z,s.y*z),origin+ImVec2((s.x+s.w)*z,(s.y+s.h)*z),color,0,0,i==u.selected?2.f:1.f);dl->AddText(origin+ImVec2(s.x*z+3,s.y*z+3),color,u.requests[i].label.c_str());if(i==u.selected&&!s.mask.empty())draw_selection(s,dl,origin,z,a,b);}
        auto &r=u.requests[u.selected];auto &s=r.selection;
        if(s.valid&&s.mask.empty()&&s.fits(im->w,im->h)){
            ImVec2 corners[]={origin+ImVec2(s.x*z,s.y*z),origin+ImVec2((s.x+s.w)*z,s.y*z),origin+ImVec2(s.x*z,(s.y+s.h)*z),origin+ImVec2((s.x+s.w)*z,(s.y+s.h)*z)};int handle=-1;
            for(int i=0;i<4;++i){dl->AddRectFilled(corners[i]-ImVec2(3,3),corners[i]+ImVec2(3,3),IM_COL32(239,193,94,255));if(std::abs(io.MousePos.x-corners[i].x)<9&&std::abs(io.MousePos.y-corners[i].y)<9)handle=i;}
            bool inside=io.MousePos.x>=corners[0].x&&io.MousePos.y>=corners[0].y&&io.MousePos.x<corners[3].x&&io.MousePos.y<corners[3].y;
            if(hovered&&(inside||handle>=0)){ImGui::SetMouseCursor(handle<0?ImGuiMouseCursor_ResizeAll:handle==0||handle==3?ImGuiMouseCursor_ResizeNWSE:ImGuiMouseCursor_ResizeNESW);if(ImGui::IsMouseClicked(0)){u.dragging=true;u.drag_kind=handle;u.drag_start=s;u.drag_mouse=io.MousePos;}}
            if(u.dragging&&ImGui::IsMouseDown(0)){int dx=(int)std::lround((io.MousePos.x-u.drag_mouse.x)/z),dy=(int)std::lround((io.MousePos.y-u.drag_mouse.y)/z);auto old=u.drag_start;
                if(u.drag_kind<0)s=rectangle_selection(im->w,im->h,std::clamp(old.x+dx,0,(int)im->w-old.w),std::clamp(old.y+dy,0,(int)im->h-old.h),old.w,old.h);
                else{bool left=u.drag_kind==0||u.drag_kind==2,top=u.drag_kind<2;int x0=old.x,y0=old.y,x1=old.x+old.w,y1=old.y+old.h;if(left)x0=std::clamp(old.x+dx,0,x1-1);else x1=std::clamp(x1+dx,x0+1,(int)im->w);if(top)y0=std::clamp(old.y+dy,0,y1-1);else y1=std::clamp(y1+dy,y0+1,(int)im->h);s=rectangle_selection(im->w,im->h,x0,y0,x1-x0,y1-y0);}u.dirty=true;}
        }
    }
    if(ImGui::IsMouseReleased(0))u.dragging=false;dl->PopClipRect();ImGui::EndChild();
}
void region_dialog(){
    auto &u=regions;if(!u.open){if(ImGui::BeginPopupModal("Extract sprite regions",nullptr,panel_flags|ImGuiWindowFlags_NoResize)){ImGui::CloseCurrentPopup();ImGui::EndPopup();}clear_region_preview();return;}auto t=find(u.doc);int source=t?t->art->sprite_index(u.source):-1;if(source<0){u.open=false;return;}auto im=t->art->sprite(source);
    if(u.valid&&u.plan.generation!=t->art->generation()){u.valid=false;u.error="The source library changed. Rebuild the extraction preview.";}
    auto display=ImGui::GetIO().DisplaySize;ImGui::OpenPopup("Extract sprite regions");ImGui::SetNextWindowSize(ImVec2(std::min(1060.f,display.x-40),std::min(760.f,display.y-40)),ImGuiCond_Always);ImGui::SetNextWindowPos(display*.5f,ImGuiCond_Always,ImVec2(.5f,.5f));if(!ImGui::BeginPopupModal("Extract sprite regions",nullptr,panel_flags|ImGuiWindowFlags_NoResize))return;
    ImGui::TextDisabled("%s / %s",t->name().c_str(),im->n_s);float height=std::max(200.f,ImGui::GetContentRegionAvail().y-130);
    ImGui::BeginChild("region-settings",ImVec2(270,height),ImGuiChildFlags_Border);ImGui::SeparatorText("Regions");
    for(int i=0;i<(int)u.requests.size();++i){ImGui::PushID(i);auto &r=u.requests[i];u.dirty|=ImGui::Checkbox("##enabled",&r.enabled);ImGui::SameLine();if(ImGui::Selectable(r.label.c_str(),u.selected==i)){u.selected=i;region_name();u.dragging=false;}ImGui::PopID();}
    auto &r=u.requests[u.selected];ImGui::Spacing();ImGui::TextUnformatted("Child name");ImGui::SetNextItemWidth(-1);if(ImGui::InputText("##region-name",u.name,sizeof u.name)){r.name=u.name;u.dirty=true;}
    if(r.selection.mask.empty()){int xywh[]={r.selection.x,r.selection.y,r.selection.w,r.selection.h};ImGui::TextUnformatted("X / Y / Width / Height");ImGui::SetNextItemWidth(-1);if(ImGui::InputInt4("##region-bounds",xywh)){r.selection=rectangle_selection(im->w,im->h,std::clamp(xywh[0],0,(int)im->w-1),std::clamp(xywh[1],0,(int)im->h-1),std::clamp(xywh[2],1,(int)im->w),std::clamp(xywh[3],1,(int)im->h));u.dirty=true;}ImGui::TextWrapped("Drag this box to move it; drag corners to resize.");}
    else ImGui::TextWrapped("The original selection shape is retained. Holes become transparent in the child.");
    ImGui::SeparatorText("Extraction");u.dirty|=ImGui::Checkbox("Trim transparent borders",&u.trim);u.dirty|=ImGui::Checkbox("Cut from source",&u.cut);u.cut_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::TextWrapped(u.cut?"Selected pixels are cleared after all children are created.":"The source sprite keeps its pixels.");
    ImGui::TextWrapped("Children share the source palette and alternate palettes. Their primary anchors are rebased to keep placement. Secondary anchors, point tables and frame metadata start empty.");
    ImGui::TextWrapped("Embedded animations keep referencing the source sprite.");
    if(u.fallback)ImGui::TextColored(accent_surface(1),"Proportional body guess — adjust boxes.");
    if(u.valid){ImGui::SeparatorText("Generated sprites");for(const auto &child:u.plan.children){ImGui::Text("%s  %d x %d",child.name.c_str(),child.w,child.h);if(ImGui::IsItemHovered())ImGui::SetTooltip("Source origin %d, %d / anchor %d, %d",child.x,child.y,child.anix,child.aniy);}}
    if(ImGui::Button("Rebuild preview",ImVec2(-1,0)))u.dirty=true;ImGui::EndChild();
    if(u.dirty&&!u.dragging)rebuild_regions();ImGui::SameLine();region_canvas(*t,source,height);
    if(u.valid){ImGui::Text("%d child sprites / %d visible source pixels",(int)u.plan.children.size(),u.plan.opaque_pixels);if(u.plan.overlap_pixels)ImGui::TextColored(accent_surface(1),"%d overlapping visible pixels appear in multiple children.",u.plan.overlap_pixels);else ImGui::TextDisabled("Appending sprites changes LOAD2 packing. Review packing checks before a game build.");}
    else ImGui::TextWrapped("%s",u.error.c_str());
    bool apply=false,cancel=false;ImGui::BeginDisabled(!u.valid||u.dirty||u.dragging);apply=ImGui::Button(u.cut?"Cut into sprites":"Copy into sprites",ImVec2(160,0));u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();cancel=ImGui::Button("Cancel",ImVec2(100,0));u.cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(!ImGui::GetIO().WantTextInput&&ImGui::IsKeyPressed(ImGuiKey_Escape)){if(u.dragging){r.selection=u.drag_start;u.dragging=false;u.dirty=true;}else cancel=true;}
    if(apply){int first=t->art->extract_regions(u.plan,u.error);if(first>=0){u.open=false;u.dragging=false;t->selected=first;t->selected_id=t->art->sprite_id(first);t->selection={};t->reveal_selection=true;status="Created "+std::to_string(u.plan.children.size())+" aligned child sprites. Undo restores the entire extraction.";ImGui::CloseCurrentPopup();}else u.valid=false;}
    if(cancel){u.open=false;u.dragging=false;ImGui::CloseCurrentPopup();}ImGui::EndPopup();
}
