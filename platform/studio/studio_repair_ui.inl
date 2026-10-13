void start_repair(Tab &t,int x,int y){
    finish();RepairBrush brush;brush.kind=tool==Tool::Heal?RepairKind::Heal:RepairKind::Defringe;brush.radius=t.radius;brush.shape=t.brush_shape;brush.passes=t.heal_passes;brush.edge_width=t.edge_width;
    if(!begin_repair_stroke(*t.art,t.selected,x,y,brush,t.selection,repair,error))return;
    stroke=true;last_x=x;last_y=y;if(t.art->repair_stroke(repair,x,y,false,error)<0){stroke=false;repair={};return;}status=tool==Tool::Heal?"Healing the brushed area from its rim. Escape cancels.":"Cleaning brushed edges. Escape cancels.";
}
void repair_controls(Tab &t){
    paint_brush_controls(t);
    if(tool==Tool::Heal){ImGui::SetNextItemWidth(-1);ImGui::SliderInt("##heal-passes",&t.heal_passes,1,32,"Fill depth: %d px");ImGui::TextWrapped("Drag over a blemish to replace it with surrounding colors. More fill depth repairs wider areas. Pixels without a usable rim stay unchanged.");}
    else{ImGui::SetNextItemWidth(-1);ImGui::SliderInt("##edge-width",&t.edge_width,1,4,"Edge width: %d px");
        if(ImGui::Button(t.selection.valid?"Clean selected edges":"Clean all edges")){finish();RepairBrush brush;brush.kind=RepairKind::Defringe;brush.edge_width=t.edge_width;RepairStroke edit;if(begin_repair_stroke(*t.art,t.selected,0,0,brush,t.selection,edit,error)){int n=t.art->repair_stroke(edit,0,0,true,error);t.art->commit();if(n>=0){status="Cleaned "+std::to_string(n)+" edge pixels.";if(edit.unresolved)status+=" "+std::to_string(edit.unresolved)+" stayed unchanged without usable interior colors.";}}}
        tools_ui.edges_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
        ImGui::TextWrapped("Drag over color spill at the sprite outline. Clean colors flow from the interior into this edge band; thin features stay unchanged if no interior is available.");
    }
    ImGui::TextWrapped("Uses existing palette colors. Transparency and sprite outlines stay intact. Selections limit sampling and painting.");
}
