//#pragma once
//SHN 上边注释了
#include "ALVR-common/exception.h"

Exception MakeException(const char *format, ...);

void Error(const char *format, ...);
void Warn(const char *format, ...);
void Info(const char *format, ...);
void Debug(const char *format, ...);
void TxtPrint(const char *format, ...);