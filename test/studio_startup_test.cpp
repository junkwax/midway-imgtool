#include "../platform/studio/studio_startup.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(0)
static std::vector<char> bytes(const std::filesystem::path &p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(){namespace fs=std::filesystem;auto temp=fs::temp_directory_path();auto root=temp/("studio-startup-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));bool safe=fs::equivalent(root.parent_path(),temp);CHECK(safe);if(!safe)return 1;bool created=fs::create_directory(root);CHECK(created);if(!created)return 1;auto folder=root/fs::u8path(u8"日志");fs::create_directory(folder);
    std::string log_path,dump_path;
    {
        studio::StartupLog log(folder.u8string());log_path=log.path;dump_path=log.crash_path;log.stage("Editor rendered successfully");CHECK(fs::exists(fs::u8path(log_path))&&!fs::exists(fs::u8path(dump_path)));
        // Supply real CPU context with a fabricated AV record. This exercises
        // the OS dump writer without crashing or displaying a Windows dialog.
        CONTEXT context{};RtlCaptureContext(&context);EXCEPTION_RECORD record{};record.ExceptionCode=EXCEPTION_ACCESS_VIOLATION;record.ExceptionAddress=(void *)&main;record.NumberParameters=2;record.ExceptionInformation[0]=0;record.ExceptionInformation[1]=0xdeadbeef;EXCEPTION_POINTERS info{&record,&context};CHECK(log.native_exception(info));CHECK(fs::exists(fs::u8path(dump_path)));CHECK(!log.native_exception(info));
    }
    auto content=bytes(fs::u8path(log_path));std::string text(content.begin(),content.end());CHECK(text.find("0xC0000005")!=std::string::npos&&text.find("Memory operation: read")!=std::string::npos&&text.find(dump_path)!=std::string::npos);
    auto dump=bytes(fs::u8path(dump_path));CHECK(dump.size()>sizeof(MINIDUMP_HEADER));if(dump.size()>sizeof(MINIDUMP_HEADER)){auto header=(MINIDUMP_HEADER *)dump.data();CHECK(header->Signature==MINIDUMP_SIGNATURE);PMINIDUMP_DIRECTORY directory=nullptr;void *stream=nullptr;ULONG size=0;bool found=MiniDumpReadDumpStream(dump.data(),ExceptionStream,&directory,&stream,&size)!=FALSE;CHECK(found);if(found&&size>=sizeof(MINIDUMP_EXCEPTION_STREAM)){auto exception=(MINIDUMP_EXCEPTION_STREAM *)stream;CHECK(exception->ExceptionRecord.ExceptionCode==EXCEPTION_ACCESS_VIOLATION&&exception->ExceptionRecord.ExceptionInformation[1]==0xdeadbeef&&exception->ThreadId==GetCurrentThreadId());}else CHECK(false);}
    {
        studio::StartupLog log((root/"missing-folder").u8string());CONTEXT context{};RtlCaptureContext(&context);EXCEPTION_RECORD record{};record.ExceptionCode=EXCEPTION_ACCESS_VIOLATION;EXCEPTION_POINTERS info{&record,&context};CHECK(!log.native_exception(info));
    }
    fs::remove_all(root);return failures?1:0;
}
