#pragma once
// Procedural footage for unit tests and hidden Studio UI checks. No shell is used.
#include "video_decode.h"
#include "stb_image_write.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#endif
inline int run_video_fixture_command(const std::vector<std::string> &args){
#ifdef _WIN32
    std::string text;for(auto &arg:args){if(!text.empty())text+=' ';text+=VideoQuoteWindowsArg(arg);}int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),(int)text.size(),nullptr,0);if(n<1)return -1;std::wstring cmd(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),(int)text.size(),&cmd[0],n);STARTUPINFOW si{};si.cb=sizeof si;PROCESS_INFORMATION p{};if(!CreateProcessW(nullptr,&cmd[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&p))return -1;CloseHandle(p.hThread);auto wait=WaitForSingleObject(p.hProcess,30000);if(wait!=WAIT_OBJECT_0){TerminateProcess(p.hProcess,1);WaitForSingleObject(p.hProcess,1000);}DWORD code=1;GetExitCodeProcess(p.hProcess,&code);CloseHandle(p.hProcess);return (int)code;
#else
    std::vector<char *> argv;for(auto &arg:args)argv.push_back(const_cast<char *>(arg.c_str()));argv.push_back(nullptr);auto pid=fork();if(pid<0)return -1;if(!pid){execvp(argv[0],argv.data());_exit(127);}int status=0;auto start=std::chrono::steady_clock::now();for(;;){auto got=waitpid(pid,&status,WNOHANG);if(got==pid)return WIFEXITED(status)?WEXITSTATUS(status):-1;if(got<0)return -1;if(std::chrono::steady_clock::now()-start>std::chrono::seconds(30)){kill(pid,SIGKILL);waitpid(pid,&status,0);return -1;}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
#endif
}
inline std::vector<unsigned char> video_fixture_pixels(int frame){std::vector<unsigned char> rgba(128*64*4,255);for(int y=8;y<56;++y)for(int x=32;x<96;++x){auto p=rgba.data()+(y*128+x)*4;p[0]=x<64?(unsigned char)std::min(255,80+(y-8)*3+frame*3):0;p[1]=0;p[2]=x>=64?(unsigned char)std::min(255,80+(y-8)*3+frame*3):0;}rgba[(2*128+2)*4]=rgba[(2*128+2)*4+1]=rgba[(2*128+2)*4+2]=(unsigned char)(frame*10+3);return rgba;}
inline bool make_video_fixture(const std::filesystem::path &root,std::string &path){namespace fs=std::filesystem;auto ffmpeg=VideoFindFfmpeg(true);if(ffmpeg.empty())return false;fs::create_directories(root);for(int frame=0;frame<16;++frame){auto rgba=video_fixture_pixels(frame);std::vector<unsigned char> encoded;auto sink=[](void *ctx,void *data,int n){auto &b=*(std::vector<unsigned char> *)ctx;b.insert(b.end(),(unsigned char *)data,(unsigned char *)data+n);};if(!stbi_write_png_to_func(sink,&encoded,128,64,4,rgba.data(),128*4))return false;auto file=root/("frame"+std::to_string(frame)+".png");std::ofstream out(file,std::ios::binary);if(!out.write((char *)encoded.data(),encoded.size()))return false;}path=(root/fs::u8path(u8"角色 footage.mkv")).u8string();return run_video_fixture_command({ffmpeg,"-hide_banner","-nostdin","-v","error","-y","-framerate","8","-i",(root/"frame%d.png").u8string(),"-frames:v","16","-c:v","ffv1","-pix_fmt","bgr0",path})==0;}
