// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
// WinHTTP flow adapted from dd-trace-dotnet PR 9252, SHA
// 832190564416d0d703f6dc4e97a5eaeead563fc6.
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <winhttp.h>

#include "profile_exporter.h"

struct pe_exporter {
  HINTERNET session;
  wchar_t *url, *host, *headers;
  const wchar_t* path;
  INTERNET_PORT port;
  DWORD flags;
  int timeout;
};
static pe_status finish(pe_result* r, pe_status s, DWORD error) {
  if (r) {
    r->status = s;
    r->system_error = error;
  }
  return s;
}
static wchar_t* wide(const char* s) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
  if (!n) return NULL;
  wchar_t* out = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
  if (out && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, n)) {
    free(out);
    return NULL;
  }
  return out;
}
static void append(char** dst, pe_slice s) {
  if (s.len) memcpy(*dst, s.ptr, s.len);
  *dst += s.len;
}
#define PE_TEXT(s) ((pe_slice){s, sizeof(s) - 1})
void pe_exporter_destroy(pe_exporter* e) {
  if (!e) return;
  if (e->session) WinHttpCloseHandle(e->session);
  free(e->url);
  free(e->host);
  if (e->headers) {
    SecureZeroMemory(e->headers, wcslen(e->headers) * sizeof(wchar_t));
    free(e->headers);
  }
  free(e);
}
pe_status pe_exporter_create(const pe_config* c, pe_exporter** out, pe_result* r) {
  if (r) memset(r, 0, sizeof(*r));
  if (out) *out = NULL;
  if (!out || pe_validate_config(c) != PE_OK) return finish(r, PE_INVALID, 0);
  pe_exporter* e = (pe_exporter*)calloc(1, sizeof(*e));
  if (!e) return finish(r, PE_NOMEM, 0);
  pe_status status = PE_NOMEM;
  DWORD error = 0;
  // Config slices are bounded to 8192 bytes before any allocation or narrowing.
  size_t size = c->endpoint.len + c->api_key.len + 2 * c->library_name.len +
                2 * c->library_version.len + 1024;
  char* buffer = (char*)malloc(size);
  if (!buffer) goto done;
  char* p = buffer;
  if (c->agentless) append(&p, PE_TEXT("https://intake.profile."));
  pe_slice endpoint = c->endpoint;
  if (!c->agentless)
    while (endpoint.len && ((const char*)endpoint.ptr)[endpoint.len - 1] == '/')
      --endpoint.len;
  append(&p, endpoint);
  if (c->agentless)
    append(&p, PE_TEXT("/api/v2/profile"));
  else
    append(&p, PE_TEXT("/profiling/v1/input"));
  *p = 0;
  e->url = wide(buffer);
  if (!e->url) goto done;
  URL_COMPONENTS url;
  memset(&url, 0, sizeof(url));
  url.dwStructSize = sizeof(url);
  url.dwHostNameLength = url.dwUrlPathLength = url.dwUserNameLength =
      url.dwPasswordLength = url.dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(e->url, 0, 0, &url) ||
      (url.nScheme != INTERNET_SCHEME_HTTP && url.nScheme != INTERNET_SCHEME_HTTPS) ||
      !url.dwHostNameLength || url.dwUserNameLength || url.dwPasswordLength ||
      url.dwExtraInfoLength) {
    status = PE_INVALID;
    goto done;
  }
  e->host = (wchar_t*)calloc((size_t)url.dwHostNameLength + 1, sizeof(wchar_t));
  if (!e->host) goto done;
  memcpy(e->host, url.lpszHostName, (size_t)url.dwHostNameLength * sizeof(wchar_t));
  e->path = url.lpszUrlPath;
  e->port = url.nPort;
  e->flags = url.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  e->timeout = (int)c->timeout_ms;
  p = buffer;
  append(&p, PE_TEXT("Content-Type: " PE_CONTENT_TYPE "\r\nDD-EVP-ORIGIN: "));
  append(&p, c->library_name);
  append(&p, PE_TEXT("\r\nDD-EVP-ORIGIN-VERSION: "));
  append(&p, c->library_version);
  append(&p, PE_TEXT("\r\nUser-Agent: "));
  append(&p, c->library_name);
  append(&p, PE_TEXT("/"));
  append(&p, c->library_version);
  append(&p, PE_TEXT("\r\n"));
  if (c->agentless) {
    append(&p, PE_TEXT("DD-API-KEY: "));
    append(&p, c->api_key);
    append(&p, PE_TEXT("\r\n"));
  }
  *p = 0;
  e->headers = wide(buffer);
  if (!e->headers) goto done;
  e->session = WinHttpOpen(
      L"dd-win-prof",
      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS,
      0
  );
  if (!e->session ||
      !WinHttpSetTimeouts(e->session, e->timeout, e->timeout, e->timeout, e->timeout)) {
    status = PE_TRANSPORT;
    error = GetLastError();
    goto done;
  }
  *out = e;
  e = NULL;
  status = PE_OK;
done:
  if (buffer) {
    SecureZeroMemory(buffer, size);
    free(buffer);
  }
  pe_exporter_destroy(e);
  return finish(r, status, error);
}
pe_status pe_exporter_send(pe_exporter* e, const pe_profile* p, pe_result* r) {
  if (r) memset(r, 0, sizeof(*r));
  if (!e) return finish(r, PE_INVALID, 0);
  pe_request body;
  pe_status status = pe_request_build(p, &body);
  if (status != PE_OK) return finish(r, status, 0);
  DWORD error = 0;
  HINTERNET connection = WinHttpConnect(e->session, e->host, e->port, 0);
  HINTERNET request = NULL;
  if (!connection) goto transport;
  request = WinHttpOpenRequest(
      connection,
      L"POST",
      e->path,
      NULL,
      WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      e->flags
  );
  if (!request) goto transport;
  DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  DWORD disable = WINHTTP_DISABLE_COOKIES;
  if (!WinHttpSetTimeouts(request, e->timeout, e->timeout, e->timeout, e->timeout) ||
      !WinHttpSetOption(
          request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)
      ) ||
      !WinHttpSetOption(
          request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)
      ) ||
      !WinHttpSendRequest(
          request,
          e->headers,
          (DWORD)-1,
          WINHTTP_NO_REQUEST_DATA,
          0,
          body.content_length,
          0
      ))
    goto transport;
  for (size_t i = 0; i < body.segment_count; ++i) {
    const char* data = (const char*)body.segments[i].ptr;
    DWORD remaining = (DWORD)body.segments[i].len;
    while (remaining) {
      DWORD written = 0;
      if (!WinHttpWriteData(request, data, remaining, &written)) goto transport;
      if (!written || written > remaining) {
        status = PE_TRANSPORT;
        error = ERROR_WRITE_FAULT;
        goto done;
      }
      data += written;
      remaining -= written;
    }
  }
  if (!WinHttpReceiveResponse(request, NULL)) goto transport;
  DWORD code = 0, size = sizeof(code);
  if (!WinHttpQueryHeaders(
          request,
          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX,
          &code,
          &size,
          WINHTTP_NO_HEADER_INDEX
      ))
    goto transport;
  if (r) r->http_status = code;
  status = code >= 200 && code < 300 ? PE_OK : PE_HTTP;
  goto done;
transport:
  error = GetLastError();
  status = PE_TRANSPORT;
done:
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  pe_request_destroy(&body);
  return finish(r, status, error);
}
