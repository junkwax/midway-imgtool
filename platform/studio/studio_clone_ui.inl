void validate_clone_source(Tab &t){
    auto im=t.art->sprite(t.selected);if(t.clone_sprite&&(t.clone_sprite!=t.art->sprite_id(t.selected)||t.clone_w!=im->w||t.clone_h!=im->h)){t.clone_sprite=0;t.clone_offset=false;}
}
void start_clone(Tab &t,int x,int y,bool alt){
    finish();auto im=t.art->sprite(t.selected);
    if(alt){t.clone_sprite=t.art->sprite_id(t.selected);t.clone_w=im->w;t.clone_h=im->h;t.clone_x=x;t.clone_y=y;t.clone_offset=false;status="Clone source set. Drag over the area to repair.";return;}
    if(!t.clone_sprite){status="Alt-click the sprite to choose a clone source first.";return;}
    if(!t.clone_aligned||!t.clone_offset){t.clone_dx=t.clone_x-x;t.clone_dy=t.clone_y-y;t.clone_offset=true;}
    if(!begin_clone_stroke(*t.art,t.selected,t.clone_dx,t.clone_dy,paint_brush(t),t.clone_transparent,t.selection,clone,error))return;
    stroke=true;last_x=x;last_y=y;t.art->clone_stroke(clone,x,y,x,y,error);status="Cloning pixels. Escape cancels this stroke.";
}
void clone_controls(Tab &t){
    validate_clone_source(t);ImGui::TextWrapped("Alt-click a source, then drag to clone. Escape cancels the stroke.");
    if(t.clone_sprite){ImGui::TextDisabled("Source: %d, %d",t.clone_x,t.clone_y);if(ImGui::Button("Clear source")){finish();t.clone_sprite=0;t.clone_offset=false;}}
    else ImGui::TextColored(accent_surface(1),"Alt-click to choose a source");
    if(ImGui::Checkbox("Aligned strokes",&t.clone_aligned)){finish();t.clone_offset=false;}
    if(ImGui::IsItemHovered())ImGui::SetTooltip("On: keep the source offset between strokes. Off: each stroke starts at the chosen source. Alt-click resets the offset.");
    ImGui::Checkbox("Copy transparent pixels",&t.clone_transparent);if(ImGui::IsItemHovered())ImGui::SetTooltip("On: transparent source pixels erase the destination. Off: they leave it unchanged.");
    paint_brush_controls(t);
}
void draw_clone_source(Tab &t,ImDrawList *dl,ImVec2 origin,float z,int x,int y,bool hovered){
    if(tool!=Tool::Clone||t.flip||!t.clone_sprite)return;
    auto source=origin+ImVec2((t.clone_x+.5f)*z,(t.clone_y+.5f)*z);auto color=IM_COL32(239,193,94,255);dl->AddCircle(source,4,color,0,1.5f);
    int sx=t.clone_x,sy=t.clone_y;if(hovered&&!ImGui::GetIO().KeyAlt&&t.clone_offset&&(t.clone_aligned||stroke)){sx=x+t.clone_dx;sy=y+t.clone_dy;}
    auto mark=origin+ImVec2((sx+.5f)*z,(sy+.5f)*z);auto im=t.art->sprite(t.selected);bool inside=sx>=0&&sy>=0&&sx<im->w&&sy<im->h;if(!inside)color=IM_COL32(243,104,104,255);
    dl->AddLine(mark-ImVec2(9,0),mark+ImVec2(9,0),color,1.5f);dl->AddLine(mark-ImVec2(0,9),mark+ImVec2(0,9),color,1.5f);
    if(hovered){auto target=origin+ImVec2((x+.5f)*z,(y+.5f)*z);dl->AddLine(mark,target,IM_COL32(239,193,94,85));float r=(t.radius-.5f)*z;if(t.brush_shape==BrushShape::Square||t.radius==1)dl->AddRect(mark-ImVec2(r,r),mark+ImVec2(r,r),color);else dl->AddCircle(mark,r,color);}
}
