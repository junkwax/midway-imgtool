#pragma once
#include <cstdio>
#include <filesystem>
#include <string>
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace studio {
// Short, flushed startup log survives an early native crash. Never logs artwork.
class StartupLog {
    FILE *file=nullptr;
public:
    std::string path;
    const char *phase="Starting";
    StartupLog(const std::string &folder){
        try{path=(std::filesystem::u8path(folder)/"startup.log").u8string();
#ifdef _WIN32
            file=_wfopen(std::filesystem::u8path(path).c_str(),L"wb");
#else
            file=fopen(path.c_str(),"wb");
#endif
        }catch(...){ }
    }
    ~StartupLog(){if(file)fclose(file);}
    void write(const std::string &line){if(file){fprintf(file,"%s\n",line.c_str());fflush(file);}}
    void stage(const char *value){phase=value;write(value);}
#ifdef _WIN32
    void native_exception(const EXCEPTION_RECORD &record){if(file){fprintf(file,"Native exception 0x%08lX at %p (module base %p). Last stage: %s\n",record.ExceptionCode,record.ExceptionAddress,(void *)GetModuleHandleW(nullptr),phase);fflush(file);}}
#endif
    void failure(const std::string &message,bool quiet){write(std::string("FAILED: ")+message);fprintf(stderr,"Studio startup failed: %s\n",message.c_str());
        if(!quiet){std::string shown=message+"\n\nStartup log: "+path;
#ifdef _WIN32
            auto wide=std::filesystem::u8path(shown).wstring();MessageBoxW(nullptr,wide.c_str(),L"Imgtool Studio could not start",MB_OK|MB_ICONERROR);
#else
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Imgtool Studio could not start",shown.c_str(),nullptr);
#endif
        }
    }
};
#ifdef _WIN32
inline StartupLog *startup_log=nullptr;
inline LONG WINAPI startup_exception(EXCEPTION_POINTERS *info){
    if(startup_log)startup_log->native_exception(*info->ExceptionRecord);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
}
