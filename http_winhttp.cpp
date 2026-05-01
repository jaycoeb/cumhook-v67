#include "includes.h"
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace {
    static std::wstring widen_utf8( const std::string& s ) {
        return util::MultiByteToWide( s );
    }

    static std::string narrow_utf8( const std::wstring& s ) {
        return util::WideToMultiByte( s );
    }

    static std::string winhttp_error_to_string( DWORD err ) {
        return tfm::format( "WinHTTP error %u", (unsigned)err );
    }
}

http::Response http::post( const wchar_t* host, const wchar_t* path, const std::string& body, const std::wstring& extra_headers ) {
    Response out{};

    if( !host || !*host || !path || !*path ) {
        out.ok = false;
        out.error = "invalid host/path";
        return out;
    }

    HINTERNET h_session = nullptr;
    HINTERNET h_connect = nullptr;
    HINTERNET h_request = nullptr;

    h_session = WinHttpOpen( L"CUMHOOK/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
    if( !h_session ) {
        out.ok = false;
        out.error = winhttp_error_to_string( GetLastError( ) );
        return out;
    }

    h_connect = WinHttpConnect( h_session, host, INTERNET_DEFAULT_HTTPS_PORT, 0 );
    if( !h_connect ) {
        out.ok = false;
        out.error = winhttp_error_to_string( GetLastError( ) );
        WinHttpCloseHandle( h_session );
        return out;
    }

    h_request = WinHttpOpenRequest( h_connect, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE );
    if( !h_request ) {
        out.ok = false;
        out.error = winhttp_error_to_string( GetLastError( ) );
        WinHttpCloseHandle( h_connect );
        WinHttpCloseHandle( h_session );
        return out;
    }

    std::wstring headers = L"Content-Type: application/json\r\n";
    if( !extra_headers.empty( ) ) {
        headers += extra_headers;
        if( headers.size( ) < 2 || headers.substr( headers.size( ) - 2 ) != L"\r\n" )
            headers += L"\r\n";
    }

    BOOL ok = WinHttpAddRequestHeaders( h_request, headers.c_str( ), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE );
    if( !ok ) {
        out.ok = false;
        out.error = winhttp_error_to_string( GetLastError( ) );
        WinHttpCloseHandle( h_request );
        WinHttpCloseHandle( h_connect );
        WinHttpCloseHandle( h_session );
        return out;
    }

    DWORD total_len = (DWORD)body.size( );
    ok = WinHttpSendRequest( h_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body.data( ), total_len, total_len, 0 );
    if( !ok ) {
        out.ok = false;
        out.error = winhttp_error_to_string( GetLastError( ) );
        WinHttpCloseHandle( h_request );
        WinHttpCloseHandle( h_connect );
        WinHttpCloseHandle( h_session );
        return out;
    }

    ok = WinHttpReceiveResponse( h_request, nullptr );
    if( !ok ) {
        out.ok = false;
        out.error = winhttp_error_to_string( GetLastError( ) );
        WinHttpCloseHandle( h_request );
        WinHttpCloseHandle( h_connect );
        WinHttpCloseHandle( h_session );
        return out;
    }

    DWORD status = 0;
    DWORD status_size = sizeof( status );
    WinHttpQueryHeaders( h_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX );
    out.status = status;

    std::string response;
    for( ;; ) {
        DWORD available = 0;
        if( !WinHttpQueryDataAvailable( h_request, &available ) )
            break;

        if( available == 0 )
            break;

        std::string chunk;
        chunk.resize( available );

        DWORD read = 0;
        if( !WinHttpReadData( h_request, chunk.data( ), available, &read ) )
            break;

        chunk.resize( read );
        response += chunk;
    }

    out.body = std::move( response );
    out.ok = ( out.status >= 200 && out.status < 300 );

    WinHttpCloseHandle( h_request );
    WinHttpCloseHandle( h_connect );
    WinHttpCloseHandle( h_session );

    return out;
}
