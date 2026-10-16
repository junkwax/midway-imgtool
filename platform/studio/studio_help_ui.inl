std::string keyboard_reference()const{
    std::string text="Shortcuts pause while entering text or using a modal review. Palette and source dialogs own their local keys.\n\nFILE, EDIT & LIBRARY COMMANDS\n";
    for(const auto &k:shortcut_bindings){text+=k.label;text+="  —  ";text+=k.description;if(k.artwork)text+=" (Artwork)";text+="\n";}
    text+="\nARTWORK TOOLS\n";
    for(const auto &k:tool_bindings){text+=k.label;text+="  —  ";text+=tool_specs[(int)k.tool].name;text+="\n";}
    text+="\nCONTEXT CONTROLS\n[ / ] — Brush smaller / larger; otherwise palette assignment review\n* — Palette merge review (typed character or keypad multiply)\nUp / Down — Previous / next sprite; animation workspaces step frames\nPageUp / PageDown — Browse ten sprites\nLeft / Right — Previous / next preview frame\nCtrl+Tab / Ctrl+Shift+Tab — Next / previous IMG library\nTab — Switch Artwork / Animation & alignment\nK — Play / pause (opens alignment when in Artwork)\nSpace — Tap to mark in Artwork; play / pause in animation workspaces\nMouse wheel — Zoom; middle drag or Space+left drag — Pan (Artwork)\nAlt+arrows — Nudge selected pixels; add Shift for 10 pixels\nShift / Alt / Shift+Alt — Add / subtract / intersect selection\nRight-click — Pick color; Alt-click with Clone — Set source\n\nFLOATING SELECTION / PASTE TRANSFORM\nCtrl+T — Start paste transform; press again to apply\nEnter / Ctrl+E — Apply preview\nEscape / Ctrl+Z — Cancel preview\nH / V — Flip preview horizontally / vertically (also while placing a paste)\nCtrl+J — Stamp duplicate and keep a fresh floating copy\nL — Keep the pixel paste as an editable sprite overlay\nShift+V — Move; choose Overlay / Base selection in Sprite inspector\nOverlay target — Drag and release; Shift locks axis; Escape / Ctrl+Z cancels\nOverlay arrows / Shift+arrows — Nudge layer 1 / 10 pixels\nArrows / Shift+arrows — Move preview 1 / 10 pixels\n\nVIDEO CAPTURE\nLeft / Right — Scrub frames (Shift = 10)\nHome / End — First / last frame\nI / O / P — Set In / Out / toggle picked frame\nSpace — Play / pause\n\nPALETTE MANAGER (no pending preview)\nCtrl+C / Ctrl+V — Copy marked palettes (else selected) / append all in one Undo\nCopy right-click — Copy selected only; Clear beside Paste releases palette clipboard\nSwatch right-click / Copy-paste colors — Copy selected slots; review original indices / append / one opaque slot\nUp / Down — Previous / next palette\nSpace / Shift+M / M — Toggle mark / mark all / clear marks\nShift+R — Focus palette name; Enter applies rename\nDelete — Delete unused palette only\nCtrl+Z / Ctrl+Y / Ctrl+Shift+Z — Palette Undo / Redo\n* / Keypad* — Merge marked sources into selected palette; no marks uses single source\n\nHELP\nH / F1 — Open Help, including from the opening screen\nEscape / Done — Close Help\n\nSee Classic shortcuts & migration for commands whose original behavior needs a remaining Studio feature.\n";
    return text;
}
void help_dialog(){
    if(help)ImGui::OpenPopup("Using Imgtool Studio");auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(1050.f,display.x-40),std::min(760.f,display.y-40)),ImGuiCond_Always);
    ImGui::SetNextWindowPos(display*.5f,ImGuiCond_Always,ImVec2(.5f,.5f));
    if(!ImGui::BeginPopupModal("Using Imgtool Studio",nullptr,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoSavedSettings))return;
    ImGui::TextColored(accent_surface(1),"IMGTOOL STUDIO");ImGui::SameLine();ImGui::TextDisabled("Guide & keyboard reference");
    ImGui::SetNextItemWidth(-1);if(ImGui::InputTextWithHint("##help-search","Search tools, shortcuts, formats and workflows...",help_search.InputBuf,sizeof help_search.InputBuf))help_search.Build();help_search_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Search topic names and full contents, including shortcut keys.");
    const auto keys=keyboard_reference();const int count=(int)(sizeof(help_topics)/sizeof(help_topics[0]));
    auto matches=[&](int i){return help_search.PassFilter(help_topics[i].title)||help_search.PassFilter(i==1?keys.c_str():help_topics[i].text);};
    if(help_topic<0||help_topic>=count||!matches(help_topic)){help_topic=-1;for(int i=0;i<count;++i)if(matches(i)){help_topic=i;break;}}
    float height=std::max(180.f,ImGui::GetContentRegionAvail().y-ImGui::GetFrameHeightWithSpacing()-10);
    ImGui::BeginChild("help-topics",ImVec2(215,height),ImGuiChildFlags_Border);
    for(int i=0;i<count;++i)if(matches(i)){if(ImGui::Selectable(help_topics[i].title,help_topic==i))help_topic=i;help_topic_points[i]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;}
    if(help_topic<0)ImGui::TextWrapped("No matching topics.");ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild(help_topic>=0?help_topics[help_topic].title:"help-empty",ImVec2(0,height),ImGuiChildFlags_Border);
    if(help_topic>=0){ImGui::SeparatorText(help_topics[help_topic].title);ImGui::PushTextWrapPos(0);ImGui::TextUnformatted(help_topic==1?keys.c_str():help_topics[help_topic].text);ImGui::PopTextWrapPos();}
    else ImGui::TextWrapped("Try a tool name, key combination, file format or workflow.");ImGui::EndChild();
    if(ImGui::Button("Done",ImVec2(100,0))||ImGui::IsKeyPressed(ImGuiKey_Escape)){help=false;ImGui::CloseCurrentPopup();}help_done_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::SameLine();ImGui::TextDisabled("IMG versions and record layouts stay unchanged.");ImGui::EndPopup();
}
void diagnostics_dialog(){
    if(!diagnostics)return;ImGui::SetNextWindowSize(ImVec2(520,300),ImGuiCond_FirstUseEver);
    if(ImGui::Begin("Studio diagnostics (F9)",&diagnostics)){
        SDL_RendererInfo info{};SDL_GetRendererInfo(renderer,&info);ImGui::Text("Renderer: %s",info.name?info.name:"unknown");
        ImGui::Text("Open IMG libraries: %zu",tabs.size());if(auto t=tab()){ImGui::Text("IMG version: 0x%03X",t->art->disk_version());ImGui::Text("Sprites: %d / Palettes: %d",t->art->count(),t->art->palette_count());ImGui::Text("Unsaved changes: %s",t->art->dirty()?"yes":"no");ImGui::Text("Undo: %s",t->art->can_undo()?t->art->undo_label():"none");}
        ImGui::TextWrapped("Settings folder: %s",prefs.c_str());ImGui::TextDisabled("Read-only diagnostics; F9 closes this window.");
    }ImGui::End();
}
