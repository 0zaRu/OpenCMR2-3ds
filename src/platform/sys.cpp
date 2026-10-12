// System services on SDL3: see port/sys.h.

#include "platform/platform.h"
#include "platform/testing.h"
#include "port/sys.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

SDL_Window *s_window;
std::string s_dataDir;     // game data, read only (ends with '/')
std::string s_userDir;     // saves, settings, logs (ends with '/')
std::string s_commandLine;
std::map<std::string, std::string> s_settings;  // "section.key" -> value
FILE *s_logFile;
const PlatformTestClock *s_testClock;
const PlatformTestDialogs *s_testDialogs;
bool s_setupCanceled;

FILE *OpenHostFile(const char *path, const char *mode)
{
#ifdef _WIN32
    // SDL paths and folder dialog results are UTF-8. The Windows narrow CRT
    // uses the system code page, so open them through its Unicode interface.
    wchar_t *widePath = (wchar_t *)SDL_iconv_string("WCHAR_T", "UTF-8", path, strlen(path) + 1);
    wchar_t *wideMode = (wchar_t *)SDL_iconv_string("WCHAR_T", "UTF-8", mode, strlen(mode) + 1);
    FILE *file = widePath && wideMode ? _wfopen(widePath, wideMode) : NULL;
    SDL_free(widePath);
    SDL_free(wideMode);
    return file;
#else
    return fopen(path, mode);
#endif
}

// ---- paths -----------------------------------------------------------------

std::string Lower(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
        c = (char)SDL_tolower((unsigned char)c);
    return out;
}

// Splits a game path into its components: drive letters, "." and empty
// components dropped, ".." applied.
std::vector<std::string> SplitGamePath(const char *path)
{
    std::vector<std::string> parts;
    const char *p = path;

    if (p[0] && p[1] == ':')
        p += 2;
    std::string part;
    for (;; p++) {
        if (*p == '\\' || *p == '/' || *p == 0) {
            if (part == "..") {
                if (!parts.empty())
                    parts.pop_back();
            } else if (!part.empty() && part != ".") {
                parts.push_back(part);
            }
            part.clear();
            if (*p == 0)
                break;
        } else {
            part += *p;
        }
    }
    return parts;
}

// Directory listings, lower-case name -> name on disk.
std::map<std::string, std::map<std::string, std::string>> s_dirCache;

const std::map<std::string, std::string> &ListDir(const std::string &dir)
{
    auto it = s_dirCache.find(dir);
    if (it != s_dirCache.end())
        return it->second;

    std::map<std::string, std::string> &entries = s_dirCache[dir];
    int count = 0;
    char **names = SDL_GlobDirectory(dir.c_str(), NULL, 0, &count);
    if (names != NULL) {
        for (int i = 0; i < count; i++) {
            std::string name = names[i];
            if (!name.empty() && name.back() == '/')
                name.pop_back();
            entries.emplace(Lower(name), name);
        }
        SDL_free(names);
    }
    return entries;
}

// Finds parts under root without regard to case; returns the host path or "".
std::string FindUnder(const std::string &root, const std::vector<std::string> &parts)
{
    std::string path = root;
    for (size_t i = 0; i < parts.size(); i++) {
        const auto &entries = ListDir(path);
        auto it = entries.find(Lower(parts[i]));
        if (it == entries.end())
            return std::string();
        path += it->second;
        if (i + 1 < parts.size())
            path += '/';
    }
    return path;
}

// Host path for writing under the user directory; creates the directories,
// reusing existing ones whatever their case.
std::string PathForWriting(const std::vector<std::string> &parts)
{
    std::string path = s_userDir;
    for (size_t i = 0; i < parts.size(); i++) {
        const auto &entries = ListDir(path);
        auto it = entries.find(Lower(parts[i]));
        std::string name = it != entries.end() ? it->second : parts[i];
        bool isDir = i + 1 < parts.size();
        if (it == entries.end()) {
            if (isDir)
                SDL_CreateDirectory((path + name).c_str());
            s_dirCache[path].emplace(Lower(name), name);
        }
        path += name;
        if (isDir)
            path += '/';
    }
    return path;
}

std::string ResolveForReading(const char *gamePath)
{
    std::vector<std::string> parts = SplitGamePath(gamePath);
    if (parts.empty())
        return std::string();
    std::string found = FindUnder(s_userDir, parts);
    if (found.empty())
        found = FindUnder(s_dataDir, parts);
    return found;
}

bool MatchPattern(const std::string &name, const std::string &pattern)
{
    // One '*' at most, compared without regard to case ("*.sav").
    std::string n = Lower(name), p = Lower(pattern);
    size_t star = p.find('*');
    if (star == std::string::npos)
        return n == p;
    std::string head = p.substr(0, star), tail = p.substr(star + 1);
    return n.size() >= head.size() + tail.size() && n.compare(0, head.size(), head) == 0 &&
           n.compare(n.size() - tail.size(), tail.size(), tail) == 0;
}

// ---- settings ----------------------------------------------------------------

void LoadSettings(const std::string &file)
{
    char *contents = (char *)SDL_LoadFile(file.c_str(), NULL);
    if (contents == NULL)
        return;
    std::istringstream input(contents);
    SDL_free(contents);
    std::string line;
    std::string section;
    while (std::getline(input, line)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        size_t start = s.find_first_not_of(" \t");
        if (start == std::string::npos || s[start] == ';' || s[start] == '#')
            continue;
        s = s.substr(start);
        if (s[0] == '[') {
            section = Lower(s.substr(1, s.find(']') - 1));
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = s.substr(0, eq), value = s.substr(eq + 1);
        while (!key.empty() && key.back() == ' ')
            key.pop_back();
        value.erase(0, value.find_first_not_of(' '));
        s_settings[section + "." + Lower(key)] = value;
    }
}

// Keep unrelated settings and comments, using SDL's UTF-8 paths on Windows too.
bool SaveDataSetting(const std::string &file, const std::string &dir)
{
    if (dir.find_first_of("\r\n") != std::string::npos)
        return SDL_SetError("The folder name contains a line break");
    char *contents = (char *)SDL_LoadFile(file.c_str(), NULL);
    if (!contents)
        return false;
    std::istringstream input(contents);
    SDL_free(contents);
    std::string output, line, section;
    bool inserted = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        std::string text = start == std::string::npos ? "" : line.substr(start);
        if (!text.empty() && text[0] == '[') {
            section = Lower(text.substr(1, text.find(']') - 1));
            output += line + '\n';
            if (section == "paths" && !inserted) {
                output += "data = " + dir + '\n';
                inserted = true;
            }
            continue;
        }
        size_t eq = text.find('=');
        if (section == "paths" && eq != std::string::npos) {
            std::string key = text.substr(0, eq);
            size_t end = key.find_last_not_of(" \t");
            if (end != std::string::npos && Lower(key.substr(0, end + 1)) == "data")
                continue;
        }
        output += line + '\n';
    }
    if (!inserted)
        output += "\n[paths]\ndata = " + dir + '\n';
    const std::string temporary = file + ".tmp";
    if (!SDL_SaveFile(temporary.c_str(), output.data(), output.size()))
        return false;
    if (!SDL_RenamePath(temporary.c_str(), file.c_str())) {
        std::string error = SDL_GetError();
        SDL_RemovePath(temporary.c_str());
        return SDL_SetError("%s", error.c_str());
    }
    s_settings["paths.data"] = dir;
    return true;
}

const char s_defaultSettings[] =
    "; OpenCMR2 settings\n"
    "\n"
    "[paths]\n"
    "; Directory of the original game data (Game, FrontEnd, sounds, ...).\n"
    "; data = /path/to/cmr2\n"
    "\n"
    "[install]\n"
    "; English, American, French, German, Italian, Spanish or Polish.\n"
    "language = English\n"
    "; EUROPE, AMERICA, POLAND or JAPAN.\n"
    "sku = EUROPE\n"
    "\n"
    "[video]\n"
    "; 1 = fullscreen (on the desktop resolution), 0 = window.\n"
    "fullscreen = 1\n"
    "; Field of view, 30 to 150 (70 = original).\n"
    "; fov = 70\n"
    "; 1 = keep running when the window loses the focus or is minimised.\n"
    "; run_in_background = 0\n"
    "\n"
    "[net]\n"
    "; Network play (see docs/NETWORK.md). UDP port games are hosted on.\n"
    "; port = 28620\n"
    "; More hosts to look for games at, for internet play: names or addresses,\n"
    "; with :port when not the default (peers = 203.0.113.7, friend.example.org).\n"
    "; peers =\n"
    "; Also look at the Tailscale machines that are online (auto, 1 or 0).\n"
    "; tailscale = auto\n"
    "; Look on the local network.\n"
    "; broadcast = 1\n";

std::string MissingGameData(const std::string &dir)
{
    std::string missing;
    // These archives are common to the PC editions; folder names alone also
    // match empty directories and unextracted installer media.
    for (const char *file : {"Game/Bigfiles/common.bfl", "FrontEnd/common.bfl", "Sounds/surface.bfl"}) {
        std::string path = FindUnder(dir, SplitGamePath(file));
        SDL_PathInfo info{};
        if (path.empty() || !SDL_GetPathInfo(path.c_str(), &info) ||
            info.type != SDL_PATHTYPE_FILE || info.size == 0)
            missing += std::string("\n- ") + file;
    }
    return missing;
}

std::string WithSlash(std::string dir)
{
    if (!dir.empty() && dir.back() != '/')
        dir += '/';
    return dir;
}

bool AskForGameFolder(const std::string &message, bool warning = false)
{
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Exit"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Select folder"}
    };
    SDL_MessageBoxData box{};
    box.flags = warning ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION;
    box.title = "OpenCMR2 - Game setup";
    box.message = message.c_str();
    box.numbuttons = 2;
    box.buttons = buttons;
    int choice = 0;
    bool shown = s_testDialogs ? s_testDialogs->messageBox(&box, &choice) : SDL_ShowMessageBox(&box, &choice);
    if (shown && choice != 1)
        s_setupCanceled = true;
    return shown && choice == 1;
}

struct FolderSelection {
    std::atomic<bool> done{false};
    std::string path, error;
};

void SDLCALL FolderSelected(void *userdata, const char *const *files, int)
{
    auto &selection = *(FolderSelection *)userdata;
    if (!files) {
        selection.error = SDL_GetError();
        if (selection.error.empty())
            selection.error = "The folder selector is unavailable.";
    } else if (*files)
        selection.path = *files; // SDL frees the list after this callback.
    selection.done.store(true, std::memory_order_release);
}

bool SelectGameData(const std::string &settingsFile)
{
    std::string message = "Welcome to OpenCMR2\n\n"
                          "Choose the folder of your Colin McRae Rally 2.0 copy for PC.\n"
                          "It should contain Game, FrontEnd and Sounds.\n\n"
                          "Your files stay in place. You only need to choose this folder once.";
    bool warning = false;
    while (AskForGameFolder(message, warning)) {
        FolderSelection selection;
        if (s_testDialogs)
            s_testDialogs->openFolder(FolderSelected, &selection);
        else
            SDL_ShowOpenFolderDialog(FolderSelected, &selection, NULL, NULL, false);
        // Linux portals need events even though the game window is not open yet.
        // The callback can run on another thread; publish its strings atomically.
        while (!selection.done.load(std::memory_order_acquire)) {
            SDL_PumpEvents();
            SDL_Delay(10);
        }
        if (!selection.error.empty()) {
            message = "The folder selector could not be opened.\n\n" + selection.error +
                      "\n\nYou can also set data = <folder> under [paths] in:\n" + settingsFile;
            Sys_Log("%s", message.c_str());
            warning = true;
            continue;
        }
        if (selection.path.empty()) {
            s_setupCanceled = true;
            return false; // Cancel is a normal exit, with no extra error box.
        }
        std::string dir = WithSlash(selection.path);
        s_dirCache.clear(); // Allow retry after the user copies missing files.
        std::string missing = MissingGameData(dir);
        if (!missing.empty()) {
            message = "This folder is missing required game files:" + missing +
                      "\n\nSelect the folder of a complete PC installation.\n"
                      "Files on a CD or ISO may need to be installed or extracted first.";
            warning = true;
            continue;
        }
        if (!SaveDataSetting(settingsFile, dir)) {
            message = "The game folder is valid, but its location could not be saved.\n\n" +
                      std::string(SDL_GetError()) + "\n\nCheck that this settings file is writable:\n" + settingsFile;
            Sys_Log("%s", message.c_str());
            warning = true;
            continue;
        }
        s_dataDir = dir;
        return true;
    }
    return false;
}

} // namespace

// ---- platform lifecycle ------------------------------------------------------

bool Platform_Init(int argc, char **argv)
{
    s_dataDir.clear();
    s_commandLine.clear();
    s_settings.clear();
    s_dirCache.clear();
    s_setupCanceled = false;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    std::string userDir;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--user-dir") == 0 && i + 1 < argc)
            userDir = argv[++i];
    }
    if (!userDir.empty()) {
        s_userDir = WithSlash(userDir);
        if (!SDL_CreateDirectory(s_userDir.c_str())) {
            fprintf(stderr, "Cannot create user directory: %s\n", SDL_GetError());
            return false;
        }
    } else {
        char *pref = SDL_GetPrefPath("OpenCMR2", "OpenCMR2");
        s_userDir = pref ? pref : "./";
        SDL_free(pref);
    }
    s_logFile = OpenHostFile((s_userDir + "opencmr2.log").c_str(), "w");

    std::string settingsFile = s_userDir + "opencmr2.ini";
    if (!SDL_GetPathInfo(settingsFile.c_str(), NULL)) {
        SDL_SaveFile(settingsFile.c_str(), s_defaultSettings, sizeof(s_defaultSettings) - 1);
    }
    LoadSettings(settingsFile);

    std::string dataDir;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            dataDir = argv[++i];
        } else if (strcmp(argv[i], "--user-dir") == 0 && i + 1 < argc) {
            ++i;
        } else {
            if (!s_commandLine.empty())
                s_commandLine += ' ';
            s_commandLine += argv[i];
        }
    }

    std::vector<std::string> candidates;
    if (!dataDir.empty()) {
        const std::string missing = MissingGameData(WithSlash(dataDir));
        if (!missing.empty()) {
            const std::string message = "Missing required game files in " + dataDir + ":" + missing;
            fprintf(stderr, "%s\n", message.c_str());
            Sys_Log("%s", message.c_str());
            Platform_Shutdown();
            return false;
        }
        candidates.push_back(dataDir);
    }
    if (const char *env = SDL_getenv("OPENCMR2_DATA"))
        candidates.push_back(env);
    // A portable Data folder stays usable if the package moves to another PC,
    // even when its settings remember an external installation on the old PC.
    candidates.push_back("./Data");
    if (const char *base = SDL_GetBasePath())
        candidates.push_back(std::string(base) + "Data");
    if (const char *setting = Platform_GetSetting("paths.data", NULL))
        candidates.push_back(setting);
    candidates.push_back(".");
    if (const char *base = SDL_GetBasePath())
        candidates.push_back(base);

    for (const std::string &candidate : candidates) {
        if (candidate.empty())
            continue;
        std::string dir = WithSlash(candidate);
        if (MissingGameData(dir).empty()) {
            s_dataDir = dir;
            break;
        }
    }
    if (s_dataDir.empty() && !SelectGameData(settingsFile)) {
        Platform_Shutdown();
        return false;
    }
    Sys_Log("game data: %s", s_dataDir.c_str());
    Sys_Log("user directory: %s", s_userDir.c_str());
    return true;
}

void Platform_Shutdown(void)
{
    Sys_DestroyWindow();
    if (s_logFile != NULL) {
        fclose(s_logFile);
        s_logFile = NULL;
    }
    SDL_Quit();
}

SDL_Window *Platform_GetWindow(void)
{
    return s_window;
}

bool Platform_WasSetupCanceled(void) { return s_setupCanceled; }

const char *Platform_GetDataDir(void)
{
    return s_dataDir.c_str();
}

const char *Platform_GetUserDir(void)
{
    return s_userDir.c_str();
}

const char *Platform_GetSetting(const char *key, const char *fallback)
{
    auto it = s_settings.find(Lower(key));
    return it != s_settings.end() ? it->second.c_str() : fallback;
}

int Platform_GetSettingInt(const char *key, int fallback)
{
    const char *value = Platform_GetSetting(key, NULL);
    return value != NULL ? (int)strtol(value, NULL, 0) : fallback;
}

// ---- time ----------------------------------------------------------------------

extern "C" DWORD Sys_GetTicks(void)
{
    if (s_testClock)
        return s_testClock->ticks();
    return (DWORD)SDL_GetTicks();
}

extern "C" unsigned int Sys_GetUnixTime(void)
{
    return s_testClock ? s_testClock->unixTime : (unsigned int)time(NULL);
}

void Platform_SetTestClock(const PlatformTestClock *clock) { s_testClock = clock; }
void Platform_SetTestDialogs(const PlatformTestDialogs *dialogs) { s_testDialogs = dialogs; }

extern "C" void Sys_Sleep(DWORD ms)
{
    if (s_testClock) {
        s_testClock->sleep(ms);
        return;
    }
    SDL_Delay(ms);
}

// ---- events ----------------------------------------------------------------------

namespace {

// Windows-1252 code of a Unicode code point, or 0.
int ToWindows1252(Uint32 cp)
{
    static const Uint16 high[32] = { 0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
                                     0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
                                     0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
                                     0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178 };
    if (cp < 0x80 || (cp >= 0xa0 && cp <= 0xff))
        return (int)cp;
    for (int i = 0; i < 32; i++)
        if (high[i] == cp)
            return 0x80 + i;
    return 0;
}

// Text input can carry several characters; they are handed out one by one.
std::vector<int> s_pendingText;

bool RunInBackground()
{
    return Platform_GetSettingInt("video.run_in_background", 0) != 0;
}

bool Translate(const SDL_Event &e, SysEvent *out)
{
    memset(out, 0, sizeof(*out));
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        out->type = SYS_EVENT_QUIT;
        return true;
    // The game pauses when its window loses the focus or is minimised, as
    // the original did; video.run_in_background keeps it running (network
    // play, two copies on one machine).
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        out->type = SYS_EVENT_FOCUS_LOST;
        return !RunInBackground();
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        out->type = SYS_EVENT_FOCUS_GAINED;
        return !RunInBackground();
    case SDL_EVENT_WINDOW_MINIMIZED:
        out->type = SYS_EVENT_MINIMIZED;
        return !RunInBackground();
    case SDL_EVENT_WINDOW_RESTORED:
        out->type = SYS_EVENT_RESTORED;
        return !RunInBackground();
    case SDL_EVENT_KEY_DOWN:
        out->type = SYS_EVENT_KEY_DOWN;
        out->key = Keys_FromScancode(e.key.scancode);
        out->repeat = e.key.repeat;
        return out->key != 0;
    case SDL_EVENT_TEXT_INPUT: {
        const char *text = e.text.text;
        Uint32 cp;
        while ((cp = SDL_StepUTF8(&text, NULL)) != 0) {
            int c = ToWindows1252(cp);
            if (c != 0)
                s_pendingText.push_back(c);
        }
        return false;
    }
    default:
        return false;
    }
}

bool TakePendingText(SysEvent *out)
{
    if (s_pendingText.empty())
        return false;
    memset(out, 0, sizeof(*out));
    out->type = SYS_EVENT_TEXT;
    out->character = s_pendingText.front();
    s_pendingText.erase(s_pendingText.begin());
    return true;
}

} // namespace

extern "C" int Sys_PollEvent(SysEvent *event)
{
    SDL_Event e;
    if (TakePendingText(event))
        return 1;
    while (SDL_PollEvent(&e)) {
        if (Translate(e, event))
            return 1;
        if (TakePendingText(event))
            return 1;
    }
    return 0;
}

extern "C" int Sys_WaitEvent(SysEvent *event)
{
    SDL_Event e;
    if (TakePendingText(event))
        return 1;
    while (SDL_WaitEvent(&e)) {
        if (Translate(e, event))
            return 1;
        if (TakePendingText(event))
            return 1;
    }
    return 0;
}

// ---- window ----------------------------------------------------------------------

extern "C" BOOL Sys_CreateWindow(const char *title, BOOL fullscreen)
{
    if (s_window != NULL)
        return TRUE;
    // The renderer sizes the window once it knows the resolution.
    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
    s_window = SDL_CreateWindow(title, 640, 480, flags);
    if (s_window == NULL) {
        Sys_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return FALSE;
    }
    SDL_StartTextInput(s_window);
    return TRUE;
}

extern "C" void Sys_DestroyWindow(void)
{
    if (s_window != NULL) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
    }
}

extern "C" void Sys_ShowCursor(BOOL show)
{
    if (show)
        SDL_ShowCursor();
    else
        SDL_HideCursor();
}

extern "C" void Sys_GetDesktopSize(int *width, int *height)
{
    SDL_DisplayID display = s_window ? SDL_GetDisplayForWindow(s_window) : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display);
    *width = mode ? mode->w : 640;
    *height = mode ? mode->h : 480;
}

// ---- messages ----------------------------------------------------------------------

extern "C" int Sys_MessageBox(const char *title, const char *text, int buttons)
{
    SDL_MessageBoxButtonData okButtons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
          SYS_MESSAGEBOX_RESULT_OK, "OK" },
    };
    SDL_MessageBoxButtonData retryButtons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, SYS_MESSAGEBOX_RESULT_RETRY, "Retry" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, SYS_MESSAGEBOX_RESULT_CANCEL, "Cancel" },
    };
    SDL_MessageBoxData data = {};
    data.flags = SDL_MESSAGEBOX_ERROR;
    data.window = s_window;
    data.title = title;
    data.message = text;
    if (buttons == SYS_MESSAGEBOX_RETRY_CANCEL) {
        data.numbuttons = 2;
        data.buttons = retryButtons;
    } else {
        data.numbuttons = 1;
        data.buttons = okButtons;
    }
    int result = SYS_MESSAGEBOX_RESULT_CANCEL;
    if (!SDL_ShowMessageBox(&data, &result))
        Sys_Log("message box \"%s\": %s", title, text);
    return result;
}

extern "C" void Sys_Log(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    fprintf(stderr, "%s\n", line);
    if (s_logFile != NULL) {
        fprintf(s_logFile, "%s\n", line);
        fflush(s_logFile);
    }
}

extern "C" void Sys_Fatal(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    Sys_Log("fatal: %s", line);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenCMR2", line, s_window);
    exit(1);
}

extern "C" void Sys_Exit(int code)
{
    Platform_Shutdown();
    exit(code);
}

extern "C" const char *Sys_GetCommandLine(void)
{
    return s_commandLine.c_str();
}

// ---- files ----------------------------------------------------------------------

struct SysFile {
    FILE *f;
};

extern "C" BOOL Sys_ResolvePath(const char *path, BOOL forWriting, char *out, size_t outSize)
{
    std::string host;
    if (forWriting) {
        std::vector<std::string> parts = SplitGamePath(path);
        if (parts.empty())
            return FALSE;
        host = PathForWriting(parts);
    } else {
        host = ResolveForReading(path);
        if (host.empty())
            return FALSE;
    }
    if (host.size() + 1 > outSize)
        return FALSE;
    memcpy(out, host.c_str(), host.size() + 1);
    return TRUE;
}

extern "C" SysFile *Sys_OpenFile(const char *path, int mode)
{
    char host[1024];
    if (!Sys_ResolvePath(path, mode == SYS_FILE_WRITE, host, sizeof(host)))
        return NULL;
    FILE *f = OpenHostFile(host, mode == SYS_FILE_WRITE ? "wb" : "rb");
    if (f == NULL)
        return NULL;
    SysFile *file = new SysFile;
    file->f = f;
    return file;
}

extern "C" void Sys_CloseFile(SysFile *file)
{
    if (file != NULL) {
        fclose(file->f);
        delete file;
    }
}

extern "C" DWORD Sys_ReadFile(SysFile *file, void *buffer, DWORD size)
{
    return (DWORD)fread(buffer, 1, size, file->f);
}

extern "C" DWORD Sys_WriteFile(SysFile *file, const void *buffer, DWORD size)
{
    return (DWORD)fwrite(buffer, 1, size, file->f);
}

extern "C" DWORD Sys_SeekFile(SysFile *file, LONG offset, int origin)
{
    int whence = origin == SYS_SEEK_CUR ? SEEK_CUR : origin == SYS_SEEK_END ? SEEK_END : SEEK_SET;
    if (fseek(file->f, offset, whence) != 0)
        return 0xffffffff;
    return (DWORD)ftell(file->f);
}

extern "C" DWORD Sys_GetFileSize(SysFile *file)
{
    long position = ftell(file->f);
    if (position < 0 || fseek(file->f, 0, SEEK_END) != 0)
        return 0xffffffff;
    long size = ftell(file->f);
    fseek(file->f, position, SEEK_SET);
    return size < 0 ? 0xffffffff : (DWORD)size;
}

extern "C" void Sys_FlushFile(SysFile *file)
{
    fflush(file->f);
}

extern "C" BOOL Sys_FileExists(const char *path)
{
    return !ResolveForReading(path).empty();
}

extern "C" BOOL Sys_DeleteFile(const char *path)
{
    // Only files in the user directory can be deleted.
    std::string host = FindUnder(s_userDir, SplitGamePath(path));
    if (host.empty() || !SDL_RemovePath(host.c_str()))
        return FALSE;
    s_dirCache.clear();
    return TRUE;
}

extern "C" void Sys_ListFiles(const char *directory, const char *pattern, SysListCallback callback, void *context)
{
    std::vector<std::string> parts = SplitGamePath(directory);
    std::map<std::string, std::string> names;
    for (const std::string *root : { &s_dataDir, &s_userDir }) {
        std::string dir = parts.empty() ? *root : FindUnder(*root, parts);
        if (dir.empty())
            continue;
        dir = WithSlash(dir);
        for (const auto &entry : ListDir(dir)) {
            SDL_PathInfo info;
            if (MatchPattern(entry.second, pattern) && SDL_GetPathInfo((dir + entry.second).c_str(), &info) &&
                info.type == SDL_PATHTYPE_FILE)
                names[entry.first] = entry.second;   // the user directory's copy wins
        }
    }
    for (const auto &name : names)
        if (!callback(name.second.c_str(), context))
            break;
}

// ---- settings ----------------------------------------------------------------------

extern "C" const char *Sys_GetInstallSetting(const char *name)
{
    if (SDL_strcasecmp(name, "Game_HDPath") == 0 || SDL_strcasecmp(name, "Game_CDPath") == 0)
        return ".";
    if (SDL_strcasecmp(name, "Install_Version") == 0)
        return "Full";
    if (SDL_strcasecmp(name, "Language") == 0)
        return Platform_GetSetting("install.language", "English");
    if (SDL_strcasecmp(name, "Sku_Type") == 0)
        return Platform_GetSetting("install.sku", "EUROPE");
    return "";
}

extern "C" int Sys_GetOption(const char *name, int fallback)
{
    return Platform_GetSettingInt(name, fallback);
}
