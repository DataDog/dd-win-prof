// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture.h"
#include "profile_exporter.h"
static pe_slice text(const char* s) { return (pe_slice){s, strlen(s)}; }
int main(int argc, char** argv) {
  if (argc != 4) {
    fprintf(
        stderr,
        "usage: exporter_probe <agent-base-url> <expected-pe-status> <timeout-ms>\n"
    );
    return 2;
  }
  pe_config config = {
      text(argv[1]),
      {0},
      text("dd-win-prof"),
      text("test"),
      strtoull(argv[3], NULL, 10),
      0
  };
  pe_exporter* exporter = NULL;
  pe_result result = {0};
  pe_config invalid = config;
  invalid.timeout_ms = 0;
  result = (pe_result){PE_HTTP, 999, 999};
  if (pe_exporter_create(&invalid, &exporter, &result) != PE_INVALID || exporter ||
      result.status != PE_INVALID || result.http_status || result.system_error)
    return 1;
  result = (pe_result){PE_HTTP, 999, 999};
  if (pe_exporter_send(NULL, NULL, &result) != PE_INVALID ||
      result.status != PE_INVALID || result.http_status || result.system_error)
    return 1;
  pe_status status = pe_exporter_create(&config, &exporter, &result);
  if (status != PE_OK) {
    fprintf(
        stderr, "create: %d system=%lu\n", status, (unsigned long)result.system_error
    );
    return 1;
  }
  pe_tag tags[] = {
      {text("service"), text("exporter-mock")}, {text("env"), text("test")}
  };
  pe_file extra = {text("extra.json"), text("{\"unmodified\":true}")};
  pe_profile profile = {
      {fixture, sizeof(fixture)},
      1700000000000LL,
      1700000001000LL,
      tags,
      2,
      text("{\"views\":[],\"sessions\":[]}"),
      &extra,
      1
  };
  status = pe_exporter_send(exporter, &profile, &result);
  pe_exporter_destroy(exporter);
  printf(
      "status=%d http=%lu system=%lu\n",
      status,
      (unsigned long)result.http_status,
      (unsigned long)result.system_error
  );
  return (int)status == atoi(argv[2]) && result.status == status &&
                 ((status == PE_HTTP && result.http_status >= 300 &&
                   !result.system_error) ||
                  (status == PE_TRANSPORT && !result.http_status &&
                   result.system_error) ||
                  (status == PE_OK && result.http_status == 200 &&
                   !result.system_error))
             ? 0
             : 1;
}
