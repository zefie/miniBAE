/* In-app HTTPS GET for nbeditor (hyphen dictionary downloads).
 * Included from nbeditor_main.cpp when USE_LIB_HYPHEN == TRUE. */

#if USE_LIB_HYPHEN == TRUE

#include <cstddef>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#if defined(NBEDITOR_HAVE_LIBCURL)
#include <curl/curl.h>
#endif
#endif

namespace
{

#if !defined(_WIN32) && defined(NBEDITOR_HAVE_LIBCURL)
struct NbCurlWriteCtx
{
    FILE *fp = nullptr;
    size_t written = 0;
    size_t max_bytes = 0;
    bool overflow = false;
};

static size_t NbCurlWrite(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    NbCurlWriteCtx *ctx = static_cast<NbCurlWriteCtx *>(userdata);
    const size_t n = size * nmemb;
    if (!ctx || !ctx->fp)
    {
        return 0;
    }
    if (ctx->max_bytes > 0 && ctx->written + n > ctx->max_bytes)
    {
        ctx->overflow = true;
        return 0;
    }
    const size_t w = fwrite(ptr, 1, n, ctx->fp);
    ctx->written += w;
    return w;
}
#endif

} // namespace

/* Download URL to dest_path (atomic .tmp → rename). Returns true on success. */
static bool NbHttpGetToFile(const char *url,
                            const char *dest_path,
                            char *err,
                            size_t err_size,
                            size_t max_bytes)
{
    if (err && err_size)
    {
        err[0] = '\0';
    }
    if (!url || !url[0] || !dest_path || !dest_path[0])
    {
        if (err && err_size)
        {
            std::snprintf(err, err_size, "Invalid URL or path");
        }
        return false;
    }

    char tmp_path[1100];
    if (std::snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dest_path) < 0)
    {
        if (err && err_size)
        {
            std::snprintf(err, err_size, "Path too long");
        }
        return false;
    }

#if defined(_WIN32)
    /* Convert URL to wide for WinHTTP. */
    wchar_t wurl[2048];
    int nwide = MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048);
    if (nwide <= 0)
    {
        if (err && err_size)
        {
            std::snprintf(err, err_size, "URL UTF-8 conversion failed");
        }
        return false;
    }

    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256];
    wchar_t path[1536];
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 1536;
    if (!WinHttpCrackUrl(wurl, 0, 0, &parts))
    {
        if (err && err_size)
        {
            std::snprintf(err, err_size, "WinHttpCrackUrl failed (%lu)", GetLastError());
        }
        return false;
    }

    HINTERNET session = WinHttpOpen(L"NeoBAE-nbeditor/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session)
    {
        if (err && err_size)
        {
            std::snprintf(err, err_size, "WinHttpOpen failed (%lu)", GetLastError());
        }
        return false;
    }

    const INTERNET_PORT port = parts.nPort ? parts.nPort
                                           : ((parts.nScheme == INTERNET_SCHEME_HTTPS)
                                                  ? INTERNET_DEFAULT_HTTPS_PORT
                                                  : INTERNET_DEFAULT_HTTP_PORT);
    HINTERNET connect = WinHttpConnect(session, host, port, 0);
    if (!connect)
    {
        if (err && err_size)
        {
            std::snprintf(err, err_size, "WinHttpConnect failed (%lu)", GetLastError());
        }
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect,
                                           L"GET",
                                           path,
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           flags);
    if (!request)
    {
        if (err && err_size)
        {
            std::snprintf(err, err_size, "WinHttpOpenRequest failed (%lu)", GetLastError());
        }
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    BOOL ok = WinHttpSendRequest(request,
                                 WINHTTP_NO_ADDITIONAL_HEADERS,
                                 0,
                                 WINHTTP_NO_REQUEST_DATA,
                                 0,
                                 0,
                                 0);
    if (!ok || !WinHttpReceiveResponse(request, nullptr))
    {
        if (err && err_size)
        {
            std::snprintf(err, err_size, "WinHTTP request failed (%lu)", GetLastError());
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status,
                        &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300)
    {
        if (err && err_size)
        {
            std::snprintf(err, err_size, "HTTP status %lu", static_cast<unsigned long>(status));
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    FILE *fp = std::fopen(tmp_path, "wb");
    if (!fp)
    {
        if (err && err_size)
        {
            std::snprintf(err, err_size, "Cannot write %s", tmp_path);
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    size_t total = 0;
    bool overflow = false;
    for (;;)
    {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail))
        {
            overflow = true;
            break;
        }
        if (avail == 0)
        {
            break;
        }
        char buf[8192];
        DWORD to_read = avail;
        if (to_read > sizeof(buf))
        {
            to_read = sizeof(buf);
        }
        DWORD got = 0;
        if (!WinHttpReadData(request, buf, to_read, &got) || got == 0)
        {
            break;
        }
        if (max_bytes > 0 && total + got > max_bytes)
        {
            overflow = true;
            break;
        }
        if (std::fwrite(buf, 1, got, fp) != got)
        {
            overflow = true;
            break;
        }
        total += got;
    }
    std::fclose(fp);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (overflow || total == 0)
    {
        DeleteFileA(tmp_path);
        if (err && err_size)
        {
            std::snprintf(err, err_size, overflow ? "Download too large or truncated" : "Empty response");
        }
        return false;
    }
    DeleteFileA(dest_path);
    if (!MoveFileA(tmp_path, dest_path))
    {
        /* Fallback: copy then delete. */
        if (!CopyFileA(tmp_path, dest_path, FALSE))
        {
            DeleteFileA(tmp_path);
            if (err && err_size)
            {
                std::snprintf(err, err_size, "Rename failed (%lu)", GetLastError());
            }
            return false;
        }
        DeleteFileA(tmp_path);
    }
    return true;

#elif defined(NBEDITOR_HAVE_LIBCURL)
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        if (err && err_size)
        {
            std::snprintf(err, err_size, "curl_easy_init failed");
        }
        return false;
    }
    FILE *fp = std::fopen(tmp_path, "wb");
    if (!fp)
    {
        curl_easy_cleanup(curl);
        if (err && err_size)
        {
            std::snprintf(err, err_size, "Cannot write %s", tmp_path);
        }
        return false;
    }
    NbCurlWriteCtx ctx;
    ctx.fp = fp;
    ctx.max_bytes = max_bytes;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NeoBAE-nbeditor/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NbCurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    const CURLcode rc = curl_easy_perform(curl);
    std::fclose(fp);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || ctx.overflow || ctx.written == 0)
    {
        std::remove(tmp_path);
        if (err && err_size)
        {
            if (ctx.overflow)
            {
                std::snprintf(err, err_size, "Download too large");
            }
            else
            {
                std::snprintf(err, err_size, "%s", curl_easy_strerror(rc));
            }
        }
        return false;
    }
    std::remove(dest_path);
    if (std::rename(tmp_path, dest_path) != 0)
    {
        std::remove(tmp_path);
        if (err && err_size)
        {
            std::snprintf(err, err_size, "Rename failed");
        }
        return false;
    }
    return true;

#else
    (void)max_bytes;
    if (err && err_size)
    {
        std::snprintf(err, err_size, "HTTPS downloads require libcurl on this platform");
    }
    return false;
#endif
}

#endif /* USE_LIB_HYPHEN == TRUE */
