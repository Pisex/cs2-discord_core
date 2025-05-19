#include <stdio.h>
#include "discord_core.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <locale>
#include <codecvt>
#include <condition_variable>

using json = nlohmann::json;

discord_core g_discord_core;
PLUGIN_EXPOSE(discord_core, g_discord_core);
IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars *gpGlobals = nullptr;

IUtilsApi* g_pUtils;

DiscordWebhookApi* g_pDiscordWebhookApi = nullptr;
IDiscordWebhookApi* g_pDiscordCore = nullptr;

DiscordBotApi* g_pDiscordBotApi = nullptr;
IDiscordBotApi* g_pDiscordBotCore = nullptr;

std::map<std::string, json> g_mQueue;

CSteamGameServerAPIContext g_steamAPI;
ISteamHTTP *g_http = nullptr;

SH_DECL_HOOK0_void(IServerGameDLL, GameServerSteamAPIActivated, SH_NOATTRIB, 0);

CGameEntitySystem* GameEntitySystem()
{
	return g_pUtils->GetCGameEntitySystem();
}

class CCallResultHandler {
public:
    CCallResultHandler(DiscordCallback cb)
        : callback(cb) {
            m_ready = false;
        }

    void Set(SteamAPICall_t call) {
        m_callResult.SetGameserverFlag();
        m_callResult.Set(call, this, &CCallResultHandler::OnResponse);
        
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_ready; });
        
        if (callback) {
            callback(m_statusCode, m_body.c_str());
        }
    }

private:
    void OnResponse(HTTPRequestCompleted_t* pResult, bool bFailed) {
        m_statusCode = pResult->m_eStatusCode;
        if(bFailed) {
            m_body = "Request failed";
        }
        else {
            uint32 size;
            g_http->GetHTTPResponseBodySize(pResult->m_hRequest, &size);
            uint8* response = new uint8[size + 1];
            g_http->GetHTTPResponseBodyData(pResult->m_hRequest, response, size);
            response[size] = 0;
            if (size <= 0) m_body = "Empty response";
            else m_body = std::string((char*)response);
            delete[] response;
        }

        if (g_http)
            g_http->ReleaseHTTPRequest(pResult->m_hRequest);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ready = true;
        }
        m_cv.notify_one();
    }

    int m_statusCode = 0;
    std::string m_body;

    bool m_ready = false;
    std::mutex m_mutex;
    std::condition_variable m_cv;

    DiscordCallback callback;
    CCallResult<CCallResultHandler, HTTPRequestCompleted_t> m_callResult;
};

void StartupServer()
{
	g_pGameEntitySystem = GameEntitySystem();
	g_pEntitySystem = g_pUtils->GetCEntitySystem();
	gpGlobals = g_pUtils->GetCGlobalVars();
}

bool discord_core::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);

	g_SMAPI->AddListener( this, this );

	g_pDiscordWebhookApi = new DiscordWebhookApi();
	g_pDiscordCore = g_pDiscordWebhookApi;

    g_pDiscordBotApi = new DiscordBotApi();
    g_pDiscordBotCore = g_pDiscordBotApi;

	SH_ADD_HOOK_MEMFUNC(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server, this, &discord_core::OnGameServerSteamAPIActivated, false);
	return true;
}

bool discord_core::Unload(char *error, size_t maxlen)
{
	SH_REMOVE_HOOK_MEMFUNC(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server, this, &discord_core::OnGameServerSteamAPIActivated, false);
	ConVar_Unregister();
	
	return true;
}

void* discord_core::OnMetamodQuery(const char* iface, int* ret)
{
	if (!strcmp(iface, DISCORD_WEBHOOK_INTERFACE))
	{
		*ret = META_IFACE_OK;
		return g_pDiscordCore;
	}
    if (!strcmp(iface, DISCORD_BOT_INTERFACE))
    {
        *ret = META_IFACE_OK;
        return g_pDiscordBotCore;
    }

	*ret = META_IFACE_FAILED;
	return nullptr;
}

void discord_core::OnGameServerSteamAPIActivated()
{
	g_steamAPI.Init();
	g_http = g_steamAPI.SteamHTTP();
	
	RETURN_META(MRES_IGNORED);
}

void discord_core::AllPluginsLoaded()
{
	char error[64];
	int ret;
	g_pUtils = (IUtilsApi *)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_SMAPI->Format(error, sizeof(error), "Missing Utils system plugin");
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	g_pUtils->StartupServer(g_PLID, StartupServer);
    g_pUtils->CreateTimer(1.0f, [](){
        if(g_mQueue.size() > 0)
        {
            //Получение самого первого элемента из очереди и удаление его из очереди
            auto it = g_mQueue.begin();
            std::string szWebHook = it->first;
            json j = it->second;
            g_mQueue.erase(it);
            try {
                std::string sRequestBody = j.dump(4);
                std::thread([szWebHook, sRequestBody](){
                    try {
                        auto hReq = g_http->CreateHTTPRequest(k_EHTTPMethodPOST, szWebHook.c_str());
                        g_http->SetHTTPRequestHeaderValue(hReq, "Content-Type", "application/json");
                        g_http->SetHTTPRequestRawPostBody(hReq, "application/json", (uint8*)sRequestBody.c_str(), sRequestBody.size());

                        SteamAPICall_t hCall;
                        g_http->SendHTTPRequest(hReq, &hCall);
                    } catch (const std::exception& e) {
                        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", g_PLAPI->GetLogTag(), e.what());
                    } catch (...) {
                        ConColorMsg(Color(255, 0, 0, 255), "[%s] Unknown error\n", g_PLAPI->GetLogTag());
                    }
                }).detach();
            } catch (const std::exception& e) {
                ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", g_PLAPI->GetLogTag(), e.what());
            } catch (...) {
                ConColorMsg(Color(255, 0, 0, 255), "[%s] Unknown error\n", g_PLAPI->GetLogTag());
            }
        }
        return 1.0f;
    });
}

bool IsValidUTF8(const std::string& str)
{
    try
    {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        std::wstring wstr = converter.from_bytes(str);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

std::string EscapeString(const std::string& str)
{
    // Escape backslashes and double quotes and "
    std::string escaped = str;
    size_t pos = 0;
    while ((pos = escaped.find("\\", pos)) != std::string::npos) {
        escaped.insert(pos, "\\");
        pos += 2;
    }
    pos = 0;
    while ((pos = escaped.find("\"", pos)) != std::string::npos) {
        escaped.insert(pos, "\\");
        pos += 2;
    }
    return escaped;
}

std::string CleanInvalidUTF8(const std::string& str)
{
    std::string result;
    for (size_t i = 0; i < str.size(); ++i)
    {
        unsigned char c = str[i];
        if (c < 0x80)
        {
            result += c;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            if (i + 1 < str.size() && (str[i + 1] & 0xC0) == 0x80)
            {
                result += c;
                result += str[++i];
            }
        }
        else if ((c & 0xF0) == 0xE0)
        {
            if (i + 2 < str.size() && (str[i + 1] & 0xC0) == 0x80 && (str[i + 2] & 0xC0) == 0x80)
            {
                result += c;
                result += str[++i];
                result += str[++i];
            }
        }
        else if ((c & 0xF8) == 0xF0)
        {
            if (i + 3 < str.size() && (str[i + 1] & 0xC0) == 0x80 && (str[i + 2] & 0xC0) == 0x80 && (str[i + 3] & 0xC0) == 0x80)
            {
                result += c;
                result += str[++i];
                result += str[++i];
                result += str[++i];
            }
        }
    }
    return EscapeString(result);
}

void DiscordWebhookApi::SendWebHook(const char* szWebHook, const char* szContent, std::vector<Embed*> hEmbeds)
{
    try {
        json j;
        j["content"] = szContent ? CleanInvalidUTF8(szContent) : "";
        json embeds;
        for (auto& embed : hEmbeds)
        {
            json jEmbed;
            const char* szAuthorName = embed->GetAuthorName();
            if (szAuthorName && szAuthorName[0] != '\0')
            {
                json jAuthor;
                jAuthor["name"] = CleanInvalidUTF8(szAuthorName);
                const char* szAuthorURL = embed->GetAuthorURL();
                const char* szAuthorIcon = embed->GetAuthorIcon();
                if (szAuthorURL && szAuthorURL[0] != '\0')
                    jAuthor["url"] = CleanInvalidUTF8(szAuthorURL);
                if (szAuthorIcon && szAuthorIcon[0] != '\0')
                    jAuthor["icon_url"] = CleanInvalidUTF8(szAuthorIcon);
                jEmbed["author"] = jAuthor;
            }
            const char* szTitle = embed->GetTitle();
            if (szTitle && szTitle[0] != '\0')
                jEmbed["title"] = CleanInvalidUTF8(szTitle);
            const char* szDescription = embed->GetDescription();
            if (szDescription && szDescription[0] != '\0')
                jEmbed["description"] = CleanInvalidUTF8(szDescription);
            const char* szURL = embed->GetURL();
            if (szURL && szURL[0] != '\0')
                jEmbed["url"] = CleanInvalidUTF8(szURL);
            if (embed->GetColor())
                jEmbed["color"] = embed->GetColor();

            json fields;
            for (auto& field : embed->GetFields())
            {
                json jField;
                std::string name = std::get<0>(field);
                std::string value = std::get<1>(field);
                bool inline_ = std::get<2>(field);

                jField["name"] = CleanInvalidUTF8(name);
                jField["value"] = CleanInvalidUTF8(value);
                jField["inline"] = inline_;
                fields.push_back(jField);
            }
            jEmbed["fields"] = fields;

            const char* szImage = embed->GetImage();
            if (szImage && szImage[0] != '\0')
                jEmbed["image"] = { {"url", CleanInvalidUTF8(szImage)} };

            const char* szThumbnail = embed->GetThumbnail();
            if (szThumbnail && szThumbnail[0] != '\0')
                jEmbed["thumbnail"] = { {"url", CleanInvalidUTF8(szThumbnail)} };

            const char* szFooterText = embed->GetFooterText();
            if (szFooterText && szFooterText[0] != '\0')
            {
                json jFooter;
                jFooter["text"] = CleanInvalidUTF8(szFooterText);
                const char* szFooterIcon = embed->GetFooterIcon();
                if (szFooterIcon && szFooterIcon[0] != '\0')
                    jFooter["icon_url"] = CleanInvalidUTF8(szFooterIcon);
                jEmbed["footer"] = jFooter;
            }
            embeds.push_back(jEmbed);
        }
        j["embeds"] = embeds;

        g_mQueue[szWebHook] = j;
    } catch (const std::exception& e) {
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", g_PLAPI->GetLogTag(), e.what());
    } catch (...) {
        ConColorMsg(Color(255, 0, 0, 255), "[%s] Unknown error\n", g_PLAPI->GetLogTag());
    }
}

void SendRequest(std::string sURL, EHTTPMethod eMethod, std::string sContent, std::string sAuth, DiscordCallback callback)
{
    std::thread([sURL, eMethod, sContent, sAuth, callback]() {
        try {
            auto hReq = g_http->CreateHTTPRequest(eMethod, sURL.c_str());
            g_http->SetHTTPRequestHeaderValue(hReq, "Content-Type", "application/json");
            g_http->SetHTTPRequestHeaderValue(hReq, "Authorization", sAuth.c_str());

            if (!sContent.empty()) {
                const char* szContent = sContent.c_str();
                g_http->SetHTTPRequestRawPostBody(hReq, "application/json", (uint8*)szContent, strlen(szContent));
            }

            SteamAPICall_t hCall;
            g_http->SendHTTPRequest(hReq, &hCall);

            if (callback)
            {
                CCallResultHandler* pHandler = new CCallResultHandler(callback);
                pHandler->Set(hCall);
            }
        } catch (const std::exception& e) {
            ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", g_PLAPI->GetLogTag(), e.what());
        } catch (...) {
            ConColorMsg(Color(255, 0, 0, 255), "[%s] Unknown error\n", g_PLAPI->GetLogTag());
        }
    }).detach();
}

void DiscordBotApi::SendMessage(DiscordBot* pBot, const char* szChannelID, const char* szContent, std::vector<Embed*> hEmbeds, DiscordCallback callback)
{
    if (!pBot || !szChannelID)
    {
        g_pUtils->ErrorLog("[%s] Bot or ChannelID is null", g_PLAPI->GetLogTag());
        return;
    }

    try {
        json j;
        j["content"] = szContent ? CleanInvalidUTF8(szContent) : "";
        json embeds;

        for (auto& embed : hEmbeds)
        {
            json jEmbed;
            const char* szAuthorName = embed->GetAuthorName();
            if (szAuthorName && szAuthorName[0] != '\0')
            {
                json jAuthor;
                jAuthor["name"] = CleanInvalidUTF8(szAuthorName);
                const char* szAuthorURL = embed->GetAuthorURL();
                const char* szAuthorIcon = embed->GetAuthorIcon();
                if (szAuthorURL && szAuthorURL[0] != '\0')
                    jAuthor["url"] = CleanInvalidUTF8(szAuthorURL);
                if (szAuthorIcon && szAuthorIcon[0] != '\0')
                    jAuthor["icon_url"] = CleanInvalidUTF8(szAuthorIcon);
                jEmbed["author"] = jAuthor;
            }
            const char* szTitle = embed->GetTitle();
            if (szTitle && szTitle[0] != '\0')
                jEmbed["title"] = CleanInvalidUTF8(szTitle);
            const char* szDescription = embed->GetDescription();
            if (szDescription && szDescription[0] != '\0')
                jEmbed["description"] = CleanInvalidUTF8(szDescription);
            const char* szURL = embed->GetURL();
            if (szURL && szURL[0] != '\0')
                jEmbed["url"] = CleanInvalidUTF8(szURL);
            if (embed->GetColor())
                jEmbed["color"] = embed->GetColor();

            json fields;
            for (auto& field : embed->GetFields())
            {
                json jField;
                std::string name = std::get<0>(field);
                std::string value = std::get<1>(field);
                bool inline_ = std::get<2>(field);

                jField["name"] = CleanInvalidUTF8(name);
                jField["value"] = CleanInvalidUTF8(value);
                jField["inline"] = inline_;
                fields.push_back(jField);
            }
            jEmbed["fields"] = fields;

            const char* szImage = embed->GetImage();
            if (szImage && szImage[0] != '\0')
                jEmbed["image"] = { {"url", CleanInvalidUTF8(szImage)} };

            const char* szThumbnail = embed->GetThumbnail();
            if (szThumbnail && szThumbnail[0] != '\0')
                jEmbed["thumbnail"] = { {"url", CleanInvalidUTF8(szThumbnail)} };

            const char* szFooterText = embed->GetFooterText();
            if (szFooterText && szFooterText[0] != '\0')
            {
                json jFooter;
                jFooter["text"] = CleanInvalidUTF8(szFooterText);
                const char* szFooterIcon = embed->GetFooterIcon();
                if (szFooterIcon && szFooterIcon[0] != '\0')
                    jFooter["icon_url"] = CleanInvalidUTF8(szFooterIcon);
                jEmbed["footer"] = jFooter;
            }

            embeds.push_back(jEmbed);
        }

        j["embeds"] = embeds;
        std::string sRequestBody = j.dump(4);

        char szURL[256];
        g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/channels/%s/messages", szChannelID);

        std::string sAuth = "Bot " + pBot->GetToken();

        SendRequest(szURL, k_EHTTPMethodPOST, sRequestBody, sAuth, callback);
    }
    catch (const std::exception& e) {
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", g_PLAPI->GetLogTag(), e.what());
    }
    catch (...) {
        ConColorMsg(Color(255, 0, 0, 255), "[%s] Unknown error\n", g_PLAPI->GetLogTag());
    }
}

void DiscordBotApi::DeleteMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, DiscordCallback callback)
{
    if (!pBot || !szChannelID || !szMessageID)
    {
        g_pUtils->ErrorLog("[%s] Bot, ChannelID or MessageID is null", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/channels/%s/messages/%s", szChannelID, szMessageID);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodDELETE, nullptr, sAuth, callback);
}

void DiscordBotApi::EditMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, const char* szContent, std::vector<Embed*> hEmbeds, DiscordCallback callback)
{
    if (!pBot || !szChannelID || !szMessageID)
    {
        g_pUtils->ErrorLog("[%s] Bot, ChannelID or MessageID is null", g_PLAPI->GetLogTag());
        return;
    }

    try {
        json j;
        j["content"] = szContent ? CleanInvalidUTF8(szContent) : "";
        json embeds;

        for (auto& embed : hEmbeds)
        {
            json jEmbed;
            const char* szAuthorName = embed->GetAuthorName();
            if (szAuthorName && szAuthorName[0] != '\0')
            {
                json jAuthor;
                jAuthor["name"] = CleanInvalidUTF8(szAuthorName);
                const char* szAuthorURL = embed->GetAuthorURL();
                const char* szAuthorIcon = embed->GetAuthorIcon();
                if (szAuthorURL && szAuthorURL[0] != '\0')
                    jAuthor["url"] = CleanInvalidUTF8(szAuthorURL);
                if (szAuthorIcon && szAuthorIcon[0] != '\0')
                    jAuthor["icon_url"] = CleanInvalidUTF8(szAuthorIcon);
                jEmbed["author"] = jAuthor;
            }
            const char* szTitle = embed->GetTitle();
            if (szTitle && szTitle[0] != '\0')
                jEmbed["title"] = CleanInvalidUTF8(szTitle);
            const char* szDescription = embed->GetDescription();
            if (szDescription && szDescription[0] != '\0')
                jEmbed["description"] = CleanInvalidUTF8(szDescription);
            const char* szURL = embed->GetURL();
            if (szURL && szURL[0] != '\0')
                jEmbed["url"] = CleanInvalidUTF8(szURL);
            if (embed->GetColor())
                jEmbed["color"] = embed->GetColor();

            json fields;
            for (auto& field : embed->GetFields())
            {
                json jField;
                std::string name = std::get<0>(field);
                std::string value = std::get<1>(field);
                bool inline_ = std::get<2>(field);

                jField["name"] = CleanInvalidUTF8(name);
                jField["value"] = CleanInvalidUTF8(value);
                jField["inline"] = inline_;
                fields.push_back(jField);
            }
            jEmbed["fields"] = fields;

            const char* szImage = embed->GetImage();
            if (szImage && szImage[0] != '\0')
                jEmbed["image"] = { {"url", CleanInvalidUTF8(szImage)} };

            const char* szThumbnail = embed->GetThumbnail();
            if (szThumbnail && szThumbnail[0] != '\0')
                jEmbed["thumbnail"] = { {"url", CleanInvalidUTF8(szThumbnail)} };

            const char* szFooterText = embed->GetFooterText();
            if (szFooterText && szFooterText[0] != '\0')
            {
                json jFooter;
                jFooter["text"] = CleanInvalidUTF8(szFooterText);
                const char* szFooterIcon = embed->GetFooterIcon();
                if (szFooterIcon && szFooterIcon[0] != '\0')
                    jFooter["icon_url"] = CleanInvalidUTF8(szFooterIcon);
                jEmbed["footer"] = jFooter;
            }
            embeds.push_back(jEmbed);
        }
        j["embeds"] = embeds;
        std::string sRequestBody = j.dump(4);

        char szURL[256];
        g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/channels/%s/messages/%s", szChannelID, szMessageID);
        
        std::string sAuth = "Bot " + pBot->GetToken();
        SendRequest(szURL, k_EHTTPMethodPATCH, sRequestBody, sAuth, callback);
    } catch (const std::exception& e) {
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", g_PLAPI->GetLogTag(), e.what());
    } catch (...) {
        ConColorMsg(Color(255, 0, 0, 255), "[%s] Unknown error\n", g_PLAPI->GetLogTag());
    }
}

void DiscordBotApi::PinMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, DiscordCallback callback)
{
    if (!pBot || !szChannelID || !szMessageID)
    {
        g_pUtils->ErrorLog("[%s] Bot, ChannelID or MessageID is null", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/channels/%s/pins/%s", szChannelID, szMessageID);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodPUT, nullptr, sAuth, callback);
}

void DiscordBotApi::UnpinMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, DiscordCallback callback)
{
    if (!pBot || !szChannelID || !szMessageID)
    {
        g_pUtils->ErrorLog("[%s] Bot, ChannelID or MessageID is null", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/channels/%s/pins/%s", szChannelID, szMessageID);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodDELETE, nullptr, sAuth, callback);
}

void DiscordBotApi::AddReaction(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, const char* emoji, DiscordCallback callback)
{
    if (!pBot || !szChannelID || !szMessageID || !emoji)
    {
        g_pUtils->ErrorLog("[%s] Bot, ChannelID, MessageID or Emoji is null", g_PLAPI->GetLogTag());
        return;
    }
    
    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/channels/%s/messages/%s/reactions/%s/@me", szChannelID, szMessageID, emoji);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodPUT, nullptr, sAuth, callback);
}

void DiscordBotApi::RemoveReaction(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, const char* emoji, DiscordCallback callback)
{
    if (!pBot || !szChannelID || !szMessageID || !emoji)
    {
        g_pUtils->ErrorLog("[%s] Bot, ChannelID, MessageID or Emoji is null", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/channels/%s/messages/%s/reactions/%s/@me", szChannelID, szMessageID, emoji);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodDELETE, nullptr, sAuth, callback);
}

void DiscordBotApi::AddRole(DiscordBot* pBot, const char* szGuildID, const char* szUserID, const char* szRoleID, DiscordCallback callback)
{
    if (!pBot || !szGuildID || !szUserID || !szRoleID)
    {
        g_pUtils->ErrorLog("[%s] Bot, GuildID, UserID or RoleID is null", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/guilds/%s/members/%s/roles/%s", szGuildID, szUserID, szRoleID);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodPUT, nullptr, sAuth, callback);
}

void DiscordBotApi::RemoveRole(DiscordBot* pBot, const char* szGuildID, const char* szUserID, const char* szRoleID, DiscordCallback callback)
{
    if (!pBot || !szGuildID || !szUserID || !szRoleID)
    {
        g_pUtils->ErrorLog("[%s] Bot, GuildID, UserID or RoleID is null", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/guilds/%s/members/%s/roles/%s", szGuildID, szUserID, szRoleID);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodDELETE, nullptr, sAuth, callback);
}

void DiscordBotApi::GetMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, DiscordCallback callback)
{
    if (!pBot || !szChannelID || !szMessageID)
    {
        g_pUtils->ErrorLog("[%s] Bot, ChannelID or MessageID is null", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/channels/%s/messages/%s", szChannelID, szMessageID);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodGET, nullptr, sAuth, callback);
}

void DiscordBotApi::GetMessages(DiscordBot* pBot, const char* szChannelID, int iLimit, const char* szBefore, const char* szAfter, DiscordCallback callback)
{
    if (!pBot || !szChannelID)
    {
        g_pUtils->ErrorLog("[%s] Bot or ChannelID is null", g_PLAPI->GetLogTag());
        return;
    }

    if (iLimit < 1 || iLimit > 100)
    {
        g_pUtils->ErrorLog("[%s] Limit must be between 1 and 100", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/channels/%s/messages?limit=%d%s%s%s%s", szChannelID, iLimit, szBefore ? "&before=" : "", szBefore ? szBefore : "", szAfter ? "&after=" : "", szAfter ? szAfter : "");

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodGET, nullptr, sAuth, callback);
}

void DiscordBotApi::GetPinnedMessages(DiscordBot* pBot, const char* szChannelID, DiscordCallback callback)
{
    if (!pBot || !szChannelID)
    {
        g_pUtils->ErrorLog("[%s] Bot or ChannelID is null", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/channels/%s/pins", szChannelID);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodGET, nullptr, sAuth, callback);
}

void DiscordBotApi::GetGuildMember(DiscordBot* pBot, const char* szGuildID, const char* szUserID, DiscordCallback callback)
{
    if (!pBot || !szGuildID || !szUserID)
    {
        g_pUtils->ErrorLog("[%s] Bot, GuildID or UserID is null", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/guilds/%s/members/%s", szGuildID, szUserID);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodGET, nullptr, sAuth, callback);
}

void DiscordBotApi::GetGuildMembers(DiscordBot* pBot, const char* szGuildID, int iLimit, const char* szAfter, DiscordCallback callback)
{
    if (!pBot || !szGuildID)
    {
        g_pUtils->ErrorLog("[%s] Bot or GuildID is null", g_PLAPI->GetLogTag());
        return;
    }

    if (iLimit < 1 || iLimit > 1000)
    {
        g_pUtils->ErrorLog("[%s] Limit must be between 1 and 1000", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/guilds/%s/members?limit=%d%s%s", szGuildID, iLimit, szAfter ? "&after=" : "", szAfter ? szAfter : "");

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodGET, nullptr, sAuth, callback);
}

void DiscordBotApi::GetGuildRoles(DiscordBot* pBot, const char* szGuildID, DiscordCallback callback)
{
    if (!pBot || !szGuildID)
    {
        g_pUtils->ErrorLog("[%s] Bot or GuildID is null", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/guilds/%s/roles", szGuildID);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodGET, nullptr, sAuth, callback);
}

void DiscordBotApi::GetGuildChannels(DiscordBot* pBot, const char* szGuildID, DiscordCallback callback)
{
    if (!pBot || !szGuildID)
    {
        g_pUtils->ErrorLog("[%s] Bot or GuildID is null", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/guilds/%s/channels", szGuildID);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodGET, nullptr, sAuth, callback);
}

void DiscordBotApi::GetGuildEmojis(DiscordBot* pBot, const char* szGuildID, DiscordCallback callback)
{
    if (!pBot || !szGuildID)
    {
        g_pUtils->ErrorLog("[%s] Bot or GuildID is null", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/guilds/%s/emojis", szGuildID);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodGET, nullptr, sAuth, callback);
}

void DiscordBotApi::GetGuildInvites(DiscordBot* pBot, const char* szGuildID, DiscordCallback callback)
{
    if (!pBot || !szGuildID)
    {
        g_pUtils->ErrorLog("[%s] Bot or GuildID is null", g_PLAPI->GetLogTag());
        return;
    }

    char szURL[256];
    g_SMAPI->Format(szURL, sizeof(szURL), "https://discord.com/api/guilds/%s/invites", szGuildID);

    std::string sAuth = "Bot " + pBot->GetToken();

    SendRequest(szURL, k_EHTTPMethodGET, nullptr, sAuth, callback);
}

///////////////////////////////////////
const char* discord_core::GetLicense()
{
	return "GPL";
}

const char* discord_core::GetVersion()
{
	return "1.0";
}

const char* discord_core::GetDate()
{
	return __DATE__;
}

const char *discord_core::GetLogTag()
{
	return "discord_core";
}

const char* discord_core::GetAuthor()
{
	return "Pisex";
}

const char* discord_core::GetDescription()
{
	return "discord_core";
}

const char* discord_core::GetName()
{
	return "[Discord] Core";
}

const char* discord_core::GetURL()
{
	return "https://discord.gg/g798xERK5Y";
}
