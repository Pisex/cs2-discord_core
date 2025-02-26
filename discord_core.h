#ifndef _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_
#define _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_

#include <ISmmPlugin.h>
#include <sh_vector.h>
#include "utlvector.h"
#include "ehandle.h"
#include <iserver.h>
#include <steam/steam_gameserver.h>
#include <entity2/entitysystem.h>
#include "igameevents.h"
#include "vector.h"
#include <deque>
#include <functional>
#include <utlstring.h>
#include <KeyValues.h>
#include "CGameRules.h"
#include "CCSPlayerController.h"
#include "include/menus.h"
#include "include/discord.h"

class discord_core final : public ISmmPlugin, public IMetamodListener
{
public:
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
    bool Unload(char* error, size_t maxlen);
    void AllPluginsLoaded();
    void* OnMetamodQuery(const char* iface, int* ret);
private:
    const char* GetAuthor();
    const char* GetName();
    const char* GetDescription();
    const char* GetURL();
    const char* GetLicense();
    const char* GetVersion();
    const char* GetDate();
    const char* GetLogTag();
private:
    void OnGameServerSteamAPIActivated();
    
    CCallResult< discord_core, HTTPRequestCompleted_t > m_httpRequestCallback;
};

class DiscordApi : public IDiscordApi {
public:
    void SendWebHook(const char* szWebHookName, const char* szContent, std::vector<Embed*> hEmbeds);
};

#endif //_INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_