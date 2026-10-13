void start_filter(Tab &t,int x,int y){
    finish();FilterBrush brush;brush.kind=tool==Tool::Blur?FilterKind::Blur:FilterKind::Smudge;brush.radius=t.radius;brush.shape=t.brush_shape;brush.strength=tool==Tool::Blur?t.blur_strength:t.smudge_strength;
    if(!begin_filter_stroke(*t.art,t.selected,x,y,brush,t.selection,filter,error))return;
    stroke=true;last_x=x;last_y=y;if(t.art->filter_stroke(filter,x,y,error)<0){stroke=false;filter={};return;}status=tool==Tool::Blur?"Blurring selected colors. Escape cancels this stroke.":"Drag to carry selected colors. Escape cancels this stroke.";
}
void filter_controls(Tab &t){
    paint_brush_controls(t);int &strength=tool==Tool::Blur?t.blur_strength:t.smudge_strength;ImGui::SetNextItemWidth(-1);ImGui::SliderInt("##filter-strength",&strength,1,100,"Strength: %d%%");
    ImGui::TextWrapped(tool==Tool::Blur?"Drag to soften neighboring colors. Each stroke applies one pass; another stroke builds up the effect.":"Drag to pull paint from the previous brush position. Stronger strokes carry more color.");
    ImGui::TextWrapped("Uses existing palette colors. Transparent pixels and sprite outlines stay intact. Selections limit both sampling and painting.");
}
