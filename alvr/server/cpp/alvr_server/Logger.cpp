#include "Logger.h"
#include "driverlog.h"
#include "bindings.h"
#include <stdio.h>
#include <stdlib.h>
#include <cstdarg>
#include <iostream>
#include <string>
#include <windows.h>
#include <thread>
using namespace std;
static FILE* fpLog = NULL;
string sys_time, BeginTime;
int timestamp = 0;
int updateflag = -1;
void OpenLog(const char* fileName) {

   if(fpLog ==nullptr) {
         fpLog = _fsopen(fileName, "at+", _SH_DENYNO);
    }
}
void CloseLog() {
    if (fpLog != nullptr) {
        fclose(fpLog);
        fpLog = nullptr;
    }
}
void LogGetLocalTime() {
    SYSTEMTIME sys{ 0 };
    GetLocalTime(&sys);//本地时间
    sys_time = "\n#"
       // + to_string(sys.wYear) + "-"
        + to_string(sys.wMonth) + "-"
        + to_string(sys.wDay) + "-"
        + to_string(sys.wHour) + ":"
        + to_string(sys.wMinute) + ":"
        + to_string(sys.wSecond) + ":"
        + to_string(sys.wMilliseconds) + "#";
    timestamp = sys.wMinute/5;//整型
    BeginTime =
        to_string(sys.wMonth) + "-"
        + to_string(sys.wDay) + "-"
        + to_string(sys.wHour) + "-"
        + to_string(timestamp);

}
void LogFileUpDate() {
     LogGetLocalTime();
    //初次打开进入延时状态
	//if (updateflag==-1) 
	//{
	//	Sleep(10*1000);
	//	updateflag==-2;
	//}
    if (timestamp != updateflag)
    {   //重新命名log文件名并进行CloseLog、OpenLog操作
        CloseLog();
	    string LogFile= "D:\\AX\\Logs\\Debug\\Debug"+ BeginTime + ".txt";
       //string LogFile = "D:\\Pose\\Debug2" + BeginTime + +".txt";
        OpenLog(LogFile.c_str());
        //更新时间戳
        updateflag = timestamp;
    }
}
void _logSV(const char* format, va_list args, string Type)
{   
	LogFileUpDate();
    char buf[1024*4];
	string sys_timeType=sys_time+Type;    
    vsnprintf(buf, sizeof(buf), format, args);
    fprintf(fpLog, sys_timeType.c_str());
    fprintf(fpLog, buf);
}

void _log(const char *format, va_list args, void (*logFn)(const char *), bool driverLog = false)
{
	char buf[1024];
	int count = vsnprintf(buf, sizeof(buf), format, args);
	if (count > (int)sizeof(buf))
		count = (int)sizeof(buf);
	if (count > 0 && buf[count - 1] == '\n')
		buf[count - 1] = '\0';

	logFn(buf);

	//TODO: driver logger should concider current log level
#ifndef ALVR_DEBUG_LOG
	if (driverLog)
#endif
	DriverLogVarArgs(format, args);
}

Exception MakeException(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	Exception e = FormatExceptionV(format, args);
	va_end(args);

	return e;
}

void Error(const char *format, ...)
{  
	string Error_Type ="Error:";
	va_list args;
	va_start(args, format);
	_log(format, args, LogError, true);
	va_end(args);
}

void Warn(const char *format, ...)
{   string Warn_Type ="Warn:";
	va_list args;
	va_start(args, format);
	_log(format, args, LogWarn, true);
	va_end(args);
}

void Info(const char *format, ...)
{  
	string Info_Type = "Info:";
	va_list args;
	va_start(args, format);
	// Don't log to SteamVR/writing to file for info level, this is mostly statistics info
	_log(format, args, LogInfo);
	//_logSV(format, args, Info_Type);
	va_end(args);
}

void Debug(const char *format, ...)
{  string Info_Type = "Debug:";
   // Infobug(format);
   // Use our define instead of _DEBUG - see build.rs for details.
#ifdef ALVR_DEBUG_LOG
	va_list args;
	va_start(args, format);
	//_logSV(format, args, Info_Type);
	_log(format, args, LogDebug);
	va_end(args);
#else
	(void)format;
#endif
	
}
//SHNChanged 第一个打印文件的路径加命名，第二个是内容函数
void TxtPrint(const char *format, ...)
{   string Info_Type = "Info:";
	va_list args;
	va_start(args, format);
	//_log(format, args, LogInfo); 关闭使用系统的session_logging
	_logSV(format, args, Info_Type);
	va_end(args);
}