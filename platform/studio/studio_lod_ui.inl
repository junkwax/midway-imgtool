void clear_lod_preview(){if(lod.preview)SDL_DestroyTexture(lod.preview);lod.preview=nullptr;lod.preview_row=-1;}
Tab *lod_existing(const LodLibrary &row){for(auto &t:tabs)if(!t->art->path().empty()){std::error_code ec;if(fs::equivalent(fs::u8path(row.path),fs::u8path(t->art->path()),ec))return t.get();}return nullptr;}
void rebuild_lod(){clear_lod_preview();lod.valid=review_lod(lod.path,lod.override_folder,lod.img_directory,lod.review,lod.error);lod.settings_changed=false;lod.selected=0;}
void open_lod(const std::string &path){finish();paste_mode=false;clear_lod_preview();lod=LodUi{};lod.open=true;lod.path=path;
#ifdef _WIN32
    if(auto env=_wgetenv(L"IMGDIR")){auto value=fs::path(env).u8string();if(value.size()<sizeof lod.img_directory)snprintf(lod.img_directory,sizeof lod.img_directory,"%s",value.c_str());}
#else
    if(auto env=getenv("IMGDIR"))snprintf(lod.img_directory,sizeof lod.img_directory,"%s",env);
#endif
    for(auto &t:tabs)t->alignment.player.playing=t->embedded.playing=t->scene.playing=false;rebuild_lod();
}
void choose_lod(){
#ifdef _WIN32
    auto path=native_file(window,false,{},false,false,false,false,false,false,true);if(!path.empty())open_lod(path);
#else
    path_save=false;path_dialog=true;path_input[0]=0;
#endif
}
bool install_lod(){
    auto &u=lod;std::vector<size_t> included;if(!u.valid||u.settings_changed){u.error="Build the review before opening libraries.";return false;}if(!validate_lod(u.review,included,u.error))return false;
    // Construct all tabs and reserve their destination before transferring any ownership.
    std::vector<std::pair<size_t,std::unique_ptr<Tab>>> staged;std::vector<Tab *> reused;
    for(auto index:included){auto &row=u.review.libraries[index];if(auto t=lod_existing(row)){reused.push_back(t);continue;}auto t=std::make_unique<Tab>();t->uid=next_uid+staged.size();t->color=1;staged.push_back({index,std::move(t)});}
    tabs.reserve(tabs.size()+staged.size());
    for(auto &entry:staged){auto &t=*entry.second;auto &row=u.review.libraries[entry.first];t.art=std::move(row.artwork);t.ppp=row.ppp;t.selected_id=t.art->sprite_id(0);t.alignment.initialize(*t.art);sync_alignment(t);}
    uint64_t selected=staged.empty()?reused.front()->uid:staged.front().second->uid;next_uid+=staged.size();for(auto &entry:staged)tabs.push_back(std::move(entry.second));
    for(int i=0;i<(int)tabs.size();++i)if(tabs[i]->uid==selected)active=i;select_uid=selected;
    recent_add(u.review.path);status="LOD: opened "+std::to_string(staged.size())+" libraries; reused "+std::to_string(reused.size())+" open tabs with their current edits and PPP settings.";return true;
}
void lod_artwork_preview(float height){
    auto &u=lod;auto &row=u.review.libraries[u.selected];auto art=row.artwork.get();
    if(u.preview_row!=u.selected){clear_lod_preview();u.preview_row=u.selected;if(art){auto im=art->sprite(0);auto pixels=art->copy(0,0,0,im->w,im->h);std::vector<unsigned char> rgba((size_t)pixels.w*pixels.h*4);for(size_t i=0;i<pixels.pixels.size();++i){auto ci=pixels.pixels[i];auto q=rgba.data()+i*4;if(ci<pixels.colors.size()){auto word=pixels.colors[ci];unsigned char color[]={(unsigned char)word,(unsigned char)(word>>8)};pal_word_to_rgb8(color,q,q+1,q+2);}else{q[0]=255;q[2]=255;}q[3]=ci?255:0;}u.preview=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,pixels.w,pixels.h);if(u.preview){SDL_SetTextureBlendMode(u.preview,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(u.preview,SDL_ScaleModeNearest);SDL_UpdateTexture(u.preview,nullptr,rgba.data(),pixels.w*4);}}}
    ImGui::BeginChild("lod-artwork",ImVec2(0,height),ImGuiChildFlags_Border);
    if(art){auto im=art->sprite(0);ImGui::TextDisabled("%s / first sprite / %d x %d",im->n_s,im->w,im->h);auto space=ImGui::GetContentRegionAvail();auto pos=ImGui::GetCursorScreenPos();auto dl=ImGui::GetWindowDrawList();float scale=std::max(.001f,std::min(6.f,std::min(space.x/im->w,space.y/im->h)));auto size=ImVec2(im->w*scale,im->h*scale),a=pos+(space-size)*.5f,b=a+size;dl->PushClipRect(pos,pos+space,true);for(float y=pos.y;y<pos.y+space.y;y+=16)for(float x=pos.x;x<pos.x+space.x;x+=16)dl->AddRectFilled(ImVec2(x,y),ImVec2(x+16,y+16),((int)((x-pos.x)/16)+(int)((y-pos.y)/16))%2?IM_COL32(29,33,39,255):IM_COL32(24,28,34,255));if(u.preview)dl->AddImage((ImTextureID)(intptr_t)u.preview,a,b);dl->PopClipRect();}
    else ImGui::TextWrapped("%s",row.error.c_str());ImGui::EndChild();
}
void lod_dialog(){
    auto &u=lod;if(!u.open){if(ImGui::BeginPopupModal("Open LOD libraries",nullptr,panel_flags|ImGuiWindowFlags_NoResize)){ImGui::CloseCurrentPopup();ImGui::EndPopup();}clear_lod_preview();u.review={};u.valid=false;return;}
    auto display=ImGui::GetIO().DisplaySize;ImGui::OpenPopup("Open LOD libraries");ImGui::SetNextWindowSize(ImVec2(std::min(1100.f,display.x-40),std::min(780.f,display.y-40)),ImGuiCond_Always);ImGui::SetNextWindowPos(display*.5f,ImGuiCond_Always,ImVec2(.5f,.5f));if(!ImGui::BeginPopupModal("Open LOD libraries",&u.open,panel_flags|ImGuiWindowFlags_NoResize))return;
    ImGui::TextColored(accent_surface(1),"LIBRARY REVIEW");ImGui::SameLine();ImGui::TextUnformatted(fs::u8path(u.path).filename().u8string().c_str());float height=std::max(280.f,ImGui::GetContentRegionAvail().y-100);
    ImGui::BeginChild("lod-settings",ImVec2(260,height),ImGuiChildFlags_Border);ImGui::SeparatorText("Find libraries");ImGui::TextWrapped("Paths relative to this manifest are tried first, then the working folder and IMGDIR. Override forces every filename into one folder.");
    ImGui::Spacing();ImGui::TextUnformatted("Override folder");ImGui::SetNextItemWidth(-1);if(ImGui::InputTextWithHint("##lod-override","Optional folder...",u.override_folder,sizeof u.override_folder))u.settings_changed=true;
    ImGui::TextUnformatted("IMGDIR fallback");ImGui::SetNextItemWidth(-1);if(ImGui::InputTextWithHint("##lod-imgdir","Optional folder...",u.img_directory,sizeof u.img_directory))u.settings_changed=true;
    ImGui::Spacing();if(ImGui::Button("Rebuild review",ImVec2(-1,0)))rebuild_lod();u.rebuild_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;
    ImGui::SeparatorText("Opening behavior");ImGui::TextWrapped("Each included library opens in its own tab. Open tabs keep their edits and current PPP. Repeated paths open once.");ImGui::Spacing();ImGui::TextWrapped("PPP is carried forward per library; 0 means auto. Packing, image-selection and table directives do not run here.");
    if(u.valid&&!u.review.directives.empty()&&ImGui::TreeNode("Other LOAD2 directives")){ImGui::TextWrapped("These lines are recorded for review. No ROM, ASM or table files are written.");for(auto &d:u.review.directives)ImGui::TextWrapped("%d: %s",d.line,d.text.c_str());ImGui::TreePop();}ImGui::EndChild();ImGui::SameLine();ImGui::BeginChild("lod-review",ImVec2(0,height));
    int included=0,blocked=0,reused=0;if(u.valid){float rh=std::min(height*.42f,ImGui::GetFrameHeightWithSpacing()*((float)std::min(7,(int)u.review.libraries.size())+1)+12);if(ImGui::BeginTable("lod-libraries",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY|ImGuiTableFlags_BordersInnerH,ImVec2(0,rh))){ImGui::TableSetupColumn("Use",ImGuiTableColumnFlags_WidthFixed,28);ImGui::TableSetupColumn("Library",ImGuiTableColumnFlags_WidthStretch);ImGui::TableSetupColumn("Sprites",ImGuiTableColumnFlags_WidthFixed,52);ImGui::TableSetupColumn("PPP",ImGuiTableColumnFlags_WidthFixed,40);ImGui::TableSetupColumn("Result",ImGuiTableColumnFlags_WidthFixed,88);ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();ImGuiListClipper clip;clip.Begin((int)u.review.libraries.size());while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i){auto &r=u.review.libraries[i];auto existing=lod_existing(r);ImGui::PushID(i);ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::BeginDisabled(r.duplicate_of>=0);if(ImGui::Checkbox("##include",&r.enabled))u.error.clear();if(i<4)u.row_points[i]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::TableNextColumn();auto name=fs::u8path(r.path).filename().u8string();if(ImGui::Selectable(name.c_str(),u.selected==i))u.selected=i;if(i<4)u.name_points[i]=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::TableNextColumn();if(r.artwork)ImGui::Text("%d",r.artwork->count());else ImGui::TextDisabled("-");ImGui::TableNextColumn();if(r.ppp)ImGui::Text("%d",r.ppp);else ImGui::TextUnformatted("Auto");ImGui::TableNextColumn();if(r.duplicate_of>=0)ImGui::TextDisabled("Repeated");else if(!r.artwork)ImGui::TextColored(accent_surface(1),"Needs fix");else ImGui::TextUnformatted(existing?"Open tab":"Ready");ImGui::PopID();}ImGui::EndTable();}
        for(auto &r:u.review.libraries)if(r.enabled){++included;if(!r.artwork||r.duplicate_of>=0)++blocked;if(lod_existing(r))++reused;}
        auto &r=u.review.libraries[u.selected];ImGui::TextWrapped("Line %d: %s",r.line,r.source.c_str());ImGui::TextWrapped("%s: %s",r.resolution.c_str(),r.path.c_str());if(auto existing=lod_existing(r))ImGui::TextWrapped("Open tab kept as-is: %s / current PPP %d, manifest PPP %d",existing->art->dirty()?"unsaved edits":"saved artwork",existing->ppp,r.ppp);else if(r.artwork)ImGui::TextDisabled("%d palettes / PPP %d%s",r.artwork->palette_count(),r.ppp,r.explicit_ppp?" from manifest":" (LOAD2 default: auto)");
        lod_artwork_preview(std::max(60.f,ImGui::GetContentRegionAvail().y));
    }else ImGui::TextWrapped("%s",u.error.c_str());ImGui::EndChild();
    if(u.settings_changed)ImGui::TextColored(accent_surface(1),"Paths changed. Rebuild the review before opening.");else if(!u.error.empty())ImGui::TextWrapped("%s",u.error.c_str());else ImGui::Text("%d included / %d need attention / %d open tabs reused",included,blocked,reused);
    ImGui::TextDisabled("Exclude unresolved rows to open the rest. Your current libraries stay open.");ImGui::BeginDisabled(!u.valid||u.settings_changed||!included||blocked);if(ImGui::Button("Open libraries",ImVec2(145,0))&&install_lod()){u.open=false;ImGui::CloseCurrentPopup();}u.apply_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Cancel",ImVec2(100,0))||ImGui::IsKeyPressed(ImGuiKey_Escape)){u.open=false;ImGui::CloseCurrentPopup();}u.cancel_point=(ImGui::GetItemRectMin()+ImGui::GetItemRectMax())*.5f;ImGui::EndPopup();
}
