#include "taskbarattributeworker.hpp"
#include "appvisibilitysink.hpp"
#include "common.hpp"
#include "createinstance.hpp"
#include "../ExplorerDetour/hook.hpp"
#include "ttberror.hpp"
#include "ttblog.hpp"
#include "win32.hpp"

bool TaskbarAttributeWorker::SetAttribute(const Window &window, const Config::TASKBAR_APPEARANCE &config)
{
	if (user32::SetWindowCompositionAttribute)
	{
		if (config.ACCENT == swca::ACCENT::ACCENT_NORMAL)
		{
			return false;
		}

		swca::ACCENTPOLICY policy = {
			config.ACCENT,
			2,
			(config.COLOR & 0xFF00FF00) + ((config.COLOR & 0x00FF0000) >> 16) + ((config.COLOR & 0x000000FF) << 16),
			0
		};

		if (policy.nAccentState == swca::ACCENT::ACCENT_ENABLE_FLUENT && policy.nColor >> 24 == 0x00)
		{
			// Fluent mode doesn't likes a completely 0 opacity
			policy.nColor = (0x01 << 24) + (policy.nColor & 0x00FFFFFF);
		}

		swca::WINCOMPATTRDATA data = {
			swca::WindowCompositionAttribute::WCA_ACCENT_POLICY,
			&policy,
			sizeof(policy)
		};

		user32::SetWindowCompositionAttribute(window, &data);
		return true;
	}
	else
	{
		return false;
	}
}

long TaskbarAttributeWorker::RefreshAttribute(HMONITOR monitor)
{
	if (m_Taskbars.count(monitor) != 0)
	{
		const auto &[taskbar, state] = m_Taskbars.at(monitor);
		if (m_CurrentStartMonitor == monitor)
		{
			return SetAttribute(taskbar, Config::START_APPEARANCE);
		}
		return SetAttribute(taskbar, Config::REGULAR_APPEARANCE);
	}

	return 0;
}

void TaskbarAttributeWorker::OnStartVisibilityChange(bool state)
{
	if (state)
	{
		m_CurrentStartMonitor = GetStartMenuMonitor();
		RefreshAttribute(m_CurrentStartMonitor);
	}
	else
	{
		HMONITOR old_start_mon = std::exchange(m_CurrentStartMonitor, nullptr);
		RefreshAttribute(old_start_mon);
	}
}

HMONITOR TaskbarAttributeWorker::GetStartMenuMonitor()
{
	return Window::ForegroundWindow().monitor();
}

void TaskbarAttributeWorker::Poll()
{
	m_CurrentStartMonitor = nullptr;

	BOOL start_visible;
	if (ErrorHandle(m_IAV->IsLauncherVisible(&start_visible), Error::Level::Log, L"Failed to query launcher visibility state.") && start_visible)
	{
		m_CurrentStartMonitor = GetStartMenuMonitor();
	}
}

// todo: get state for mon instead of default to regular
void TaskbarAttributeWorker::RefreshTaskbars()
{
	if (Config::VERBOSE)
	{
		Log::OutputMessage(L"Refreshing taskbar handles.");
	}

	// Older handles are invalid, so clear the map to be ready for new ones
	m_Taskbars.clear();
	std::vector<TTBHook> hooks = std::move(m_Hooks); // Keep old hooks alive while we rehook to keep the DLL loaded in Explorer. They will unhook automatically at the end of this function.
	m_Hooks.clear(); // Bring back m_Hooks to a known state after being moved from.

	const Window main_taskbar = Window::Find(TASKBAR);
	m_Taskbars[main_taskbar.monitor()] = { main_taskbar, TaskbarState::Regular };
	HookTaskbar(main_taskbar);

	for (const Window secondtaskbar : Window::FindEnum(SECONDARY_TASKBAR))
	{
		m_Taskbars[secondtaskbar.monitor()] = { secondtaskbar, TaskbarState::Regular };
		HookTaskbar(secondtaskbar);
	}
}

void TaskbarAttributeWorker::HookTaskbar(const Window &window)
{
	const auto [hook, hr] = Hook::HookExplorer(window);
	if (SUCCEEDED(hr))
	{
		m_Hooks.emplace_back(hook);
	}
	else
	{
		ErrorHandle(hr, Error::Level::Fatal, L"Failed to set hook.");
	}
}

TaskbarAttributeWorker::TaskbarAttributeWorker(const HINSTANCE &hInstance) :
	MessageWindow(WORKER_WINDOW, WORKER_WINDOW, hInstance),
	m_IAV(create_instance<IAppVisibility>(CLSID_AppVisibility)),
	m_IAVECookie(0)
{
	if (m_IAV)
	{
		using namespace Microsoft::WRL; // See comment on AppVisibilitySink for reason why WRL is used here
		ComPtr<IAppVisibilityEvents> av_sink = Make<AppVisibilitySink>(std::bind(&TaskbarAttributeWorker::OnStartVisibilityChange, this, std::placeholders::_1));
		ErrorHandle(m_IAV->Advise(av_sink.Get(), &m_IAVECookie), Error::Level::Log, L"Failed to register app visibility sink.");
	}

	RegisterCallback(Hook::RequestAttributeRefresh, [this](WPARAM, const LPARAM lParam)
	{
		const Window taskbar = reinterpret_cast<HWND>(lParam);
		return RefreshAttribute(taskbar.monitor());
	});

	const auto refresh_taskbars = [this](...)
	{
		RefreshTaskbars();
		return 0;
	};

	RegisterCallback(WM_DISPLAYCHANGE, refresh_taskbars);
	RegisterCallback(WM_TASKBARCREATED, refresh_taskbars);

	RefreshTaskbars();
}

void TaskbarAttributeWorker::ReturnToStock()
{
	for (const auto &[monitor, pair] : m_Taskbars)
	{
		SetAttribute(pair.first, { swca::ACCENT::ACCENT_NORMAL });
	}
}

TaskbarAttributeWorker::~TaskbarAttributeWorker()
{
	if (m_IAVECookie)
	{
		ErrorHandle(m_IAV->Unadvise(m_IAVECookie), Error::Level::Log, L"Failed to unregister app visibility sink");
	}
}