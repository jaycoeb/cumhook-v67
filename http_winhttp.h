#pragma once

namespace http {
    struct Response {
        bool        ok{};
        uint32_t    status{};
        std::string body{};
        std::string error{};
    };

    Response post( const wchar_t* host, const wchar_t* path, const std::string& body, const std::wstring& extra_headers = L"" );
}
