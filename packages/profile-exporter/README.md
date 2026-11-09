# Experimental profile exporter

Pure C multipart/event builder with a Windows WinHTTP backend. This is an
experimental standalone uploader, not a production-qualified transport. No Rust,
libcurl, aggregation implementation or compression library is linked here.

`include/profile_exporter.h` takes already-Zstd-compressed pprof bytes, Unix
millisecond interval, tags, optional internal JSON and optional unmodified files.
All profile/metadata inputs are borrowed until synchronous send returns. The
exporter owns a copy of endpoint/HTTP headers and a WinHTTP session. Instances are
single-threaded. Callback delivery does not construct this exporter.

The builder allocates one block containing event/framing and segment descriptors.
Attachment bytes are not copied. WinHTTP writes these segments with their exact
DWORD-bounded total Content-Length. Metadata memory is additional to the C core's
encoding/output memory. Internal JSON is emitted unmodified in event.json's
`internal` field, preserving the previous native RUM convention. Event family is
`windows`, language is `native` in native tags, event version is `4`, attachment is
`auto.pprof`. Native service/env/version/host, runtime-id, hardware, profiler,
profile_seq, pid and rum.application_id tags are preserved.

## Endpoints and failures

- Agent: HTTP or HTTPS base URL, including optional path prefix. Append
  `/profiling/v1/input`, removing trailing slashes. Native configuration uses
  DD_TRACE_AGENT_URL, or DD_AGENT_HOST and DD_TRACE_AGENT_PORT.
- Agentless: bare site, e.g. `datadoghq.com`. Construct
  `https://intake.profile.<site>/api/v2/profile`, with DD-API-KEY. Full URLs are not
  accepted as sites. Native configuration uses existing DD_SITE/API key fields.
- No redirects, automatic application retries, queue or delivery replay. HTTP
  non-2xx is `PE_HTTP`; WinHTTP failure is `PE_TRANSPORT`, with system error code.
  Result fields are initialized on every call. Native Export returns false on
  failure and still resets the profile; callback error codes/ownership are unchanged.
  Successful standalone sends log `Experimental HTTP export succeeded, HTTP: <status>`.
  Failure logs include HTTP status and/or Win32 system error, not credentials.
  Runner's exit status or generic completion log alone does not prove intake acceptance.
- Native timeout is 10 seconds per WinHTTP phase, not a total request deadline.
  API timeouts must be 1..INT_MAX milliseconds. WinHttpSetTimeouts is checked.
- Invalid header controls/NUL/non-ASCII or values over 8192 bytes are rejected,
  never truncated. Request totals over UINT32_MAX are rejected before allocation.
  Internal JSON must be a UTF-8 object (nesting <=64). Tags must be UTF-8. Optional
  file names are restricted to ASCII alphanumerics, period, underscore and hyphen.
  Duplicate/reserved file names and boundary collisions in payloads are rejected.

WinHTTP is a Windows system dependency. MSVC uses standard `/DELAYLOAD:winhttp.dll`
with delayimp so callback-only consumers do not load it. No custom DLL loader or
additional runtime binary is shipped. Default WinHTTP proxy/TLS behavior applies;
there is no custom certificate handling. Named pipes and non-Windows HTTP backends
are not implemented. HTTPS, proxy/auth environments, real agentless intake,
backend indexing, concurrency and soak behavior remain separate qualification gates.

## Host tests

```sh
cmake -S packages/profile-exporter -B /tmp/profile-exporter -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/profile-exporter
ctest --test-dir /tmp/profile-exporter --output-on-failure
```

Tests check exact multipart bytes and length, timestamps/escaping, borrowed binary
profile data, optional files, malformed metadata, overflow and allocation failure.
The test-only allocator injection compiles protocol.c separately; it is not in the
shipped library. `PE_BUILD_TESTS` defaults on only when this package is top-level,
so SDK FetchContent/add_subdirectory consumers do not pull developer tests.

## Windows loopback test

From this worktree's root on the existing worker (Python 3 required):

```powershell
cmake -S packages/profile-exporter -B build-exporter -G 'Visual Studio 17 2022' -A x64
cmake --build build-exporter --config Release
ctest --test-dir build-exporter -C Release --output-on-failure
python packages/profile-exporter/tests/mock_intake.py `
  --probe build-exporter/Release/exporter_probe.exe --output received.zst.pprof
```

The probe uses a fixed actual C-core Zstd pprof fixture (one cpu-time sample=42),
internal RUM and an unmodified file. The mock checks the actual WinHTTP POST path,
headers, event JSON and attachment bytes; exercises 403, redirect refusal, receive
timeout and connection refusal. It binds loopback only and needs no credentials.
Retrieve `received.zst.pprof` from the worker, then decode on a host with libzstd
or the `zstandard` Python module:

```sh
python3 packages/profile-exporter/tests/mock_intake.py --decode received.zst.pprof
```

The decode verifies Zstd decompression and the fixture's protobuf sample/type.
`e2e-tests/validate-c-aggregation.ps1` builds both exporter targets and runs this
mock and decode using its existing integration-test venv (which installs zstandard).
Also run the native Tests target, particularly ProfileExporterExportTests and
ProfilingDataExportTests, plus scripts/test-consumer.ps1 for both SDK consumption
paths. Windows execution, real uploads and backend indexing are not proven by host
protocol tests. The worker must build this worktree, not the primary checkout.

## Provenance and license

Protocol fields, multipart framing and WinHTTP flow are adapted from
[dd-trace-dotnet PR #9252](https://github.com/DataDog/dd-trace-dotnet/pull/9252),
`shared/src/datadog-profiling-poc/src/exporter_win.c`, pinned at
`832190564416d0d703f6dc4e97a5eaeead563fc6`. Source was treated as an unvalidated
prototype. This implementation does not use its opaque encoded-profile layout,
aggregation core, named-pipe backend or duplicate Zstd implementation.

Copyright Datadog, Inc., Apache License 2.0. See the repository LICENSE and NOTICE,
which accompany installed binary distributions.
