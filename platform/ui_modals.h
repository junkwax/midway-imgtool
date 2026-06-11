#pragma once
#include <string>
#include <vector>

enum class FileDialogMode {
    OpenImg, AppendImg, OpenLod, SaveImg, ExportTga, LoadLbm, SaveLbm,
    SaveMarkedLbm, LoadTga, SaveTga, ImportPng, ImportPngMatch,
    ImportSpriteSheetMatch, ImportGif, ExportPng, ExportPalette,
    ImportPalette, WriteAniLst, WriteTbl, WriteIrw, LoadAsmAnim, SaveAsmAnim
};

struct FileEntry {
    std::string  name;
    bool         is_dir;
    long long    size;     /* bytes; 0 for dirs */
    long long    mtime;    /* unix-epoch-ish seconds; 0 for dirs */
};

extern bool g_show_file_dialog;
extern std::vector<std::string> g_recent_files;

void OpenFileDialog(FileDialogMode mode);
void DrawFileDialog(void);

void RecentLoad(void);
void SessionRestore(void);
void SessionSave(void);

void GetDirectoryFiles(const std::string& dir, std::vector<FileEntry>& entries, const char* ext_filter);
std::string GetParentDirectory(const std::string& dir);
void load_last_dir_cat(char *dir, size_t dirsz, const char *category);

std::string PathCombine(const std::string& dir, const std::string& file);
void SetActiveDocumentPath(const std::string &full_path);
void RecentAdd(const std::string &full_path);
void RecentSave();
void save_last_dir_cat(const char *dir, const char *category);
void ActivateDocumentTab(int idx);
void OpenImgFile(const std::string &full_path);
void PrepareDocumentForOpenedFile(void);
void RequestOpenDialog(void);
void RequestOpenPath(const std::string &path);
void RequestOpenLodDialog(void);
