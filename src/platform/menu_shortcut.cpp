// Applications menu entry of an unpacked release: a .desktop file and icon
// under XDG_DATA_HOME on Linux, a Start menu shortcut on Windows. Kept apart
// from sys.cpp because <windows.h> clashes with port/types.h.

#include "platform/platform.h"

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include "opencmr2_icon.h"
#endif

namespace fs = std::filesystem;

#ifdef _WIN32

namespace {

std::wstring ExecutablePath()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD length = GetModuleFileNameW(NULL, path.data(), (DWORD)path.size());
        if (length == 0)
            return std::wstring();
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

fs::path ShortcutPath()
{
    PWSTR programs = NULL;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, NULL, &programs)))
        return fs::path();
    fs::path path = fs::path(programs) / L"OpenCMR2.lnk";
    CoTaskMemFree(programs);
    return path;
}

bool WriteShortcut(const std::wstring &exe, const fs::path &file)
{
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE)
        return SDL_SetError("COM is unavailable (0x%08lx)", (unsigned long)init);
    IShellLinkW *link = NULL;
    IPersistFile *persist = NULL;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&link);
    if (SUCCEEDED(result)) {
        link->SetPath(exe.c_str());
        link->SetWorkingDirectory(fs::path(exe).parent_path().c_str());
        link->SetIconLocation(exe.c_str(), 0);
        link->SetDescription(L"Colin McRae Rally 2.0 (needs the original game files)");
        result = link->QueryInterface(IID_IPersistFile, (void **)&persist);
    }
    if (SUCCEEDED(result))
        result = persist->Save(file.c_str(), TRUE);
    if (persist != NULL)
        persist->Release();
    if (link != NULL)
        link->Release();
    if (SUCCEEDED(init))
        CoUninitialize();
    return SUCCEEDED(result) ? true : SDL_SetError("Cannot create the shortcut (0x%08lx)", (unsigned long)result);
}

} // namespace

bool Shortcut_IsSupported(void)
{
    return true;
}

const char *Shortcut_MenuName(void)
{
    return "the Start menu";
}

bool Shortcut_Update(bool present)
{
    const fs::path file = ShortcutPath();
    if (file.empty())
        return SDL_SetError("The Start menu folder was not found");
    if (!present) {
        std::error_code error;
        fs::remove(file, error);
        return !error || SDL_SetError("%s", error.message().c_str());
    }
    const std::wstring exe = ExecutablePath();
    if (exe.empty())
        return SDL_SetError("The game's own path is unknown");
    return WriteShortcut(exe, file);
}

#elif defined(__linux__) && !defined(__ANDROID__)

namespace {

const char s_appId[] = "io.github.pablocpas.OpenCMR2";

fs::path DataHome()
{
    const char *xdg = SDL_getenv("XDG_DATA_HOME");
    if (xdg != NULL && xdg[0] == '/')
        return fs::u8path(xdg);
    const char *home = SDL_getenv("HOME");
    return home != NULL && home[0] == '/' ? fs::u8path(home) / ".local/share" : fs::path();
}

// The file the menu starts: the AppImage itself when running from one.
std::string ExecutablePath()
{
    const char *appImage = SDL_getenv("APPIMAGE");
    if (appImage != NULL && appImage[0] == '/')
        return appImage;
    std::error_code error;
    fs::path exe = fs::read_symlink("/proc/self/exe", error);
    return error ? std::string() : exe.u8string();
}

// A quoted Exec argument (Desktop Entry Specification, "The Exec key").
std::string ExecArgument(const std::string &path)
{
    std::string out = "\"";
    for (char c : path) {
        if (c == '"' || c == '`' || c == '$')
            out += "\\\\";
        else if (c == '\\')
            out += "\\\\\\";
        else if (c == '%')
            out += '%';
        out += c;
    }
    return out + '"';
}

// A string value: only backslashes need escaping in a path.
std::string StringValue(const std::string &text)
{
    std::string out;
    for (char c : text) {
        if (c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

// Writes the file unless it already holds these bytes.
bool WriteIfChanged(const fs::path &file, const void *data, size_t size)
{
    size_t oldSize = 0;
    void *old = SDL_LoadFile(file.u8string().c_str(), &oldSize);
    bool same = old != NULL && oldSize == size && SDL_memcmp(old, data, size) == 0;
    SDL_free(old);
    if (same)
        return true;
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    const std::string temporary = file.u8string() + ".tmp";
    if (!SDL_SaveFile(temporary.c_str(), data, size))
        return false;
    if (!SDL_RenamePath(temporary.c_str(), file.u8string().c_str())) {
        SDL_RemovePath(temporary.c_str());
        return false;
    }
    return true;
}

} // namespace

// Flatpak and distribution packages install their own menu entry.
bool Shortcut_IsSupported(void)
{
    if (SDL_getenv("FLATPAK_ID") != NULL || SDL_getenv("SNAP") != NULL ||
        SDL_GetPathInfo("/.flatpak-info", NULL))
        return false;
    const std::string exe = ExecutablePath();
    return !exe.empty() && exe.compare(0, 5, "/usr/") != 0 && !DataHome().empty();
}

const char *Shortcut_MenuName(void)
{
    return "your applications menu";
}

bool Shortcut_Update(bool present)
{
    const fs::path home = DataHome();
    if (home.empty())
        return SDL_SetError("Neither XDG_DATA_HOME nor HOME is set");
    const fs::path desktop = home / "applications" / (std::string(s_appId) + ".desktop");
    const fs::path icon = home / "icons/hicolor/256x256/apps" / (std::string(s_appId) + ".png");
    if (!present) {
        std::error_code error;
        fs::remove(desktop, error);
        fs::remove(icon, error);
        return true;
    }
    const std::string exe = ExecutablePath();
    if (exe.empty())
        return SDL_SetError("The game's own path is unknown");
    const std::string entry = "[Desktop Entry]\n"
                              "Type=Application\n"
                              "Name=OpenCMR2\n"
                              "GenericName=Rally game\n"
                              "Comment=Colin McRae Rally 2.0 (needs the original game files)\n"
                              "Exec=" + ExecArgument(exe) + "\n"
                              // By path: a theme name would depend on the user's
                              // icon cache, which GTK does not refresh for a new file.
                              "Icon=" + StringValue(icon.u8string()) + "\n"
                              "Terminal=false\n"
                              "Categories=Game;SportsGame;\n"
                              "StartupWMClass=" + s_appId + "\n";
    return WriteIfChanged(icon, g_opencmr2_256_png, sizeof(g_opencmr2_256_png)) &&
           WriteIfChanged(desktop, entry.data(), entry.size());
}

#else

// macOS and Android: the app bundle or package is the menu entry.
bool Shortcut_IsSupported(void) { return false; }
const char *Shortcut_MenuName(void) { return ""; }
bool Shortcut_Update(bool) { return true; }

#endif
