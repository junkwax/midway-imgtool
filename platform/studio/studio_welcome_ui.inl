// Home screen uses existing commands; recent rows retain full-path identity.
std::string home_elide(const std::string &text,float width,bool tail=false){
    if(ImGui::CalcTextSize(text.c_str()).x<=width)return text;std::string out=text;
    while(!out.empty()&&ImGui::CalcTextSize((tail?"..."+out:out+"...").c_str()).x>width){if(tail){size_t at=1;while(at<out.size()&&((unsigned char)out[at]&0xc0)==0x80)++at;out.erase(0,at);}else{size_t at=out.size()-1;while(at&&((unsigned char)out[at]&0xc0)==0x80)--at;out.resize(at);}}return tail?"..."+out:out+"...";
}
void refresh_home(){
    home.paths=recent;home.rows.clear();for(auto &path:recent){WelcomeUi::Recent row;row.path=path;auto p=fs::u8path(path);row.name=p.filename().u8string();row.directory=p.parent_path().u8string();std::error_code ec;row.available=fs::is_regular_file(p,ec);home.rows.push_back(std::move(row));}
}
bool home_card(const char *id,const char *label,const char *description,ImVec2 size,bool sample,bool horizontal=false){
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,8);ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,1);
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.085f,.10f,.125f,1));ImGui::PushStyleColor(ImGuiCol_ButtonHovered,accent_surface(.16f));
    bool clicked=ImGui::Button(id,size);auto a=ImGui::GetItemRectMin();auto dl=ImGui::GetWindowDrawList();
    auto icon=a+ImVec2(24,horizontal?36.f:24.f);auto red=ImGui::GetColorU32(ImGuiCol_CheckMark);
    if(sample)dl->AddTriangleFilled(icon-ImVec2(4,8),icon+ImVec2(8,0),icon+ImVec2(-4,8),red);
    else {dl->AddLine(icon-ImVec2(8,0),icon+ImVec2(8,0),red,1.8f);dl->AddLine(icon-ImVec2(0,8),icon+ImVec2(0,8),red,1.8f);}
    dl->AddText(a+(horizontal?ImVec2(48,17):ImVec2(16,50)),ImGui::GetColorU32(ImGuiCol_Text),label);auto detail=home_elide(description,size.x-(horizontal?64:32));dl->AddText(a+(horizontal?ImVec2(48,44):ImVec2(16,76)),ImGui::GetColorU32(ImGuiCol_TextDisabled),detail.c_str());
    ImGui::PopStyleColor(2);ImGui::PopStyleVar(2);return clicked;
}
void welcome(){
    int frame=ImGui::GetFrameCount();if(home.paths!=recent||home.last_frame!=frame-1)refresh_home();home.last_frame=frame;
    auto space=ImGui::GetContentRegionAvail();float width=std::min(1040.f,space.x-64),gap=36,left=(width-gap)*.44f,right=width-gap-left;
    int visible=std::min(4,(int)home.rows.size());float list_height=home.rows.empty()?120.f:visible*71.f+4;
    float recent_height=80+list_height+18,group_height=std::max(410.f,recent_height+102);
    auto start=ImGui::GetCursorPos();ImGui::SetCursorPos(start+ImVec2((space.x-width)*.5f,std::max(24.f,(space.y-group_height)*.40f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,0));
    ImGui::BeginChild("home-layout",ImVec2(width,group_height),ImGuiChildFlags_None,panel_flags|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    bool browse=false,create=false,sample=false,project=false;std::string opening,removing;
    ImGui::BeginChild("home-start",ImVec2(left,group_height),ImGuiChildFlags_None,panel_flags|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    auto brand=ImGui::GetCursorScreenPos();auto dl=ImGui::GetWindowDrawList();auto red=ImGui::GetColorU32(ImGuiCol_CheckMark);
    dl->AddQuadFilled(brand+ImVec2(6,2),brand+ImVec2(18,2),brand+ImVec2(14,11),brand+ImVec2(2,11),IM_COL32(137,148,163,255));
    dl->AddQuadFilled(brand+ImVec2(2,13),brand+ImVec2(14,13),brand+ImVec2(10,22),brand+ImVec2(0,22),red);
    ImGui::SetCursorPos(ImVec2(28,3));ImGui::TextColored(accent_surface(1),"IMGTOOL STUDIO");
    ImGui::SetCursorPos(ImVec2(0,44));ImGui::PushFont(home_title);ImGui::TextUnformatted("Your sprite workspace.");ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(0,99));ImGui::PushTextWrapPos(left);ImGui::TextWrapped("Open an IMG library to edit sprites and palettes,\nand check how your animation frames line up.");ImGui::PopTextWrapPos();
    ImGui::SetCursorPos(ImVec2(0,162));ImGui::PushStyleColor(ImGuiCol_Button,accent_surface(.52f));ImGui::PushStyleColor(ImGuiCol_ButtonHovered,accent_surface(.65f));
    browse=ImGui::Button("Open IMG library",ImVec2(left,50));home.open_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    auto key=ImGui::GetItemRectMax()-ImVec2(64,33);ImGui::GetWindowDrawList()->AddText(key,IM_COL32(211,166,174,255),"Ctrl+O");ImGui::PopStyleColor(2);
    ImGui::SetCursorPos(ImVec2(0,232));float card=(left-14)*.5f;create=home_card("##home-new","New library","Start from scratch.",ImVec2(card,108),false);home.new_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SameLine(0,14);sample=home_card("##home-sample","Explore sample","Try the editor.",ImVec2(card,108),true);home.sample_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SetCursorPos(ImVec2(0,362));ImGui::PushTextWrapPos(left);ImGui::TextDisabled("Or drop an IMG file anywhere in the window.");ImGui::PopTextWrapPos();
    ImGui::EndChild();ImGui::SameLine(0,gap);
    ImGui::BeginChild("home-continue",ImVec2(right,group_height),ImGuiChildFlags_None,panel_flags|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.09f,.105f,.13f,1));ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(20,18));ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,10);
    ImGui::BeginChild("home-recents",ImVec2(right,recent_height),ImGuiChildFlags_Border,panel_flags|ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(home_section);ImGui::TextUnformatted("Recent libraries");ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(right-91,16));ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));if(ImGui::Button("Refresh"))refresh_home();home.refresh_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::PopStyleColor();
    ImGui::SetCursorPos(ImVec2(20,53));ImGui::TextDisabled("Pick up where you left off.");ImGui::SetCursorPos(ImVec2(12,80));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0,0,0,0));ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(8,0));
    ImGui::BeginChild("home-recent-list",ImVec2(right-24,list_height),ImGuiChildFlags_AlwaysUseWindowPadding,panel_flags);
    home.recent_points.clear();if(home.rows.empty()){
        ImGui::SetCursorPosY(24);ImGui::TextUnformatted("No libraries opened yet.");ImGui::PushTextWrapPos(ImGui::GetWindowWidth()-16);ImGui::TextDisabled("Open an IMG library and it will appear here next time.");ImGui::PopTextWrapPos();
    }
    for(size_t i=0;i<home.rows.size();++i){auto &row=home.rows[i];ImGui::PushID((int)i);auto a=ImGui::GetCursorScreenPos();float row_width=ImGui::GetContentRegionAvail().x;
        if(ImGui::Selectable("##recent-library",false,row.available?ImGuiSelectableFlags_None:ImGuiSelectableFlags_Disabled,ImVec2(row_width,64)))opening=row.path;
        home.recent_points.push_back((ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f);
        bool hovered=ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);if(hovered){ImGui::BeginTooltip();ImGui::TextUnformatted(row.path.c_str());if(!row.available)ImGui::TextDisabled("File unavailable. Right-click to remove this recent entry.");ImGui::EndTooltip();}
        if(hovered&&ImGui::IsMouseReleased(ImGuiMouseButton_Right))ImGui::OpenPopup("home-recent-actions");
        if(ImGui::BeginPopup("home-recent-actions")){if(ImGui::MenuItem("Remove from recent libraries"))removing=row.path;ImGui::EndPopup();}
        auto draw=ImGui::GetWindowDrawList();auto icon=a+ImVec2(4,12);draw->AddRectFilled(icon,icon+ImVec2(40,40),row.available?ImGui::GetColorU32(accent_surface(.15f)):IM_COL32(36,40,49,255),6);
        draw->AddText(icon+ImVec2(6,11),row.available?ImGui::GetColorU32(ImGuiCol_Text):ImGui::GetColorU32(ImGuiCol_TextDisabled),"IMG");
        float text_width=std::max(20.f,row_width-96);auto name=home_elide(row.name,text_width),folder=home_elide(row.directory,text_width,true);auto color=ImGui::GetColorU32(row.available?ImGuiCol_Text:ImGuiCol_TextDisabled);
        draw->AddText(a+ImVec2(57,10),color,name.c_str());draw->AddText(a+ImVec2(57,34),ImGui::GetColorU32(ImGuiCol_TextDisabled),folder.c_str());
        auto end=a+ImVec2(row_width-15,32);if(row.available){draw->AddLine(end-ImVec2(4,4),end,IM_COL32(145,153,168,255),1.4f);draw->AddLine(end,end+ImVec2(-4,4),IM_COL32(145,153,168,255),1.4f);}else draw->AddLine(end-ImVec2(5,0),end+ImVec2(3,0),IM_COL32(145,153,168,255),1.4f);
        ImGui::PopID();
    }
    ImGui::EndChild();ImGui::PopStyleVar();ImGui::PopStyleColor();ImGui::EndChild();ImGui::PopStyleVar(2);ImGui::PopStyleColor();
    ImGui::SetCursorPosY(recent_height+18);project=home_card("##home-project","Open a Studio project","Continue a saved animation or composition.",ImVec2(right,80),true,true);home.project_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::EndChild();ImGui::EndChild();ImGui::PopStyleColor();
    if(!removing.empty()){recent.erase(std::remove(recent.begin(),recent.end(),removing),recent.end());std::ofstream f(fs::u8path(prefs)/"recent.txt");for(auto &path:recent)f<<path<<'\n';refresh_home();}
    if(!opening.empty())open(opening);else if(browse)action(Action::Open);else if(create)action(Action::NewLibrary);else if(sample)action(Action::Demo);else if(project)action(Action::OpenProject);
}
