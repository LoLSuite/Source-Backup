#define WIN32_LEAN_AND_MEAN
#define _UNICODE
#define UNICODE
#include <windows.h>
#include <VersionHelpers.h>
#include <winhttp.h>
#include <TlHelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <filesystem>
#include <atomic>
#include <unordered_map>
#include <fstream>
#include <thread>
#include "resource.h"

std::error_code ec;
static std::atomic<bool> g_isBusy = false;
int cb_index = 0;
std::vector<std::wstring> b(159);
HWND hWnd, patch, listbox;
HFONT font = CreateFont(-MulDiv(16, GetDpiForWindow(hWnd), 96), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI Variable");

static std::wstring Join(const std::wstring& base, const std::wstring& addition) {
	return (std::filesystem::path(base) / addition).wstring();
}

static void Append(int index, const std::wstring& addition) {
	b[index] = Join(b[index], addition);
}

static void Combine(int destIndex, int srcIndex, const std::wstring& addition) {
	b[destIndex] = Join(b[srcIndex], addition);
}

static void Combine(int destIndex, const std::filesystem::path& src, const std::wstring& addition) {
	b[destIndex] = Join(src, addition);
}

bool checkUAC()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return false;

	TOKEN_ELEVATION elevation{};
	DWORD size = sizeof(elevation);

	BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
	CloseHandle(token);

	return ok && elevation.TokenIsElevated;
}

bool download(const std::wstring& url, const std::filesystem::path& outputPath, bool offsite = false)
{
	const std::wstring fullUrl = offsite ? url : (L"https://lolroms.com/" + url);

	URL_COMPONENTSW uc{};
	wchar_t host[256]{};
	wchar_t path[2048]{};

	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = host;
	uc.dwHostNameLength = _countof(host);
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = _countof(path);

	if (!WinHttpCrackUrl(fullUrl.c_str(), 0, 0, &uc))
		return false;

	if (path[0] == L'\0')
		wcscpy_s(path, L"/");

	HINTERNET hSession = WinHttpOpen(
		L"LoLSuite/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS,
		0
	);
	if (!hSession)
		return false;

	DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
	WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

	DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
	WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

	HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
	if (!hConnect) {
		WinHttpCloseHandle(hSession);
		return false;
	}

	HINTERNET hRequest = WinHttpOpenRequest(
		hConnect,
		L"GET",
		path,
		nullptr,
		WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		(uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0
	);
	if (!hRequest) {
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return false;
	}

	if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
		!WinHttpReceiveResponse(hRequest, nullptr))
	{
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return false;
	}

	DWORD size = 0;
	DWORD sizeLen = sizeof(size);
	WinHttpQueryHeaders(
		hRequest,
		WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
		nullptr,
		&size,
		&sizeLen,
		nullptr
	);

	const bool hasSize = (size > 0);

	std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
	if (!out.is_open()) {
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return false;
	}

	BYTE buffer[8192];
	DWORD bytesRead = 0;
	uint64_t totalRead = 0;

	while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0)
	{
		out.write(reinterpret_cast<char*>(buffer), bytesRead);
		totalRead += bytesRead;
	}

	out.close();

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);

	ec.clear();
	std::filesystem::remove(outputPath.wstring() + L":Zone.Identifier", ec);

	return true;
}

bool gen_shortcut()
{
	PWSTR desktopPath = nullptr;
	HRESULT hr = SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktopPath);
	if (FAILED(hr)) return false;
	wchar_t shortcutPath[MAX_PATH + 1];
	swprintf_s(shortcutPath, L"%s\\LoLSuite - FPS Booster.lnk", desktopPath);
	CoTaskMemFree(desktopPath);
	wchar_t exePath[MAX_PATH + 1];
	GetModuleFileName(nullptr, exePath, MAX_PATH + 1);
	IShellLinkW* link = nullptr;
	hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link));
	if (FAILED(hr)) return false;
	link->SetPath(exePath);
	link->SetArguments(L"");
	link->SetDescription(L"FPS Booster");
	link->SetIconLocation(exePath, 0);
	IPersistFile* file = nullptr;
	hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
	if (FAILED(hr)) {
		link->Release();
		return false;
	}
	hr = file->Save(shortcutPath, TRUE);
	file->Release();
	link->Release();
	return SUCCEEDED(hr);
}

bool term(const std::wstring& processName)
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return false;

	PROCESSENTRY32 entry{ sizeof(entry) };

	if (!Process32First(snapshot, &entry)) {
		CloseHandle(snapshot);
		return false;
	}

	do {
		if (_wcsicmp(entry.szExeFile, processName.c_str()) == 0) {

			HANDLE hProc = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION |
				PROCESS_VM_READ, FALSE, entry.th32ProcessID);

			if (!hProc)
				continue;

			DWORD exitCode = 0;
			if (GetExitCodeProcess(hProc, &exitCode) &&
				exitCode == STILL_ACTIVE)
			{
				TerminateProcess(hProc, 0);
			}

			CloseHandle(hProc);
		}
	} while (Process32Next(snapshot, &entry));

	CloseHandle(snapshot);
	return true;
}

bool x64()
{
	USHORT processMachine = 0, nativeMachine = 0;
	auto fn = reinterpret_cast<BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*)>(GetProcAddress(GetModuleHandle(L"kernel32.dll"), "IsWow64Process2"));

	if (fn && fn(GetCurrentProcess(), &processMachine, &nativeMachine))
		return nativeMachine == IMAGE_FILE_MACHINE_AMD64 ||
		nativeMachine == IMAGE_FILE_MACHINE_ARM64;

	BOOL wow = FALSE;
	auto fn2 = reinterpret_cast<BOOL(WINAPI*)(HANDLE, PBOOL)>(GetProcAddress(GetModuleHandle(L"kernel32.dll"), "IsWow64Process"));
	if (fn2 && fn2(GetCurrentProcess(), &wow))
		return wow;

	return false;
}

struct RunOptions
{
	bool wait = true;
	bool checkExit = false;
	bool hidden = false;
	const wchar_t* verb = L"open";
	const wchar_t* params = nullptr;
};

bool runEx(const std::wstring& file, const RunOptions& opt)
{
	SHELLEXECUTEINFO sei{};
	sei.cbSize = sizeof(sei);
	sei.fMask = opt.wait ? SEE_MASK_NOCLOSEPROCESS : 0;
	sei.hwnd = nullptr;
	sei.lpVerb = opt.verb;
	sei.lpFile = file.c_str();
	sei.lpParameters = opt.params;
	sei.lpDirectory = nullptr;
	sei.nShow = opt.hidden ? SW_HIDE : SW_SHOWNORMAL;
	sei.hInstApp = nullptr;

	if (!ShellExecuteEx(&sei))
		return false;

	if (!opt.wait || !sei.hProcess)
		return true;

	SetPriorityClass(sei.hProcess, HIGH_PRIORITY_CLASS);

	while (WaitForSingleObject(sei.hProcess, 50) == WAIT_TIMEOUT)
	{
		MSG msg;
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	DWORD exitCode = 0;
	GetExitCodeProcess(sei.hProcess, &exitCode);
	CloseHandle(sei.hProcess);

	return opt.checkExit ? (exitCode == 0) : true;
}

void runProcess(const std::wstring& exe, const std::wstring& args, bool wait)
{
	STARTUPINFOW si{ sizeof(si) };
	PROCESS_INFORMATION pi{};
	std::wstring cmd = exe + L" " + args;

	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(L'\0');

	if (!CreateProcess(nullptr, buf.data(), nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		return;

	if (wait)
		WaitForSingleObject(pi.hProcess, INFINITE);

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
}


void shell(const std::vector<std::wstring>& commands)
{
	std::wstring script;
	for (auto& c : commands) {
		std::wstring safe = c;
		std::ranges::replace(safe, L'"', L'`"');
		script += safe + L"; ";
	}

	runProcess(
		L"powershell.exe",
		L"-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"& { " + script + L" }\"",
		true
	);
}



std::wstring folder(const std::wstring& pathLabel)
{
	const std::filesystem::path iniPath = std::filesystem::current_path() / L"LoLSuite.ini";
	wchar_t saved[MAX_PATH + 1]{};
	GetPrivateProfileString(pathLabel.c_str(), L"path", L"", saved, MAX_PATH, iniPath.c_str());

	if (saved[0] != L'\0') {
		b[0] = saved;
		return b[0];
	}

	MessageBox(nullptr, (L"Select: " + pathLabel).c_str(), L"LoLSuite", MB_OK);

	b[0].clear();
	std::wstring selected;

	HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(hrInit))
		return L"";

	{
		IFileDialog* dlg = nullptr;
		if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
			CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
		{
			DWORD opts = 0;
			dlg->GetOptions(&opts);
			dlg->SetOptions(opts | FOS_PICKFOLDERS);

			if (SUCCEEDED(dlg->Show(nullptr))) {
				IShellItem* item = nullptr;
				if (SUCCEEDED(dlg->GetResult(&item))) {
					PWSTR psz = nullptr;
					if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz))) {
						selected = psz;
						CoTaskMemFree(psz);
					}
					item->Release();
				}
			}
			dlg->Release();
		}
	}

	CoUninitialize();

	if (!selected.empty()) {
		b[0] = selected;
		WritePrivateProfileString(
			pathLabel.c_str(), L"path",
			b[0].c_str(), iniPath.c_str()
		);
	}

	return b[0];
}

static void service(const std::wstring& serviceName, bool start, bool restart = false) {
	struct ServiceHandleDeleter {
		void operator()(SC_HANDLE h) const {
			if (h) CloseServiceHandle(h);
		}
	};
	using ServiceHandle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ServiceHandleDeleter>;
	ServiceHandle scm(OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
	ServiceHandle svc(OpenService(scm.get(), serviceName.c_str(), SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS));

	auto stopService = [&](SC_HANDLE h) {
		SERVICE_STATUS status{};
		if (ControlService(h, SERVICE_CONTROL_STOP, &status)) {
			do {
				std::this_thread::sleep_for(std::chrono::milliseconds(300));
				if (!QueryServiceStatus(h, &status)) break;
			} while (status.dwCurrentState != SERVICE_STOPPED);
		}
		};

	if (restart) {
		stopService(svc.get());
		StartService(svc.get(), 0, nullptr);
	}
	else if (start) {
		StartService(svc.get(), 0, nullptr);
	}
	else {
		stopService(svc.get());
	}
}


bool desktop_clear()
{
	SHEmptyRecycleBin(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);

	if (OpenClipboard(nullptr)) {
		EmptyClipboard();
		if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(wchar_t))) {
			if (auto p = static_cast<wchar_t*>(GlobalLock(h))) {
				*p = L'\0';
				GlobalUnlock(h);
				SetClipboardData(CF_UNICODETEXT, h);
			}
			else {
				GlobalFree(h);
			}
		}
		CloseClipboard();
	}

	auto rm = [&](auto&& path) {
		ec.clear();
		std::filesystem::remove_all(path, ec);
		};

	auto rmf = [&](auto&& path) {
		ec.clear();
		std::filesystem::remove(path, ec);
		};

	wchar_t buf[MAX_PATH + 1];

	if (GetTempPath(MAX_PATH + 1, buf))
		rm(buf);

	if (GetWindowsDirectory(buf, MAX_PATH + 1)) {
		rm(std::filesystem::path(buf) / L"Temp");
		rm(std::filesystem::path(buf) / L"Prefetch");
	}
	wchar_t* appdata = nullptr;
	size_t len = 0;
	if (_wdupenv_s(&appdata, &len, L"APPDATA") == 0 && appdata) {
		rm(std::filesystem::path(appdata) / L"Microsoft\\Windows\\Recent");
		free(appdata);
	}

	ec.clear();
	for (auto& p : std::filesystem::recursive_directory_iterator(
		std::filesystem::temp_directory_path(),
		std::filesystem::directory_options::skip_permission_denied, ec))
	{
		if (p.is_regular_file(ec) && p.path().extension() == L".log")
			rmf(p.path());
	}

	PWSTR programData = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)))
	{
		std::filesystem::path wer = std::filesystem::path(programData) / L"Microsoft" / L"Windows" / L"WER";
		CoTaskMemFree(programData);
		rm(wer.c_str());
	}

	wchar_t lad[MAX_PATH + 1];
	if (SHGetFolderPath(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, lad) == S_OK) {
		rmf(std::filesystem::path(lad) / L"IconCache.db");

		std::filesystem::path explorer = std::filesystem::path(lad) / L"Microsoft\\Windows\\Explorer";

		constexpr const wchar_t* patterns[] = {
			L"thumbcache_*.db",
			L"iconcache_*.db",
			L"ExplorerStartupLog*.etl"
		};

		for (auto pat : patterns) {
			WIN32_FIND_DATAW fd;
			HANDLE h = FindFirstFile((explorer / pat).c_str(), &fd);
			if (h != INVALID_HANDLE_VALUE) {
				do rmf(explorer / fd.cFileName);
				while (FindNextFile(h, &fd));
				FindClose(h);
			}
		}
	}

	return true;
}

struct FileOp {
	int dstId;
	int srcId;
	std::wstring relativePath;
	std::wstring patchPath;
	std::wstring restorePath;
};

struct GameConfig {
	std::wstring name;
	std::wstring baseDir;
	std::vector<std::wstring> processes;
	std::vector<std::tuple<int, int, std::wstring>> cpaths;
	std::vector<FileOp> fileOps;
	std::wstring steamUrl;
	std::vector<std::wstring> preAppends;
};

void Patch(const GameConfig& config, bool restore)
{
	folder(config.baseDir);

	for (auto it = std::filesystem::recursive_directory_iterator(config.baseDir, std::filesystem::directory_options::skip_permission_denied, ec); it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
	{
		if (ec) {
			ec.clear();
			continue;
		}

		if (it->is_regular_file(ec))
		{
			std::filesystem::path ads = it->path();
			ads += L":Zone.Identifier";

			ec.clear();
			std::filesystem::remove(ads, ec);
		}
	}

	for (auto& s : config.preAppends)
		Append(0, s);

	for (const auto& proc : config.processes)
		term(proc);

	for (const auto& [dst, src, rel] : config.cpaths)
		Combine(dst, src, rel);

	for (const auto& op : config.fileOps)
	{
		if (restore)
		{
			if (op.restorePath.empty())
			{
				ec.clear();
				std::filesystem::remove(b[op.dstId], ec);
				continue;
			}
		}

		const std::wstring& url = restore ? op.restorePath : op.patchPath;
		const std::filesystem::path outputPath = b[op.dstId];

		download(url, outputPath);
	}

	runEx(config.steamUrl, { .wait = false, .params = L"" });
}

static GameConfig Dota2() {
	return {
		L"dota2",
		L"<drive>:\\Program Files (x86)\\Steam\\steamapps\\common\\dota 2 beta",
		{ L"dota2.exe" },
		{
			{8, 0, L"game\\bin\\win64"},
			{1, 8, L"embree3.dll"},
			{2, 8, L"d3dcompiler_47.dll"},
		},
		{
			{1, 8, L"embree3.dll", L"patch/embree4.dll", L"restore/dota2/embree3.dll"},
			{2, 8, L"d3dcompiler_47.dll", L"patch/D3DCompiler_47.dll", L"restore/dota2/d3dcompiler_47.dll"},
		},
		L"steam://rungameid/570"
	};
}

static GameConfig Smite2() {
	return {
		L"smite2",
		L"<drive>:\\Program Files (x86)\\Steam\\steamapps\\common\\SMITE2",
		{ L"Hemingway.exe", L"Hemingway-Win64-Shipping.exe" },
		{
			{8, 0, L"Windows\\Engine\\Binaries\\Win64"},
			{7, 0, L"Windows\\Hemingway\\Binaries\\Win64"},
			{1, 8, L"tbb.dll"},
			{2, 8, L"tbbmalloc.dll"},
			{3, 7, L"tbb.dll"},
			{4, 7, L"tbbmalloc.dll"}
		},
		{
			{1, 8, L"tbb.dll", L"patch/tbb.dll", L"restore/smite2/tbb.dll"},
			{2, 8, L"tbbmalloc.dll", L"patch/tbbmalloc.dll", L"restore/smite2/tbbmalloc.dll"},
			{3, 7, L"tbb.dll", L"patch/tbb.dll", L"restore/smite2/tbb.dll"},
			{4, 7, L"tbbmalloc.dll", L"patch/tbbmalloc.dll", L"restore/smite2/tbbmalloc.dll"}
		},
		L"steam://rungameid/2437170"
	};
}

static GameConfig MGS() {
	return {
		L"mgs",
		L"<drive>:\Program Files (x86)\Steam\steamapps\common\MGSDelta",
		{ L"MGSDelta.exe", L"MGSDelta-Win64-Shipping.exe", L"Nightmare-Win64-Shipping.exe", L"Foxhunt-Win64-Shipping.exe"},
		{
			{9, 0, L"MGSDelta_Foxhunt\\Binaries\\Win64"},
			{8, 0, L"MGSDelta\\Binaries\\Win64"},
			{7, 0, L"MGSDelta_Nightmare\\Binaries\\Win64"},
			{1, 8, L"tbb.dll"},
			{2, 8, L"tbb12.dll"},
			{3, 8, L"tbbmalloc.dll"},
			{4, 7, L"tbb.dll"},
			{5, 7, L"tbb12.dll"},
			{6, 7, L"tbbmalloc.dll"},
			{10, 9, L"tbb.dll"},
			{11, 9, L"tbb12.dll"},
			{12, 9, L"tbbmalloc.dll"}
		},
		{
			{10, 9, L"tbb.dll", L"patch/tbb.dll", L"restore/mgs/tbb.dll"},
			{11, 9, L"tbb12.dll", L"patch/tbb.dll", L"restore/mgs/tbb12.dll"},
			{12, 9, L"tbbmalloc.dll", L"patch/tbbmalloc.dll", L"restore/mgs/tbbmalloc.dll"},
			{1, 8, L"tbb.dll", L"patch/tbb.dll", L"restore/mgs/tbb.dll"},
			{2, 8, L"tbb12.dll", L"patch/tbb.dll", L"restore/mgs/tbb12.dll"},
			{3, 8, L"tbbmalloc.dll", L"patch/tbbmalloc.dll", L"restore/mgs/tbbmalloc.dll"},
			{4, 7, L"tbb.dll", L"patch/tbb.dll", L"restore/mgs/tbb.dll"},
			{5, 7, L"tbb12.dll", L"patch/tbb.dll", L"restore/mgs/tbb12.dll"},
			{6, 7, L"tbbmalloc.dll", L"patch/tbbmalloc.dll", L"restore/mgs/tbbmalloc.dll"}
		},
		L"steam://rungameid/2417610"
	};
}

static GameConfig Blands4() {
	return {
		L"blands4",
		L"<drive>:\\Program Files (x86)\\Steam\\steamapps\\common\\Borderlands 4",
		{ L"Borderlands4.exe", L"Borderlands4-Win64-Shipping.exe", L"BL4Launcher.exe" },
		{
			{8, 0, L"OakGame\\Binaries\\Win64"},
			{7, 0, L"Engine\\Binaries\\Win64"},
			{1, 8, L"tbb.dll"},
			{2, 8, L"tbbmalloc.dll"},
			{3, 7, L"tbb.dll"},
			{4, 7, L"tbbmalloc.dll"}
		},
		{
			{1, 8, L"tbb.dll", L"patch/tbb.dll", L"restore/blands4/tbb.dll"},
			{2, 8, L"tbbmalloc.dll", L"patch/tbbmalloc.dll", L"restore/blands4/tbbmalloc.dll"},
			{3, 7, L"tbb.dll", L"patch/tbb.dll", L"restore/blands4/tbb.dll"},
			{4, 7, L"tbbmalloc.dll", L"patch/tbbmalloc.dll", L"restore/blands4/tbbmalloc.dll"}
		},
		L"steam://rungameid/1285190"
	};
}

static GameConfig OblivionR() {
	return {
		L"oblivionr",
		L"<drive>:\\Program Files (x86)\\Steam\\steamapps\\common\\The Elder Scrolls IV - Oblivion Remastered",
		{ L"OblivionRemastered.exe", L"OblivionRemastered-Win64-Shipping.exe" },
		{
			{8, 0, L"OblivionRemastered\\Binaries\\Win64"},
			{7, 0, L"Engine\\Binaries\\Win64"},
			{1, 8, L"tbb.dll"},
			{2, 8, L"tbbmalloc.dll"},
			{3, 8, L"tbb12.dll"},
			{4, 7, L"tbb.dll"},
			{5, 7, L"tbbmalloc.dll"}
		},
		{
			{1, 8, L"tbb.dll", L"patch/tbb.dll", L"restore/oblivionr/tbb.dll"},
			{2, 8, L"tbbmalloc.dll", L"patch/tbbmalloc.dll", L"restore/oblivionr/tbbmalloc.dll"},
			{3, 8, L"tbb12.dll", L"patch/tbb.dll", L"restore/oblivionr/tbb12.dll"},
			{4, 7, L"tbb.dll", L"patch/tbb.dll", L"restore/oblivionr/tbb.dll"},
			{5, 7, L"tbbmalloc.dll", L"patch/tbbmalloc.dll", L"restore/oblivionr/tbbmalloc.dll"},
		},
		L"steam://rungameid/2623190"
	};
}

static GameConfig SilentHillF() {
	return {
		L"silenthillf",
		L"<drive>:\\Program Files (x86)\\Steam\\steamapps\\common\\SILENT HILL f",
		{ L"SHf-Win64-Shipping.exe", L"SHf.exe"},
		{
			{8, 0, L"SHf\\Binaries\\Win64"},
			{7, 0, L"Engine\\Binaries\\Win64"},
			{1, 8, L"tbb.dll"},
			{2, 8, L"tbbmalloc.dll"},
			{3, 8, L"tbb12.dll"},
			{4, 7, L"tbb.dll"},
			{5, 7, L"tbbmalloc.dll"}
		},
		{
			{1, 8, L"tbb.dll", L"patch/tbb.dll", L"restore/silenthillf/tbb.dll"},
			{2, 8, L"tbbmalloc.dll", L"patch/tbbmalloc.dll", L"restore/silenthillf/tbbmalloc.dll"},
			{3, 8, L"tbb12.dll", L"patch/tbb.dll", L"restore/silenthillf/tbb12.dll"},
			{4, 7, L"tbb.dll", L"patch/tbb.dll", L"restore/silenthillf/tbb.dll"},
			{5, 7, L"tbbmalloc.dll", L"patch/tbbmalloc.dll", L"restore/silenthillf/tbbmalloc.dll"},
		},
		L"steam://rungameid/2947440"
	};
}

static GameConfig Outworlds2() {
	return {
		L"outworlds2",
		L"<drive>:\\Program Files (x86)\\Steam\\steamapps\\common\\The Outer Worlds 2",
		{ L"TheOuterWorlds2-Win64-Shipping.exe", L"TheOuterWorlds2.exe"},
		{
			{8, 0, L"Arkansas\\Binaries\\Win64"},
			{1, 8, L"tbb.dll"},
			{2, 8, L"tbbmalloc.dll"},
			{3, 8, L"tbb12.dll"}
		},
		{
			{1, 8, L"tbb.dll", L"patch/tbb.dll", L"restore/outworlds2/tbb.dll"},
			{2, 8, L"tbbmalloc.dll", L"patch/tbbmalloc.dll", L"restore/outworlds2/tbbmalloc.dll"},
			{3, 8, L"tbb12.dll", L"patch/tbb.dll", L"restore/outworlds2/tbb12.dll"}
		},
		L"steam://rungameid/1449110"
	};
}

static const std::unordered_map<std::wstring, GameConfig(*)()> gameMap = {
	{ L"dota2", Dota2 },
	{ L"smite2", Smite2 },
	{ L"mgs", MGS },
	{ L"blands4", Blands4 },
	{ L"oblivionr", OblivionR },
	{ L"silenthillf", SilentHillF },
	{ L"outworlds2", Outworlds2 }
};

static void manage(const std::wstring& game, bool restore) {
	auto it = gameMap.find(game);
	if (it != gameMap.end()) {
		Patch(it->second(), restore);
	}

	if (game == L"minecraft")
	{
		if (!checkUAC())
		{
			MessageBox(hWnd, L"Run LoLSuite as admin", L"LoLSuite", MB_OK);
		}
		else
		{
			for (const auto& proc : { L"Minecraft.exe", L"MinecraftLauncher.exe", L"javaw.exe", L"MinecraftServer.exe", L"java.exe", L"Minecraft.Windows.exe" }) term(proc);
			char appdata[MAX_PATH + 1];
			size_t size = 0;
			getenv_s(&size, appdata, MAX_PATH + 1, "APPDATA");
			std::filesystem::path configPath = std::filesystem::path(appdata) / ".minecraft";
			std::filesystem::remove_all(configPath);
			configPath /= "launcher_profiles.json";
			std::vector<std::wstring> cmds;
			cmds.push_back(L"winget uninstall Mojang.MinecraftLauncher --purge");
			for (auto* v : { L"JavaRuntimeEnvironment", L"JDK.17", L"JDK.18", L"JDK.19", L"JDK.20", L"JDK.21", L"JDK.22", L"JDK.23", L"JDK.24", L"JDK.25" })
			{
				cmds.push_back(L"winget uninstall Oracle." + std::wstring(v) + L" --purge");
			}
			cmds.push_back(L"winget install Oracle.JDK.26 --accept-package-agreements");
			cmds.push_back(L"winget install Mojang.MinecraftLauncher");
			shell(cmds);
			runEx(L"C:\\Program Files (x86)\\Minecraft Launcher\\MinecraftLauncher.exe", { .wait = false, .params = L"" });
			while (!std::filesystem::exists(configPath)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
			std::wifstream in(configPath);
			in.imbue(std::locale("en_US.UTF-8"));
			std::wstring config((std::istreambuf_iterator<wchar_t>(in)), std::istreambuf_iterator<wchar_t>());
			in.close();
			std::wstring updated;
			std::wstringstream ss(config);
			std::wstring line;
			while (std::getline(ss, line)) {
				if (line.find(L"\"javaDir\"") == std::wstring::npos && line.find(L"\"skipJreVersionCheck\"") == std::wstring::npos)
					updated += line + L"\n";
			}
			std::wstring jdkpath = L"C:\\\\Program Files\\\\Java\\\\jdk-26.0.2\\\\bin\\\\javaw.exe";
			for (auto& type : { L"\"type\" : \"latest-release\"", L"\"type\" : \"latest-snapshot\"" }) {
				size_t pos = updated.find(type);
				if (pos != std::wstring::npos) {
					size_t start = updated.rfind(L'\n', pos);
					if (start != std::wstring::npos) updated.insert(start + 1, L"      \"skipJreVersionCheck\" : true,\n");
					size_t javaDirPos = pos;
					for (int i = 0; i < 4 && javaDirPos != std::wstring::npos; ++i)
						javaDirPos = updated.rfind(L'\n', javaDirPos - 1);
					if (javaDirPos != std::wstring::npos)
						updated.insert(javaDirPos + 1, L"      \"javaDir\" : \"" + jdkpath + L"\",\n");
				}
			}
			std::wofstream out(configPath);
			out.imbue(std::locale("en_US.UTF-8"));
			out << updated;
			out.close();
			for (const auto& proc : { L"Minecraft.exe", L"MinecraftLauncher.exe", L"java.exe", L"javaw.exe", L"MinecraftServer.exe", L"Minecraft.Windows.exe" })
			{
				term(proc);
			}
			runEx(L"C:\\Program Files (x86)\\Minecraft Launcher\\MinecraftLauncher.exe", { .wait = false, .params = L"" });
		}
	}
}
void gamec() {
	if (!checkUAC())
	{
		MessageBox(hWnd, L"Run LoLSuite as admin", L"LoLSuite", MB_OK);
	}
	else
	{
		for (const auto& proc : { L"cmd.exe", L"DXSETUP.exe", L"pwsh.exe", L"powershell.exe", L"cleanmgr.exe", L"WindowsTerminal.exe", L"OpenConsole.exe", L"wt.exe", L"Battle.net.exe", L"steam.exe", L"Origin.exe", L"EADesktop.exe", L"EpicGamesLauncher.exe"}) term(proc);
		std::filesystem::path tmp = std::filesystem::current_path() / "tmp";
		ec.clear();
		std::filesystem::remove_all(tmp, ec);
		std::filesystem::create_directory(tmp, ec);

		const auto file = tmp / L"vcredist_x86.exe";

		download(L"https://download.microsoft.com/download/8/b/4/8b42259f-5d70-43f4-ac2e-4b208fd8d66a/vcredist_x64.EXE", file.c_str(), true);
		runEx(file.c_str(), { .wait = true, .checkExit = true, .hidden = true, .params = L" /q /r:n" });

			constexpr int baseIndex = 0;
			const std::vector<std::wstring> files = {
			L"Apr2005_d3dx9_25_x64.cab",
			L"Apr2005_d3dx9_25_x86.cab",
			L"Apr2006_d3dx9_30_x64.cab",
			L"Apr2006_d3dx9_30_x86.cab",
			L"Apr2006_MDX1_x86_Archive.cab",
			L"Apr2006_MDX1_x86.cab",
			L"Apr2006_XACT_x64.cab",
			L"Apr2006_XACT_x86.cab",
			L"Apr2006_xinput_x64.cab",
			L"Apr2006_xinput_x86.cab",
			L"APR2007_d3dx10_33_x64.cab",
			L"APR2007_d3dx10_33_x86.cab",
			L"APR2007_d3dx9_33_x64.cab",
			L"APR2007_d3dx9_33_x86.cab",
			L"APR2007_XACT_x64.cab",
			L"APR2007_XACT_x86.cab",
			L"APR2007_xinput_x64.cab",
			L"APR2007_xinput_x86.cab",
			L"Aug2005_d3dx9_27_x64.cab",
			L"Aug2005_d3dx9_27_x86.cab",
			L"AUG2006_XACT_x64.cab",
			L"AUG2006_XACT_x86.cab",
			L"AUG2006_xinput_x64.cab",
			L"AUG2006_xinput_x86.cab",
			L"AUG2007_d3dx10_35_x64.cab",
			L"AUG2007_d3dx10_35_x86.cab",
			L"AUG2007_d3dx9_35_x64.cab",
			L"AUG2007_d3dx9_35_x86.cab",
			L"AUG2007_XACT_x64.cab",
			L"AUG2007_XACT_x86.cab",
			L"Aug2008_d3dx10_39_x64.cab",
			L"Aug2008_d3dx10_39_x86.cab",
			L"Aug2008_d3dx9_39_x64.cab",
			L"Aug2008_d3dx9_39_x86.cab",
			L"Aug2008_XACT_x64.cab",
			L"Aug2008_XACT_x86.cab",
			L"Aug2008_XAudio_x64.cab",
			L"Aug2008_XAudio_x86.cab",
			L"Aug2009_D3DCompiler_42_x64.cab",
			L"Aug2009_D3DCompiler_42_x86.cab",
			L"Aug2009_d3dcsx_42_x64.cab",
			L"Aug2009_d3dcsx_42_x86.cab",
			L"Aug2009_d3dx10_42_x64.cab",
			L"Aug2009_d3dx10_42_x86.cab",
			L"Aug2009_d3dx11_42_x64.cab",
			L"Aug2009_d3dx11_42_x86.cab",
			L"Aug2009_d3dx9_42_x64.cab",
			L"Aug2009_d3dx9_42_x86.cab",
			L"Aug2009_XACT_x64.cab",
			L"Aug2009_XACT_x86.cab",
			L"Aug2009_XAudio_x64.cab",
			L"Aug2009_XAudio_x86.cab",
			L"Dec2005_d3dx9_28_x64.cab",
			L"Dec2005_d3dx9_28_x86.cab",
			L"DEC2006_d3dx10_00_x64.cab",
			L"DEC2006_d3dx10_00_x86.cab",
			L"DEC2006_d3dx9_32_x64.cab",
			L"DEC2006_d3dx9_32_x86.cab",
			L"DEC2006_XACT_x64.cab",
			L"DEC2006_XACT_x86.cab",
			L"DSETUP.dll",
			L"dsetup32.dll",
			L"dxdllreg_x86.cab",
			L"DXSETUP.exe",
			L"dxupdate.cab",
			L"Feb2005_d3dx9_24_x64.cab",
			L"Feb2005_d3dx9_24_x86.cab",
			L"Feb2006_d3dx9_29_x64.cab",
			L"Feb2006_d3dx9_29_x86.cab",
			L"Feb2006_XACT_x64.cab",
			L"Feb2006_XACT_x86.cab",
			L"FEB2007_XACT_x64.cab",
			L"FEB2007_XACT_x86.cab",
			L"Feb2010_X3DAudio_x64.cab",
			L"Feb2010_X3DAudio_x86.cab",
			L"Feb2010_XACT_x64.cab",
			L"Feb2010_XACT_x86.cab",
			L"Feb2010_XAudio_x64.cab",
			L"Feb2010_XAudio_x86.cab",
			L"Jun2005_d3dx9_26_x64.cab",
			L"Jun2005_d3dx9_26_x86.cab",
			L"JUN2006_XACT_x64.cab",
			L"JUN2006_XACT_x86.cab",
			L"JUN2007_d3dx10_34_x64.cab",
			L"JUN2007_d3dx10_34_x86.cab",
			L"JUN2007_d3dx9_34_x64.cab",
			L"JUN2007_d3dx9_34_x86.cab",
			L"JUN2007_XACT_x64.cab",
			L"JUN2007_XACT_x86.cab",
			L"JUN2008_d3dx10_38_x64.cab",
			L"JUN2008_d3dx10_38_x86.cab",
			L"JUN2008_d3dx9_38_x64.cab",
			L"JUN2008_d3dx9_38_x86.cab",
			L"JUN2008_X3DAudio_x64.cab",
			L"JUN2008_X3DAudio_x86.cab",
			L"JUN2008_XACT_x64.cab",
			L"JUN2008_XACT_x86.cab",
			L"JUN2008_XAudio_x64.cab",
			L"JUN2008_XAudio_x86.cab",
			L"Jun2010_D3DCompiler_43_x64.cab",
			L"Jun2010_D3DCompiler_43_x86.cab",
			L"Jun2010_d3dcsx_43_x64.cab",
			L"Jun2010_d3dcsx_43_x86.cab",
			L"Jun2010_d3dx10_43_x64.cab",
			L"Jun2010_d3dx10_43_x86.cab",
			L"Jun2010_d3dx11_43_x64.cab",
			L"Jun2010_d3dx11_43_x86.cab",
			L"Jun2010_d3dx9_43_x64.cab",
			L"Jun2010_d3dx9_43_x86.cab",
			L"Jun2010_XACT_x64.cab",
			L"Jun2010_XACT_x86.cab",
			L"Jun2010_XAudio_x64.cab",
			L"Jun2010_XAudio_x86.cab",
			L"Mar2008_d3dx10_37_x64.cab",
			L"Mar2008_d3dx10_37_x86.cab",
			L"Mar2008_d3dx9_37_x64.cab",
			L"Mar2008_d3dx9_37_x86.cab",
			L"Mar2008_X3DAudio_x64.cab",
			L"Mar2008_X3DAudio_x86.cab",
			L"Mar2008_XACT_x64.cab",
			L"Mar2008_XACT_x86.cab",
			L"Mar2008_XAudio_x64.cab",
			L"Mar2008_XAudio_x86.cab",
			L"Mar2009_d3dx10_41_x64.cab",
			L"Mar2009_d3dx10_41_x86.cab",
			L"Mar2009_d3dx9_41_x64.cab",
			L"Mar2009_d3dx9_41_x86.cab",
			L"Mar2009_X3DAudio_x64.cab",
			L"Mar2009_X3DAudio_x86.cab",
			L"Mar2009_XACT_x64.cab",
			L"Mar2009_XACT_x86.cab",
			L"Mar2009_XAudio_x64.cab",
			L"Mar2009_XAudio_x86.cab",
			L"Nov2007_d3dx10_36_x64.cab",
			L"Nov2007_d3dx10_36_x86.cab",
			L"Nov2007_d3dx9_36_x64.cab",
			L"Nov2007_d3dx9_36_x86.cab",
			L"NOV2007_X3DAudio_x64.cab",
			L"NOV2007_X3DAudio_x86.cab",
			L"NOV2007_XACT_x64.cab",
			L"NOV2007_XACT_x86.cab",
			L"Nov2008_d3dx10_40_x64.cab",
			L"Nov2008_d3dx10_40_x86.cab",
			L"Nov2008_d3dx9_40_x64.cab",
			L"Nov2008_d3dx9_40_x86.cab",
			L"Nov2008_X3DAudio_x64.cab",
			L"Nov2008_X3DAudio_x86.cab",
			L"Nov2008_XACT_x64.cab",
			L"Nov2008_XACT_x86.cab",
			L"Nov2008_XAudio_x64.cab",
			L"Nov2008_XAudio_x86.cab",
			L"Oct2005_xinput_x64.cab",
			L"Oct2005_xinput_x86.cab",
			L"OCT2006_d3dx9_31_x64.cab",
			L"OCT2006_d3dx9_31_x86.cab",
			L"OCT2006_XACT_x64.cab",
			L"OCT2006_XACT_x86.cab"
			};

			for (size_t i = 0; i < files.size(); ++i)
			{
				const int idx = baseIndex + static_cast<int>(i);

				b[idx].clear();
				Combine(idx, tmp, files[i]);

				const std::wstring url = L"DX/" + files[i];
				download(url, b[idx]);
			}

			bool allFilesPresent = std::all_of(files.begin(), files.end(),
				[&](const std::wstring& f) {
					size_t i = &f - &files[0];
					return std::filesystem::exists(b[baseIndex + i]);
				}
			);

			if (allFilesPresent)
				runEx(b[baseIndex + 63], { .wait = true, .params = L"/silent" });
		ec.clear();
		std::filesystem::remove_all(tmp, ec);

		service(L"W32Time", true);
		shell({
	L"Get-ChildItem -Path 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VolumeCaches' | ForEach-Object { $subkeyPath = $_.PsPath; $values = (Get-ItemProperty -Path $subkeyPath | Get-Member -MemberType NoteProperty | Select-Object -ExpandProperty Name); foreach ($val in $values) { if ($val -like 'StateFlags*') { Remove-ItemProperty -Path $subkeyPath -Name $val -ErrorAction SilentlyContinue } }; New-ItemProperty -Path $subkeyPath -Name 'StateFlags0001' -Value 2 -PropertyType DWord -Force }; Start-Process -FilePath 'cleanmgr' -ArgumentList '/sagerun:1'",
	L"wsreset -i",
	L"w32tm /resync",
	L"netsh int ip reset",
	L"netsh winsock reset",
	L"netsh interface ip delete arpcache",
	L"netsh winhttp reset proxy",
	L"Get-EventLog -LogName \"*\" | ForEach-Object { Clear-EventLog -LogName $_.Log }",
	L"ie4uinit -ClearIconCache",
	L"powercfg -restoredefaultschemes",
	L"Add-WindowsCapability -Online -Name NetFx3~~~~",
	L"Update-MpSignature -UpdateSource MicrosoftUpdateServer",
	L"Update-Help -UICulture en-US -Force"
			});
		if (IsWindows10OrGreater())
		{
			shell({
				L"powercfg -duplicatescheme e9a42b02-d5df-448d-aa00-03f14749eb61",
				L"sc config tzautoupdate start= auto",
				L"sc config W32Time start= auto",
				L"DISM /Online /Cleanup-Image /RestoreHealth"
				});
		}

		service(L"tzautoupdate", true);
		std::vector<std::wstring> services = { L"wuauserv", L"BITS", L"CryptSvc" };
		for (auto& s : services) service(s, false);
		WCHAR winDir[MAX_PATH + 1];
		if (GetWindowsDirectory(winDir, MAX_PATH + 1) > 0) {
			std::filesystem::remove_all(std::filesystem::path(winDir) / L"SoftwareDistribution");
		}
		for (auto& s : services)
			service(s, true);
		std::vector<std::wstring> apps = { L"Microsoft.OpenCLGLVulkanCompatibilityPack",L"Microsoft.VCRedist.2005.x86",
			L"Microsoft.VCRedist.2008.x64", L"Microsoft.VCRedist.2008.x86", L"Microsoft.VCRedist.2010.x64", L"Microsoft.VCRedist.2010.x86", L"Microsoft.VCRedist.2012.x64",
			L"Microsoft.VCRedist.2012.x86", L"Microsoft.VCRedist.2013.x64", L"Microsoft.VCRedist.2013.x86", L"Microsoft.VCRedist.2015+.x86", L"Microsoft.VCRedist.2015+.x64", L"Microsoft.PowerShell", L"Microsoft.WindowsTerminal", L"9MZPRTH5C0TB", L"9N4D0MSMP0PT", L"9N5TDP8VCMHS", L"9N95Q1ZZPMH4", L"9NCTDW2W1BH8", L"9PB0TRCNRHFX", L"9PCSD6N03BKV", L"9PG2DK419DRG", L"9PMMSR1CGPWG", L"Blizzard.BattleNet", L"ElectronicArts.EADesktop",
			L"ElectronicArts.Origin", L"EpicGames.EpicGamesLauncher", L"Valve.Steam", L"Microsoft.EdgeWebView2Runtime"
		};
		std::vector<std::wstring> filteredApps;
		for (const auto& app : apps) {
			if (!x64() &&
				app.find(L"Microsoft.VCRedist.") != std::wstring::npos &&
				app.find(L".x64") != std::wstring::npos) {
				continue;
			}
			filteredApps.push_back(app);
		}
		std::vector<std::wstring> uninstall, install;
		for (auto& app : filteredApps) {
			uninstall.push_back(L"winget uninstall " + app + L" --purge");
			if (app != L"ElectronicArts.Origin") {
				std::wstring cmd = L"winget install " + app + L" --accept-package-agreements --accept-source-agreements";
				if (app == L"Blizzard.BattleNet") cmd += L" --location \"C:\\Battle.Net\"";
				install.push_back(cmd);
			}
		}
		shell(uninstall);
		shell(install);

		HMODULE dns = LoadLibrary(L"dnsapi.dll");
		if (dns) {
			using Fn = DWORD(WINAPI*)(PCWSTR);
			auto flush = reinterpret_cast<Fn>(
				GetProcAddress(dns, "DnsFlushResolverCacheEntry_W")
				);
			if (flush) {
				flush(nullptr);
			}
			FreeLibrary(dns);
		}

		runEx(L"ipconfig", { .wait = true, .checkExit = true, .hidden = true, .params = L"/flushdns" });
		runEx(L"ipconfig", { .wait = true, .checkExit = true, .hidden = true, .params = L"/registerdns" });
		runEx(L"rundll32", { .wait = true, .checkExit = true, .hidden = true, .params = L"InetCpl.cpl,ClearMyTracksByProcess 4351"
			});
		for (const auto& proc : { L"firefox.exe", L"msedge.exe", L"chrome.exe", L"iexplore.exe", L"opera.exe" }) {
			term(proc);
		}

		auto CacheClear = [](const std::filesystem::path& path) {
			if (std::filesystem::exists(path)) {
				std::filesystem::remove_all(path);
			}
			};
		auto getFolder = [](int csidl) -> std::optional<std::filesystem::path> {
			wchar_t buf[MAX_PATH + 1]{};
			return SUCCEEDED(SHGetFolderPath(nullptr, csidl, nullptr, 0, buf))
				? std::optional<std::filesystem::path>(buf)
				: std::nullopt;
			};

		if (auto local = getFolder(CSIDL_LOCAL_APPDATA)) {
			const std::vector<std::wstring> Chromium = {
				L"Microsoft/Edge", L"Microsoft/Edge Beta", L"Microsoft/Edge Dev", L"Microsoft/Edge SxS",
				L"Google/Chrome", L"Google/Chrome Beta", L"Google/Chrome Dev", L"Google/Chrome SxS"
			};
			const std::vector<std::wstring> Caches = {
				L"Cache", L"Code Cache", L"GPUCache", L"ShaderCache"
			};
			for (const auto& vendor : Chromium) {
				for (const auto& cache : Caches) {
					CacheClear(*local / vendor / L"User Data/Default" / cache);
				}
			}
			const std::filesystem::path profiles = *local / L"Mozilla/Firefox/Profiles";
			if (std::filesystem::exists(profiles)) {
				for (const auto& entry : std::filesystem::directory_iterator(profiles)) {
					if (entry.is_directory()) {
						CacheClear(entry.path() / L"cache2");
					}
				}
			}
			const std::vector<std::wstring> opera = { L"Opera Software/Opera Stable", L"Opera Software/Opera GX Stable", L"Opera Software/Opera Air Stable", L"Opera Software/Opera Next" };
			for (const auto& browser : opera) {
				CacheClear(*local / browser / L"Default/Cache");
			}
		}
	}
	if (IsWindows10OrGreater())
	{
		HKEY hKey;
		RegOpenKeyEx(HKEY_CURRENT_USER, L"Console\\%%Startup", 0, KEY_SET_VALUE, &hKey);
		const wchar_t* value = L"WindowsTerminal";
		RegSetValueEx(hKey, L"DelegationConsole", 0, REG_SZ, reinterpret_cast<const BYTE*>(value), (wcslen(value) + 1) * sizeof(wchar_t));
		RegSetValueEx(hKey, L"DelegationTerminal", 0, REG_SZ, reinterpret_cast<const BYTE*>(value), (wcslen(value) + 1) * sizeof(wchar_t));
		RegCloseKey(hKey);
	}
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{

	case WM_CTLCOLORLISTBOX:
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORSCROLLBAR:
	{
		HDC dc = (HDC)wParam;
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(32, 32, 32));
		static HBRUSH hBrush = CreateSolidBrush(RGB(180, 210, 255));
		return (LRESULT)hBrush;

	}

	case WM_DPICHANGED:
	{
		SendMessage(hWnd, WM_SETFONT, (WPARAM)font, TRUE);
		auto r = (RECT*)lParam;
		SetWindowPos(hWnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}

	case WM_DRAWITEM:
	{
		auto* dis = (LPDRAWITEMSTRUCT)lParam;
		if (dis && dis->CtlType == ODT_BUTTON) {

			dis->itemState &= ~ODS_FOCUS;

			HPEN pen = CreatePen(PS_SOLID, 1, RGB(200, 220, 255));
			HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
			HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));

			RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom, 10, 10);
			SelectObject(dis->hDC, oldBrush);
			SelectObject(dis->hDC, oldPen);
			DeleteObject(pen);
			SetTextColor(dis->hDC, RGB(20, 40, 80));
			SetBkMode(dis->hDC, TRANSPARENT);

			wchar_t text[256];
			GetWindowText(dis->hwndItem, text, 256);
			DrawText(dis->hDC, text, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

			return TRUE;
		}
		return FALSE;
	}

	case WM_COMMAND:
	{
		const UINT id = LOWORD(wParam);

		if (HIWORD(wParam) == CBN_SELCHANGE)
			cb_index = (int)SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0);

		if (id == 1 || id == 2)
		{
			if (g_isBusy.exchange(true))
				return 0;

			EnableWindow(patch, FALSE);
			EnableWindow(listbox, FALSE);

			const int index = cb_index;
			const bool rest = (id == 2);

			std::thread([index, rest]() {

				static const std::vector<std::wstring> gameKeys = {L"dota2", L"smite2", L"mgs",
					L"blands4", L"oblivionr", L"silenthillf",
					L"outworlds2", L"minecraft"
				};

				if (index >= 0 && index < (int)gameKeys.size())
				{
					manage(gameKeys[index], rest);
				}
				if (index == 9)
				{
					gamec();
				}

				}).detach();

			PostMessage(hWnd, WM_APP + 1, 0, 0);
			return 0;
		}
		if (id == IDM_EXIT) {
			SendMessage(hWnd, WM_CLOSE, 0, 0);
			return 0;
		}
		break;
	}

	case WM_APP + 1:
		g_isBusy = false;
		EnableWindow(patch, TRUE);
		EnableWindow(listbox, TRUE);
		SetFocus(listbox);
		return 0;

	case WM_CLOSE:
		DestroyWindow(hWnd);
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProc(hWnd, msg, wParam, lParam);
}

struct Layout {
	static constexpr int W = 300;
	static constexpr int H = 130;
	static constexpr int TOP = 20;
	static constexpr int CH = 30;
	static constexpr int BW = 63;
	static constexpr int BS = 15;
	static constexpr int xPatch = BS;
	static constexpr int comboLeft = BS;
	static constexpr int comboTop = TOP + CH + 10;
	static constexpr int comboWidth = W - BS * 2;
};


int WINAPI wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nShowCmd)
{

	WNDCLASSEXW wcx{ sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, WndProc, 0, 0, hInstance, LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON)), LoadCursor(nullptr, IDC_ARROW), (HBRUSH)NULL_BRUSH, nullptr, L"LoLSuite", nullptr };
	RegisterClassEx(&wcx);

	hWnd = CreateWindowEx(
		0, L"LoLSuite", L"LoLSuite",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT,
		Layout::W, Layout::H,
		nullptr, nullptr, hInstance, nullptr
	);

	patch = CreateWindowEx(
		0, L"BUTTON", L"Patch",
		WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_OWNERDRAW | BS_DEFPUSHBUTTON,
		Layout::xPatch, Layout::TOP, Layout::BW, Layout::CH,
		hWnd, HMENU(1), hInstance, nullptr
	);

	listbox = CreateWindowEx(
		0, WC_COMBOBOX, nullptr,
		CBS_DROPDOWN | WS_CHILD | WS_VISIBLE | WS_VSCROLL,
		Layout::comboLeft, Layout::comboTop, Layout::comboWidth, 210,
		hWnd, HMENU(3), hInstance, nullptr
	);

	CoInitialize(nullptr);
	gen_shortcut();
	CoUninitialize();

	for (HWND h : {patch, listbox})
		SendMessage(h, WM_SETFONT, (WPARAM)font, TRUE);

	for (LPCWSTR s : {
		L"DOTA 2", L"SMITE 2", L"Metal Gear Solid Delta : Snake Eater", L"Borderlands 4", L"The Elder Scrolls IV: Oblivion Remastered", L"SILENT HILL f", L"The Outer Worlds 2", L"MineCraft : Java", L"Café Clients"
	})
		SendMessage(listbox, CB_ADDSTRING, 0, (LPARAM)s);

	SendMessage(listbox, CB_SETCURSEL, 0, 0);
	runProcess(L"powershell.exe", L"-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Get-AppxPackage -Name Microsoft.DesktopAppInstaller|Foreach{Add-AppxPackage -DisableDevelopmentMode -Register '$($_.InstallLocation)\\AppXManifest.xml'}\"", true);
	runProcess(L"powershell.exe", L"winget source update", true);

	ShowWindow(hWnd, nShowCmd);
	UpdateWindow(hWnd);

	desktop_clear();

	MSG msg;

	while (GetMessage(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return (int)msg.wParam;
}