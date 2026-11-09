// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
// Protocol adapted from dd-trace-dotnet PR 9252, SHA
// 832190564416d0d703f6dc4e97a5eaeead563fc6.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "profile_exporter.h"

typedef struct {
  char* data;
  size_t len;
  pe_status status;
} writer;
static int slice_ok(pe_slice s) { return s.len == 0 || s.ptr != NULL; }
static int utf8_ok(pe_slice s) {
  const unsigned char* p = (const unsigned char*)s.ptr;
  for (size_t i = 0; i < s.len;) {
    unsigned char c = p[i++];
    if (c < 128) continue;
    unsigned extra;
    uint32_t code, minimum;
    if (c >= 0xc2 && c <= 0xdf) {
      extra = 1;
      code = c & 31;
      minimum = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      extra = 2;
      code = c & 15;
      minimum = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      extra = 3;
      code = c & 7;
      minimum = 0x10000;
    } else
      return 0;
    if (extra > s.len - i) return 0;
    while (extra--) {
      c = p[i++];
      if ((c & 0xc0) != 0x80) return 0;
      code = (code << 6) | (c & 63);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
      return 0;
  }
  return 1;
}
static void put(writer* w, const void* p, size_t n) {
  if (w->status != PE_OK) return;
  if (n > UINT32_MAX - w->len) {
    w->status = PE_TOO_LARGE;
    return;
  }
  if (w->data && n) memcpy(w->data + w->len, p, n);
  w->len += n;
}
static void lit(writer* w, const char* s) { put(w, s, strlen(s)); }
static void escaped(writer* w, pe_slice s) {
  const unsigned char* p = (const unsigned char*)s.ptr;
  for (size_t i = 0; i < s.len && w->status == PE_OK; ++i) {
    unsigned char c = p[i];
    if (c == '"' || c == '\\') {
      lit(w, "\\");
      put(w, &c, 1);
    } else if (c < 32) {
      char hex[] = "\\u0000";
      const char* digits = "0123456789abcdef";
      hex[4] = digits[c >> 4];
      hex[5] = digits[c & 15];
      put(w, hex, 6);
    } else
      put(w, &c, 1);
  }
}
static void quoted(writer* w, pe_slice s) {
  lit(w, "\"");
  escaped(w, s);
  lit(w, "\"");
}
static int header_ok(pe_slice s) {
  if (!slice_ok(s) || s.len > 8192) return 0;
  const unsigned char* p = (const unsigned char*)s.ptr;
  for (size_t i = 0; i < s.len; ++i)
    if (p[i] < 32 || p[i] >= 127) return 0;
  return 1;
}
pe_status pe_validate_config(const pe_config* c) {
  if (!c || !c->timeout_ms || c->timeout_ms > INT_MAX || !header_ok(c->endpoint) ||
      !c->endpoint.len || !header_ok(c->api_key) || !header_ok(c->library_name) ||
      !c->library_name.len || !header_ok(c->library_version) || !c->library_version.len)
    return PE_INVALID;
  const char* p = (const char*)c->endpoint.ptr;
  if (c->agentless) {
    if (!c->api_key.len || p[0] == '.' || p[c->endpoint.len - 1] == '.')
      return PE_INVALID;
    for (size_t i = 0; i < c->endpoint.len; ++i)
      if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') ||
            (p[i] >= '0' && p[i] <= '9') || p[i] == '.' || p[i] == '-'))
        return PE_INVALID;
  } else {
    if (c->api_key.len || !((c->endpoint.len > 7 && !memcmp(p, "http://", 7)) ||
                            (c->endpoint.len > 8 && !memcmp(p, "https://", 8))))
      return PE_INVALID;
    for (size_t i = 0; i < c->endpoint.len; ++i)
      if (p[i] == '@' || p[i] == '?' || p[i] == '#' || p[i] == ' ' || p[i] == '\\')
        return PE_INVALID;
  }
  return PE_OK;
}

// Bounded JSON validation prevents malformed internal metadata from corrupting
// event.json.
typedef struct {
  const unsigned char *p, *end;
} json;
static void ws(json* j) {
  while (j->p < j->end &&
         (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
    ++j->p;
}
static int string(json* j) {
  if (j->p == j->end || *j->p++ != '"') return 0;
  while (j->p < j->end) {
    unsigned char c = *j->p++;
    if (c == '"') return 1;
    if (c < 32) return 0;
    if (c == '\\') {
      if (j->p == j->end) return 0;
      c = *j->p++;
      if (c == 'u') {
        for (int n = 0; n < 4; ++n) {
          if (j->p == j->end) return 0;
          c = *j->p++;
          if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F')))
            return 0;
        }
      } else if (!strchr("\"\\/bfnrt", c) || !c)
        return 0;
    }
  }
  return 0;
}
static int value(json* j, unsigned depth) {
  ws(j);
  if (depth > 64 || j->p == j->end) return 0;
  unsigned char c = *j->p;
  if (c == '"') return string(j);
  if (c == '{' || c == '[') {
    unsigned char close = c == '{' ? '}' : ']';
    ++j->p;
    ws(j);
    if (j->p < j->end && *j->p == close) {
      ++j->p;
      return 1;
    }
    for (;;) {
      if (c == '{') {
        if (!string(j)) return 0;
        ws(j);
        if (j->p == j->end || *j->p++ != ':') return 0;
      }
      if (!value(j, depth + 1)) return 0;
      ws(j);
      if (j->p == j->end) return 0;
      unsigned char next = *j->p++;
      if (next == close) return 1;
      if (next != ',') return 0;
      ws(j);
    }
  }
  const char* word = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : NULL;
  if (word) {
    size_t n = strlen(word);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, word, n)) return 0;
    j->p += n;
    return 1;
  }
  if (*j->p == '-') ++j->p;
  if (j->p == j->end) return 0;
  if (*j->p == '0')
    ++j->p;
  else {
    if (*j->p < '1' || *j->p > '9') return 0;
    do {
      ++j->p;
    } while (j->p < j->end && *j->p >= '0' && *j->p <= '9');
  }
  if (j->p < j->end && *j->p == '.') {
    ++j->p;
    const unsigned char* start = j->p;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') ++j->p;
    if (start == j->p) return 0;
  }
  if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
    ++j->p;
    if (j->p < j->end && (*j->p == '+' || *j->p == '-')) ++j->p;
    const unsigned char* start = j->p;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') ++j->p;
    if (start == j->p) return 0;
  }
  return 1;
}
static int internal_ok(pe_slice s) {
  if (!s.len) return 1;
  json j = {(const unsigned char*)s.ptr, (const unsigned char*)s.ptr + s.len};
  ws(&j);
  if (j.p == j.end || *j.p != '{' || !value(&j, 0)) return 0;
  ws(&j);
  return j.p == j.end;
}
static int timestamp(int64_t ms, char out[32]) {
  if (ms < 0 || ms > INT64_C(253402300799999)) return 0;
  time_t seconds = (time_t)(ms / 1000);
  if ((int64_t)seconds != ms / 1000) return 0;
  struct tm utc;
#ifdef _WIN32
  if (gmtime_s(&utc, &seconds)) return 0;
#else
  if (!gmtime_r(&seconds, &utc)) return 0;
#endif
  int n = snprintf(
      out,
      32,
      "%04d-%02d-%02dT%02d:%02d:%02d.%03d000000Z",
      utc.tm_year + 1900,
      utc.tm_mon + 1,
      utc.tm_mday,
      utc.tm_hour,
      utc.tm_min,
      utc.tm_sec,
      (int)(ms % 1000)
  );
  return n == 30;
}
static void part(writer* w, const char* name, const char* type) {
  lit(w, "--" PE_BOUNDARY "\r\nContent-Disposition: form-data; name=\"");
  lit(w, name);
  lit(w, "\"; filename=\"");
  lit(w, !strcmp(name, "event") ? "event.json" : name);
  lit(w, "\"\r\nContent-Type: ");
  lit(w, type);
  lit(w, "\r\n\r\n");
}
static void file_part(writer* w, pe_slice name) {
  lit(w, "\r\n--" PE_BOUNDARY "\r\nContent-Disposition: form-data; name=\"");
  put(w, name.ptr, name.len);
  lit(w, "\"; filename=\"");
  put(w, name.ptr, name.len);
  lit(w, "\"\r\nContent-Type: application/octet-stream\r\n\r\n");
}
static void event(writer* w, const pe_profile* p, const char* start, const char* end) {
  part(w, "event", "application/json");
  lit(w, "{\"attachments\":[\"auto.pprof\"");
  for (size_t i = 0; i < p->file_count; ++i) {
    lit(w, ",");
    quoted(w, p->files[i].name);
  }
  lit(w, "],\"tags_profiler\":\"");
  for (size_t i = 0; i < p->tag_count; ++i) {
    if (i) lit(w, ",");
    escaped(w, p->tags[i].key);
    lit(w, ":");
    escaped(w, p->tags[i].value);
  }
  lit(w, "\",\"start\":\"");
  lit(w, start);
  lit(w, "\",\"end\":\"");
  lit(w, end);
  lit(w, "\",\"family\":\"windows\",\"version\":\"4\"");
  if (p->internal_json.len) {
    lit(w, ",\"internal\":");
    put(w, p->internal_json.ptr, p->internal_json.len);
  }
  lit(w, "}");
}
static int filename_ok(pe_slice s) {
  if (!s.len || s.len > 255 || !header_ok(s)) return 0;
  const char* p = (const char*)s.ptr;
  for (size_t i = 0; i < s.len; ++i)
    if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') ||
          (p[i] >= '0' && p[i] <= '9') || p[i] == '.' || p[i] == '-' || p[i] == '_'))
      return 0;
  return !(s.len == 10 && !memcmp(s.ptr, "auto.pprof", 10)) &&
         !(s.len == 5 && !memcmp(s.ptr, "event", 5));
}
static int collision(pe_slice s) {
  const char* p = (const char*)s.ptr;
  const size_t n = sizeof(PE_BOUNDARY) - 1;
  if (s.len < n) return 0;
  for (size_t i = 0; i <= s.len - n; ++i)
    if (p[i] == PE_BOUNDARY[0] && !memcmp(p + i, PE_BOUNDARY, n)) return 1;
  return 0;
}
pe_status pe_request_build(const pe_profile* p, pe_request* out) {
  if (!out) return PE_INVALID;
  memset(out, 0, sizeof(*out));
  if (!p || !slice_ok(p->pprof) || !p->pprof.len || !slice_ok(p->internal_json) ||
      (p->tag_count && !p->tags) || (p->file_count && !p->files))
    return PE_INVALID;
  if (p->pprof.len > UINT32_MAX || p->internal_json.len > UINT32_MAX ||
      p->file_count > UINT32_MAX / 256 || p->tag_count > UINT32_MAX / 4)
    return PE_TOO_LARGE;
  size_t payload = p->pprof.len;
  for (size_t i = 0; i < p->file_count; ++i) {
    if (!slice_ok(p->files[i].bytes) || !filename_ok(p->files[i].name))
      return PE_INVALID;
    for (size_t j = 0; j < i; ++j)
      if (p->files[j].name.len == p->files[i].name.len &&
          !memcmp(p->files[j].name.ptr, p->files[i].name.ptr, p->files[i].name.len))
        return PE_INVALID;
    if (p->files[i].bytes.len > UINT32_MAX - payload) return PE_TOO_LARGE;
    payload += p->files[i].bytes.len;
  }
  for (size_t i = 0; i < p->tag_count; ++i) {
    if (!slice_ok(p->tags[i].key) || !slice_ok(p->tags[i].value)) return PE_INVALID;
    if (p->tags[i].key.len > UINT32_MAX / 6 || p->tags[i].value.len > UINT32_MAX / 6)
      return PE_TOO_LARGE;
  }
  for (size_t i = 0; i < p->tag_count; ++i)
    if (!utf8_ok(p->tags[i].key) || !utf8_ok(p->tags[i].value)) return PE_INVALID;
  char start[32], end[32];
  if (p->end_ms < p->start_ms || !timestamp(p->start_ms, start) ||
      !timestamp(p->end_ms, end) ||
      (!utf8_ok(p->internal_json) || !internal_ok(p->internal_json)))
    return PE_INVALID;
  writer w = {NULL, 0, PE_OK};
  event(&w, p, start, end);
  for (size_t i = 0; i < p->file_count; ++i) file_part(&w, p->files[i].name);
  lit(&w, "\r\n");
  part(&w, "auto.pprof", "application/octet-stream");
  lit(&w, "\r\n--" PE_BOUNDARY "--\r\n");
  if (w.status != PE_OK) return w.status;
  if (payload > UINT32_MAX - w.len) return PE_TOO_LARGE;
  if (collision(p->pprof) || collision(p->internal_json)) return PE_INVALID;
  for (size_t i = 0; i < p->file_count; ++i)
    if (collision(p->files[i].bytes)) return PE_INVALID;
  size_t count = 2 * p->file_count + 3;
  if (count > (SIZE_MAX - w.len) / sizeof(pe_slice)) return PE_TOO_LARGE;
  void* storage = malloc(count * sizeof(pe_slice) + w.len);
  if (!storage) return PE_NOMEM;
  pe_slice* segments = (pe_slice*)storage;
  w.data = (char*)(segments + count);
  w.len = 0;
  event(&w, p, start, end);
  size_t previous = 0, n = 0;
  for (size_t i = 0; i < p->file_count; ++i) {
    file_part(&w, p->files[i].name);
    segments[n++] = (pe_slice){w.data + previous, w.len - previous};
    segments[n++] = p->files[i].bytes;
    previous = w.len;
  }
  lit(&w, "\r\n");
  part(&w, "auto.pprof", "application/octet-stream");
  segments[n++] = (pe_slice){w.data + previous, w.len - previous};
  segments[n++] = p->pprof;
  previous = w.len;
  lit(&w, "\r\n--" PE_BOUNDARY "--\r\n");
  segments[n++] = (pe_slice){w.data + previous, w.len - previous};
  out->segments = segments;
  out->segment_count = n;
  out->content_length = (uint32_t)(w.len + payload);
  out->storage = storage;
  return PE_OK;
}
void pe_request_destroy(pe_request* r) {
  if (r) {
    free(r->storage);
    memset(r, 0, sizeof(*r));
  }
}
