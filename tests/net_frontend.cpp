// Real UDP notifications passed to the game's lobby handler. Receive writes
// native pointers into the caller's buffer; these are not 32-bit wire fields.
#include "port/types.h"
#include "net/net_session.h"
#include "GameInfo.h"
#include "NetPlayers.h"
#include "high_address.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

extern void FrontendNetwork_HandleJoinerMessage(NetPlayerID *, unsigned int *);
extern void OptionMenu_HandleItemNotification(unsigned int *, int *);
extern void NetRace_HandleDeviceNotification(DWORD *, int *);
extern NetSessionDesc g_networkSession;
extern void GameInfo_SetConfiguredGameMode(BYTE);
extern int GameInfo_GetSessionField398C();

static void Check(bool condition, const char *message)
{
    if (!condition) { fprintf(stderr, "net frontend: %s\n", message); exit(1); }
}

static void *buffer;
static void *Receive(net::Session &session, DWORD type)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < end) {
        NetPlayerID from = 0, to = 0;
        DWORD size = 4096;
        const HRESULT hr = session.Receive(&from, &to, buffer, &size);
        Check(hr == NET_OK || hr == NET_ERR_NOMESSAGES, "receive failed");
        if (hr == NET_OK && from == NET_ID_SYSTEM && *(DWORD *)buffer == type)
            return buffer;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Check(false, "notification timed out");
    return nullptr;
}

static NetPlayer *Find(NetPlayerID id)
{
    for (auto &player : g_netPlayers)
        if ((player.flags & 0x80) && player.id == int(id)) return &player;
    return nullptr;
}

int main()
{
    buffer = Test_AllocateHighAddress(2 * 1024 * 1024);
    Check(buffer != nullptr, "allocate receive buffer");
    if (sizeof(void *) == 8)
        Check(uintptr_t(buffer) > UINT32_MAX, "receive buffer must be above 4 GB");
    net::Config config;
    config.port = 0;
    config.broadcast = false;
    net::Session host(config);
    Check(host.Start() == NET_OK, "start host");
    NetSessionDesc desc{};
    desc.dwSize = sizeof(desc);
    desc.guidApplication.bytes[0] = 42;
    desc.dwMaxPlayers = 8;
    desc.lpszSessionNameA = (char *)"LAN regression";
    desc.dwUser1 = 8;
    desc.dwUser3 = 3;
    Check(host.Open(&desc, NET_OPEN_CREATE) == NET_OK, "open host");
    NetName name{sizeof(NetName), 0, (char *)"host", (char *)"host"};
    NetPlayerInfo info{0, {0x83}, 123, 456};
    NetPlayerID hostId = 0, guestId = 0;
    Check(host.CreatePlayer(&hostId, &name, &info, sizeof(info)) == NET_OK, "create host player");
    config.peers.push_back("127.0.0.1:" + std::to_string(host.BoundPort()));
    net::Session guest(config);
    Check(guest.Start() == NET_OK, "start guest");
    bool found = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!found && std::chrono::steady_clock::now() < end) {
        struct Search { NetSessionDesc *desc; bool *found; } search{&desc, &found};
        guest.EnumSessions(&desc.guidApplication, [](const NetSessionDesc *session, void *context) -> BOOL {
            auto &search = *(Search *)context;
            search.desc->guidInstance = session->guidInstance;
            *search.found = true;
            return FALSE;
        }, &search);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Check(found && guest.Open(&desc, NET_OPEN_JOIN) == NET_OK, "discover and join host");
    name.lpszShortNameA = name.lpszLongNameA = (char *)"guest";
    Check(guest.CreatePlayer(&guestId, &name, &info, sizeof(info)) == NET_OK, "create guest player");

    // The actual game state is updated, without a second game singleton:
    // outgoing descriptions are covered by net_session's transport tests.
    GameInfo_SetConfiguredGameMode(8);
    auto *create = (NetMsgCreatePlayer *)Receive(host, NET_SYS_CREATEPLAYER);
    Check(create->dpId == guestId && create->dwDataSize == sizeof(info), "create payload");
    FrontendNetwork_HandleJoinerMessage(nullptr, (unsigned int *)create);
    Check(Find(guestId) && Find(guestId)->flags == info.flags && Find(guestId)->field_0x8 == 123,
          "host lobby must retain guest description");
    puts("net frontend: guest created"); fflush(stdout);

    info.flags = 0x89;
    info.field_0x8 = 789;
    Check(guest.SetPlayerData(guestId, &info, sizeof(info)) == NET_OK, "update guest data");
    FrontendNetwork_HandleJoinerMessage(nullptr, (unsigned int *)Receive(host, NET_SYS_SETPLAYERDATA));
    Check(Find(guestId) && Find(guestId)->flags == info.flags && Find(guestId)->field_0x8 == 789,
          "host lobby must update guest description");
    for (DWORD size : {DWORD(0), DWORD(4)}) {
        Check(guest.SetPlayerData(guestId, &info, size) == NET_OK, "publish incomplete player data");
        FrontendNetwork_HandleJoinerMessage(nullptr, (unsigned int *)Receive(host, NET_SYS_SETPLAYERDATA));
        Check(Find(guestId) && Find(guestId)->field_0x8 == 789, "incomplete data must not overwrite player");
    }

    desc.dwUser1 = 10;
    desc.dwUser3 = 7;
    Check(host.SetSessionDesc(&desc) == NET_OK, "update host options");
    FrontendNetwork_HandleJoinerMessage(nullptr, (unsigned int *)Receive(guest, NET_SYS_SETSESSIONDESC));
    Check(CGameInfo::GetConfiguredGameMode() == 10 && GameInfo_GetSessionField398C() == 7 &&
          g_networkSession.dwMaxPlayers == 8 &&
          memcmp(&g_networkSession.guidInstance, &desc.guidInstance, sizeof(NetGuid)) == 0,
          "guest must read host options at native alignment");

    Check(guest.Close() == NET_OK, "guest leaves");
    auto *destroy = (NetMsgDestroyPlayer *)Receive(host, NET_SYS_DESTROYPLAYER);
    FrontendNetwork_HandleJoinerMessage(nullptr, (unsigned int *)destroy);
    Check(!Find(guestId), "lobby removes guest");
    for (int handler = 0; handler < 2; handler++) {
        DWORD from = 0;

        g_netPlayers[0].id = int(guestId);
        g_netPlayers[0].flags = 0x80;
        if (handler == 0)
            OptionMenu_HandleItemNotification((unsigned int *)&from, (int *)destroy);
        else
            NetRace_HandleDeviceNotification(&from, (int *)destroy);
        Check(!Find(guestId), "in-race/results handler removes guest");
    }
    // A player can initially have no custom data. Its later description
    // arrives as SETPLAYERDATA, so CREATEPLAYER must not dereference null.
    Check(guest.Open(&desc, NET_OPEN_JOIN) == NET_OK, "rejoin host");
    Check(guest.CreatePlayer(&guestId, &name, nullptr, 0) == NET_OK, "create player without data");
    FrontendNetwork_HandleJoinerMessage(nullptr, (unsigned int *)Receive(host, NET_SYS_CREATEPLAYER));
    Check(!Find(guestId), "empty create must wait for player description");
    Check(guest.SetPlayerData(guestId, &info, sizeof(info)) == NET_OK, "publish initial player description");
    FrontendNetwork_HandleJoinerMessage(nullptr, (unsigned int *)Receive(host, NET_SYS_SETPLAYERDATA));
    Check(Find(guestId) && Find(guestId)->field_0x8 == 789, "late description adds player");
    Test_FreeHighAddress(buffer);
    puts("net frontend: UDP join, player update, host options and departure passed");
}
