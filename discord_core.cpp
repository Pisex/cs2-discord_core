#include <stdio.h>
#include "discord_core.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <locale>
#include <codecvt>

using json = nlohmann::json;

discord_core g_discord_core;
PLUGIN_EXPOSE(discord_core, g_discord_core);
IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars *gpGlobals = nullptr;

IUtilsApi* g_pUtils;

DiscordApi* g_pDiscordApi = nullptr;
IDiscordApi* g_pDiscordCore = nullptr;

CSteamGameServerAPIContext g_steamAPI;
ISteamHTTP *g_http = nullptr;

std::map<std::string, std::string> g_mapWebHooks;

SH_DECL_HOOK0_void(IServerGameDLL, GameServerSteamAPIActivated, SH_NOATTRIB, 0);

CGameEntitySystem* GameEntitySystem()
{
	return g_pUtils->GetCGameEntitySystem();
}

void StartupServer()
{
	g_pGameEntitySystem = GameEntitySystem();
	g_pEntitySystem = g_pUtils->GetCEntitySystem();
	gpGlobals = g_pUtils->GetCGlobalVars();
}

void LoadConfig()
{
	KeyValues* hKv = new KeyValues("Discord");
	const char *pszPath = "addons/configs/discord.ini";

	if (!hKv->LoadFromFile(g_pFullFileSystem, pszPath))
	{
		g_pUtils->ErrorLog("[%s] Failed to load %s", g_PLAPI->GetLogTag(), pszPath);
		return;
	}

	FOR_EACH_VALUE(hKv, pValue)
	{
		g_mapWebHooks[pValue->GetName()] = pValue->GetString(nullptr, nullptr);
	}
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

	g_pDiscordApi = new DiscordApi();
	g_pDiscordCore = g_pDiscordApi;

	SH_ADD_HOOK_MEMFUNC(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server, this, &discord_core::OnGameServerSteamAPIActivated, false);
	LoadConfig();
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
	if (!strcmp(iface, DISCORD_INTERFACE))
	{
		*ret = META_IFACE_OK;
		return g_pDiscordCore;
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
    return result;
}

void DiscordApi::SendWebHook(const char* szWebHookName, const char* szContent, std::vector<Embed*> hEmbeds)
{
    if (!szWebHookName || g_mapWebHooks.find(szWebHookName) == g_mapWebHooks.end())
    {
        g_pUtils->ErrorLog("[%s] WebHook %s not found or szWebHookName is null", g_PLAPI->GetLogTag(), szWebHookName ? szWebHookName : "null");
        return;
    }

    try {
        json j;
        j["content"] = szContent ? CleanInvalidUTF8(szContent) : "";
        json embeds;
        for (auto& embed : hEmbeds)
        {
            json jEmbed;
            if (embed->GetAuthorName() && embed->GetAuthorName()[0] != '\0')
            {
                json jAuthor;
                jAuthor["name"] = CleanInvalidUTF8(embed->GetAuthorName());
                if (embed->GetAuthorURL() && embed->GetAuthorURL()[0] != '\0')
                    jAuthor["url"] = CleanInvalidUTF8(embed->GetAuthorURL());
                if (embed->GetAuthorIcon() && embed->GetAuthorIcon()[0] != '\0')
                    jAuthor["icon_url"] = CleanInvalidUTF8(embed->GetAuthorIcon());
                jEmbed["author"] = jAuthor;
            }
            if (embed->GetTitle() && embed->GetTitle()[0] != '\0')
                jEmbed["title"] = CleanInvalidUTF8(embed->GetTitle());
            if (embed->GetDescription() && embed->GetDescription()[0] != '\0')
                jEmbed["description"] = CleanInvalidUTF8(embed->GetDescription());
            if (embed->GetURL() && embed->GetURL()[0] != '\0')
                jEmbed["url"] = CleanInvalidUTF8(embed->GetURL());
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
            if (embed->GetImage() && embed->GetImage()[0] != '\0')
            {
                json jImage;
                jImage["url"] = CleanInvalidUTF8(embed->GetImage());
                jEmbed["image"] = jImage;
            }
            if (embed->GetThumbnail() && embed->GetThumbnail()[0] != '\0')
            {
                json jThumbnail;
                jThumbnail["url"] = CleanInvalidUTF8(embed->GetThumbnail());
                jEmbed["thumbnail"] = jThumbnail;
            }
            if (embed->GetFooterText() && embed->GetFooterText()[0] != '\0')
            {
                json jFooter;
                jFooter["text"] = CleanInvalidUTF8(embed->GetFooterText());
                if (embed->GetFooterIcon() && embed->GetFooterIcon()[0] != '\0')
                    jFooter["icon_url"] = CleanInvalidUTF8(embed->GetFooterIcon());
                jEmbed["footer"] = jFooter;
            }
            embeds.push_back(jEmbed);
        }
        j["embeds"] = embeds;

        std::string sRequestBody = j.dump();
        std::thread([szWebHookName, sRequestBody](){
            try {
                auto hReq = g_http->CreateHTTPRequest(k_EHTTPMethodPOST, g_mapWebHooks[szWebHookName].c_str());
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
