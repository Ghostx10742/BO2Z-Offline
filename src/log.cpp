#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
static SRWLOCK lock=SRWLOCK_INIT;
static FILE* file=nullptr;
void Log_Init() {
    wchar_t path[32768]{};
    wchar_t dataRoot[32768]{};
    if(GetEnvironmentVariableW(L"BO2Z_OFFLINE_DATA_ROOT",dataRoot,32768)) {
        wcscpy_s(path,dataRoot);
        auto* name=wcsrchr(path,L'\\');if(!name)return;
        wcscpy_s(name+1,32768-(name+1-path),L"BO2Z-Offline.log");
    } else {
        GetModuleFileNameW(nullptr,path,32768);
        auto* name=wcsrchr(path,L'\\');if(!name)return;
        wcscpy_s(name+1,32768-(name+1-path),L"BO2Z-Offline.log");
    }
    _wfopen_s(&file,path,L"a");
}
void Log(const char* format,...) {
    if(!file)return;
    char text[2048]{};va_list args;va_start(args,format);vsnprintf(text,sizeof text,format,args);va_end(args);
    SYSTEMTIME t{};GetLocalTime(&t);AcquireSRWLockExclusive(&lock);
    fprintf(file,"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\n",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,text);
    fflush(file);ReleaseSRWLockExclusive(&lock);
}
