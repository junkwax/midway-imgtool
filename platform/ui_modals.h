#pragma once
#include <string>
#include <vector>

enum class FileDialogMode {
    OpenImg, AppendImg, OpenLod, SaveImg, ExportTga, LoadLbm, SaveLbm,
    SaveMarkedLbm, LoadTga, SaveTga, ImportPng, ImportPngMatch,
    ImportSpriteSheetMatch, ImportGif, ExportPng, ExportGif, ExportPalette,
    ImportPalette, WriteAniLst, WriteTbl, CompareTbl, WriteIrw, LoadAsmAnim,
    SaveAsmAnim, LoadWorldProject, AppendWorldProject, SaveWorldProject, ExportWorldPng,
    ExportWorldPngSeq, LoadWorldBdd
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
/* Full path of the World View project last loaded OR saved this session, ""
   before either happens. The World View header shows it: with several .wax
   variants of one lane in flight, "which one am I looking at" is not
   answerable from the scene itself. */
const char *WorldLastProjectPath(void);
void PrepareDocumentForOpenedFile(void);
void RequestOpenDialog(void);
void RequestOpenPath(const std::string &path);
void RequestOpenLodDialog(void);
