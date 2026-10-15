#pragma once
#include <cstdio>
#include <filesystem>
#include <string>
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

namespace studio {
// Short, flushed startup log survives an early native crash. Never logs artwork.
class StartupLog {
    FILE *file=nullptr;
#ifdef _WIN32
    std::wstring dump_file;
    volatile LONG saving_dump=0;
#endif
public:
    std::string path;
    std::string crash_path;
    const char *phase="Starting";
    StartupLog(const std::string &folder){
        try{path=(std::filesystem::u8path(folder)/"startup.log").u8string();
#ifdef _WIN32
            file=_wfopen(std::filesystem::u8path(path).c_str(),L"wb");
            auto dump=std::filesystem::u8path(folder)/("startup-crash-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+".dmp");
            crash_path=dump.u8string();dump_file=dump.wstring();
#else
            file=fopen(path.c_str(),"wb");
#endif
        }catch(...){ }
    }
    ~StartupLog(){if(file)fclose(file);}
    void write(const std::string &line){if(file){fprintf(file,"%s\n",line.c_str());fflush(file);}}
    void stage(const char *value){phase=value;write(value);}
#ifdef _WIN32
    bool native_exception(EXCEPTION_POINTERS &info) noexcept {
        // DbgHelp is not thread-safe. Avoid recursion or simultaneous dump writers.
        if(InterlockedCompareExchange(&saving_dump,1,0)!=0)return false;
        auto &record=*info.ExceptionRecord;
        if(file){
            fprintf(file,"Native exception 0x%08lX at %p (module base %p). Last stage: %s\n",record.ExceptionCode,record.ExceptionAddress,(void *)GetModuleHandleW(nullptr),phase);
            if((record.ExceptionCode==EXCEPTION_ACCESS_VIOLATION||record.ExceptionCode==EXCEPTION_IN_PAGE_ERROR)&&record.NumberParameters>=2){auto operation=record.ExceptionInformation[0];fprintf(file,"Memory operation: %s; address: %p\n",operation==0?"read":operation==1?"write":operation==8?"execute":"unknown",(void *)record.ExceptionInformation[1]);}
            fflush(file);
        }
        // Paths and DLL imports are prepared before the fault. Do not allocate
        // C++ buffers or attempt to recover/edit artwork from the crash handler.
        HANDLE output=dump_file.empty()?INVALID_HANDLE_VALUE:CreateFileW(dump_file.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(output==INVALID_HANDLE_VALUE){if(file){fprintf(file,"Could not create crash dump (Windows error %lu).\n",GetLastError());fflush(file);}return false;}
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(),&info,FALSE};
        bool saved=MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),output,MiniDumpNormal,&exception,nullptr,nullptr)!=FALSE;
        auto failure=GetLastError();if(saved)FlushFileBuffers(output);CloseHandle(output);
        if(file){if(saved)fprintf(file,"Crash dump: %s\n",crash_path.c_str());else fprintf(file,"Could not write crash dump (Windows error %lu).\n",failure);fflush(file);}
        return saved;
    }
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
    if(startup_log&&info&&info->ExceptionRecord&&info->ContextRecord)startup_log->native_exception(*info);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
}
