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

class DiscordWebhookApi : public IDiscordWebhookApi {
public:
    void SendWebHook(const char* szWebHook, const char* szContent, std::vector<Embed*> hEmbeds);
};

class DiscordBotApi : public IDiscordBotApi {
public:
    void SendMessage(DiscordBot* pBot, const char* szChannelID, const char* szContent, std::vector<Embed*> hEmbeds, DiscordCallback callback);
    void DeleteMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, DiscordCallback callback);
    void EditMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, const char* szContent, std::vector<Embed*> hEmbeds, DiscordCallback callback);
    void PinMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, DiscordCallback callback);
    void UnpinMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, DiscordCallback callback);
    void GetMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, DiscordCallback callback);
    void GetMessages(DiscordBot* pBot, const char* szChannelID, int iLimit, const char* szBefore, const char* szAfter, DiscordCallback callback);
    void GetPinnedMessages(DiscordBot* pBot, const char* szChannelID, DiscordCallback callback);
    
    void AddReaction(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, const char* emoji, DiscordCallback callback);
    void RemoveReaction(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, const char* emoji, DiscordCallback callback);

    void AddRole(DiscordBot* pBot, const char* szGuildID, const char* szUserID, const char* szRoleID, DiscordCallback callback);
    void RemoveRole(DiscordBot* pBot, const char* szGuildID, const char* szUserID, const char* szRoleID, DiscordCallback callback);

    void GetGuildMember(DiscordBot* pBot, const char* szGuildID, const char* szUserID, DiscordCallback callback);
    void GetGuildMembers(DiscordBot* pBot, const char* szGuildID, int iLimit, const char* szAfter, DiscordCallback callback);
    void GetGuildRoles(DiscordBot* pBot, const char* szGuildID, DiscordCallback callback);
    void GetGuildChannels(DiscordBot* pBot, const char* szGuildID, DiscordCallback callback);
    void GetGuildEmojis(DiscordBot* pBot, const char* szGuildID, DiscordCallback callback);
    void GetGuildInvites(DiscordBot* pBot, const char* szGuildID, DiscordCallback callback);
};

#endif //_INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_