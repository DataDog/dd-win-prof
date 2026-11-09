// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture.h"
static int allocation_failure;
static size_t allocations, live_allocations;
static void* test_malloc(size_t n) {
  ++allocations;
  void* p = allocation_failure ? NULL : malloc(n);
  if (p) ++live_allocations;
  return p;
}
static void test_free(void* p) {
  if (p) {
    if (!live_allocations) abort();
    --live_allocations;
  }
  free(p);
}
#define malloc test_malloc
#define free test_free
#include "../src/protocol.c"
#undef malloc
#undef free
#define S(s) ((pe_slice){s, sizeof(s) - 1})
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      exit(1);                                                \
    }                                                         \
  } while (0)

static pe_profile minimal(void) {
  pe_profile p = {0};
  p.pprof = S("\x28\xb5\x2f\xfd\0\xff\r\n");
  p.end_ms = 1234;
  return p;
}
static char* flatten(pe_request* r) {
  char* bytes = malloc((size_t)r->content_length + 1);
  CHECK(bytes);
  size_t n = 0;
  for (size_t i = 0; i < r->segment_count; ++i) {
    CHECK(r->segments[i].len <= r->content_length - n);
    memcpy(bytes + n, r->segments[i].ptr, r->segments[i].len);
    n += r->segments[i].len;
  }
  CHECK(n == r->content_length);
  bytes[n] = 0;
  return bytes;
}
static void exact_request(void) {
  pe_profile p = minimal();
  pe_request r;
  pe_tag tags[] = {{S("service"), S("a\"b\\c\n\0\1")}, {S("env"), S("test")}};
  p.tags = tags;
  p.tag_count = 2;
  p.internal_json = S("{\"views\":[],\"sessions\":[]}");
  CHECK(pe_request_build(&p, &r) == PE_OK);
  const char prefix[] =
      "--" PE_BOUNDARY
      "\r\nContent-Disposition: form-data; name=\"event\"; "
      "filename=\"event.json\"\r\nContent-Type: application/json\r\n\r\n"
      "{\"attachments\":[\"auto.pprof\"],\"tags_profiler\":\"service:"
      "a\\\"b\\\\c\\u000a\\u0000\\u0001,env:test\",\"start\":\"1970-01-01T00:00:00."
      "000000000Z\",\"end\":\"1970-01-01T00:00:01.234000000Z\",\"family\":\"windows\","
      "\"version\":\"4\",\"internal\":{\"views\":[],\"sessions\":[]}}"
      "\r\n--" PE_BOUNDARY
      "\r\nContent-Disposition: form-data; name=\"auto.pprof\"; "
      "filename=\"auto.pprof\"\r\nContent-Type: application/octet-stream\r\n\r\n";
  const char footer[] = "\r\n--" PE_BOUNDARY "--\r\n";
  CHECK(r.segment_count == 3 && r.segments[1].ptr == p.pprof.ptr);
  CHECK(r.content_length == sizeof(prefix) - 1 + p.pprof.len + sizeof(footer) - 1);
  char* bytes = flatten(&r);
  CHECK(!memcmp(bytes, prefix, sizeof(prefix) - 1));
  CHECK(!memcmp(bytes + sizeof(prefix) - 1, p.pprof.ptr, p.pprof.len));
  CHECK(!memcmp(bytes + sizeof(prefix) - 1 + p.pprof.len, footer, sizeof(footer) - 1));
  free(bytes);
  pe_request_destroy(&r);
  CHECK(!r.storage && !r.segment_count && !r.content_length);

  pe_file files[] = {
      {S("extra.json"), S("{\"a\":true}")}, {S("binary.bin"), S("\0\xff\r\n")}
  };
  p.files = files;
  p.file_count = 2;
  CHECK(pe_request_build(&p, &r) == PE_OK);
  CHECK(
      r.segment_count == 7 && r.segments[1].ptr == files[0].bytes.ptr &&
      r.segments[3].ptr == files[1].bytes.ptr
  );
  bytes = flatten(&r);
  CHECK(
      strstr(bytes, "\"attachments\":[\"auto.pprof\",\"extra.json\",\"binary.bin\"]")
  );
  CHECK(strstr(
      bytes,
      "name=\"extra.json\"; filename=\"extra.json\"\r\nContent-Type: "
      "application/octet-stream\r\n\r\n{\"a\":true}\r\n--" PE_BOUNDARY
  ));
  CHECK(!memcmp(r.segments[3].ptr, files[1].bytes.ptr, files[1].bytes.len));
  free(bytes);
  pe_request_destroy(&r);
}
static void invalid_requests(void) {
  pe_profile p = minimal();
  pe_request r;
  CHECK(pe_request_build(NULL, &r) == PE_INVALID && !r.storage);
  CHECK(pe_request_build(&p, NULL) == PE_INVALID);
  allocation_failure = 1;
  CHECK(pe_request_build(&p, &r) == PE_NOMEM && !r.storage);
  allocation_failure = 0;
  p.pprof.len = SIZE_MAX;
  size_t before = allocations;
  CHECK(pe_request_build(&p, &r) == PE_TOO_LARGE && allocations == before);
  p.pprof.len = UINT32_MAX;
  CHECK(pe_request_build(&p, &r) == PE_TOO_LARGE && allocations == before);
  p = minimal();
  p.pprof.ptr = NULL;
  CHECK(pe_request_build(&p, &r) == PE_INVALID);
  p = minimal();
  p.start_ms = -1;
  CHECK(pe_request_build(&p, &r) == PE_INVALID);
  p = minimal();
  p.end_ms = INT64_MAX;
  CHECK(pe_request_build(&p, &r) == PE_INVALID);
  p = minimal();
  p.start_ms = 2000;
  CHECK(pe_request_build(&p, &r) == PE_INVALID);
  p = minimal();
  p.pprof = S(PE_BOUNDARY);
  CHECK(pe_request_build(&p, &r) == PE_INVALID);
  const char* invalid[] = {
      "[]",
      "{",
      "{}junk",
      "{\"x\":01}",
      "{\"x\":1.}",
      "{\"x\":1e}",
      "{\"x\":true,}",
      "{\"x\":\"\\u0xyz\"}",
      "{\"x\":\"\\z\"}",
      "{\"x\":nul}"
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
    p = minimal();
    p.internal_json = (pe_slice){invalid[i], strlen(invalid[i])};
    CHECK(pe_request_build(&p, &r) == PE_INVALID && !r.storage);
  }
  p = minimal();
  p.internal_json = S(" {\"a\":[true,false,null,1.25e-3,{},[]],\"b\":\"\\u1234\"} \n");
  CHECK(pe_request_build(&p, &r) == PE_OK);
  pe_request_destroy(&r);
  pe_file file = {S("bad\"name"), S("x")};
  p = minimal();
  p.files = &file;
  p.file_count = 1;
  CHECK(pe_request_build(&p, &r) == PE_INVALID);
  file.name = S("auto.pprof");
  CHECK(pe_request_build(&p, &r) == PE_INVALID);
  file.name = S("ok");
  file.bytes.len = UINT32_MAX;
  CHECK(pe_request_build(&p, &r) == PE_TOO_LARGE);
  p = minimal();
  p.tag_count = 1;
  CHECK(pe_request_build(&p, &r) == PE_INVALID);
  pe_tag tag = {S("x"), S("x")};
  tag.value.len = SIZE_MAX;
  p.tags = &tag;
  CHECK(pe_request_build(&p, &r) == PE_TOO_LARGE);
  pe_request_destroy(NULL);
}
static void config_validation(void) {
  pe_config c = {
      S("http://localhost:8126"), {0}, S("dd-win-prof"), S("v0.4.0w"), 10000, 0
  };
  CHECK(pe_validate_config(&c) == PE_OK);
  c.endpoint = S("https://localhost:443/prefix/");
  CHECK(pe_validate_config(&c) == PE_OK);
  c.timeout_ms = 0;
  CHECK(pe_validate_config(&c) == PE_INVALID);
  c.timeout_ms = (uint64_t)INT_MAX + 1;
  CHECK(pe_validate_config(&c) == PE_INVALID);
  c.timeout_ms = INT_MAX;
  CHECK(pe_validate_config(&c) == PE_OK);
  c.endpoint = S("http://user:password@localhost");
  CHECK(pe_validate_config(&c) == PE_INVALID);
  c.endpoint = S("windows:\\pipe");
  CHECK(pe_validate_config(&c) == PE_INVALID);
  c.endpoint = S("datadoghq.com");
  c.agentless = 1;
  c.api_key = S("test-only");
  CHECK(pe_validate_config(&c) == PE_OK);
  c.api_key = S("a\r\nb");
  CHECK(pe_validate_config(&c) == PE_INVALID);
  c.api_key = S("a\0b");
  CHECK(pe_validate_config(&c) == PE_INVALID);
  c.api_key = S("test-only");
  c.api_key.len = 8193;
  CHECK(pe_validate_config(&c) == PE_INVALID);
  c.api_key = S("test-only");
  c.endpoint = S("datadoghq.com/path");
  CHECK(pe_validate_config(&c) == PE_INVALID);
  CHECK(pe_validate_config(NULL) == PE_INVALID);
}
static void more_validation(void) {
  pe_profile p = minimal();
  pe_request r;
  pe_tag tag = {S("name"), S("\xc0\x80")};
  p.tags = &tag;
  p.tag_count = 1;
  CHECK(pe_request_build(&p, &r) == PE_INVALID);
  tag.value = S("\xf0\x9f\x92\xa9");
  CHECK(pe_request_build(&p, &r) == PE_OK);
  pe_request_destroy(&r);
  const char* invalid_utf8[] = {
      "\x80",
      "\xc2",
      "\xe0\x80\x80",
      "\xed\xa0\x80",
      "\xf4\x90\x80\x80",
      "\xf5\x80\x80\x80"
  };
  for (size_t i = 0; i < sizeof(invalid_utf8) / sizeof(*invalid_utf8); ++i) {
    tag.value = (pe_slice){invalid_utf8[i], strlen(invalid_utf8[i])};
    CHECK(pe_request_build(&p, &r) == PE_INVALID);
  }
  p = minimal();
  pe_file files[] = {{S("a"), S("")}, {S("a"), S("")}};
  p.files = files;
  p.file_count = 2;
  CHECK(pe_request_build(&p, &r) == PE_INVALID);
  files[1].name = S("b");
  files[1].bytes = S(PE_BOUNDARY);
  CHECK(pe_request_build(&p, &r) == PE_INVALID);
  files[1].bytes = S("");
  CHECK(pe_request_build(&p, &r) == PE_OK);
  char* bytes = flatten(&r);
  const char optional[] =
      "name=\"a\"; filename=\"a\"\r\nContent-Type: "
      "application/octet-stream\r\n\r\n\r\n--" PE_BOUNDARY;
  CHECK(strstr(bytes, optional));
  free(bytes);
  pe_request_destroy(&r);
  char deep[256];
  size_t n = 0;
  memcpy(deep, "{\"a\":", 5);
  n = 5;
  for (int i = 0; i < 65; ++i) deep[n++] = '[';
  deep[n++] = '0';
  for (int i = 0; i < 65; ++i) deep[n++] = ']';
  deep[n++] = '}';
  p = minimal();
  p.internal_json = (pe_slice){deep, n};
  CHECK(pe_request_build(&p, &r) == PE_INVALID);
  p = minimal();
  p.start_ms = INT64_C(1700000000000);
  p.end_ms = INT64_C(1700000001000);
  CHECK(pe_request_build(&p, &r) == PE_OK);
  pe_request_destroy(&r);
}
static void write_fixture(const char* path) {
  pe_tag tags[] = {{S("service"), S("exporter-mock")}, {S("env"), S("test")}};
  pe_file file = {S("extra.json"), S("{\"unmodified\":true}")};
  pe_profile p = {
      {fixture, sizeof(fixture)},
      INT64_C(1700000000000),
      INT64_C(1700000001000),
      tags,
      2,
      S("{\"views\":[],\"sessions\":[]}"),
      &file,
      1
  };
  pe_request r;
  CHECK(pe_request_build(&p, &r) == PE_OK);
  char* bytes = flatten(&r);
  FILE* output = fopen(path, "wb");
  CHECK(output);
  CHECK(fwrite(bytes, 1, r.content_length, output) == r.content_length);
  CHECK(!fclose(output));
  free(bytes);
  pe_request_destroy(&r);
}
int main(int argc, char** argv) {
  exact_request();
  invalid_requests();
  config_validation();
  more_validation();
  if (argc == 2) write_fixture(argv[1]);
  CHECK(live_allocations == 0);
  puts(
      "exporter protocol tests passed: exact framing, borrowed bytes, optional files, "
      "validation, overflow, allocation failure"
  );
  return 0;
}
