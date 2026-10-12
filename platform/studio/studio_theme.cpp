#include "studio_theme.h"
namespace studio {
ImVec4 accent_surface(float t) { return ImVec4(.10f+(.86f-.10f)*t,.12f+(.22f-.12f)*t,.15f+(.27f-.15f)*t,1); }
void apply_theme() {
    ImGui::StyleColorsDark(); auto &s=ImGui::GetStyle();
    s.WindowPadding=ImVec2(14,12);s.FramePadding=ImVec2(9,6);s.ItemSpacing=ImVec2(8,7);
    s.WindowRounding=0;s.ChildRounding=5;s.FrameRounding=4;s.PopupRounding=6;s.TabRounding=4;
    s.WindowBorderSize=0;s.ChildBorderSize=1;s.ScrollbarSize=12;s.GrabRounding=3;
    auto c=s.Colors;
    c[ImGuiCol_WindowBg]=ImVec4(.06f,.07f,.09f,1);
    c[ImGuiCol_ChildBg]=ImVec4(.09f,.10f,.125f,1);
    c[ImGuiCol_PopupBg]=ImVec4(.105f,.12f,.145f,1);
    c[ImGuiCol_MenuBarBg]=c[ImGuiCol_WindowBg];
    c[ImGuiCol_Border]=ImVec4(.18f,.20f,.24f,1);
    c[ImGuiCol_Text]=ImVec4(.88f,.90f,.94f,1);c[ImGuiCol_TextDisabled]=ImVec4(.52f,.57f,.64f,1);
    c[ImGuiCol_FrameBg]=ImVec4(.13f,.15f,.18f,1);
    c[ImGuiCol_FrameBgHovered]=accent_surface(.22f);c[ImGuiCol_FrameBgActive]=accent_surface(.38f);
    c[ImGuiCol_Button]=ImVec4(.16f,.18f,.22f,1);
    c[ImGuiCol_ButtonHovered]=accent_surface(.35f);c[ImGuiCol_ButtonActive]=accent_surface(.55f);
    c[ImGuiCol_Header]=accent_surface(.28f);c[ImGuiCol_HeaderHovered]=accent_surface(.37f);c[ImGuiCol_HeaderActive]=accent_surface(.5f);
    c[ImGuiCol_Tab]=ImVec4(.10f,.12f,.15f,1);c[ImGuiCol_TabSelected]=accent_surface(.23f);
    c[ImGuiCol_TabHovered]=accent_surface(.40f);c[ImGuiCol_TabSelectedOverline]=accent_surface(1);
    c[ImGuiCol_TabDimmedSelected]=accent_surface(.16f);
    c[ImGuiCol_CheckMark]=accent_surface(1);c[ImGuiCol_SliderGrab]=accent_surface(.85f);c[ImGuiCol_SliderGrabActive]=accent_surface(1);
    c[ImGuiCol_Separator]=c[ImGuiCol_Border];c[ImGuiCol_SeparatorHovered]=accent_surface(.7f);c[ImGuiCol_SeparatorActive]=accent_surface(1);
    c[ImGuiCol_ResizeGrip]=accent_surface(.3f);c[ImGuiCol_ResizeGripHovered]=accent_surface(.6f);c[ImGuiCol_ResizeGripActive]=accent_surface(1);
    c[ImGuiCol_NavHighlight]=accent_surface(1);c[ImGuiCol_DragDropTarget]=accent_surface(1);
    c[ImGuiCol_TextSelectedBg]=ImVec4(.86f,.22f,.27f,.35f);
    c[ImGuiCol_TitleBgActive]=accent_surface(.16f);
}
}
