// WindowSonless.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include <Windows.h>
#include <jsoncpp/include/json/json.h>
#include <pystring/pystring.h>
#include <map>
#include <vector>
#include <TlHelp32.h>
#include <regex>
#include "MacroName2Value.h"
#include <ctime>

#pragma comment(lib, "Shell32.lib")

struct ACTION_MSG
{
	bool sendMessage;
	UINT msg;
	WPARAM wparam;
	LPARAM lparam;
	DWORD sleep;
};

struct ACTION_CLICK
{
	std::string class_name;
	std::string title_regex;
};

struct ACTION_INFO
{
	std::string class_name;
	std::string title_regex;
	std::vector<ACTION_MSG> spypp_messages;
	std::vector<short> keyboard;
	ACTION_CLICK click;
};

bool g_mainLoopQuit = false;

//<class_name, <ACTION_INFO>>
std::map<std::string, std::vector<ACTION_INFO> > g_actions;
std::vector<std::pair<HWND, ACTION_INFO>> g_willDoTasks;

bool g_isInWindowScope = false;				//是否在窗口创建的过程中
ACTION_INFO* g_parentWindowAction = NULL;	//窗口创建过程中的父窗口句柄


DWORD GetProcessIdByName(const TCHAR* name);
//将Ansi字符转换为Unicode字符串
std::wstring AnsiToUnicode(const std::string& multiByteStr);
//将Unicode字符转换为Ansi字符串
std::string UnicodeToAnsi(const std::wstring& wideByteStr);
std::string UnicodeToUtf8(const std::wstring& wideByteStr);
std::wstring Utf8ToUnicode(const std::string& utf8Str);
std::string AnsiToUtf8(const std::string& multiByteStr);
std::string Utf8ToAnsi(const std::string& utf8Str);
DWORD Get_0x(const char* str, DWORD defVal = 0);
//hex转换integer算法
DWORD htoi(const char hex[]);


bool InitActions(const std::string& config_file, std::string& errMsg)
{
	Json::Value js;
	if (!Json::Reader().parseFile(config_file, js, false))
	{
		errMsg = "打开动作文件失败";
		return false;
	}

	int nAction = js.size();
	for (int i = 0; i < nAction; i++)
	{
		ACTION_INFO action;
		action.class_name = Utf8ToAnsi(js[i]["class_name"].asString());
		action.title_regex = Utf8ToAnsi(js[i]["title_regex"].asString());
		if (pystring::iscempty(action.class_name) && pystring::iscempty(action.title_regex))
		{
			errMsg = "至少包含class_name与title_regex其中之一";
			return false;
		}

		//读取来自于spy++的消息
		Json::Value& arrMessages = js[i]["spypp_messages"];
		DWORD lastSleepTime = 0;//上一次动作的时间，用于时间相减得到延迟时间的
		for (int j = 0; j < arrMessages.size(); j++)
		{
			std::string message = arrMessages[j].asString();
			if (message.empty())
				continue;

			ACTION_MSG postMsg;
			postMsg.sendMessage = false;
			postMsg.msg = 0;
			postMsg.wparam = 0;
			postMsg.lparam = 0;
			postMsg.sleep = 0;

			std::vector<std::string> texts;
			pystring::split(message, texts, " ");
			for (size_t k = 0; k < texts.size(); k++)
			{
				std::string text = pystring::strip(texts[k], " ");
				if (text.empty())
					continue;

				//消息体中有两种类型的字段，一种是整体字段i.e.WM_CLOSE，一种是key:value字段i.e.wParam:0000012
				size_t idx = text.find(':');
				if (idx == std::string::npos)
				{//whole block
					//判断是否是SendMessage/PostMessage
					if (text == "S")
						postMsg.sendMessage = true;
					else if (text == "P")
						postMsg.sendMessage = false;
					else
					{
						//判断是否是消息类型
						MacroName2Value nv;
						DWORD msgType = nv.GetValueByName(text);
						if (msgType != WM_NULL)
						{
							postMsg.msg = msgType;
						}
					}
				}
				else
				{//key value 
					std::string key = text.substr(0, idx);
					std::string val = text.substr(idx + 1);

					if (pystring::equal(key, "wParam", true))//读wParam
					{
						postMsg.wparam = Get_0x(("0x" + val).c_str());
					}
					else if (pystring::equal(key, "lParam", true))//读lParam
					{
						postMsg.lparam = Get_0x(("0x" + val).c_str());
					}
					else if (pystring::equal(key, "time", true))//读时间戳
					{
						val = pystring::replace(pystring::replace(val, ":", ""), ".", "");
						DWORD timesamp = _atoi64(val.c_str());
						if (timesamp == 0)
						{
							postMsg.sleep = 0;
						}
						else if (lastSleepTime == 0)
						{
							postMsg.sleep = 0;
						}
						else
						{
							if (timesamp > lastSleepTime)
								postMsg.sleep = timesamp - lastSleepTime;
							else
								postMsg.sleep = 0;
						}

						lastSleepTime = timesamp;
					}
				}
			}

			action.spypp_messages.push_back(postMsg);
		}

		//读取键盘
		std::string keyboard = pystring::strip(js[i]["keyboard"].asString());
		std::vector<std::string> keys;
		pystring::split(keyboard, keys, "+");
		MacroName2Value nv;
		for (size_t k = 0; k < keys.size(); k++)
		{
			std::string skey = pystring::strip(keys[k]);
			if (pystring::iscempty(skey))
				continue;

			short key = (short)nv.GetValueByName(skey);
			if (!key)
				key = VkKeyScanA(skey[0]);
			action.keyboard.push_back(key);
		}

		//读取点击
		const Json::Value& jClick = js[i]["click"];
		action.click.class_name = Utf8ToAnsi(jClick["class_name"].asString());
		action.click.title_regex = Utf8ToAnsi(jClick["title_regex"].asString());

		g_actions[action.class_name.c_str()].push_back(action);
	}
	return true;
}

VOID CALLBACK WinEventsProc(HWINEVENTHOOK hWinEventHook, DWORD dwEvent, HWND hwnd,
	LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
	char title[256];
	char className[256];

	switch (dwEvent)
	{
	//case EVENT_SYSTEM_DIALOGSTART:
	case EVENT_OBJECT_CREATE:
		if (!g_isInWindowScope)
		{
			std::cout << "===窗口创建开始===" << std::endl;
		}
		g_isInWindowScope = true;

		title[0] = '\0';
		className[0] = '\0';
		GetClassNameA(hwnd, className, 256);
		::GetWindowTextA(hwnd, title, sizeof(title));
		std::cout << "title=[" << title << "] class=[" << className << "]" << std::endl;

		auto itFinder = g_actions.find(className);
		if (itFinder != g_actions.end())
		{
			for (size_t i = 0; i < itFinder->second.size(); i++)
			{
				auto& action = itFinder->second[i];
				std::regex title_regex(action.title_regex);
				if (std::regex_match(title, title_regex))
				{
					g_parentWindowAction = &action;

					/*
					* 为什么不在这里处理：为了防止父窗口创建了，但子控件还没有创建完成，
					*   而创建窗口（包括其中的子控件）完成后才会触发main:while
					* 为什么g_willDoTasks的读写不需要加锁：WinEventsProc和main:while都是主线程处理的
					*/
					g_willDoTasks.push_back(std::make_pair(hwnd, action));
					std::cout << "!!!触发!!!" << std::endl;
				}
			}
		}

		//如果g_parentWindowAction有值，则表示在一个窗口创建的过程中
		if (g_parentWindowAction && 
			g_parentWindowAction->click.class_name == className)
		{
			std::regex title_regex(g_parentWindowAction->click.title_regex);
			if (std::regex_match(title, title_regex))
			{
				g_willDoTasks.push_back(std::make_pair(hwnd, *g_parentWindowAction));
				std::cout << "!!!触发!!!" << std::endl;
			}
		}

		break;
	}
}

HWINEVENTHOOK SetHooks(DWORD processId)
{
	HWINEVENTHOOK hHook = ::SetWinEventHook(
		EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE,
		NULL, WinEventsProc, (DWORD)processId, 0, WINEVENT_OUTOFCONTEXT);

	//HWINEVENTHOOK hHook = ::SetWinEventHook(
	//	EVENT_MIN, EVENT_MAX,
	//	NULL, WinEventsProc, (DWORD)30460, 0, WINEVENT_OUTOFCONTEXT);

	return hHook;
}

int main(int argc, char** argv)
{
	int errCode = 0;
	if (argc < 2)
	{
		std::cout << "WindowSonless.exe \"process_name/process_id\" \"config.json\"" << std::endl;
		return ++errCode;
	}

	std::string sProcessId = argv[1];
	DWORD processId = _atoi64(sProcessId.c_str());
	if (processId == 0)
		processId = GetProcessIdByName(AnsiToUnicode(sProcessId).c_str());
	if (processId == 0)
	{
		std::cout << "找不到:" << sProcessId << std::endl;
		return ++errCode;
	}
	std::string sConfigFile;
	if (argc >=3 )
		sConfigFile = argv[2];

	std::string errMsg;
	if (!InitActions(sConfigFile, errMsg))
	{
		std::cout << "读取动作出错：" << errMsg << std::endl << "接下来将不处理动作。" << std::endl;
		g_actions.clear();
	}

	HWINEVENTHOOK hHook = SetHooks(processId);
	if (!hHook)
	{
		std::cout << "Set hook error." << std::endl;
		return ++errCode;
	}

	DWORD lastCheckProcess = 0;
	DWORD checkProcessTime = 5000;
	MSG msg;
	while (!g_mainLoopQuit)
	{
		//检查程序是否已退出
		if (::GetTickCount() - lastCheckProcess > checkProcessTime)
		{
			HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
			if (!h || h == INVALID_HANDLE_VALUE)
				break;
			DWORD checkExit = 0;
			if (GetExitCodeProcess(h, (LPDWORD)&checkExit) == FALSE)
			{
				CloseHandle(h);
				break;
			}
			CloseHandle(h);
			if (checkExit != STILL_ACTIVE)
			{
				break;
			}

			lastCheckProcess = ::GetTickCount();
		}

		//消息循环，使得hook能运行
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		
		//处理动作
		if (g_willDoTasks.size() > 0)
		{
			for (size_t i = 0; i < g_willDoTasks.size(); i++)
			{
				HWND hwnd = g_willDoTasks[i].first;
				if (!::IsWindow(hwnd))
					continue;
				ACTION_INFO& action = g_willDoTasks[i].second;

				//1.处理消息
				for (size_t j = 0; j < action.spypp_messages.size(); j++)
				{
					if (action.spypp_messages[j].sendMessage)
					{
						::SendMessage(hwnd,
							action.spypp_messages[j].msg,
							action.spypp_messages[j].wparam,
							action.spypp_messages[j].lparam);
					}
					else
					{
						::PostMessage(hwnd,
							action.spypp_messages[j].msg,
							action.spypp_messages[j].wparam,
							action.spypp_messages[j].lparam);
					}
					::Sleep(action.spypp_messages[j].sleep);
				}

				//2.处理键盘
				::BringWindowToTop(hwnd);
				//  按下按键
				for (size_t k = 0; k < action.keyboard.size(); k++)
				{
					keybd_event(action.keyboard[k], 0, 0, 0);
				}
				Sleep(100);
				//  释放按键
				for (int k = action.keyboard.size(); k > 0; k--)
				{
					keybd_event(action.keyboard[k - 1], 0, KEYEVENTF_KEYUP, 0);
				}

				//3.处理点击
				if (!action.click.class_name.empty())
				{
					::PostMessageA(hwnd, WM_LBUTTONDOWN, 0, 0);
					::PostMessageA(hwnd, WM_LBUTTONUP, 0, 0);

					Sleep(1000);

					::PostMessageA(hwnd, WM_LBUTTONDOWN, 0, 0);
					::PostMessageA(hwnd, WM_LBUTTONUP, 0, 0);
				}
				
			}

			g_willDoTasks.clear();
		}

		if (g_isInWindowScope)
		{
			std::cout << "===窗口创建结束===" << std::endl << std::endl;
			g_isInWindowScope = false;
		}		
		g_parentWindowAction = NULL;

		Sleep(100);
	}

	UnhookWinEvent(hHook);

    return errCode;
}


DWORD GetProcessIdByName(const TCHAR* name)
{
	PROCESSENTRY32 pe;
	DWORD id = 0;
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (!hSnapshot || hSnapshot == INVALID_HANDLE_VALUE)
		return 0;

	pe.dwSize = sizeof(PROCESSENTRY32);
	if (!Process32First(hSnapshot, &pe))
	{
		CloseHandle(hSnapshot);
		return 0;
	}

#ifdef _UNICODE
	if (_wcsicmp(name, pe.szExeFile) == 0)
#else
	if (_stricmp(name, pe.szExeFile) == 0)
#endif
	{
		CloseHandle(hSnapshot);
		return pe.th32ProcessID;
	}

	while (1)
	{
		pe.dwSize = sizeof(PROCESSENTRY32);
		if (Process32Next(hSnapshot, &pe) == FALSE)
			break;
#ifdef _UNICODE
		if (_wcsicmp(name, pe.szExeFile) == 0)
#else
		if (_stricmp(name, pe.szExeFile) == 0)
#endif
		{
			id = pe.th32ProcessID;
			break;
		}
	}

	CloseHandle(hSnapshot);
	return id;
}


//将Ansi字符转换为Unicode字符串
std::wstring AnsiToUnicode(const std::string& multiByteStr)
{
	wchar_t* pWideCharStr; //定义返回的宽字符指针
	int nLenOfWideCharStr; //保存宽字符个数，注意不是字节数
	const char* pMultiByteStr = multiByteStr.c_str();
	//获取宽字符的个数
	nLenOfWideCharStr = MultiByteToWideChar(CP_ACP, 0, pMultiByteStr, -1, NULL, 0);
	//获得宽字符指针
	pWideCharStr = (wchar_t*)(HeapAlloc(GetProcessHeap(), 0, nLenOfWideCharStr * sizeof(wchar_t)));
	MultiByteToWideChar(CP_ACP, 0, pMultiByteStr, -1, pWideCharStr, nLenOfWideCharStr);
	//返回
	std::wstring wideByteRet(pWideCharStr, nLenOfWideCharStr);
	//销毁内存中的字符串
	HeapFree(GetProcessHeap(), 0, pWideCharStr);
	return wideByteRet.c_str();
}

//将Unicode字符转换为Ansi字符串
std::string UnicodeToAnsi(const std::wstring& wideByteStr)
{
	char* pMultiCharStr; //定义返回的多字符指针
	int nLenOfMultiCharStr; //保存多字符个数，注意不是字节数
	const wchar_t* pWideByteStr = wideByteStr.c_str();
	//获取多字符的个数
	nLenOfMultiCharStr = WideCharToMultiByte(CP_ACP, 0, pWideByteStr, -1, NULL, 0, NULL, NULL);
	//获得多字符指针
	pMultiCharStr = (char*)(HeapAlloc(GetProcessHeap(), 0, nLenOfMultiCharStr * sizeof(char)));
	WideCharToMultiByte(CP_ACP, 0, pWideByteStr, -1, pMultiCharStr, nLenOfMultiCharStr, NULL, NULL);
	//返回
	std::string sRet(pMultiCharStr, nLenOfMultiCharStr);
	//销毁内存中的字符串
	HeapFree(GetProcessHeap(), 0, pMultiCharStr);
	return sRet.c_str();
}


std::string UnicodeToUtf8(const std::wstring& wideByteStr)
{
	int len = WideCharToMultiByte(CP_UTF8, 0, wideByteStr.c_str(), -1, NULL, 0, NULL, NULL);
	char* szUtf8 = new char[len + 1];
	memset(szUtf8, 0, len + 1);
	WideCharToMultiByte(CP_UTF8, 0, wideByteStr.c_str(), -1, szUtf8, len, NULL, NULL);
	std::string s = szUtf8;
	delete[] szUtf8;
	return s.c_str();
}

std::wstring Utf8ToUnicode(const std::string& utf8Str)
{
	//预转换，得到所需空间的大小;
	int wcsLen = ::MultiByteToWideChar(CP_UTF8, NULL, utf8Str.c_str(), strlen(utf8Str.c_str()), NULL, 0);
	//分配空间要给'\0'留个空间，MultiByteToWideChar不会给'\0'空间
	wchar_t* wszString = new wchar_t[wcsLen + 1];
	//转换
	::MultiByteToWideChar(CP_UTF8, NULL, utf8Str.c_str(), strlen(utf8Str.c_str()), wszString, wcsLen);
	//最后加上'\0'
	wszString[wcsLen] = '\0';
	std::wstring s(wszString);
	delete[] wszString;
	return s;
}

std::string AnsiToUtf8(const std::string& multiByteStr)
{
	std::wstring ws = AnsiToUnicode(multiByteStr);
	return UnicodeToUtf8(ws);
}

std::string Utf8ToAnsi(const std::string& utf8Str)
{
	std::wstring ws = Utf8ToUnicode(utf8Str);
	return UnicodeToAnsi(ws);
}

DWORD Get_0x(const char* str, DWORD defVal/* = 0*/)
{
	int sum = 0;
	while (isspace(*str))
	{
		str++;
	}
	//此时 空格处理结束
	int index = 1;
	if (*str == '-' || *str == '+')
	{
		if (*str == '-')
		{
			index *= -1;
		}
		else
		{
			index = 1;
		}
		str++;
	}

	if ((*str == '0') && (*(str + 1) == 'x' || *(str + 1) == 'X'))
	{
		str += 2;
	}
	else
	{
		return defVal;
	}

	while (isxdigit(*str))
	{
		if (isdigit(*str))
		{
			sum = sum * 16 + (*str - '0');
		}
		else if (islower(*str))
		{
			sum = sum * 16 + (*str - 'a' + 10);
		}
		else
		{
			sum = sum * 16 + (*str - 'A' + 10);
		}
		str++;
	}
	if (*str == '\0')
		return sum * index;

	return defVal;
}

//转换算法如下
DWORD htoi(const char hex[])
{
	DWORD i, inhex, hexdigit, n;
	i = 0;
	if (hex[i] == '0')//转换之前如有0X或0x等表明该数为十六进制数的前缀需进行跳过
		++i;
	if (hex[i] == 'x' || hex[i] == 'X')
		++i;
	n = 0;
	inhex = 1;
	for (; inhex == 1; i++) {
		if (hex[i] >= '0' && hex[i] <= '9')
			hexdigit = hex[i] - '0';          //将char类型转换为int类型并换算该数位数值大小
		else if (hex[i] >= 'a' && hex[i] <= 'f')
			hexdigit = hex[i] - 'a' + 10;
		else if (hex[i] >= 'A' && hex[i] <= 'F')
			hexdigit = hex[i] - 'A' + 10;
		else
			inhex = 0;
		if (inhex == 1)
			n = 16 * n + hexdigit;               //累加计算
	}
	return n;
}