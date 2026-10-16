// Single source for command dispatch, menu hints and the searchable Help reference.
struct ShortcutBinding {ImGuiKeyChord key;const char *label;Action action;const char *description;bool artwork;};
static constexpr ShortcutBinding shortcut_bindings[]={
    {ImGuiMod_Ctrl|ImGuiKey_N,"Ctrl+N",Action::NewLibrary,"New IMG library",false},
    {ImGuiMod_Ctrl|ImGuiKey_O,"Ctrl+O",Action::Open,"Open IMG library",false},
    {ImGuiMod_Ctrl|ImGuiMod_Shift|ImGuiKey_O,"Ctrl+Shift+O",Action::OpenProject,"Open project / WAX",false},
    {ImGuiMod_Ctrl|ImGuiKey_S,"Ctrl+S",Action::Save,"Save active IMG or project",false},
    {ImGuiMod_Ctrl|ImGuiMod_Shift|ImGuiKey_S,"Ctrl+Shift+S",Action::SaveAs,"Save As",false},
    {ImGuiMod_Ctrl|ImGuiKey_Z,"Ctrl+Z",Action::Undo,"Undo",false},
    {ImGuiMod_Ctrl|ImGuiKey_Y,"Ctrl+Y",Action::Redo,"Redo",false},
    {ImGuiMod_Ctrl|ImGuiMod_Shift|ImGuiKey_Z,"Ctrl+Shift+Z",Action::Redo,"Redo (alternate)",false},
    {ImGuiMod_Ctrl|ImGuiKey_C,"Ctrl+C",Action::Copy,"Copy pixels",false},
    {ImGuiMod_Ctrl|ImGuiKey_X,"Ctrl+X",Action::Cut,"Cut pixels",false},
    {ImGuiMod_Ctrl|ImGuiKey_V,"Ctrl+V",Action::Paste,"Place pixel clipboard",false},
    {ImGuiMod_Ctrl|ImGuiMod_Shift|ImGuiKey_C,"Ctrl+Shift+C",Action::CopyNew,"Copy selection to new sprite review",true},
    {ImGuiMod_Ctrl|ImGuiMod_Shift|ImGuiKey_X,"Ctrl+Shift+X",Action::CutNew,"Cut selection to new sprite review",true},
    {ImGuiMod_Ctrl|ImGuiMod_Shift|ImGuiKey_V,"Ctrl+Shift+V",Action::PasteNew,"Paste clipboard as new sprite",true},
    {ImGuiMod_Ctrl|ImGuiKey_A,"Ctrl+A",Action::SelectAll,"Select all pixels",true},
    {ImGuiMod_Ctrl|ImGuiKey_D,"Ctrl+D",Action::Deselect,"Deselect pixels",true},
    {ImGuiMod_Ctrl|ImGuiMod_Shift|ImGuiKey_I,"Ctrl+Shift+I",Action::InvertSelection,"Invert pixel selection",true},
    {ImGuiMod_Ctrl|ImGuiKey_T,"Ctrl+T",Action::TransformSelection,"Transform selected pixels",true},
    {ImGuiMod_Ctrl|ImGuiKey_J,"Ctrl+J",Action::Duplicate,"Duplicate sprite / animation entry",false},
    {ImGuiMod_Ctrl|ImGuiKey_R,"Ctrl+R",Action::Rename,"Rename sprite / animation",false},
    {ImGuiKey_F2,"F2",Action::Rename,"Rename (alternate)",false},
    {ImGuiKey_Delete,"Delete",Action::Clear,"Clear selected pixels; otherwise review sprite deletion",true},
    {ImGuiKey_Backspace,"Backspace",Action::Clear,"Clear selected pixels (alternate)",true},
    {ImGuiMod_Shift|ImGuiKey_Delete,"Shift+Delete",Action::Delete,"Review sprite deletion",true},
    {ImGuiMod_Alt|ImGuiKey_PageUp,"Alt+PageUp",Action::MoveUp,"Move sprite up in library",true},
    {ImGuiMod_Alt|ImGuiKey_PageDown,"Alt+PageDown",Action::MoveDown,"Move sprite down in library",true},
    {ImGuiMod_Ctrl|ImGuiKey_P,"Ctrl+P",Action::Points,"Review point table",true},
    {ImGuiMod_Alt|ImGuiKey_L,"Alt+L",Action::ImportIndexed,"Import indexed LBM / TGA",true},
    {ImGuiMod_Ctrl|ImGuiKey_L,"Ctrl+L",Action::ImportIndexed,"Import indexed TGA / LBM",true},
    {ImGuiMod_Alt|ImGuiKey_S,"Alt+S",Action::ExportLbm,"Export selected LBM",true},
    {ImGuiMod_Ctrl|ImGuiKey_B,"Ctrl+B",Action::ExportSheet,"Build TGA sprite sheet review",false},
    {ImGuiMod_Ctrl|ImGuiKey_W,"Ctrl+W",Action::Close,"Close library",false},
    {ImGuiMod_Ctrl|ImGuiKey_0,"Ctrl+0",Action::Fit,"Fit artwork / scene",false},
    {ImGuiMod_Ctrl|ImGuiKey_Keypad0,"Ctrl+Keypad0",Action::Fit,"Fit (keypad)",false},
    {ImGuiMod_Ctrl|ImGuiKey_Equal,"Ctrl+=",Action::ZoomIn,"Zoom in",false},
    {ImGuiMod_Ctrl|ImGuiMod_Shift|ImGuiKey_Equal,"Ctrl++",Action::ZoomIn,"Zoom in (plus)",false},
    {ImGuiMod_Ctrl|ImGuiKey_KeypadAdd,"Ctrl+Keypad+",Action::ZoomIn,"Zoom in (keypad)",false},
    {ImGuiMod_Ctrl|ImGuiKey_Minus,"Ctrl+-",Action::ZoomOut,"Zoom out",false},
    {ImGuiMod_Ctrl|ImGuiKey_KeypadSubtract,"Ctrl+Keypad-",Action::ZoomOut,"Zoom out (keypad)",false},
    {ImGuiKey_H,"H",Action::Help,"Open Help",false},
    {ImGuiKey_F1,"F1",Action::Help,"Open Help (alternate)",false},
    {ImGuiKey_F9,"F9",Action::Diagnostics,"Toggle diagnostics",false},
    {ImGuiKey_Space,"Space",Action::Mark,"Mark / unmark current sprite",true},
    {ImGuiMod_Shift|ImGuiKey_M,"Shift+M",Action::MarkAll,"Mark all sprites",true},
    {ImGuiKey_M,"M",Action::UnmarkAll,"Unmark all sprites",true},
    {ImGuiMod_Shift|ImGuiKey_R,"Shift+R",Action::PaletteRename,"Rename selected sprite palette",true},
    {ImGuiMod_Ctrl|ImGuiKey_LeftArrow,"Ctrl+Left",Action::PreviewEarlier,"Move current preview / stored frame earlier",false},
    {ImGuiMod_Ctrl|ImGuiKey_RightArrow,"Ctrl+Right",Action::PreviewLater,"Move current preview / stored frame later",false},
    {ImGuiKey_Semicolon,";",Action::ReduceMarked,"Review marked sprite reduction",true},
};
struct ToolBinding {ImGuiKeyChord key;const char *label;Tool tool;};
static constexpr ToolBinding tool_bindings[]={
    {ImGuiKey_R,"R",Tool::Select},
    {ImGuiKey_P,"P",Tool::Pencil},
    {ImGuiMod_Shift|ImGuiKey_E,"Shift+E",Tool::Erase},
    {ImGuiKey_G,"G",Tool::Fill},
    {ImGuiKey_I,"I",Tool::Pick},
    {ImGuiMod_Shift|ImGuiKey_H,"Shift+H",Tool::Pan},
    {ImGuiKey_W,"W",Tool::Wand},
    {ImGuiKey_L,"L",Tool::Lasso},
    {ImGuiMod_Shift|ImGuiKey_V,"Shift+V",Tool::Move},
    {ImGuiMod_Shift|ImGuiKey_K,"Shift+K",Tool::Line},
    {ImGuiKey_B,"B",Tool::Replace},
    {ImGuiKey_E,"E",Tool::SmartErase},
    {ImGuiKey_U,"U",Tool::SmartErase},
    {ImGuiKey_C,"C",Tool::Clone},
    {ImGuiKey_V,"V",Tool::Variant},
    {ImGuiKey_T,"T",Tool::Variant},
    {ImGuiKey_O,"O",Tool::Blur},
    {ImGuiKey_S,"S",Tool::Smudge},
    {ImGuiKey_J,"J",Tool::Heal},
    {ImGuiKey_F,"F",Tool::Defringe},
};
// Catch accidental key collisions when a future command or tool is added.
constexpr bool unique_shortcut_bindings(){
    for(size_t i=0;i<sizeof(shortcut_bindings)/sizeof(shortcut_bindings[0]);++i){
        for(size_t j=0;j<i;++j)if(shortcut_bindings[i].key==shortcut_bindings[j].key)return false;
        for(const auto &tool:tool_bindings)if(shortcut_bindings[i].key==tool.key)return false;
    }
    for(size_t i=0;i<sizeof(tool_bindings)/sizeof(tool_bindings[0]);++i)for(size_t j=0;j<i;++j)if(tool_bindings[i].key==tool_bindings[j].key)return false;
    return true;
}
static_assert(unique_shortcut_bindings(),"Studio shortcut bindings must not conflict");
