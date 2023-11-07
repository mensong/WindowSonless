#pragma once
#include <windows.h>
#include <map>
#include <string>

class MacroName2Value
{
public:
	MacroName2Value();
	DWORD GetValueByName(const std::string& mname, DWORD defVal = 0);
	static std::map<std::string, DWORD> NameValueMap;
};

