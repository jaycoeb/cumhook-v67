#include "includes.h"

namespace {
    struct PendingRequest {
        std::string prompt;
        uint32_t    execute_time;
    };

    std::deque< PendingRequest > g_queue{};
    uint32_t g_last_request_tick{};

    constexpr uint32_t k_rate_limit_ms = 5000;
    constexpr size_t   k_max_prompt_len = 256;

 static std::string trim_left( std::string s ) {
        s.erase( s.begin( ), std::find_if( s.begin( ), s.end( ), []( unsigned char ch ) { return !std::isspace( ch ); } ) );
        return s;
    }
}


int get_player_index_by_name(const char* name) {
    if (!name)
        return -1;

    for (int i = 1; i <= g_csgo.m_globals->m_max_clients; ++i) {
        player_info_t info{};
        if (!g_csgo.m_engine->GetPlayerInfo(i, &info))
            continue;

        if (std::strcmp(info.m_name, name) == 0)
            return i;
    }

    return -1;
}


bool chat_assistant::test_connection() {
    std::string api_key = g_menu.main.misc.gemini_api_key.get_string();
    // trim whitespace/newlines from copy-pasted keys.
    api_key.erase(api_key.begin(), std::find_if(api_key.begin(), api_key.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    api_key.erase(std::find_if(api_key.rbegin(), api_key.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), api_key.end());
    if (api_key.empty())
        return false;

    nlohmann::json body{};

    body["model"] = "llama-3.1-8b-instant";

    body["messages"] = {
        {
            {"role", "user"},
            {"content", "ping"}
        }
    };

    std::string path = "/openai/v1/chat/completions";

    std::wstring headers =
        L"Authorization: Bearer " + util::MultiByteToWide(api_key) + L"\r\n" +
        L"Content-Type: application/json\r\n";

    auto res = http::post(
        L"api.groq.com",
        util::MultiByteToWide(path).c_str(),
        body.dump(),
        headers.c_str()
    );

    g_cl.print(tfm::format("[BOT] test status: http %u\n", res.status));

    if (!res.body.empty())
        g_cl.print(tfm::format("[BOT] response: %s\n", res.body.c_str()));

    return res.ok;
}

void chat_assistant::on_player_say(const char* name, const char* text) {
    if (!callbacks::IsAITrashTalkerOn())
        return;

    if (!text || !*text)
        return;

    if (!name)
        return;

    // don't respond to ourselves
    player_info_t info{};
    int local = g_csgo.m_engine->GetLocalPlayer();
    if (local <= 0)
        return;

    g_csgo.m_engine->GetPlayerInfo(local, &info);

    if (std::strcmp(name, info.m_name) == 0)
        return;

    std::string msg{ text };
    msg = trim_left(msg);

    if (msg.empty() || msg.size() < 3)
        return;

    // 🔽 enemy-only filter 🔽


    int idx = get_player_index_by_name(name);
    if (idx == -1)
        return;

    auto ent = g_csgo.m_entlist->GetClientEntity(idx);
    if (!ent)
        return;

    Player* player = reinterpret_cast<Player*>(ent);
    if (!player || !g_cl.m_local)
        return;

    if (player->m_iTeamNum() == g_cl.m_local->m_iTeamNum())
        return;

    // 🔼 end filter 🔼

    if (msg.size() > k_max_prompt_len)
        msg.resize(k_max_prompt_len);

    std::string custom = g_menu.main.misc.ai_trash_prompt.get_string();

    if (custom.find("{name}") != std::string::npos)
        custom.replace(custom.find("{name}"), 6, name);

    if (custom.find("{msg}") != std::string::npos)
        custom.replace(custom.find("{msg}"), 5, msg);

    std::string full = custom;

    uint32_t now = g_winapi.GetTickCount();

    // 1–2 second random delay
    uint32_t delay = 1000 + (rand() % 1000);

    g_queue.push_back(PendingRequest{
        full,
        now + delay
        });

    g_cl.print(tfm::format("[BOT] queued: %s\n", full.c_str()));
}

void chat_assistant::think() {
    if (!callbacks::IsAITrashTalkerOn())
        return;

    if (g_queue.empty())
        return;

    uint32_t now = g_winapi.GetTickCount();

    auto& req = g_queue.front();

    // not ready yet → wait
    if (now < req.execute_time)
        return;

    // rate limit (optional safety)
    if (now - g_last_request_tick < k_rate_limit_ms)
        return;

    g_last_request_tick = now;
    g_queue.pop_front();

    std::string api_key = g_menu.main.misc.gemini_api_key.get_string();

    api_key.erase(api_key.begin(), std::find_if(api_key.begin(), api_key.end(),
        [](unsigned char ch) { return !std::isspace(ch); }));

    api_key.erase(std::find_if(api_key.rbegin(), api_key.rend(),
        [](unsigned char ch) { return !std::isspace(ch); }).base(), api_key.end());

    if (api_key.empty()) {
        g_cl.print("[BOT] Groq API key missing\n");
        return;
    }

    nlohmann::json body{};
    body["model"] = "llama-3.1-8b-instant";
    body["messages"] = {
        {
            {"role", "user"},
            {"content", req.prompt}
        }
    };

    std::wstring headers =
        L"Authorization: Bearer " + util::MultiByteToWide(api_key) + L"\r\n" +
        L"Content-Type: application/json\r\n";

    auto res = http::post(
        L"api.groq.com",
        util::MultiByteToWide("/openai/v1/chat/completions").c_str(),
        body.dump(),
        headers.c_str()
    );

    if (!res.ok) {
        g_cl.print(tfm::format("[BOT] request failed: %s\n", res.error.c_str()));
        return;
    }

    try {
        auto json = nlohmann::json::parse(res.body);
        std::string reply =
            json["choices"][0]["message"]["content"];

        std::string safe = reply;
        safe.erase(std::remove(safe.begin(), safe.end(), '\n'), safe.end());
        safe.erase(std::remove(safe.begin(), safe.end(), '"'), safe.end());

        if (safe.size() > 120)
            safe.resize(120);

        g_csgo.m_engine->ExecuteClientCmd(("say " + safe).c_str());
    }
    catch (...) {
        g_cl.print("[BOT] parse error\n");
    }
}
