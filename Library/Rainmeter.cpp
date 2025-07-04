/* Copyright (C) 2001 Rainmeter Project Developers
 *
 * This Source Code Form is subject to the terms of the GNU General Public
 * License; either version 2 of the License, or (at your option) any later
 * version. If a copy of the GPL was not distributed with this file, You can
 * obtain one at <https://www.gnu.org/licenses/gpl-2.0.html>. */

#include "StdAfx.h"
#include "../Common/Gfx/Canvas.h"
#include "../Common/FileUtil.h"
#include "../Common/PathUtil.h"
#include "../Common/Platform.h"
#include "Rainmeter.h"
#include "TrayIcon.h"
#include "System.h"
#include "DialogAbout.h"
#include "DialogManage.h"
#include "DialogNewSkin.h"
#include "GameMode.h"
#include "MeasureNet.h"
#include "MeasureCPU.h"
#include "MeterString.h"
#include "UpdateCheck.h"
#include "../Version.h"

using namespace Gdiplus;

enum TIMER
{
	TIMER_NETSTATS    = 1
};
enum INTERVAL
{
	INTERVAL_NETSTATS = 120000
};

/*
** Initializes Rainmeter.
**
*/
int RainmeterMain(LPWSTR cmdLine)
{
	// Avoid loading a dll from current directory
	SetDllDirectory(L"");

	const WCHAR* layout = nullptr;

	if (cmdLine[0] == L'!' || cmdLine[0] == L'[')
	{
		HWND wnd = FindWindow(RAINMETER_CLASS_NAME, RAINMETER_WINDOW_NAME);
		if (wnd)
		{
			// Deliver bang to existing Rainmeter instance
			COPYDATASTRUCT cds = { 0 };
			cds.dwData = 1;
			cds.cbData = (DWORD)((wcslen(cmdLine) + 1) * sizeof(WCHAR));
			cds.lpData = (PVOID)cmdLine;
			SendMessage(wnd, WM_COPYDATA, 0, (LPARAM)&cds);
			return 0;
		}

		// Disallow everything except !LoadLayout.
		if (_wcsnicmp(cmdLine, L"!LoadLayout ", 12) == 0)
		{
			layout = cmdLine + 12;  // Skip "!LoadLayout ".
		}
		else
		{
			return 1;
		}
	}
	else if (cmdLine[0] == L'"')
	{
		// Strip quotes
		++cmdLine;
		WCHAR* pos = wcsrchr(cmdLine, L'"');
		if (pos)
		{
			*pos = L'\0';
		}
	}

	const WCHAR* iniFile = (*cmdLine && !layout) ? cmdLine : nullptr;

	auto& rainmeter = GetRainmeter();
	int ret = rainmeter.Initialize(iniFile, layout, IsCtrlKeyDown());
	if (ret == 0)
	{
		ret = rainmeter.MessagePump();
	}
	rainmeter.Finalize();

	return ret;
}

/*
** Constructor
**
*/
Rainmeter::Rainmeter() :
	m_TrayIcon(),
	m_Debug(false),
	m_DisableVersionCheck(false),
	m_NewVersion(false),
	m_DisableAutoUpdate(false),
	m_DownloadedNewVersion(false),
	m_LanguageObsolete(false),
	m_DesktopWorkAreaChanged(false),
	m_DesktopWorkAreaType(false),
	m_NormalStayDesktop(true),
	m_DisableRDP(false),
	m_DisableDragging(false),
	m_CurrentParser(),
	m_Window(),
	m_Mutex(),
	m_Instance(),
	m_ResourceInstance(),
	m_ResourceLCID(),
	m_GDIplusToken(),
	m_GlobalOptions(),
	m_DefaultSelectedColor(),
	m_HardwareAccelerated(false)
{
	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (FAILED(hr))
	{
		MessageBox(nullptr, L"Failed to initialize COM object", APPNAME, MB_OK | MB_TOPMOST | MB_ICONERROR);
		PostQuitMessage(1);
	}

	InitCommonControls();

	// Initialize GDI+.
	GdiplusStartupInput gdiplusStartupInput;
	GdiplusStartup(&m_GDIplusToken, &gdiplusStartupInput, nullptr);
}

/*
** Destructor
**
*/
Rainmeter::~Rainmeter()
{
	CoUninitialize();

	GdiplusShutdown(m_GDIplusToken);

	// Close dialogs if open
	DialogManage::CloseDialog();
	DialogAbout::CloseDialog();
	DialogNewSkin::CloseDialog();
}

Rainmeter& Rainmeter::GetInstance()
{
	static Rainmeter s_Rainmeter;
	return s_Rainmeter;
}

/*
** The main initialization function for the module.
**
*/
int Rainmeter::Initialize(LPCWSTR iniPath, LPCWSTR layout, bool safeStart)
{
	if (!IsWindows7SP1OrGreater())
	{
		MessageBox(nullptr, L"Rainmeter requires Windows 7 SP1 (with Platform Update) or later.", APPNAME, MB_OK | MB_TOPMOST | MB_ICONERROR);
		return 1;
	}

	m_Instance = GetModuleHandle(L"Rainmeter");

	WCHAR* buffer = new WCHAR[MAX_LINE_LENGTH];
	GetModuleFileName(m_Instance, buffer, MAX_LINE_LENGTH);

	auto clearBuffer = [&buffer]() -> void
	{
		delete [] buffer;
		buffer = nullptr;
	};

	// Remove the module's name from the path
	WCHAR* pos = wcsrchr(buffer, L'\\');
	m_Path.assign(buffer, pos ? pos - buffer + 1 : 0);
	m_Drive = PathUtil::GetVolume(m_Path);

	bool bDefaultIniLocation = false;
	if (iniPath)
	{
		// The command line defines the location of Rainmeter.ini (or whatever it calls it).
		std::wstring iniFile = iniPath;
		PathUtil::ExpandEnvironmentVariables(iniFile);

		if (iniFile.empty() || PathUtil::IsSeparator(iniFile[iniFile.length() - 1]))
		{
			iniFile += L"Rainmeter.ini";
		}
		else if (iniFile.length() <= 4 || _wcsicmp(iniFile.c_str() + (iniFile.length() - 4), L".ini") != 0)
		{
			iniFile += L"\\Rainmeter.ini";
		}

		if (!PathUtil::IsSeparator(iniFile[0]) && iniFile.find_first_of(L':') == std::wstring::npos)
		{
			// Make absolute path
			iniFile.insert(0, m_Path);
		}

		m_IniFile = iniFile;
		bDefaultIniLocation = true;
	}
	else
	{
		m_IniFile = m_Path;
		m_IniFile += L"Rainmeter.ini";

		// If the ini file doesn't exist in the program folder store it to the %APPDATA% instead so that things work better in Vista/Win7
		if (_waccess_s(m_IniFile.c_str(), 0) != 0)
		{
			m_IniFile = L"%APPDATA%\\Rainmeter\\Rainmeter.ini";
			PathUtil::ExpandEnvironmentVariables(m_IniFile);
			bDefaultIniLocation = true;
		}
	}

	m_HardwareAccelerated = 0 != GetPrivateProfileInt(L"Rainmeter", L"HardwareAcceleration", 0, m_IniFile.c_str());

	if (!Gfx::Canvas::Initialize(m_HardwareAccelerated))
	{
		SetHardwareAccelerated(false);
		if (!Gfx::Canvas::Initialize(m_HardwareAccelerated))
		{
			MessageBox(nullptr, L"Rainmeter requires Windows 7 SP1 (with Platform Update) or later.", APPNAME, MB_OK | MB_TOPMOST | MB_ICONERROR);
			clearBuffer();
			return 1;
		}
	}

	if (IsAlreadyRunning())
	{
		// Instance already running with same .ini file
		clearBuffer();
		return 1;
	}

	WNDCLASS wc = { 0 };
	wc.lpfnWndProc = (WNDPROC)MainWndProc;
	wc.hInstance = m_Instance;
	wc.lpszClassName = RAINMETER_CLASS_NAME;
	ATOM className = RegisterClass(&wc);

	m_Window = CreateWindowEx(
		WS_EX_TOOLWINDOW,
		MAKEINTATOM(className),
		RAINMETER_WINDOW_NAME,
		WS_POPUP | WS_DISABLED,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		nullptr,
		nullptr,
		m_Instance,
		nullptr);

	if (!m_Window)
	{
		clearBuffer();
		return 1;
	}

	Logger& logger = GetLogger();
	const WCHAR* iniFile = m_IniFile.c_str();

	// Set file locations
	{
		m_SettingsPath = PathUtil::GetFolderFromFilePath(m_IniFile);

		size_t len = m_IniFile.length();
		if (len > 4 && _wcsicmp(iniFile + (len - 4), L".ini") == 0)
		{
			len -= 4;
		}

		std::wstring logFile(m_IniFile, 0, len);
		m_DataFile = m_StatsFile = logFile;
		logFile += L".log";
		m_StatsFile += L".stats";
		m_DataFile += L".data";

		logger.SetLogFilePath(logFile);
	}

	// Create a default Rainmeter.ini file if needed
	bool iniFileCreated = false;
	if (_waccess_s(iniFile, 0) != 0)
	{
		iniFileCreated = true;
		CreateOptionsFile();
	}

	// Check encoding of settings file
	std::wstring encodingMsg;
	CheckSettingsFileEncoding(m_IniFile, &encodingMsg);

	bool dataFileCreated = false;
	if (_waccess_s(m_DataFile.c_str(), 0) != 0)
	{
		dataFileCreated = true;
		CreateDataFile();
	}

	// Install new version
	if (GetPrivateProfileString(L"Rainmeter", L"InstallerName", L"", buffer, MAX_LINE_LENGTH, m_DataFile.c_str()) != 0)
	{
		bool runInstaller = false;
		const std::wstring installerName = buffer;
		const std::wstring updatePath = m_SettingsPath + L"Updates\\";
		const std::wstring fullPath = updatePath + installerName;
		if (PathFileExists(fullPath.c_str()))
		{
			if (GetPrivateProfileString(L"Rainmeter", L"InstallerSha256", L"", buffer, MAX_LINE_LENGTH, m_DataFile.c_str()) != 0)
			{
				const std::wstring sha256 = buffer;
				runInstaller = Updater::VerifyInstaller(updatePath, installerName, sha256, false);
				WritePrivateProfileString(L"Rainmeter", L"InstallerSha256", nullptr, m_DataFile.c_str());
			}
		}

		WritePrivateProfileString(L"Rainmeter", L"InstallerName", nullptr, m_DataFile.c_str());
		WritePrivateProfileString(L"Rainmeter", L"DeleteInstaller", installerName.c_str(), m_DataFile.c_str());

		if (runInstaller)
		{
			const std::wstring isPortable = _wcsicmp(m_Path.c_str(), m_SettingsPath.c_str()) == 0 ? L"1" : L"0";
			const std::wstring is64Bit = APPBITS == L"64-bit" ? L"1" : L"0";
			const std::wstring args = L"/S /RESTART=1 /PORTABLE=" + isPortable + L" /VERSION" + is64Bit + L" /D=" + m_Path.c_str();
			CommandHandler::RunFile(fullPath.c_str(), args.c_str());
			clearBuffer();
			return -1;
		}
	}

	// Delete installer if necessary
	if (GetPrivateProfileString(L"Rainmeter", L"DeleteInstaller", L"", buffer, MAX_LINE_LENGTH, m_DataFile.c_str()) != 0)
	{
		const std::wstring updatePath = m_SettingsPath + L"Updates\\";
		const std::wstring fullPath = updatePath + buffer;
		if (PathFileExists(fullPath.c_str()))
		{
			System::RemoveFile(fullPath);
		}
		RemoveDirectory(updatePath.c_str());
	}

	// Clean-up any installer keys
	WritePrivateProfileString(L"Rainmeter", L"InstallerName", nullptr, m_DataFile.c_str());    // Shouldn't exist at this point
	WritePrivateProfileString(L"Rainmeter", L"InstallerSha256", nullptr, m_DataFile.c_str());  // Might exist if installer (or "Updates" folder) was deleted before installation
	WritePrivateProfileString(L"Rainmeter", L"DeleteInstaller", nullptr, m_DataFile.c_str());

	// Reset log file
	System::RemoveFile(logger.GetLogFilePath());

	m_Debug = 0!=GetPrivateProfileInt(L"Rainmeter", L"Debug", 0, iniFile);

	const bool logging = GetPrivateProfileInt(L"Rainmeter", L"Logging", 0, iniFile) != 0;
	logger.SetLogToFile(logging);
	if (logging)
	{
		logger.StartLogFile();
	}

	// Determine the language resource to load
	std::wstring resource = m_Path + L"Languages\\";
	if (GetPrivateProfileString(L"Rainmeter", L"Language", L"", buffer, MAX_LINE_LENGTH, iniFile) == 0)
	{
		// Use whatever the user selected for the installer
		DWORD size = MAX_LINE_LENGTH;
		HKEY hKey;
		if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Software\\Rainmeter", 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &hKey) == ERROR_SUCCESS)
		{
			DWORD type = 0;
			if (RegQueryValueEx(hKey, L"Language", nullptr, &type, (LPBYTE)buffer, (LPDWORD)&size) != ERROR_SUCCESS ||
				type != REG_SZ)
			{
				buffer[0] = L'\0';
			}
			RegCloseKey(hKey);
		}
	}
	if (buffer[0] != L'\0')
	{
		// Try selected language
		m_ResourceLCID = wcstoul(buffer, nullptr, 10);
		resource += buffer;
		resource += L".dll";

		m_ResourceInstance = LoadLibraryEx(resource.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_DATAFILE);
	}
	if (!m_ResourceInstance)
	{
		// Try English
		resource = m_Path;
		resource += L"Languages\\1033.dll";
		m_ResourceInstance = LoadLibraryEx(resource.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_DATAFILE);
		m_ResourceLCID = 1033;
		if (!m_ResourceInstance)
		{
			MessageBox(nullptr, L"Unable to load language library", APPNAME, MB_OK | MB_TOPMOST | MB_ICONERROR);
			clearBuffer();
			return 1;
		}
	}

	// Get skin folder path
	size_t len = GetPrivateProfileString(L"Rainmeter", L"SkinPath", L"", buffer, MAX_LINE_LENGTH, iniFile);
	if (len > 0 && _waccess_s(buffer, 0) == 0)	// Temporary fix
	{
		// Try Rainmeter.ini first
		m_SkinPath.assign(buffer, len);
		PathUtil::ExpandEnvironmentVariables(m_SkinPath);
		PathUtil::AppendBackslashIfMissing(m_SkinPath);
	}
	else if (bDefaultIniLocation && SUCCEEDED(SHGetFolderPath(nullptr, CSIDL_MYDOCUMENTS, nullptr, SHGFP_TYPE_CURRENT, buffer)))
	{
		// Use My Documents/Rainmeter/Skins
		m_SkinPath = buffer;
		m_SkinPath += L"\\Rainmeter\\";
		CreateDirectory(m_SkinPath.c_str(), nullptr);
		m_SkinPath += L"Skins\\";

		WritePrivateProfileString(L"Rainmeter", L"SkinPath", m_SkinPath.c_str(), iniFile);
	}
	else
	{
		m_SkinPath = m_Path + L"Skins\\";
	}

	// Create user skins, layouts, addons, and plugins folders if needed
	CreateComponentFolders(bDefaultIniLocation);

	clearBuffer();

	// Build.bat will write to the BUILD_TIME macro when the installer is created.
	// For local builds, just use the current date and time as the build time.
#ifdef BUILD_TIME
	m_BuildTime = BUILD_TIME;
#else
	// For local builds, just create use the current date/time
	if (m_BuildTime.empty())
	{
		time_t now;
		time(&now);
		WCHAR timestamp[MAX_PATH];
		wcsftime(timestamp, MAX_PATH, L"%F %T", gmtime(&now));
		m_BuildTime = timestamp;
	}
#endif // BUILD_TIME

	WCHAR lang[LOCALE_NAME_MAX_LENGTH];
	GetLocaleInfo(m_ResourceLCID, LOCALE_SENGLISHLANGUAGENAME, lang, _countof(lang));
	LogNoticeF(L"Rainmeter %s.%i (%s)", APPVERSION, revision_number, APPBITS);
	LogNoticeF(L"Language: %s (%lu)", lang, m_ResourceLCID);
	LogNoticeF(L"Build time: %s", m_BuildTime.c_str());

	LogNoticeF(L"%s - %s (%hu)",
		GetPlatform().GetFriendlyName().c_str(),
		GetPlatform().GetUserLanguage().c_str(),
		GetUserDefaultUILanguage());

	if (!encodingMsg.empty())
	{
		// Log information about any encoding changes to |iniFile|
		LogNotice(encodingMsg.c_str());
	}

	LogNoticeF(L"Path: %s", m_Path.c_str());
	LogNoticeF(L"SkinPath: %s", m_SkinPath.c_str());
	LogNoticeF(L"SettingsPath: %s", m_SettingsPath.c_str());
	LogNoticeF(L"IniFile: %s", iniFile);

	// Test that the Rainmeter.ini file is writable
	TestSettingsFile(bDefaultIniLocation);

	System::Initialize(m_Instance);

	MeasureNet::InitializeStatic();
	MeasureCPU::InitializeStatic();
	MeterString::InitializeStatic();

	// Tray must exist before skins are read
	m_TrayIcon = new TrayIcon();
	m_TrayIcon->Initialize();

	ReloadSettings();

	// Initialize "Game mode" and read its settings
	GetGameMode().Initialize();

	if (m_SkinRegistry.IsEmpty())
	{
		std::wstring error = GetFormattedString(ID_STR_NOAVAILABLESKINS, m_SkinPath.c_str());
		ShowMessage(nullptr, error.c_str(), MB_OK | MB_ICONERROR);
	}

	// Rainmeter safe start
	// Note: This copies the default illustro skins and layout (if needed) without overwriting any
	//  changes the user has made to the skins or layout.
	if (!iniFileCreated && (safeStart || IsCtrlKeyDown()))
	{
		int result = MessageBox(
			nullptr,
			GetString(ID_STR_SAFESTART_MESSAGE),
			GetString(ID_STR_SAFESTART_TITLE),
			MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON1 | MB_TOPMOST);
		if (result == IDYES)
		{
			// Copy the default illustro layout if needed
			if (System::CopyFilesWithNoCollisions(GetDefaultLayoutPath(), GetLayoutPath()))
			{
				layout = L"\"illustro default\"";
			}

			// Copy any default illustro skins if needed
			System::CopyFilesWithNoCollisions(GetDefaultSkinPath(), GetSkinPath());
		}
	}

	ResetStats();
	ReadStats();

	// Change the work area if necessary
	if (m_DesktopWorkAreaChanged)
	{
		UpdateDesktopWorkArea(false);
	}

	bool layoutLoaded = false;
	if (layout)
	{
		std::vector<std::wstring> args = CommandHandler::ParseString(layout);
		layoutLoaded = (args.size() == 1 && LoadLayout(args[0]));
	}

	if (!layoutLoaded && GetGameMode().IsDisabled())
	{
		ActivateActiveSkins();
	}

	if (dataFileCreated)
	{
		m_TrayIcon->ShowWelcomeNotification();
	}
	else if (!m_DisableVersionCheck)
	{
		GetUpdater().CheckForUpdates(!m_DisableAutoUpdate);
	}

	return 0;	// All is OK
}

void Rainmeter::Finalize()
{
	KillTimer(m_Window, TIMER_NETSTATS);

	GetGameMode().ForceExit();

	DeleteAllUnmanagedSkins();
	DeleteAllSkins();
	DeleteAllUnmanagedSkins();  // Redelete unmanaged windows caused by OnCloseAction

	delete m_TrayIcon;
	m_TrayIcon = nullptr;

	System::Finalize();

	MeasureNet::UpdateIFTable();
	MeasureNet::UpdateStats();
	WriteStats(true);

	MeasureNet::FinalizeStatic();
	MeasureCPU::FinalizeStatic();
	MeterString::FinalizeStatic();

	Gfx::Canvas::Finalize();

	// Change the work area back
	if (m_DesktopWorkAreaChanged)
	{
		UpdateDesktopWorkArea(true);
	}

	if (m_ResourceInstance)
	{
		FreeLibrary(m_ResourceInstance);
		m_ResourceInstance = nullptr;
	}

	if (m_Mutex)
	{
		ReleaseMutex(m_Mutex);
		m_Mutex = nullptr;
	}

	if (m_Window)
	{
		DestroyWindow(m_Window);
		m_Window = nullptr;
	}

	UnregisterClass(RAINMETER_CLASS_NAME, m_Instance);
}

void Rainmeter::RestartRainmeter()
{
	// Make sure to call this function after m_Path is initialized in Rainmeter::Initialize
	std::wstring restart = m_Path;
	restart += L"RestartRainmeter.exe";
	CommandHandler::RunFile(restart.c_str());
}

bool Rainmeter::IsAlreadyRunning()
{
	typedef struct
	{
		ULONG i[2];
		ULONG buf[4];
		unsigned char in[64];
		unsigned char digest[16];
	} MD5_CTX;

	typedef void (WINAPI * FPMD5INIT)(MD5_CTX* context);
	typedef void (WINAPI * FPMD5UPDATE)(MD5_CTX* context, const unsigned char* input, unsigned int inlen);
	typedef void (WINAPI * FPMD5FINAL)(MD5_CTX* context);

	bool alreadyRunning = false;

	// Create MD5 digest from command line
	HMODULE cryptDll = System::RmLoadLibrary(L"cryptdll.dll");
	if (cryptDll)
	{
		FPMD5INIT MD5Init = (FPMD5INIT)GetProcAddress(cryptDll, "MD5Init");
		FPMD5UPDATE MD5Update = (FPMD5UPDATE)GetProcAddress(cryptDll, "MD5Update");
		FPMD5FINAL MD5Final = (FPMD5FINAL)GetProcAddress(cryptDll, "MD5Final");
		if (MD5Init && MD5Update && MD5Final)
		{
			std::wstring data = m_IniFile;
			_wcsupr(&data[0]);

			MD5_CTX ctx = { 0 };
			MD5Init(&ctx);
			MD5Update(&ctx, (LPBYTE)&data[0], (UINT)data.length() * sizeof(WCHAR));
			MD5Final(&ctx);
			FreeLibrary(cryptDll);

			// Convert MD5 digest to mutex string (e.g. "Rainmeter0123456789abcdef0123456789abcdef")
			const WCHAR hexChars[] = L"0123456789abcdef";
			WCHAR mutexName[64] = L"Rainmeter";
			WCHAR* pos = mutexName + (_countof(L"Rainmeter") - 1);
			for (size_t i = 0; i < 16; ++i)
			{
				*(pos++) = hexChars[ctx.digest[i] >> 4];
				*(pos++) = hexChars[ctx.digest[i] & 0xF];
			}
			*pos = L'\0';

			m_Mutex = CreateMutex(nullptr, FALSE, mutexName);
			if (GetLastError() == ERROR_ALREADY_EXISTS)
			{
				alreadyRunning = true;
				m_Mutex = nullptr;
			}
		}

		FreeLibrary(cryptDll);
	}

	return alreadyRunning;
}

int Rainmeter::MessagePump()
{
	MSG msg;
	BOOL ret;

	// Run the standard window message loop
	while ((ret = GetMessage(&msg, nullptr, 0, 0)) != 0)
	{
		if (ret == -1)
		{
			break;
		}

		if (!Dialog::HandleMessage(msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	return (int)msg.wParam;
}

LRESULT CALLBACK Rainmeter::MainWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_DESTROY:
		PlaySound(nullptr, nullptr, SND_PURGE);  // Stop any sounds. See CommandHandler::ExecuteCommand
		PostQuitMessage(0);
		break;

	case WM_COPYDATA:
		{
			COPYDATASTRUCT* cds = (COPYDATASTRUCT*)lParam;
			if (cds)
			{
				const WCHAR* data = (const WCHAR*)cds->lpData;
				if (cds->dwData == 1 && (cds->cbData > 0))
				{
					// Disallow any bangs in manual "Game mode" except any overrides. See GameMode::GetBangOverrideList
					if (!GetGameMode().IsEnabled() || GetGameMode().HasBangOverride(data))
					{
						GetRainmeter().DelayedExecuteCommand(data);
					}
				}
			}
		}
		break;

	case WM_TIMER:
		if (wParam == TIMER_NETSTATS)
		{
			MeasureNet::UpdateIFTable();
			MeasureNet::UpdateStats();
			GetRainmeter().WriteStats(false);
		}
		else
		{
			GetGameMode().OnTimerEvent(wParam);
		}
		break;

	case WM_RAINMETER_DELAYED_REFRESH_ALL:
		GetRainmeter().RefreshAll();
		break;

	case WM_RAINMETER_DELAYED_EXECUTE:
		if (!wParam || GetRainmeter().HasSkin((Skin*)wParam))
		{
			WCHAR* bang = (WCHAR*)lParam;
			GetRainmeter().ExecuteCommand(bang, (Skin*)wParam);
			free(bang);  // _wcsdup()
		}
		break;

	case WM_RAINMETER_EXECUTE:
		if (GetRainmeter().HasSkin((Skin*)wParam))
		{
			GetRainmeter().ExecuteCommand((const WCHAR*)lParam, (Skin*)wParam);
		}
		break;

	default:
		return DefWindowProc(hWnd, uMsg, wParam, lParam);
	}

	return 0;
}

void Rainmeter::SetNetworkStatisticsTimer()
{
	static bool set = SetTimer(m_Window, TIMER_NETSTATS, INTERVAL_NETSTATS, nullptr) != 0;
}

void Rainmeter::CreateOptionsFile()
{
	CreateDirectory(m_SettingsPath.c_str(), nullptr);

	std::wstring defaultIni = GetDefaultLayoutPath();
	defaultIni += L"illustro default\\Rainmeter.ini";
	System::CopyFiles(defaultIni, m_IniFile);
}

void Rainmeter::CreateDataFile()
{
	std::wstring tmpSz = m_SettingsPath + L"Plugins.ini";

	const WCHAR* pluginsFile = tmpSz.c_str();
	const WCHAR* dataFile = m_DataFile.c_str();

	if (_waccess_s(pluginsFile, 0) == 0)
	{
		MoveFile(pluginsFile, dataFile);
	}
	else
	{
		// Create empty file
		HANDLE file = CreateFile(dataFile, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE)
		{
			CloseHandle(file);
		}
	}
}

void Rainmeter::CreateComponentFolders(bool defaultIniLocation)
{
	std::wstring path;

	if (CreateDirectory(m_SkinPath.c_str(), nullptr))
	{
		// Folder just created, so copy default skins there
		std::wstring from = GetDefaultSkinPath();
		from += L"*.*";
		System::CopyFiles(from, m_SkinPath);
	}
	else
	{
		path = m_SkinPath;
		path += L"Backup";
		if (_waccess_s(path.c_str(), 0) == 0)
		{
			std::wstring newPath = m_SkinPath + L"@Backup";
			MoveFile(path.c_str(), newPath.c_str());
		}
	}

	path = m_SkinPath;
	path += L"@Vault\\";
	if (CreateDirectory(path.c_str(), nullptr))
	{
		path += L"Plugins\\";
		CreateDirectory(path.c_str(), nullptr);
	}

	path = GetLayoutPath();
	if (_waccess_s(path.c_str(), 0) != 0)
	{
		std::wstring themesPath = m_SettingsPath + L"Themes";
		if (_waccess_s(themesPath.c_str(), 0) == 0)
		{
			// Migrate Themes into Layouts for backwards compatibility and rename
			// Rainmeter.thm to Rainmeter.ini and RainThemes.bmp to Wallpaper.bmp.
			MoveFile(themesPath.c_str(), path.c_str());

			path += L'*';  // For FindFirstFile.
			WIN32_FIND_DATA fd = { 0 };
			HANDLE hFind = FindFirstFile(path.c_str(), &fd);
			path.pop_back();  // Remove '*'.

			if (hFind != INVALID_HANDLE_VALUE)
			{
				do
				{
					if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY &&
						PathUtil::IsDotOrDotDot(fd.cFileName))
					{
						std::wstring layoutFolder = path + fd.cFileName;
						layoutFolder += L'\\';

						std::wstring file = layoutFolder + L"Rainmeter.thm";
						if (_waccess_s(file.c_str(), 0) == 0)
						{
							std::wstring newFile = layoutFolder + L"Rainmeter.ini";
							MoveFile(file.c_str(), newFile.c_str());
						}

						file = layoutFolder + L"RainThemes.bmp";
						if (_waccess_s(file.c_str(), 0) == 0)
						{
							std::wstring newFile = layoutFolder + L"Wallpaper.bmp";
							MoveFile(file.c_str(), newFile.c_str());
						}
					}
				}
				while (FindNextFile(hFind, &fd));

				FindClose(hFind);
			}
		}
		else
		{
			std::wstring from = GetDefaultLayoutPath();
			if (_waccess_s(from.c_str(), 0) == 0)
			{
				System::CopyFiles(from, m_SettingsPath);
			}
		}
	}
	else
	{
		path += L"Backup";
		if (_waccess_s(path.c_str(), 0) == 0)
		{
			std::wstring newPath = GetLayoutPath();
			newPath += L"@Backup";
			MoveFile(path.c_str(), newPath.c_str());
		}
	}

	if (defaultIniLocation)
	{
		path = GetUserPluginPath();
		if (_waccess_s(path.c_str(), 0) != 0)
		{
			std::wstring from = GetDefaultPluginPath();
			if (_waccess_s(from.c_str(), 0) == 0)
			{
				System::CopyFiles(from, m_SettingsPath);
			}
		}

		path = GetAddonPath();
		if (_waccess_s(path.c_str(), 0) != 0)
		{
			std::wstring from = GetDefaultAddonPath();
			if (_waccess_s(from.c_str(), 0) == 0)
			{
				System::CopyFiles(from, m_SettingsPath);
			}
		}

		path = m_SettingsPath;
		path += L"Rainmeter.exe";
		const WCHAR* pathSz = path.c_str();
		if (_waccess_s(pathSz, 0) != 0)
		{
			// Create a hidden stub Rainmeter.exe into SettingsPath for old addon
			// using relative path to Rainmeter.exe
			std::wstring from = m_Path + L"Rainmeter.exe";
			System::CopyFiles(from, path);

			// Get rid of all resources from the stub executable
			HANDLE stub = BeginUpdateResource(pathSz, TRUE);

			// Add the manifest of Rainmeter.dll to the stub
			HRSRC manifest = FindResource(m_Instance, MAKEINTRESOURCE(2), RT_MANIFEST);
			if (manifest)
			{
				DWORD manifestSize = SizeofResource(m_Instance, manifest);
				HGLOBAL	manifestLoad = LoadResource(m_Instance, manifest);
				if (manifestLoad)
				{
					void* manifestLoadData = LockResource(manifestLoad);
					if (manifestLoadData)
					{
						LANGID langID = MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT);
						UpdateResource(stub, RT_MANIFEST, MAKEINTRESOURCE(1), langID, manifestLoadData, manifestSize);
					}
				}
			}

			EndUpdateResource(stub, FALSE);
			SetFileAttributes(pathSz, FILE_ATTRIBUTE_HIDDEN);
		}
	}
}

void Rainmeter::ReloadSettings()
{
	ReadFavorites();
	ScanForSkins();
	ScanForLayouts();
	ReadGeneralSettings(m_IniFile);
}

void Rainmeter::EditSettings()
{
	std::wstring file = L'"' + m_IniFile;
	file += L'"';
	CommandHandler::RunFile(m_SkinEditor.c_str(), file.c_str());
}

void Rainmeter::EditSkinFile(const std::wstring& name, const std::wstring& iniFile)
{
	std::wstring args = L'"' + m_SkinPath;
	args += name;
	args += L'\\';
	args += iniFile;
	args += L'"';
	CommandHandler::RunFile(m_SkinEditor.c_str(), args.c_str());
}

void Rainmeter::OpenSkinFolder(const std::wstring& name)
{
	std::wstring folderPath = m_SkinPath + name;
	CommandHandler::RunFile(folderPath.c_str());
}

bool Rainmeter::DoesSkinHaveSettings(const std::wstring& folderPath)
{
	WCHAR* buffer = new WCHAR[SHRT_MAX];
	const bool hasSettings = (GetPrivateProfileSection(folderPath.c_str(), buffer, SHRT_MAX, m_IniFile.c_str()) > 0UL);
	delete [] buffer;
	buffer = nullptr;

	if (!hasSettings)
	{
		// Since there are no settings for this skin in Rainmeter.ini, attempt to insert
		// a empty line between the last defined section, and the new section for this skin.

		HANDLE hFile = CreateFile(m_IniFile.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile != INVALID_HANDLE_VALUE)
		{
			LARGE_INTEGER fileSize = { 0 };
			if (GetFileSizeEx(hFile, &fileSize) != FALSE && fileSize.QuadPart > 4LL)
			{
				LARGE_INTEGER newSize = { 0 };
				newSize.QuadPart = -4LL;

				LARGE_INTEGER newPtr = { 0 };
				while (SetFilePointerEx(hFile, newSize, &newPtr, FILE_END) == TRUE)
				{
					WCHAR lastTwoChars[2] = { 0 };
					DWORD bytesRead = 0UL;
					if (ReadFile(hFile, lastTwoChars, 4UL, &bytesRead, nullptr) == FALSE)
					{
						break;
					}

					if (bytesRead > 0 && lastTwoChars[0] != L'\r' && lastTwoChars[1] != L'\n')
					{
						break;  // Found the last non newline character sequence "\r\n"
					}

					fileSize.QuadPart -= 4LL;

					if (SetFilePointerEx(hFile, fileSize, &newPtr, FILE_BEGIN) == FALSE)
					{
						break;
					}

					if (SetEndOfFile(hFile) == FALSE)
					{
						break;
					}
				}

				// Insert skin entry
				std::wstring section = L"\r\n\r\n[";
				section += folderPath;
				section += L"]\r\nActive=0\r\n";  // The "Active" setting is set later

				// If the following WriteFile fails, there will be no space between sections, however,
				// WritePrivateProfileSection will automatically create the section at the end of the file
				DWORD bytesWritten = 0UL;
				WriteFile(hFile, (LPCVOID)section.c_str(), (DWORD)(section.size() * 2UL), &bytesWritten, nullptr);
			}
			CloseHandle(hFile);
		}
	}

	return hasSettings;
}

void Rainmeter::ActivateActiveSkins()
{
	std::multimap<int, int>::const_iterator iter = m_SkinOrders.begin();
	for ( ; iter != m_SkinOrders.end(); ++iter)
	{
		const SkinRegistry::Folder& skinFolder = m_SkinRegistry.GetFolder((*iter).second);
		if (skinFolder.active > 0 && skinFolder.active <= (uint16_t)skinFolder.files.size())
		{
			ActivateSkin((*iter).second, skinFolder.active - 1);
		}
	}
}

/*
** Activates the skin, or, if it is already active, the next variant of the skin. Returns true
** if the skin was activated (or was already active).
*/
bool Rainmeter::ActivateSkin(const std::wstring& folderPath)
{
	const int index = m_SkinRegistry.FindFolderIndex(folderPath);
	if (index != -1)
	{
		const SkinRegistry::Folder& skinFolder = m_SkinRegistry.GetFolder(index);
		if (!(skinFolder.active == 1 && skinFolder.files.size() == 1))
		{
			// Activate the next index.
			ActivateSkin(
				index, (skinFolder.active < (int16_t)skinFolder.files.size()) ? skinFolder.active : 0);
		}

		return true;
	}

	return false;
}

/*
** Activates the skin, or, if it is already active, the next variant of the skin. Returns true
** if the skin was activated (or was already active).
*/
bool Rainmeter::ActivateSkin(const std::wstring& folderPath, const std::wstring& file)
{
	const SkinRegistry::Indexes indexes = m_SkinRegistry.FindIndexes(folderPath, file);
	if (indexes.IsValid())
	{
		ActivateSkin(indexes.folder, indexes.file);
		return true;
	}

	return false;
}

void Rainmeter::ActivateSkin(int folderIndex, int fileIndex)
{
	if (folderIndex >= 0 && folderIndex < m_SkinRegistry.GetFolderCount() &&
		fileIndex >= 0 && fileIndex < (int)m_SkinRegistry.GetFolder(folderIndex).files.size())
	{
		auto& skinFolder = m_SkinRegistry.GetFolder(folderIndex);
		const std::wstring& file = skinFolder.files[fileIndex].filename;
		const WCHAR* fileSz = file.c_str();

		std::wstring folderPath = m_SkinRegistry.GetFolderPath(folderIndex);

		// Verify that the skin is not already active
		std::map<std::wstring, Skin*>::const_iterator iter = m_Skins.find(folderPath);
		if (iter != m_Skins.end())
		{
			if (wcscmp(((*iter).second)->GetFileName().c_str(), fileSz) == 0)
			{
				LogWarningF((*iter).second, L"!ActivateConfig: \"%s\" is already active", folderPath.c_str());
				return;
			}
			else
			{
				// Deactivate the existing skin
				DeactivateSkin((*iter).second, folderIndex);
			}
		}

		// Verify whether the ini-file exists
		std::wstring skinIniPath = m_SkinPath + folderPath;
		skinIniPath += L'\\';
		skinIniPath += file;

		if (_waccess_s(skinIniPath.c_str(), 0) != 0)
		{
			std::wstring message = GetFormattedString(ID_STR_UNABLETOACTIVATESKIN, folderPath.c_str(), fileSz);
			ShowMessage(nullptr, message.c_str(), MB_OK | MB_ICONEXCLAMATION);
			return;
		}

		// Verify whether the skin config has an entry in the settings file
		const bool hasSettings = DoesSkinHaveSettings(folderPath);

		if (skinFolder.active != fileIndex + 1)
		{
			// Write only if changed.
			skinFolder.active = fileIndex + 1;
			WriteActive(folderPath, fileIndex);
		}

		// The tray icon is shown if no skins are active regardless of
		// of TrayIcon setting in Rainmeter.ini. Now that a skin is to
		// be active, we either turn it off or leave it on depending on
		// the TrayIcon setting in Rainmeter.ini.
		if (m_Skins.empty())
		{
			m_TrayIcon->SetTrayIcon(m_TrayIcon->IsTrayIconEnabled());
		}

		CreateSkin(folderPath, file, hasSettings);
	}
}

void Rainmeter::DeactivateSkin(Skin* skin, int folderIndex, bool save)
{
	if (folderIndex >= 0 && folderIndex < m_SkinRegistry.GetFolderCount())
	{
		m_SkinRegistry.GetFolder(folderIndex).active = 0;	// Deactivate the skin
	}
	else if (folderIndex == -1 && skin)
	{
		SkinRegistry::Folder* folder = m_SkinRegistry.FindFolder(skin->GetFolderPath());
		if (folder)
		{
			folder->active = 0;
		}
	}

	if (skin)
	{
		if (save)
		{
			// Disable the skin in the ini-file
			WriteActive(skin->GetFolderPath(), -1);
		}

		skin->Deactivate();

		ShowTrayIconIfNecessary();
	}
}

void Rainmeter::ToggleSkin(int folderIndex, int fileIndex)
{
	if (folderIndex >= 0 && folderIndex < m_SkinRegistry.GetFolderCount() &&
		fileIndex >= 0 && fileIndex < (int)m_SkinRegistry.GetFolder(folderIndex).files.size())
	{
		if (m_SkinRegistry.GetFolder(folderIndex).active == fileIndex + 1)
		{
			Skin* skin = GetSkin(m_SkinRegistry.GetFolderPath(folderIndex));
			DeactivateSkin(skin, folderIndex);
		}
		else
		{
			ActivateSkin(folderIndex, fileIndex);
		}
	}
}

void Rainmeter::ToggleSkinWithID(UINT id)
{
	const SkinRegistry::Indexes indexes = m_SkinRegistry.FindIndexesForID(id);
	if (indexes.IsValid())
	{
		ToggleSkin(indexes.folder, indexes.file);
	}
}

void Rainmeter::SetSkinPath(const std::wstring& skinPath)
{
	WritePrivateProfileString(L"Rainmeter", L"SkinPath", skinPath.c_str(), m_IniFile.c_str());
}

void Rainmeter::SetSkinEditor(const std::wstring& path)
{
	if (!path.empty())
	{
		m_SkinEditor = path;
		WritePrivateProfileString(L"Rainmeter", L"ConfigEditor", path.c_str(), m_IniFile.c_str());

		// Update #CONFIGEDITOR# built-in variable in all skins
		for (auto& iter : m_Skins)
		{
			iter.second->GetParser().SetBuiltInVariable(L"CONFIGEDITOR", m_SkinEditor);
		}
	}
}

void Rainmeter::SetHardwareAccelerated(bool hardwareAccelerated)
{
	m_HardwareAccelerated = hardwareAccelerated;
	WritePrivateProfileString(L"Rainmeter", L"HardwareAcceleration", m_HardwareAccelerated ? L"1" : L"0", m_IniFile.c_str());
}

void Rainmeter::WriteActive(const std::wstring& folderPath, int fileIndex)
{
	WCHAR buffer[32] = { 0 };
	_itow_s(fileIndex + 1, buffer, 10);

	DoesSkinHaveSettings(folderPath);

	WritePrivateProfileString(folderPath.c_str(), L"Active", buffer, m_IniFile.c_str());
}

void Rainmeter::CreateSkin(const std::wstring& folderPath, const std::wstring& file, bool hasSettings)
{
	Skin* skin = new Skin(folderPath, file, hasSettings);

	// Note: May modify existing key
	m_Skins[folderPath] = skin;

	skin->Initialize();

	DialogAbout::UpdateSkins();
	DialogManage::UpdateSkins(skin);
}

void Rainmeter::DeleteAllSkins()
{
	auto it = m_Skins.cbegin();
	while (it != m_Skins.cend())
	{
		Skin* skin = (*it).second;
		m_Skins.erase(it);  // Remove before deleting Skin

		DialogManage::UpdateSkins(skin, true);
		delete skin;
		skin = nullptr;

		// Get next valid iterator (Fix for iterator invalidation caused by OnCloseAction)
		it = m_Skins.cbegin();
	}

	m_Skins.clear();
	DialogAbout::UpdateSkins();
}

void Rainmeter::DeleteAllUnmanagedSkins()
{
	for (auto it = m_UnmanagedSkins.cbegin(); it != m_UnmanagedSkins.cend(); ++it)
	{
		delete (*it);
	}

	m_UnmanagedSkins.clear();
}

/*
** Removes the skin from m_Skins. The skin should delete itself.
**
*/
void Rainmeter::RemoveSkin(Skin* skin)
{
	for (auto it = m_Skins.cbegin(); it != m_Skins.cend(); ++it)
	{
		if ((*it).second == skin)
		{
			m_Skins.erase(it);
			DialogManage::UpdateSkins(skin, true);
			DialogAbout::UpdateSkins();
			break;
		}
	}
}

/*
** Adds the skin to m_UnmanagedSkins. The skin should remove itself by calling RemoveUnmanagedSkin().
**
*/
void Rainmeter::AddUnmanagedSkin(Skin* skin)
{
	for (auto it = m_UnmanagedSkins.cbegin(); it != m_UnmanagedSkins.cend(); ++it)
	{
		if ((*it) == skin)  // already added
		{
			return;
		}
	}

	m_UnmanagedSkins.push_back(skin);
}

void Rainmeter::RemoveUnmanagedSkin(Skin* skin)
{
	for (auto it = m_UnmanagedSkins.cbegin(); it != m_UnmanagedSkins.cend(); ++it)
	{
		if ((*it) == skin)
		{
			m_UnmanagedSkins.erase(it);
			break;
		}
	}
}

bool Rainmeter::HasSkin(const Skin* skin) const
{
	for (auto it = m_Skins.begin(); it != m_Skins.end(); ++it)
	{
		if ((*it).second == skin)
		{
			return true;
		}
	}

	return false;
}

Skin* Rainmeter::GetSkin(std::wstring folderPath)
{
	// Remove any leading and trailing slashes
	PathUtil::RemoveLeadingAndTrailingBackslash(folderPath);

	const WCHAR* folderSz = folderPath.c_str();
	std::map<std::wstring, Skin*>::const_iterator iter = m_Skins.begin();
	for (; iter != m_Skins.end(); ++iter)
	{
		if (_wcsicmp((*iter).first.c_str(), folderSz) == 0)
		{
			return (*iter).second;
		}
	}

	return nullptr;
}

Skin* Rainmeter::GetSkinByINI(const std::wstring& ini_searching)
{
	if (_wcsnicmp(m_SkinPath.c_str(), ini_searching.c_str(), m_SkinPath.length()) == 0)
	{
		const std::wstring config_searching = ini_searching.substr(m_SkinPath.length());

		std::map<std::wstring, Skin*>::const_iterator iter = m_Skins.begin();
		for (; iter != m_Skins.end(); ++iter)
		{
			std::wstring config_current = (*iter).second->GetFolderPath() + L'\\';
			config_current += (*iter).second->GetFileName();

			if (_wcsicmp(config_current.c_str(), config_searching.c_str()) == 0)
			{
				return (*iter).second;
			}
		}
	}

	return nullptr;
}

Skin* Rainmeter::GetSkin(HWND hwnd)
{
	std::map<std::wstring, Skin*>::const_iterator iter = m_Skins.begin();
	for (; iter != m_Skins.end(); ++iter)
	{
		if ((*iter).second->GetWindow() == hwnd)
		{
			return (*iter).second;
		}
	}

	return nullptr;
}

void Rainmeter::GetSkinsByLoadOrder(std::multimap<int, Skin*>& windows, const std::wstring& group)
{
	std::map<std::wstring, Skin*>::const_iterator iter = m_Skins.begin();
	for (; iter != m_Skins.end(); ++iter)
	{
		Skin* skin = (*iter).second;
		if (skin && (group.empty() || skin->BelongsToGroup(group)))
		{
			windows.insert(std::pair<int, Skin*>(GetLoadOrder((*iter).first), skin));
		}
	}
}

void Rainmeter::SetLoadOrder(int folderIndex, int order)
{
	std::multimap<int, int>::iterator iter = m_SkinOrders.begin();
	for ( ; iter != m_SkinOrders.end(); ++iter)
	{
		if ((*iter).second == folderIndex)  // already exists
		{
			if ((*iter).first != order)
			{
				m_SkinOrders.erase(iter);
				break;
			}
			else
			{
				return;
			}
		}
	}

	m_SkinOrders.insert(std::pair<int, int>(order, folderIndex));
}

int Rainmeter::GetLoadOrder(const std::wstring& folderPath)
{
	const int index = m_SkinRegistry.FindFolderIndex(folderPath);
	if (index != -1)
	{
		std::multimap<int, int>::const_iterator iter = m_SkinOrders.begin();
		for ( ; iter != m_SkinOrders.end(); ++iter)
		{
			if ((*iter).second == index)
			{
				return (*iter).first;
			}
		}
	}

	// LoadOrder not specified
	return 0;
}

/*
** Scans all the subfolders and locates the ini-files.
*/
void Rainmeter::ScanForSkins()
{
	m_SkinRegistry.Populate(m_SkinPath, m_Favorites);
	m_SkinOrders.clear();
}

/*
** Scans the given folder for layouts
*/
void Rainmeter::ScanForLayouts()
{
	m_Layouts.clear();

	WIN32_FIND_DATA fileData = { 0 };		// Data structure describes the file found
	HANDLE hSearch = nullptr;				// Search handle returned by FindFirstFile

	// Scan for folders
	std::wstring folders = GetLayoutPath();
	folders += L'*';

	hSearch = FindFirstFileEx(
		folders.c_str(),
		FindExInfoBasic,
		&fileData,
		FindExSearchNameMatch,
		nullptr,
		0);

	if (hSearch != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY &&
				!PathUtil::IsDotOrDotDot(fileData.cFileName))
			{
				m_Layouts.push_back(fileData.cFileName);
			}
		}
		while (FindNextFile(hSearch, &fileData));

		FindClose(hSearch);
	}

	DialogManage::UpdateLayouts();
}

void Rainmeter::ReadFavorites()
{
	m_Favorites.clear();

	// Load from [Favorites] section of Rainmeter.data

	if (!m_DataFile.empty())
	{
		WCHAR favorite[MAX_LINE_LENGTH];
		WCHAR buffer[128];
		int i = 0;

		do
		{
			_snwprintf_s(buffer, _TRUNCATE, L"Favorite%i", ++i);
			DWORD res = GetPrivateProfileString(L"Favorites", buffer, L"", favorite, MAX_LINE_LENGTH, m_DataFile.c_str());

			if (res > 4)
			{
				m_Favorites.emplace_back(favorite);
			}
		} while (*favorite);
	}
}

void Rainmeter::ExecuteBang(const WCHAR* bang, std::vector<std::wstring>& args, Skin* skin)
{
	m_CommandHandler.ExecuteBang(bang, args, skin);
}

/*
** Runs the given command or bang
**
*/
void Rainmeter::ExecuteCommand(const WCHAR* command, Skin* skin, bool multi)
{
	m_CommandHandler.ExecuteCommand(command, skin, multi);
}

/*
** Runs the given command or bang (sent from an Action)
**
*/
void Rainmeter::ExecuteActionCommand(const WCHAR* command, Section* section)
{
	Skin* skin = nullptr;
	if (section && (skin = section->GetSkin()))
	{
		skin->SetCurrentActionSection(section);
		m_CommandHandler.ExecuteCommand(command, skin);
		skin->ResetCurrentActionSection();
		return;
	}

	m_CommandHandler.ExecuteCommand(command, skin);
}

/*
** Executes command when current processing is done.
**
*/
void Rainmeter::DelayedExecuteCommand(const WCHAR* command, Skin* skin)
{
	WCHAR* bang = _wcsdup(command);
	PostMessage(m_Window, WM_RAINMETER_DELAYED_EXECUTE, (WPARAM)skin, (LPARAM)bang);
}

/*
** Reads the general settings from the Rainmeter.ini file
**
*/
void Rainmeter::ReadGeneralSettings(const std::wstring& iniFile)
{
	// Force the reload of system cursors
	SystemParametersInfo(SPI_SETCURSORS, 0U, nullptr, 0U);

	WCHAR buffer[MAX_PATH];

	// Clear old settings
	m_DesktopWorkAreas.clear();

	ConfigParser parser;
	parser.Initialize(iniFile, nullptr, nullptr);

	m_Debug = parser.ReadBool(L"Rainmeter", L"Debug", false);
	
	// Read Logging settings
	Logger& logger = GetLogger();
	const bool logging = parser.ReadBool(L"Rainmeter", L"Logging", false);
	logger.SetLogToFile(logging);
	if (logging)
	{
		logger.StartLogFile();
	}

	if (m_TrayIcon)
	{
		m_TrayIcon->ReadOptions(parser);
	}

	m_GlobalOptions.netInSpeed = parser.ReadFloat(L"Rainmeter", L"NetInSpeed", 0.0);
	m_GlobalOptions.netOutSpeed = parser.ReadFloat(L"Rainmeter", L"NetOutSpeed", 0.0);

	m_DisableDragging = parser.ReadBool(L"Rainmeter", L"DisableDragging", false);
	m_DisableRDP = parser.ReadBool(L"Rainmeter", L"DisableRDP", false);

	m_DefaultSelectedColor = parser.ReadColor(L"Rainmeter", L"SelectedColor", D2D1::ColorF(D2D1::ColorF::Red, 90.0f / 255.0f));  // RGBA: 255,0,0,90

	m_SkinEditor = parser.ReadString(L"Rainmeter", L"ConfigEditor", L"");
	if (m_SkinEditor.empty())
	{
		// Get the program path associated with .ini files
		DWORD cchOut = MAX_PATH;
		HRESULT hr = AssocQueryString(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, L".ini", L"open", buffer, &cchOut);
		m_SkinEditor = (SUCCEEDED(hr) && cchOut > 0) ? buffer : L"Notepad";
	}

	if (m_Debug)
	{
		LogNoticeF(L"ConfigEditor: %s", m_SkinEditor.c_str());
	}

	m_TrayExecuteR = parser.ReadString(L"Rainmeter", L"TrayExecuteR", L"", false);
	m_TrayExecuteM = parser.ReadString(L"Rainmeter", L"TrayExecuteM", L"", false);
	m_TrayExecuteDR = parser.ReadString(L"Rainmeter", L"TrayExecuteDR", L"", false);
	m_TrayExecuteDM = parser.ReadString(L"Rainmeter", L"TrayExecuteDM", L"", false);

	m_DisableVersionCheck = parser.ReadBool(L"Rainmeter", L"DisableVersionCheck", false);
	m_DisableAutoUpdate = parser.ReadBool(L"Rainmeter", L"DisableAutoUpdate", false);

	const std::wstring& area = parser.ReadString(L"Rainmeter", L"DesktopWorkArea", L"");
	if (!area.empty())
	{
		m_DesktopWorkAreas[0] = parser.ParseRECT(area.c_str());
		m_DesktopWorkAreaChanged = true;
	}

	const size_t monitorCount = System::GetMonitorCount();
	for (UINT i = 1; i <= monitorCount; ++i)
	{
		_snwprintf_s(buffer, _TRUNCATE, L"DesktopWorkArea@%i", (int)i);
		const std::wstring& area = parser.ReadString(L"Rainmeter", buffer, L"");
		if (!area.empty())
		{
			m_DesktopWorkAreas[i] = parser.ParseRECT(area.c_str());
			m_DesktopWorkAreaChanged = true;
		}
	}

	m_DesktopWorkAreaType = parser.ReadBool(L"Rainmeter", L"DesktopWorkAreaType", false);

	m_NormalStayDesktop = parser.ReadBool(L"Rainmeter", L"NormalStayDesktop", true);

	bool hasActiveSkins = false;
	for (auto iter = parser.GetSections().cbegin(); iter != parser.GetSections().end(); ++iter)
	{
		const WCHAR* section = (*iter).c_str();

		if (wcscmp(section, L"Rainmeter") == 0 ||
			wcscmp(section, L"TrayMeasure") == 0)
		{
			continue;
		}

		const int index = m_SkinRegistry.FindFolderIndex(*iter);
		if (index == -1)
		{
			continue;
		}

		SkinRegistry::Folder& skinFolder = m_SkinRegistry.GetFolder(index);

		// Make sure there is a ini file available
		int active = parser.ReadInt(section, L"Active", 0);
		if (active > 0 && active <= (int)skinFolder.files.size())
		{
			hasActiveSkins = true;
			skinFolder.active = active;
		}

		int order = parser.ReadInt(section, L"LoadOrder", 0);
		SetLoadOrder(index, order);
	}

	// Show tray icon if no skins are active
	if (m_TrayIcon && !hasActiveSkins)
	{
		m_TrayIcon->SetTrayIcon(true, true);
	}

	DialogManage::UpdateSettings();
}

/*
** Refreshes all active meter windows.
** Note: This function calls Skin::Refresh() directly for synchronization. Be careful about crash.
**
*/
void Rainmeter::RefreshAll()
{
	// Read skins and settings
	ReloadSettings();

	// Change the work area if necessary
	if (m_DesktopWorkAreaChanged)
	{
		UpdateDesktopWorkArea(false);
	}

	// Make the sending order by using LoadOrder
	std::multimap<int, Skin*> windows;
	GetSkinsByLoadOrder(windows);

	// Prepare the helper window
	System::PrepareHelperWindow();

	// Refresh all
	std::multimap<int, Skin*>::const_iterator iter = windows.begin();
	for ( ; iter != windows.end(); ++iter)
	{
		Skin* skin = (*iter).second;
		if (skin)
		{
			// Verify whether the cached information is valid
			const int index = m_SkinRegistry.FindFolderIndex(skin->GetFolderPath());
			if (index != -1)
			{
				SkinRegistry::Folder& skinFolder = m_SkinRegistry.GetFolder(index);
				const WCHAR* skinIniFile = skin->GetFileName().c_str();

				bool found = false;
				for (int i = 0, isize = (int)skinFolder.files.size(); i < isize; ++i)
				{
					if (_wcsicmp(skinIniFile, skinFolder.files[i].filename.c_str()) == 0)
					{
						found = true;
						if (skinFolder.active != i + 1)
						{
							// Switch to new ini-file order
							skinFolder.active = i + 1;
							WriteActive(skin->GetFolderPath(), i);
						}
						break;
					}
				}

				if (!found)
				{
					const WCHAR* skinFolderPath = skin->GetFolderPath().c_str();
					std::wstring error = GetFormattedString(ID_STR_UNABLETOREFRESHSKIN, skinFolderPath, skinIniFile);

					DeactivateSkin(skin, index);

					ShowMessage(nullptr, error.c_str(), MB_OK | MB_ICONEXCLAMATION);
					continue;
				}
			}
			else
			{
				const WCHAR* skinFolderPath = skin->GetFolderPath().c_str();
				std::wstring error = GetFormattedString(ID_STR_UNABLETOREFRESHSKIN, skinFolderPath, L"");

				DeactivateSkin(skin, -2);  // -2 = Force deactivate

				ShowMessage(nullptr, error.c_str(), MB_OK | MB_ICONEXCLAMATION);
				continue;
			}

			skin->Refresh(false, true);
		}
	}

	DialogAbout::UpdateSkins();
	DialogManage::UpdateSkins(nullptr);
}

bool Rainmeter::LoadLayout(const std::wstring& name)
{
	// Replace Rainmeter.ini with layout
	std::wstring layout = GetLayoutPath();
	layout += name;
	std::wstring wallpaper = layout + L"\\Wallpaper.bmp";
	layout += L"\\Rainmeter.ini";

	if (_waccess_s(layout.c_str(), 0) != 0)
	{
		return false;
	}

	// Check encoding of layout
	std::wstring msg;
	CheckSettingsFileEncoding(layout, &msg);
	if (!msg.empty())
	{
		// Log information about any encoding changes to |layout|
		LogNotice(msg.c_str());
	}

	DeleteAllUnmanagedSkins();
	DeleteAllSkins();

	std::wstring backup = GetLayoutPath();
	backup += L"@Backup";
	CreateDirectory(backup.c_str(), nullptr);
	backup += L"\\Rainmeter.ini";

	bool backupLayout = (_wcsicmp(name.c_str(), L"@Backup") == 0);
	if (!backupLayout)
	{
		// Make a copy of current Rainmeter.ini
		System::CopyFiles(m_IniFile, backup);
	}

	System::CopyFiles(layout, m_IniFile);

	if (!backupLayout)
	{
		PreserveSetting(backup, L"SkinPath");
		PreserveSetting(backup, L"ConfigEditor");
		PreserveSetting(backup, L"Logging");
		PreserveSetting(backup, L"DisableVersionCheck");
		PreserveSetting(backup, L"DisableAutoUpdate");
		PreserveSetting(backup, L"Language");
		PreserveSetting(backup, L"NormalStayDesktop");
		PreserveSetting(backup, L"SelectedColor");
		PreserveSetting(backup, L"HardwareAcceleration");
		PreserveSetting(backup, L"TrayExecuteM", false);
		PreserveSetting(backup, L"TrayExecuteR", false);
		PreserveSetting(backup, L"TrayExecuteDM", false);
		PreserveSetting(backup, L"TrayExecuteDR", false);

		// Set wallpaper if it exists
		if (_waccess_s(wallpaper.c_str(), 0) == 0)
		{
			SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, (void*)wallpaper.c_str(), SPIF_UPDATEINIFILE);
		}
	}

	// Game mode: Only load layouts if game is disabled. Enabled or "ForcedExit" should not any skins.
	if (GetGameMode().IsDisabled() || GetGameMode().IsLayoutEnabled())
	{
		ReloadSettings();

		// Create meter windows for active skins
		ActivateActiveSkins();
	}

	return true;
}

void Rainmeter::PreserveSetting(const std::wstring& from, LPCTSTR key, bool replace)
{
	WCHAR* buffer = new WCHAR[MAX_LINE_LENGTH];

	if ((replace || GetPrivateProfileString(L"Rainmeter", key, L"", buffer, 4, m_IniFile.c_str()) == 0) &&
		GetPrivateProfileString(L"Rainmeter", key, L"", buffer, MAX_LINE_LENGTH, from.c_str()) > 0)
	{
		WritePrivateProfileString(L"Rainmeter", key, buffer, m_IniFile.c_str());
	}

	delete [] buffer;
	buffer = nullptr;
}

bool Rainmeter::IsSkinAFavorite(const std::wstring& folder, const std::wstring& filename)
{
	for (const auto& file : m_SkinRegistry.FindFolder(folder)->files)
	{
		if (file.filename == filename)
		{
			return file.isFavorite;
		}
	}

	return false;
}

void Rainmeter::UpdateFavorites(const std::wstring& folder, const std::wstring& file, bool favorite)
{
	m_Favorites = m_SkinRegistry.UpdateFavorite(folder, file, favorite);

	// Delete entire [Favorites] section
	WritePrivateProfileSection(L"Favorites", nullptr, m_DataFile.c_str());

	// Write new section
	WCHAR buffer[128] = { 0 };
	int i = 0;
	for (const auto& fav : m_Favorites)
	{
		_snwprintf_s(buffer, _TRUNCATE, L"Favorite%i", ++i);
		WritePrivateProfileString(L"Favorites", buffer, fav.c_str(), m_DataFile.c_str());
	}
}

const std::vector<LPCWSTR>& Rainmeter::GetOldDefaultPlugins()
{
	static const std::vector<LPCWSTR> s_OldPlugins =
	{
		L"MediaKey",
		L"NowPlaying",
		L"Process",
		L"RecycleManager",
		L"SysInfo",
		L"WebParser",
		L"WifiStatus"
	};
	return s_OldPlugins;
}

/*
** Applies given DesktopWorkArea and DesktopWorkArea@n.
**
*/
void Rainmeter::UpdateDesktopWorkArea(bool reset)
{
	bool changed = false;

	if (reset)
	{
		if (!m_OldDesktopWorkAreas.empty())
		{
			int i = 1;
			for (auto iter = m_OldDesktopWorkAreas.cbegin(); iter != m_OldDesktopWorkAreas.cend(); ++iter, ++i)
			{
				RECT r = (*iter);

				BOOL result = SystemParametersInfo(SPI_SETWORKAREA, 0, &r, 0);

				if (m_Debug)
				{
					std::wstring format = L"Resetting WorkArea@%i: L=%i, T=%i, R=%i, B=%i (W=%i, H=%i)";
					if (!result)
					{
						format += L" => FAIL";
					}
					LogDebugF(format.c_str(), i, r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
				}
			}
			changed = true;
		}
	}
	else
	{
		const size_t numOfMonitors = System::GetMonitorCount();
		const MultiMonitorInfo& monitorsInfo = System::GetMultiMonitorInfo();
		const std::vector<MonitorInfo>& monitors = monitorsInfo.monitors;

		if (m_OldDesktopWorkAreas.empty())
		{
			// Store old work areas for changing them back
			for (size_t i = 0; i < numOfMonitors; ++i)
			{
				m_OldDesktopWorkAreas.push_back(monitors[i].work);
			}
		}

		if (m_Debug)
		{
			LogDebugF(L"DesktopWorkAreaType: %s", m_DesktopWorkAreaType ? L"Margin" : L"Default");
		}

		for (UINT i = 0; i <= numOfMonitors; ++i)
		{
			std::map<UINT, RECT>::const_iterator it = m_DesktopWorkAreas.find(i);
			if (it != m_DesktopWorkAreas.end())
			{
				RECT r = (*it).second;

				// Move rect to correct offset
				if (m_DesktopWorkAreaType)
				{
					r = [&]()
					{
						const int index = ((i == 0) ? monitorsInfo.primary : i) - 1;
						RECT rect = {
							monitors[index].screen.left + r.left,
							monitors[index].screen.top + r.top,
							monitors[index].screen.right - r.right,
							monitors[index].screen.bottom - r.bottom };
						return rect;
					}();
				}
				else if (i != 0)
				{
					const int index = i - 1;
					const RECT screenRect = monitors[index].screen;
					r.left += screenRect.left;
					r.top += screenRect.top;
					r.right += screenRect.left;
					r.bottom += screenRect.top;
				}

				BOOL result = SystemParametersInfo(SPI_SETWORKAREA, 0, &r, 0);
				if (result)
				{
					changed = true;
				}

				if (m_Debug)
				{
					std::wstring format = L"Applying DesktopWorkArea";
					if (i != 0)
					{
						WCHAR buffer[64];
						size_t len = _snwprintf_s(buffer, _TRUNCATE, L"@%i", i);
						format.append(buffer, len);
					}
					format += L": L=%i, T=%i, R=%i, B=%i (W=%i, H=%i)";
					if (!result)
					{
						format += L" => FAIL";
					}
					LogDebugF(format.c_str(), r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
				}
			}
		}
	}

	if (changed && System::GetWindow())
	{
		// Update System::MultiMonitorInfo for for work area variables
		SendMessageTimeout(System::GetWindow(), WM_SETTINGCHANGE, SPI_SETWORKAREA, 0, SMTO_ABORTIFHUNG, 1000, nullptr);
	}
}

/*
** Reads the statistics from the ini-file
**
*/
void Rainmeter::ReadStats()
{
	const WCHAR* statsFile = m_StatsFile.c_str();

	// If m_StatsFile doesn't exist, create it and copy the stats section from m_IniFile
	if (_waccess_s(statsFile, 0) != 0)
	{
		const WCHAR* iniFile = m_IniFile.c_str();
		WCHAR* tmpSz = new WCHAR[SHRT_MAX]; 	// Max size returned by GetPrivateProfileSection()

		if (GetPrivateProfileSection(L"Statistics", tmpSz, SHRT_MAX, iniFile) > 0)
		{
			WritePrivateProfileString(L"Statistics", nullptr, nullptr, iniFile);
		}
		else
		{
			tmpSz[0] = tmpSz[1] = L'\0';
		}
		WritePrivateProfileSection(L"Statistics", tmpSz, statsFile);

		delete [] tmpSz;
		tmpSz = nullptr;
	}

	// Only Net measure has stats at the moment
	MeasureNet::ReadStats(m_StatsFile, m_StatsDate);
}

/*
** Writes the statistics to the ini-file. If bForce is false the stats are written only once per an appropriate interval.
**
*/
void Rainmeter::WriteStats(bool bForce)
{
	static ULONGLONG lastWrite = 0;

	ULONGLONG ticks = GetTickCount64();

	if (bForce || (lastWrite + INTERVAL_NETSTATS < ticks))
	{
		lastWrite = ticks;

		// Only Net measure has stats at the moment
		const WCHAR* statsFile = m_StatsFile.c_str();
		MeasureNet::WriteStats(statsFile, m_StatsDate);

		WritePrivateProfileString(nullptr, nullptr, nullptr, statsFile);
	}
}

/*
** Clears the statistics
**
*/
void Rainmeter::ResetStats()
{
	// Set the stats-date string
	time_t now;
	time(&now);
	WCHAR timestamp[MAX_PATH];
	wcsftime(timestamp, MAX_PATH, L"%F %T", gmtime(&now));
	m_StatsDate = timestamp;

	// Only Net measure has stats at the moment
	MeasureNet::ResetStats();
}

/*
** Wraps MessageBox(). Sets RTL flag if necessary.
**
*/
int Rainmeter::ShowMessage(HWND parent, const WCHAR* text, UINT type)
{
	type |= MB_TOPMOST;

	if (*GetString(ID_STR_ISRTL) == L'1')
	{
		type |= MB_RTLREADING;
	}

	return MessageBox(parent, text, APPNAME, type);
};

void Rainmeter::ShowLogFile()
{
	std::wstring logFile = L'"' + GetLogger().GetLogFilePath();
	logFile += L'"';

	CommandHandler::RunFile(m_SkinEditor.c_str(), logFile.c_str());
}

void Rainmeter::SetDebug(bool debug)
{
	m_Debug = debug;
	WritePrivateProfileString(L"Rainmeter", L"Debug", debug ? L"1" : L"0", m_IniFile.c_str());
}

void Rainmeter::SetDisableDragging(bool dragging)
{
	m_DisableDragging = dragging;
	DialogManage::UpdateSkinDraggableCheckBox();
	DialogManage::UpdateGlobalDraggableCheckBox();
	WritePrivateProfileString(L"Rainmeter", L"DisableDragging", dragging ? L"1" : L"0", m_IniFile.c_str());
}

void Rainmeter::SetDisableVersionCheck(bool check)
{
	m_DisableVersionCheck = check;
	WritePrivateProfileString(L"Rainmeter", L"DisableVersionCheck", check ? L"1" : L"0" , m_IniFile.c_str());
}

void Rainmeter::SetDisableAutoUpdate(bool check)
{
	m_DisableAutoUpdate = check;
	WritePrivateProfileString(L"Rainmeter", L"DisableAutoUpdate", check ? L"1" : L"0", m_IniFile.c_str());
}

void Rainmeter::TestSettingsFile(bool bDefaultIniLocation)
{
	const WCHAR* iniFile = m_IniFile.c_str();
	if (!System::IsFileWritable(iniFile))
	{
		std::wstring error = GetString(ID_STR_SETTINGSNOTWRITABLE);

		if (!bDefaultIniLocation)
		{
			std::wstring strTarget = L"%APPDATA%\\Rainmeter\\";
			PathUtil::ExpandEnvironmentVariables(strTarget);

			error += GetFormattedString(ID_STR_SETTINGSMOVEFILE, iniFile, strTarget.c_str());
		}
		else
		{
			error += GetFormattedString(ID_STR_SETTINGSREADONLY, iniFile);
		}

		ShowMessage(nullptr, error.c_str(), MB_OK | MB_ICONERROR);
	}
}

/*
** Checks and converts (if necessary) the encoding of a settings file (Rainmeter.ini).
**
*/
void Rainmeter::CheckSettingsFileEncoding(const std::wstring& iniFile, std::wstring* log)
{
	size_t size = 0ULL;
	auto raw = FileUtil::ReadFullFile(iniFile, &size);
	if (!raw) return;

	auto encoding = FileUtil::GetEncoding(raw.get(), size);
	if (encoding != FileUtil::Encoding::UTF16LE)
	{
		// Make a backup of the settings file
		std::wstring layoutPath = GetLayoutPath();
		CreateDirectory(layoutPath.c_str(), nullptr);
		System::CopyFiles(iniFile, layoutPath);

		std::wstring wide;
		std::string narrow = (char*)raw.get();

		if (encoding == FileUtil::Encoding::UTF8)
		{
			narrow.erase(0, 3);				// Remove BOM
			wide = StringUtil::WidenUTF8(narrow);
		}
		else // if (encoding == FileUtil::Encoding::ANSI)
		{
			// ANSI does not have a BOM
			wide = StringUtil::Widen(narrow);
		}

		FILE* file = nullptr;
		if ((_wfopen_s(&file, iniFile.c_str(), L"wbc, ccs=UTF-16LE") == 0) && file)
		{
			fputs("\xFF\xFE", file);		// Write BOM
			fputws(wide.c_str(), file);		// Write converted text
			fflush(file);
			fclose(file);

			// Since the options in the settings file may not have been read by Rainmeter,
			// logging may be enabled at a later time. Set a log message here and log it later.
			if (log)
			{
				*log = L"Settings file \"";
				*log += iniFile;
				*log += (encoding == FileUtil::Encoding::UTF8) ? L"\" (UTF-8" : L"\" (ANSI";
				*log += L") encoding converted to UTF-16LE. A backup will be saved to: ";
				*log += layoutPath;
			}
		}
	}
}

void Rainmeter::ShowTrayIconIfNecessary()
{
	if (m_Skins.empty())  // Show tray icon if no skins are active
	{
		m_TrayIcon->SetTrayIcon(true, true);
	}
}
