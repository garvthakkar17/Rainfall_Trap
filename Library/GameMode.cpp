/* Copyright (C) 2021 Rainmeter Project Developers
 *
 * This Source Code Form is subject to the terms of the GNU General Public
 * License; either version 2 of the License, or (at your option) any later
 * version. If a copy of the GPL was not distributed with this file, You can
 * obtain one at <https://www.gnu.org/licenses/gpl-2.0.html>. */

#include "StdAfx.h"
#include "GameMode.h"
#include "ConfigParser.h"
#include "DialogAbout.h"
#include "DialogManage.h"
#include "DialogNewSkin.h"
#include "Logger.h"
#include "Rainmeter.h"
#include "System.h"
#include "../Common/StringUtil.h"

namespace
{
struct GameHash
{
	std::size_t operator()(std::wstring const& str) const noexcept
	{
		return 17ULL * 31ULL + std::hash<std::wstring>()(str);
	}
};
}

const UINT GameMode::s_TimerInterval = 500U;
const UINT_PTR GameMode::s_TimerEventID = 1000ULL;

GameMode::GameMode() :
	m_State(State::Disabled),
	m_FullScreenMode(false),
	m_ProcessListMode(false)
{
}

GameMode::~GameMode()
{
	if (!IsForcedExit()) ForceExit();
}

GameMode& GameMode::GetInstance()
{
	static GameMode s_GameMode;
	return s_GameMode;
}

void GameMode::Initialize()
{
	if (GetRainmeter().GetDebug())
	{
		LogDebug(L">> Initializing \"Game mode\" (v1)");
	}
	ReadSettings();
}

void GameMode::OnTimerEvent(WPARAM wParam)
{
	if (wParam != s_TimerEventID) return;

	bool isFullScreenOrProcessList = false;

	if (m_FullScreenMode)
	{
		// Note: "QUNS_BUSY" is triggered for other types of full screen
		// apps (like pressing F11 in a browser), but also is triggered
		// when the desktop has no open windows in some cases.
		QUERY_USER_NOTIFICATION_STATE state = QUNS_NOT_PRESENT;
		if (SHQueryUserNotificationState(&state) == S_OK &&
			state == QUNS_RUNNING_D3D_FULL_SCREEN)
		{
			isFullScreenOrProcessList = true;
		}
	}

	if (!isFullScreenOrProcessList && m_ProcessListMode)
	{
		for (const auto& process : m_ProcessList)
		{
			if (System::IsProcessRunningCached(process))
			{
				isFullScreenOrProcessList = true;
				break;
			}
		}
	}

	if ((m_State != State::Disabled) && !isFullScreenOrProcessList)
	{
		ExitGameMode();
	}
	else if ((m_State == State::Disabled) && isFullScreenOrProcessList)
	{
		EnterGameMode();
	}
	//else
	//{
		// Already in game mode and process is still running, or
		// not in game mode and process is not running
	//}
}

void GameMode::SetOnStartAction(const std::wstring& action)
{
	SetSettings(action, m_OnStopAction, m_FullScreenMode, m_ProcessListMode, m_ProcessListOriginal);
}

void GameMode::SetOnStartAction(UINT index)
{
	std::wstring action;
	const auto& layouts = GetRainmeter().m_Layouts;
	if (index > 0U && layouts.size() > 0ULL)
	{
		action = layouts[(size_t)index - 1ULL];
	}
	SetOnStartAction(action);  // Can be empty (Unload all skins)
}

void GameMode::SetOnStopAction(const std::wstring& action)
{
	SetSettings(m_OnStartAction, action, m_FullScreenMode, m_ProcessListMode, m_ProcessListOriginal);
}

void GameMode::SetOnStopAction(UINT index)
{
	std::wstring action;
	const auto& layouts = GetRainmeter().m_Layouts;
	if (index > 0U && layouts.size() > 0ULL)
	{
		action = layouts[(size_t)index - 1ULL];
	}
	SetOnStopAction(action);  // Can be empty (Load current layout or @Backup)
}

void GameMode::SetFullScreenMode(bool mode)
{
	SetSettings(m_OnStartAction, m_OnStopAction, mode, m_ProcessListMode, m_ProcessListOriginal);

	if (!IsDisabled() && !mode && !m_ProcessListMode)
	{
		ExitGameMode();
	}
}

void GameMode::SetProcessListMode(bool mode)
{
	SetSettings(m_OnStartAction, m_OnStopAction, m_FullScreenMode, mode, m_ProcessListOriginal);

	if (!IsDisabled() && !m_FullScreenMode && !mode)
	{
		ExitGameMode();
	}
}

void GameMode::SetProcessList(const std::wstring& list)
{
	SetSettings(m_OnStartAction, m_OnStopAction, m_FullScreenMode, m_ProcessListMode, list);
}

void GameMode::ChangeStateManual(bool disable)
{
	SetFullScreenMode(false);
	SetProcessListMode(false);

	if (disable)
	{
		ExitGameMode();
	}
	else
	{
		EnterGameMode();
	}
}

void GameMode::ForceExit()
{
	if (m_State != State::Disabled)
	{
		StopTimer();
		ExitGameMode(true);
	}
}

void GameMode::ValidateActions()
{
	const std::wstring oldOnStopAction = m_OnStopAction;

	auto resetAction = [](std::wstring& action) -> bool
	{
		bool found = false;
		if (!action.empty())
		{
			for (const auto& layout : GetRainmeter().GetAllLayouts())
			{
				if (_wcsicmp(layout.c_str(), action.c_str()) == 0)
				{
					found = true;
					break;
				}
			}
			if (!found)
			{
				action.clear();
			}
		}
		return found;
	};

	if (!resetAction(m_OnStartAction) || !resetAction(m_OnStopAction))
	{
		WriteSettings();
	}

	// If game mode is running and the "on stop" action layout no longer exists, exit game mode
	if (m_State != State::Disabled && oldOnStopAction != m_OnStopAction)
	{
		ChangeStateManual(true);
	}
}

bool GameMode::HasBangOverride(LPCWSTR str)
{
	std::wstring tmp = str;
	std::wstring::size_type pos = tmp.find_first_of('!');
	if (pos != std::wstring::npos)
	{
		tmp = tmp.substr(pos + 1).c_str();
		for (const auto item : GetBangOverrideList())
		{
			if (_wcsicmp(tmp.c_str(), item) == 0)
			{
				return true;
			}
		}
	}

	return false;
}

const std::vector<LPCWSTR>& GameMode::GetBangOverrideList()
{
	static const std::vector<LPCWSTR> s_BangOverrideList =
	{
		L"Quit"
	};
	return s_BangOverrideList;
}

void GameMode::StartTimer()
{
	OnTimerEvent(s_TimerEventID);  // Don't wait for the first interval
	SetTimer(GetRainmeter().GetWindow(), s_TimerEventID, s_TimerInterval, nullptr);
}

void GameMode::StopTimer()
{
	KillTimer(GetRainmeter().GetWindow(), s_TimerEventID);
}

void GameMode::SetSettings(const std::wstring& onStart, const std::wstring& onStop,
	bool fullScreenMode, bool processListMode, std::wstring processList, bool init)
{
	m_OnStartAction = onStart;
	m_OnStopAction = onStop;
	m_FullScreenMode = fullScreenMode;
	m_ProcessListMode = processListMode;
	m_ProcessListOriginal = processList;
	StringUtil::ToLowerCase(processList);

	if (!init)
	{
		WriteSettings();
	}

	m_ProcessList.clear();
	m_ProcessList = ConfigParser::Tokenize(processList, L"|");

	StopTimer();

	if (fullScreenMode || processListMode)
	{
		StartTimer();
	}

	DialogManage::UpdateGameMode();
}

void GameMode::ReadSettings()
{
	const std::wstring& dataFile = GetRainmeter().GetDataFile();

	WCHAR* buffer = new WCHAR[MAX_LINE_LENGTH];
	if (GetPrivateProfileString(L"GameMode_v1", nullptr, nullptr, buffer, MAX_LINE_LENGTH, dataFile.c_str()) > 0)
	{
		auto getStr = [buffer, &dataFile](std::wstring& key, std::wstring& str) -> bool
		{
			bool ret = (GetPrivateProfileString(L"GameMode_v1", key.c_str(), nullptr, buffer, MAX_LINE_LENGTH, dataFile.c_str()) == 0);
			if (!ret) str = buffer;
			return ret;
		};

		std::vector<std::wstring> keys;
		const WCHAR* pos = buffer;
		while (*pos)
		{
			keys.emplace_back(pos);
			pos += keys.back().size() + 1;
		}

		std::wstring hashStr;
		for (const auto& key : keys)
		{
			if (hashStr.compare(key.substr(4)) == 0) continue;  // Only process different hashes

			hashStr = key.substr(4);
			std::wstring star, keyStar = L"star";
			std::wstring stop, keyStop = L"stop";
			std::wstring full, keyFull = L"full";
			std::wstring mode, keyMode = L"mode";
			std::wstring list, keyList = L"list";

			keyStar += hashStr; keyStop += hashStr; keyFull += hashStr;
			keyMode += hashStr; keyList += hashStr;

			if (getStr(keyStar, star) || getStr(keyStop, stop) || getStr(keyFull, full) ||
				getStr(keyMode, mode) || getStr(keyList, list))
			{
				continue;
			}

			// The hash is also used as a value in some cases
			if (star == hashStr) star.clear();
			if (stop == hashStr) stop.clear();
			if (list == hashStr) list.clear();

			// Validate hash
			std::wstring newHash = star + stop + full + mode + list;
			std::size_t hash = GameHash{}(newHash);
			newHash = std::to_wstring(hash);

			if (newHash == hashStr)  // Found!
			{
				SetSettings(star, stop, (full == L"1"), (mode == L"1"), list, true);
				ValidateActions();
				break;
			}

			LogErrorF(L"Game mode: Invalid settings (%s)", hashStr.c_str());
		}
	}
	delete [] buffer;
	buffer = nullptr;
}

void GameMode::WriteSettings()
{
	const std::wstring& dataFile = GetRainmeter().GetDataFile();

	std::wstring star = m_OnStartAction, keyStar = L"star";
	std::wstring stop = m_OnStopAction, keyStop = L"stop";
	std::wstring full = (m_FullScreenMode ? L"1" : L"0"), keyFull = L"full";
	std::wstring mode = (m_ProcessListMode ? L"1" : L"0"), keyMode = L"mode";
	std::wstring list = m_ProcessListOriginal, keyList = L"list";
	std::wstring hashStr = star + stop + full + mode + list;

	std::size_t hash = GameHash{}(hashStr);
	hashStr = std::to_wstring(hash);

	keyStar += hashStr; keyStop += hashStr; keyFull += hashStr;
	keyMode += hashStr; keyList += hashStr;

	// Some values can be empty, use the hash instead
	if (star.empty()) star = hashStr;
	if (stop.empty()) stop = hashStr;
	if (list.empty()) list = hashStr;

	// Delete entire section
	WritePrivateProfileString(L"GameMode_v1", nullptr, nullptr, dataFile.c_str());

	auto setStr = [&dataFile](std::wstring& key, std::wstring& value) -> bool
	{
		return (WritePrivateProfileString(L"GameMode_v1", key.c_str(), value.c_str(), dataFile.c_str()) == 0);
	};

	if (setStr(keyStar, star) || setStr(keyStop, stop) || setStr(keyFull, full) ||
		setStr(keyMode, mode) || setStr(keyList, list))
	{
		LogError(L"Game mode: Could not write settings");
	}
}

void GameMode::EnterGameMode()
{
	if (!IsDisabled()) return;

	LogNotice(L">> Entering \"Game mode\"");

	if (m_OnStartAction.empty())  // "Unload all skins"
	{
		// Close dialogs if open
		DialogManage::CloseDialog();
		DialogAbout::CloseDialog();
		DialogNewSkin::CloseDialog();

		Rainmeter& rainmeter = GetRainmeter();
		rainmeter.DeleteAllUnmanagedSkins();
		rainmeter.DeleteAllSkins();
		rainmeter.DeleteAllUnmanagedSkins();  // Redelete unmanaged windows caused by OnCloseAction

		rainmeter.ShowTrayIconIfNecessary();

		m_State = State::Enabled;
	}
	else
	{
		LoadLayout(m_OnStartAction);
		m_State = State::LayoutEnabled;
	}
}

void GameMode::ExitGameMode(bool force)
{
	if (IsDisabled()) return;

	LogNotice(L">> Exiting \"Game mode\"");

	m_State = force ? State::ForcedExit : State::Disabled;

	if (m_OnStopAction.empty())
	{
		if (m_OnStartAction.empty())
		{
			if (force) return;  // Current layout will be loaded on next startup

			// Since no layout was loaded during "on start" action, reload the current layout
			Rainmeter& rainmeter = GetRainmeter();
			rainmeter.ReloadSettings();
			rainmeter.ActivateActiveSkins();
		}
		else
		{
			// A layout was loaded during the "on start" action, so the "old" layout is in the @Backup folder
			std::wstring backup = L"@Backup";
			LoadLayout(backup);
		}
	}
	else
	{
		LoadLayout(m_OnStopAction);
	}
}

void GameMode::LoadLayout(const std::wstring& layout)
{
	std::wstring action = L"!LoadLayout \"";
	action += layout;
	action += L'"';

	if (IsForcedExit())
	{
		// If exiting Rainmeter, load the layout, but do not activate any skins. See Rainmeter::LoadLayout
		GetRainmeter().ExecuteCommand(action.c_str(), nullptr);
	}
	else
	{
		// Delay load the layout
		GetRainmeter().DelayedExecuteCommand(action.c_str());
	}
}
