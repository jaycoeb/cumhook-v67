#include "includes.h"
#include <deque>
#include <mutex>
#include <atomic>
#include <string>
#include <thread>
#include <unordered_set>

struct PendingRequest {
    std::string prompt;
    std::string target_name;
    uint32_t    execute_time;
};

namespace {
    constexpr uint32_t k_rate_limit_ms = 5000;
    constexpr size_t   k_max_prompt_len = 256;

    struct PlayerMemory {
        int         insults = 0;
        int         deaths = 0;
        int         kills = 0;
        std::string tag;
        uint32_t    last_seen = 0;
    };

    std::deque<PendingRequest>            g_queue{};
    uint32_t                              g_last_request_tick{};
    std::mutex                            g_queue_mutex;
    std::atomic<bool>                     g_request_in_flight{ false };
    std::unordered_map<int, PlayerMemory> g_memory;

    bool pop_request(PendingRequest& out) {
        std::lock_guard<std::mutex> lock(g_queue_mutex);

        if (g_queue.empty())
            return false;

        uint32_t now = g_winapi.GetTickCount();
        auto& req = g_queue.front();

        if (now < req.execute_time)
            return false;

        if (now - g_last_request_tick < k_rate_limit_ms)
            return false;

        g_last_request_tick = now;
        out = req;
        g_queue.pop_front();
        return true;
    }

    struct RequestGuard {
        std::atomic<bool>& flag;
        RequestGuard(std::atomic<bool>& f) : flag(f) { flag = true; }
        ~RequestGuard() { flag = false; }
    };

    std::string trim_left(std::string s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        return s;
    }
}

namespace {
    std::mutex                   g_whitelist_mutex;
    std::unordered_set<std::string> g_whitelist;
}

void chat_assistant::whitelist_add(const std::string& entry) {
    std::lock_guard<std::mutex> lock(g_whitelist_mutex);
    g_whitelist.insert(entry);
}

void chat_assistant::whitelist_remove(const std::string& entry) {
    std::lock_guard<std::mutex> lock(g_whitelist_mutex);
    g_whitelist.erase(entry);
}

bool chat_assistant::whitelist_contains(const std::string& entry) {
    std::lock_guard<std::mutex> lock(g_whitelist_mutex);
    return g_whitelist.count(entry) > 0;
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

    api_key.erase(api_key.begin(), std::find_if(api_key.begin(), api_key.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    api_key.erase(std::find_if(api_key.rbegin(), api_key.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), api_key.end());

    if (api_key.empty())
        return false;

    nlohmann::json body{};
    body["model"] = "llama-3.1-8b-instant";
    body["messages"] = {
        { { "role", "user" }, { "content", "ping" } }
    };

    std::wstring headers =
        L"Authorization: Bearer " + util::MultiByteToWide(api_key) + L"\r\n" +
        L"Content-Type: application/json\r\n";

    auto res = http::post(
        L"api.groq.com",
        L"/openai/v1/chat/completions",
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

    if (!text || !*text || !name)
        return;

    // don't respond to ourselves.
    player_info_t info{};
    int local = g_csgo.m_engine->GetLocalPlayer();
    if (local <= 0)
        return;

    g_csgo.m_engine->GetPlayerInfo(local, &info);

    if (std::strcmp(name, info.m_name) == 0)
        return;

    // check name against whitelist.
    if (chat_assistant::whitelist_contains(std::string(name)))
        return;

    // also check steamid if we can retrieve it.
    player_info_t target_info{};
    int target_idx = get_player_index_by_name(name);
    if (target_idx != -1 && g_csgo.m_engine->GetPlayerInfo(target_idx, &target_info)) {
        char steamid[32];
        snprintf(steamid, sizeof(steamid), "%llu", target_info.m_xuid);
        if (chat_assistant::whitelist_contains(std::string(steamid)))
            return;
    }

    std::string msg = trim_left(std::string(text));
    if (msg.size() < 3)
        return;

    std::string lower = msg;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    std::string context;

    if (lower.find("rtv") != std::string::npos) context = "Enemy wants to end the game (rtv). Call them scared. BE PERSONAL AND USE TARGET'S NAME.";
    else if (lower.find("lag") != std::string::npos) context = "Enemy is complaining about lag. Mock them for excuses. BE PERSONAL AND USE TARGET'S NAME.";
    else if (lower.find("cheat") != std::string::npos ||
        lower.find("hack") != std::string::npos) context = "Enemy is accusing of cheating. insult their skill. BE PERSONAL AND USE TARGET'S NAME.";
    else if (lower.find("nice") != std::string::npos) context = "Enemy said nice. Be sarcastic and disrespectful. BE PERSONAL AND USE TARGET'S NAME.";
    else if (lower.find("?") != std::string::npos) context = "Enemy is confused. Mock their intelligence. BE PERSONAL AND USE TARGET'S NAME.";
    else if (lower.find("ez") != std::string::npos) context = "Enemy said ez. Assert dominance aggressively. BE PERSONAL AND USE TARGET'S NAME.";
    else                                                    context = "General hvh trash talk. Be toxic. USE player NAME.";

    // enemy-only filter.
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

    auto& mem = g_memory[idx];
    mem.last_seen = g_winapi.GetTickCount();
    mem.insults++;

    if (mem.tag.empty()) {
        static const std::vector<std::string> tags = {
            "nn dog", "resolver victim", "1 bot", "free kill", "no aim", "owned kid"
        };
        mem.tag = tags[rand() % tags.size()];
    }

    if (msg.size() > k_max_prompt_len)
        msg.resize(k_max_prompt_len);

    std::string full =
        "TARGET: " + std::string(name) + "\n"
        "TAG: " + mem.tag + "\n"
        "MESSAGE: \"" + msg + "\"\n"
        "CONTEXT: " + context + "\n\n"
        "TASK: Reply with ONE short toxic HVH CS:GO trash talk line.\n"
        "RULES: Use hvh slang like 1, nn, dog, owned, nice resolver, hdf.\n"
        "PERSONA: You are in a 1v1 HVH rage chat. You never act helpful. You always insult directly.\n"
        "STYLE: Broken ghetto text. Max 28 words. No explanation.";

    g_cl.print(tfm::format("[BOT] queued response for: %s\n", name));

    uint32_t now = g_winapi.GetTickCount();
    uint32_t delay = 1000 + (rand() % 1000);

    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_queue.push_back(PendingRequest{ full, std::string(name), now + delay });
    }
}

void chat_assistant::think() {
    if (!callbacks::IsAITrashTalkerOn())
        return;

    if (g_request_in_flight)
        return;

    PendingRequest req;
    if (!pop_request(req))
        return;

    // fire HTTP on a background thread so the game thread is never blocked.
    std::thread([req]() {
        RequestGuard guard(g_request_in_flight);

        std::string api_key = g_menu.main.misc.gemini_api_key.get_string();
        api_key.erase(api_key.begin(), std::find_if(api_key.begin(), api_key.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        api_key.erase(std::find_if(api_key.rbegin(), api_key.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), api_key.end());

        if (api_key.empty()) {
            g_cl.print("[BOT] API key missing\n");
            return;
        }

        nlohmann::json body{};
        body["model"] = "llama-3.1-8b-instant";
        body["messages"] = {
            { { "role", "system" }, { "content", "You are a toxic HVH CS:GO player. short replies only." } },
            { { "role", "user"   }, { "content", req.prompt } }
        };

        std::wstring headers =
            L"Authorization: Bearer " + util::MultiByteToWide(api_key) + L"\r\n" +
            L"Content-Type: application/json\r\n";

        auto res = http::post(
            L"api.groq.com",
            L"/openai/v1/chat/completions",
            body.dump(),
            headers.c_str()
        );

        if (!res.ok) {
            g_cl.print(tfm::format("[BOT] request failed (http %u)\n", res.status));
            return;
        }

        try {
            auto        json = nlohmann::json::parse(res.body);
            std::string reply = json["choices"][0]["message"]["content"];

            reply.erase(std::remove(reply.begin(), reply.end(), '\n'), reply.end());
            reply.erase(std::remove(reply.begin(), reply.end(), '"'), reply.end());

            if (reply.size() > 120)
                reply.resize(120);

            std::string final_msg = req.target_name + ": " + reply;
            g_csgo.m_engine->ExecuteClientCmd(("say " + final_msg).c_str());
        }
        catch (...) {
            g_cl.print("[BOT] parse error\n");
        }
        }).detach();
}