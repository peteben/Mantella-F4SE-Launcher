#include <windows.h>
#include <iostream>
#include <sstream>
#include <string>
#include <codecvt>
#include <filesystem>
#include <ShlObj.h>
#include <cstdio>
#include <tlhelp32.h>
#include <comdef.h>


void init_log() {
    std::optional<std::filesystem::path> logpath = logger::log_directory();

    const char* plugin_name = "MantellaLauncher";
    *logpath /= fmt::format(FMT_STRING("{}.log"), plugin_name);
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logpath->string(), true);

    auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

    log->set_level(spdlog::level::trace);
    log->flush_on(spdlog::level::trace);

    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%T.%e] [%=5t] [%L] %v"s);

    logger::info("{} {}", "MantellaLauncher", "1.0");
    }


// Function to convert wchar_t* to std::string
std::string WideStringToString(const wchar_t* wideString) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(wideString);
    };


/**
* Set the environment path to store Mantella.exe data
*
* When Pyinstaller .exes are run, temporary files are stored in AppData\Local\Temp by default.
* When an exe gracefully exits, these files are automatically deleted.
* However, when players close Mantella.exe manually, these files get left behind,
* so they must be identified and deleted when Mantella.exe is next run.
* Changing the storage location of these temporary files
* provides transparency to the files Mantella.exe is creating and deleting.
**/
bool SetEnvironmentTempPath() {
    PWSTR documentsPath = nullptr;
    std::wstring newTempPath;

    // Get the path to the Documents folder
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &documentsPath))) {
        bool useSecondary = wcsstr(documentsPath, L"OneDrive") != nullptr;

        if (useSecondary) {
            // Don't use the Documents path if it is synced to OneDrive (cloud)
            // The large temp files can easily overflow the default 5GB free allocation
            // Also, the temporary voice files get created, renamed and deleted rapidly,
            // making it difficult for Onedrive and causing problems with locked files
            logger::info("OneDrive detected!");
            }
        else {
            newTempPath = std::wstring(documentsPath) + L"\\My Games\\Mantella\\data\\tmp";
            CoTaskMemFree(documentsPath);  // Release the memory

            // Attempt to create the directory path if it doesn't exist
            try {
                std::filesystem::create_directories(newTempPath);
                }
            catch (const std::filesystem::filesystem_error& e) {
                std::wcerr << L"Failed to create directory path: " << newTempPath << L". Error: " << e.what() << std::endl;
                std::wcerr << L"Falling back to system temporary directory." << std::endl;
                useSecondary = true;
                }
            }

        if (useSecondary) {            // Fallback to system temp directory
            wchar_t tempPath[MAX_PATH];
            if (GetTempPath(MAX_PATH, tempPath) != 0) {
                newTempPath = std::wstring(tempPath) + L"Mantella";
                try {
                    std::filesystem::create_directories(newTempPath);
                    }
                catch (const std::filesystem::filesystem_error& e) {
                    std::wcerr << L"Failed to create fallback directory: " << newTempPath << L". Error: " << e.what()
                        << std::endl;
                    return false;
                    }
                }
            else {
                std::cerr << "Failed to get system temporary directory." << std::endl;
                return false;
                }
            }
        }
    else {
        std::cerr << "Failed to get Documents folder path." << std::endl;
        return false;
        }

    logger::info("Mantella temp files in {}", WideStringToString(newTempPath.c_str()));
    // Set new TEMP and TMP environment variables for the current process
    if (!SetEnvironmentVariable(L"TEMP", newTempPath.c_str()) || !SetEnvironmentVariable(L"TMP", newTempPath.c_str())) {
        std::cerr << "Failed to set TEMP/TMP environment variables." << std::endl;
        return false;
        }

    return true;
    };


// Common function to get module directory
std::wstring GetModuleDirectoryBase(int levels_up) {
    WCHAR path[MAX_PATH];
    HMODULE hModule = NULL;

    // Get handle to the current module (DLL)
    if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&GetModuleDirectoryBase, &hModule) == 0) {
        return L"";
        }

    // Get the path to the current module (DLL)
    DWORD size = GetModuleFileName(hModule, path, MAX_PATH);
    if (size == 0) {
        return L"";
        }

    // Convert path to std::wstring for easier manipulation
    std::wstring wpath(path);

    // Use filesystem to obtain the directory part of the path
    std::filesystem::path fs_path(wpath);
    for (int i = 0; i < levels_up; ++i) {
        fs_path = fs_path.parent_path();
        }

    return fs_path.wstring();
    };


// Helper function to retrieve the current module's directory
std::wstring GetCurrentModuleDirectory() {
    return GetModuleDirectoryBase(1);  // Go up one level (parent directory)
    };


// Helper function to retrieve the top-level game directory
std::wstring GetTopLevelDirectory() {
    return GetModuleDirectoryBase(4);  // Go up four levels to game directory
    };

// Finds all running processes called 'Mantella.exe'. It should be pretty safe to assume that ours are the only ones running on the system.
// Acquires HANDLE's with PROCESS_QUERY_INFORMATION and PROCESS_TERMINATE access rights to them.
std::vector<HANDLE> LocateExistingMantellaProcesses() {
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    std::vector<HANDLE> result;
    if (Process32First(snapshot, &entry) == TRUE) {
        while (Process32Next(snapshot, &entry) == TRUE) {
            _bstr_t b(entry.szExeFile);
            const char* fileName = b;
            if (_stricmp(fileName, "Mantella.exe") == 0) {
                result.push_back(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE, FALSE, entry.th32ProcessID));
                }
            }
        }

    CloseHandle(snapshot);

    return result;
    };

/**
* Launch Mantella.exe
**/
bool LaunchMantellaExe() {
    STARTUPINFO si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;  // SW_HIDE  SW_SHOWNORMAL SW_SHOWNOACTIVATE

    std::wstring moduleDir = GetCurrentModuleDirectory();
    std::wstring skyrimDir = GetTopLevelDirectory();
    std::wstring exePath = moduleDir + L"\\MantellaSoftware\\Mantella.exe";  // Construct the full path to Mantella.exe


    if (!SetEnvironmentTempPath()) {
        return false;  // TODO: Handle error
        }

    // Convert the full path to a narrow string for printing (optional)
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    std::string exePathStr = converter.to_bytes(exePath.c_str());
    // Convert and print the full path attempting to launch
    std::string exePathStr2 = WideStringToString(exePath.c_str());
    logger::info("EXE path: {}", exePathStr2);
    RE::ConsoleLog::GetSingleton()->PrintLine(("Attempting to launch: " + exePathStr2).c_str());

    const wchar_t* params = L"--integrated";

    std::wstring commandLine = exePath + L" " + params;

    std::vector<HANDLE> currentMantellaProcesses = LocateExistingMantellaProcesses();
    // Check if Mantella.exe is already running and if yes, close all of them
    for (const HANDLE& currentMantellaProcess : currentMantellaProcesses) {
        if (currentMantellaProcess != NULL) {
            DWORD exitCode;
            if (GetExitCodeProcess(currentMantellaProcess, &exitCode) && exitCode == STILL_ACTIVE) {
                // Mantella.exe is still running, terminate it
                if (!TerminateProcess(currentMantellaProcess, 0)) {
                    std::stringstream ss;
                    ss << "Failed to terminate existing Mantella.exe process. TerminateProcess error: "
                        << GetLastError();
                    RE::ConsoleLog::GetSingleton()->PrintLine(ss.str().c_str());
                    }
                WaitForSingleObject(currentMantellaProcess, INFINITE);  // Ensure the process is completely terminated
                RE::ConsoleLog::GetSingleton()->PrintLine("Existing Mantella.exe process terminated.");
                }
            else {
                // Process is not active, close the handle
                }
            CloseHandle(currentMantellaProcess);
            }
        }

    // Start Mantella.exe
    if (!CreateProcess(NULL, &commandLine[0], NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, moduleDir.c_str(), &si,
        &pi)) {
        std::stringstream ss;
        ss << "Failed to launch Mantella.exe. CreateProcess error: " << GetLastError();
        RE::ConsoleLog::GetSingleton()->PrintLine(ss.str().c_str());
        return false;
        }
    else {
        SetConsoleTitle(L"Mantella");
        }

    //Close thread handle
    CloseHandle(pi.hThread);

    return true;
    };



bool LaunchMantellaExePapyrus(std::monostate) {
    return LaunchMantellaExe();
    };


bool RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm) {
    a_vm->BindNativeMethod("MantellaLauncher", "LaunchMantellaExe", LaunchMantellaExePapyrus, true);
    return true;
    }

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface*, F4SE::PluginInfo* a_info) {

    a_info->infoVersion = F4SE::PluginInfo::kVersion;
    a_info->name = "MantellaLauncher";
    a_info->version = 1;

    //const auto ver = a_f4se->RuntimeVersion();
    //if (ver < F4SE::RUNTIME_1_10_162 && ver != F4SE::RUNTIME_VR_1_2_72) {
    //	logger::critical("unsupported runtime v{}", ver.string());
    //	return false;
    //}

    return true;
    }

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se) {
    F4SE::Init(a_f4se, false);

    init_log();

    const auto papyrus = F4SE::GetPapyrusInterface();

    if (!papyrus || !papyrus->Register(RegisterFunctions)) {
        //RE::ConsoleLog::GetSingleton()->PrintLine("Failed to register Papyrus functions!");
        logger::info("Failed to register Papyrus functions");
        }
    else {
        logger::info("Registered Papyrus functions.");
        //RE::ConsoleLog::GetSingleton()->PrintLine("Registered Papyrus functions.");
        }
    logger::info("Registered Papyrus");

    F4SE::GetMessagingInterface()->RegisterListener([](F4SE::MessagingInterface::Message* message) {
        if (message->type == F4SE::MessagingInterface::kGameDataReady) {
            //Get running instances of Mantella.exe here. In case there are any, we don't force spawn the integrated one.
            std::vector<HANDLE> existingProcesses = LocateExistingMantellaProcesses();
            if (existingProcesses.size() == 0) {
                logger::info("Launching EXE");
                // Attempt to launch Mantella.exe when the game data is loaded
                if (LaunchMantellaExe()) {
                    RE::ConsoleLog::GetSingleton()->PrintLine("Mantella.exe launched successfully!");
                    }
                else {
                    RE::ConsoleLog::GetSingleton()->PrintLine("Failed to launch Mantella.exe.");
                    }
                }
            else {
                for (const HANDLE& i : existingProcesses) CloseHandle(i);//close the acquired handles, we will get new ones on a potential restart
                RE::ConsoleLog::GetSingleton()->PrintLine("Found running instance of Mantella.exe. Not starting a new one. You can still restart it from the MCM.");
                }
            }
        });

    return true;
    };

F4SE_EXPORT constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData data{};

    data.PluginName("MantellaLauncher");
    data.PluginVersion(REL::Version(0, 9, 4, 0));
    data.AuthorName("peteben");
    data.UsesAddressLibrary(true);
    data.UsesSigScanning(false);
    data.IsLayoutDependent(true);
    data.HasNoStructUse(false);
    data.CompatibleVersions({ F4SE::RUNTIME_1_10_984, F4SE::RUNTIME_1_10_163, F4SE::RUNTIME_LATEST_VR });

    return data;
    }();

