#pragma once

#define DISCORD_INTERFACE "IDiscordApi"

class Embed
{
public:
    void SetAuthor(const char* szName, const char* szURL = nullptr, const char* szIcon = nullptr) {
        m_szAuthorName = szName;
        m_szAuthorURL = szURL;
        m_szAuthorIcon = szIcon;
    }
    void SetTitle(const char* szTitle) {
        m_szTitle = szTitle;
    }
    void SetDescription(const char* szDescription) {
        m_szDescription = szDescription;
    }
    void SetURL(const char* szURL) {
        m_szURL = szURL;
    }
    void SetColor(int iColor) {
        m_iColor = iColor;
    }

    void AddField(std::string szTitle, std::string szValue, bool bInline = false) {
        m_hFields.push_back(std::make_tuple(szTitle, szValue, bInline));
    }

    void SetImage(const char* szURL) {
        m_szImage = szURL;
    }
    void SetThumbnail(const char* szURL) {
        m_szThumbnail = szURL;
    }

    void SetFooter(const char* szText, const char* szIcon = nullptr) {
        m_szFooterText = szText;
        m_szFooterIcon = szIcon;
    }

    const char* GetAuthorName() const {
        return m_szAuthorName;
    }
    const char* GetAuthorURL() const {
        return m_szAuthorURL;
    }
    const char* GetAuthorIcon() const {
        return m_szAuthorIcon;
    }

    const char* GetTitle() const {
        return m_szTitle;
    }
    const char* GetDescription() const {
        return m_szDescription;
    }
    const char* GetURL() const {
        return m_szURL;
    }
    int GetColor() const {
        return m_iColor;
    }

    const std::vector<std::tuple<std::string, std::string, bool>>& GetFields() const {
        return m_hFields;
    }

    const char* GetImage() const {
        return m_szImage;
    }
    const char* GetThumbnail() const {
        return m_szThumbnail;
    }

    const char* GetFooterText() const {
        return m_szFooterText;
    }
    const char* GetFooterIcon() const {
        return m_szFooterIcon;
    }
private:
    const char* m_szAuthorName = nullptr;
    const char* m_szAuthorURL = nullptr;
    const char* m_szAuthorIcon = nullptr;

    const char* m_szTitle = nullptr;
    const char* m_szDescription = nullptr;
    const char* m_szURL = nullptr;
    int m_iColor = 0xFFFFFF;

    std::vector<std::tuple<std::string, std::string, bool>> m_hFields;

    const char* m_szImage = nullptr;
    const char* m_szThumbnail = nullptr;

    const char* m_szFooterText = nullptr;
    const char* m_szFooterIcon = nullptr;
};

class IDiscordApi
{
public:
    virtual void SendWebHook(const char* szWebHookName, const char* szContent, std::vector<Embed*> hEmbeds) = 0;
};