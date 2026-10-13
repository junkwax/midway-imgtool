#include "studio_file_io.h"
#include <filesystem>
#include <chrono>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace studio {
bool atomic_write_file(const std::string &path,const std::vector<unsigned char> &bytes,std::string &error){
    error.clear();if(bytes.size()>256*1024*1024){error="The output exceeds the 256 MiB write limit.";return false;}namespace fs=std::filesystem;std::error_code ec;auto dest=fs::absolute(fs::u8path(path),ec).lexically_normal();if(ec||dest.filename().empty()){error="Choose an output filename.";return false;}
    static unsigned serial=0;auto temp=dest;temp+=".studio-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial)+".tmp";
#ifdef _WIN32
    auto handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(handle==INVALID_HANDLE_VALUE){error="Could not create a temporary output file.";return false;}DWORD written=0;bool ok=WriteFile(handle,bytes.data(),(DWORD)bytes.size(),&written,nullptr)&&written==bytes.size();if(ok)ok=FlushFileBuffers(handle)!=0;ok=(CloseHandle(handle)!=0)&&ok;
    if(ok)ok=MoveFileExW(temp.c_str(),dest.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    int fd=::open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0){error="Could not create a temporary output file.";return false;}size_t done=0;while(done<bytes.size()){auto n=::write(fd,bytes.data()+done,bytes.size()-done);if(n<=0)break;done+=(size_t)n;}bool ok=done==bytes.size();if(ok)ok=::fsync(fd)==0;ok=(::close(fd)==0)&&ok;if(ok){fs::rename(temp,dest,ec);ok=!ec;}
#endif
    if(!ok){fs::remove(temp,ec);error="Could not finish writing the output. The destination was left intact.";}return ok;
}
}
