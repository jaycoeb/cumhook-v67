#pragma once

namespace chat_assistant {
    void on_player_say(const char* name, const char* text);
    void think();
    bool test_connection();

    void whitelist_add(const std::string& entry);    // name or steamid
    void whitelist_remove(const std::string& entry);
    bool whitelist_contains(const std::string& entry);
}