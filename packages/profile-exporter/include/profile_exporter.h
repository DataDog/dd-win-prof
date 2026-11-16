// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
#ifndef DD_PROFILE_EXPORTER_H
#define DD_PROFILE_EXPORTER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const void* ptr;
  size_t len;
} pe_slice;
typedef struct {
  pe_slice key, value;
} pe_tag;
typedef struct {
  pe_slice name, bytes;
} pe_file;
typedef enum {
  PE_OK,
  PE_INVALID,
  PE_TOO_LARGE,
  PE_NOMEM,
  PE_TRANSPORT,
  PE_HTTP
} pe_status;
typedef struct {
  pe_status status;
  uint32_t http_status, system_error;
} pe_result;
typedef struct {
  // Agent base URL, or bare Datadog site in agentless mode.
  pe_slice endpoint, api_key, library_name, library_version;
  uint64_t timeout_ms;  // Per WinHTTP phase, in [1, INT_MAX].
  int agentless;
} pe_config;
typedef struct {
  pe_slice pprof;  // Already Zstd-compressed. Borrowed through synchronous send.
  int64_t start_ms, end_ms;
  const pe_tag* tags;
  size_t tag_count;
  pe_slice internal_json;  // Optional, valid JSON object. Included unmodified.
  const pe_file* files;    // Optional, unmodified attachments.
  size_t file_count;
} pe_profile;

// Single-threaded instances. Config is copied on create; result may be NULL.
typedef struct pe_exporter pe_exporter;
pe_status pe_validate_config(const pe_config*);
pe_status pe_exporter_create(const pe_config*, pe_exporter**, pe_result*);
pe_status pe_exporter_send(pe_exporter*, const pe_profile*, pe_result*);
void pe_exporter_destroy(pe_exporter*);

// Portable multipart builder. Owns framing/event bytes, borrows attachment bytes.
// Destroy a successful request before reusing its output object.
typedef struct {
  const pe_slice* segments;
  size_t segment_count;
  uint32_t content_length;
  void* storage;
} pe_request;
#define PE_BOUNDARY "dd-win-prof-16455-6f281c9de0724b63a815"
#define PE_CONTENT_TYPE "multipart/form-data; boundary=" PE_BOUNDARY
pe_status pe_request_build(const pe_profile*, pe_request*);
void pe_request_destroy(pe_request*);
#ifdef __cplusplus
}
#endif
#endif
