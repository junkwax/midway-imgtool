// App methods: selection is view state; only pixel commands enter IMG history.
SelectionOp selection_op(const Tab &t)const{
    const auto &io=ImGui::GetIO();
    if(io.KeyShift&&io.KeyAlt)return SelectionOp::Intersect;
    if(io.KeyAlt)return SelectionOp::Subtract;
    if(io.KeyShift)return SelectionOp::Add;
    return (SelectionOp)std::clamp(t.selection_operation,0,3);
}
void append_lasso(int width,int height,float x,float y){
    SelectionPoint p{std::clamp(x,0.f,(float)width),std::clamp(y,0.f,(float)height)};
    if(lasso_points.size()>=4096){status="Selection outline limit reached. Release to close the current path.";return;}
    if(lasso_points.empty()||std::abs(p.x-lasso_points.back().x)+std::abs(p.y-lasso_points.back().y)>=.5f)lasso_points.push_back(p);
}
void nudge_selection(Tab &t,int dx,int dy){
    if(t.alignment.enabled||t.embedded.enabled||t.flip||!t.selection.valid||paste_mode)return;
    finish();auto im=t.art->sprite(t.selected);
    if(t.art->move_selection(t.selected,t.selection,dx,dy,error))t.selection=translate_selection(t.selection,dx,dy,im->w,im->h);
}
void selection_controls(Tab &t){
    if(tool==Tool::Select||tool==Tool::Wand||tool==Tool::Lasso){
        const char *operations[]={"Replace","Add","Subtract","Intersect"};ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##selection-operation",&t.selection_operation,operations,4);
        ImGui::TextDisabled("Shift adds / Alt subtracts");
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Hold Shift + Alt to intersect. Escape restores the selection before an unfinished drag.");
        if(tool==Tool::Wand){ImGui::SetNextItemWidth(-1);ImGui::SliderInt("##wand-tolerance",&t.wand_tolerance,0,255,"Tolerance: %d");ImGui::Checkbox("Connected pixels only",&t.wand_contiguous);ImGui::TextWrapped("Matches palette colors. Transparency stays separate from opaque black.");}
    }
    if(t.selection.valid){
        ImGui::TextDisabled("Selected bounds: %d x %d",t.selection.w,t.selection.h);
        ImGui::TextWrapped("Paint, erase and fill stay inside the selection.");
        button("Copy",Action::Copy);ImGui::SameLine();button("Cut",Action::Cut);ImGui::SameLine();button("Clear",Action::Clear);
        button("Extract into sprite...",Action::ExtractSelection);
        button("Move / transform...",Action::TransformSelection);
        ImGui::BeginDisabled(!enabled(Action::ManagePalettes)||t.flip||paste_mode||t.alignment.enabled||t.embedded.enabled);if(ImGui::Button("Isolate selection colors..."))begin_selection_isolation(t);ImGui::EndDisabled();
        if(ImGui::CollapsingHeader("Selection actions")){
            ImGui::BeginDisabled(!enabled(Action::ManagePalettes)||t.flip||paste_mode||t.alignment.enabled||t.embedded.enabled||t.color<=0);if(ImGui::Button("Remap Similar Regions..."))begin_region_remap(t);ImGui::EndDisabled();
            button("Deselect",Action::Deselect);ImGui::SameLine();button("Invert",Action::InvertSelection);
            ImGui::BeginDisabled(t.flip||paste_mode);
            const char *labels[]={"Left","Right","Up","Down"};const int dx[]={-1,1,0,0},dy[]={0,0,-1,1};
            for(int i=0;i<4;++i){if(i)ImGui::SameLine();if(ImGui::Button(labels[i]))nudge_selection(t,dx[i],dy[i]);}ImGui::EndDisabled();
            ImGui::TextWrapped("Alt + arrows moves pixels by 1; Shift + Alt moves by 10. Anchors stay in place.");
        }
    }else if(tool==Tool::Select||tool==Tool::Wand||tool==Tool::Lasso){button("Select all",Action::SelectAll);ImGui::SameLine();button("Artwork",Action::SelectOpaque);}
}
void draw_selection(const PixelSelection &s,ImDrawList *dl,ImVec2 origin,float z,ImVec2 clip_a,ImVec2 clip_b){
    const ImU32 color=IM_COL32(236,218,162,255);
    if(s.mask.empty()){dl->AddRect(origin+ImVec2(s.x*z,s.y*z),origin+ImVec2((s.x+s.w)*z,(s.y+s.h)*z),color,0,0,1.5f);return;}
    int x0=std::clamp((int)std::floor((clip_a.x-origin.x)/z),s.x,s.x+s.w),x1=std::clamp((int)std::ceil((clip_b.x-origin.x)/z),s.x,s.x+s.w);
    int y0=std::clamp((int)std::floor((clip_a.y-origin.y)/z),s.y,s.y+s.h),y1=std::clamp((int)std::ceil((clip_b.y-origin.y)/z),s.y,s.y+s.h);
    int lines=0;
    // Bound rendering work for unusually noisy, large masks. Zoom reveals details.
    for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x)if(s.contains(x,y)){
        auto a=origin+ImVec2(x*z,y*z),b=a+ImVec2(z,z);
        if(!s.contains(x-1,y)){dl->AddLine(a,ImVec2(a.x,b.y),color);++lines;}
        if(!s.contains(x+1,y)){dl->AddLine(ImVec2(b.x,a.y),b,color);++lines;}
        if(!s.contains(x,y-1)){dl->AddLine(a,ImVec2(b.x,a.y),color);++lines;}
        if(!s.contains(x,y+1)){dl->AddLine(ImVec2(a.x,b.y),b,color);++lines;}
        if(lines>20000){dl->AddRect(origin+ImVec2(s.x*z,s.y*z),origin+ImVec2((s.x+s.w)*z,(s.y+s.h)*z),color);return;}
    }
}
