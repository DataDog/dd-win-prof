// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <Windows.h>
#include <gtest/gtest.h>
#include <winsock2.h>

#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "../dd-win-prof/Configuration.h"
#include "../dd-win-prof/ProfileExporter.h"
#include "../dd-win-prof/Sample.h"
#include "../dd-win-prof/SampleValueType.h"
#include "../dd-win-prof/TagsHelper.h"
#include "../dd-win-prof/ThreadInfo.h"
#include "LocalHttpServer.h"
#include "ProfileTestOutput.h"
#include "pch.h"

class ProfileExporterExportTests : public ::testing::Test {
 protected:
  void SetUp() override {
    previousValuesCount = Sample::ValuesCount;
    // Create configuration and disable export for tests
    config = std::make_unique<Configuration>();
    config->SetExportEnabled(
        false
    );  // Disable export to prevent hanging on network calls

    // Create sample value type definitions
    sampleTypes = {{"cpu-time", "nanoseconds"}, {"cpu-samples", "count"}};

    // Set the global sample values count to match our sample types
    Sample::SetValuesCount(sampleTypes.size());

    // Create exporter
    exporter = std::make_unique<ProfileExporter>(config.get(), sampleTypes);
  }

  void TearDown() override {
    exporter.reset();
    config.reset();
    Sample::SetValuesCount(previousValuesCount);
    if (!debugOutputDirectory.empty()) {
      std::error_code error;
      std::filesystem::remove_all(debugOutputDirectory, error);
      EXPECT_FALSE(error) << error.message();
    }
  }

  std::filesystem::path debugOutputDirectory;
  size_t previousValuesCount;
  std::unique_ptr<Configuration> config;
  std::vector<SampleValueType> sampleTypes;
  std::unique_ptr<ProfileExporter> exporter;
};

TEST_F(ProfileExporterExportTests, InitializationWithExportEnabled) {
  // Test that exporter initializes correctly with export enabled
  ASSERT_TRUE(exporter->Initialize());
  EXPECT_TRUE(exporter->IsInitialized());
}

TEST_F(ProfileExporterExportTests, ExportWithoutSamples) {
  // Test export with empty profile
  ASSERT_TRUE(exporter->Initialize());

  // Export should succeed even with no samples
  EXPECT_TRUE(exporter->Export());
}

TEST_F(ProfileExporterExportTests, DebugProfileUsesZstdSuffix) {
  debugOutputDirectory =
      std::filesystem::temp_directory_path() /
      ("dd-win-prof-debug-" + std::to_string(::GetCurrentProcessId()) + "-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ASSERT_TRUE(std::filesystem::create_directory(debugOutputDirectory));
  exporter->SetDebugPprofPrefix((debugOutputDirectory / "profile_").string());
  exporter->SetDebugPprofFileWritingEnabled(true);
  ASSERT_TRUE(exporter->Initialize());
  ASSERT_TRUE(exporter->Export());

  std::vector<std::filesystem::path> profiles;
  for (const auto& entry : std::filesystem::directory_iterator(debugOutputDirectory)) {
    if (entry.path().extension() == ".pprof") {
      profiles.push_back(entry.path());
    }
  }
  ASSERT_EQ(profiles.size(), 1);
  EXPECT_TRUE(profiles.front().filename().string().ends_with(".zstd.pprof"));
}

TEST_F(ProfileExporterExportTests, ExportWithSamples) {
  // Test export with actual samples
  ASSERT_TRUE(exporter->Initialize());

  // Create a sample with test data using the correct constructor
  std::chrono::nanoseconds timestamp =
      std::chrono::nanoseconds(std::chrono::system_clock::now().time_since_epoch());

  // Create a valid ThreadInfo with a real handle
  HANDLE hThread;
  ::DuplicateHandle(
      ::GetCurrentProcess(),
      ::GetCurrentThread(),
      ::GetCurrentProcess(),
      &hThread,
      0,
      FALSE,
      DUPLICATE_SAME_ACCESS
  );
  auto threadInfo = std::make_shared<ThreadInfo>(::GetCurrentThreadId(), hThread);

  // Create frames array
  uint64_t frames[] = {0x1000, 0x2000, 0x3000};

  // Create sample using constructor
  auto sample = std::make_shared<Sample>(timestamp, threadInfo, frames, 3);

  // Add sample values for both types (cpu-time and cpu-samples)
  sample->AddValue(1000000, 0);  // 1ms CPU time in nanoseconds
  sample->AddValue(1, 1);        // 1 sample count

  // Add sample to exporter
  EXPECT_TRUE(exporter->Add(sample));

  // Export profile
  EXPECT_TRUE(exporter->Export());
}

TEST_F(ProfileExporterExportTests, MultipleExports) {
  // Test multiple consecutive exports
  ASSERT_TRUE(exporter->Initialize());

  // Create a valid ThreadInfo with a real handle
  HANDLE hThread;
  ::DuplicateHandle(
      ::GetCurrentProcess(),
      ::GetCurrentThread(),
      ::GetCurrentProcess(),
      &hThread,
      0,
      FALSE,
      DUPLICATE_SAME_ACCESS
  );
  auto threadInfo = std::make_shared<ThreadInfo>(::GetCurrentThreadId(), hThread);

  for (int i = 0; i < 3; ++i) {
    // Create a sample for each export
    std::chrono::nanoseconds timestamp =
        std::chrono::nanoseconds(std::chrono::system_clock::now().time_since_epoch());

    // Create frames array
    uint64_t frames[] = {
        static_cast<uint64_t>(0x1000 + i * 0x100),
        static_cast<uint64_t>(0x2000 + i * 0x100)
    };

    // Create sample using constructor
    auto sample = std::make_shared<Sample>(timestamp, threadInfo, frames, 2);

    // Add sample values for both types
    sample->AddValue(500000 * (i + 1), 0);  // CPU time in nanoseconds
    sample->AddValue(1, 1);                 // sample count

    EXPECT_TRUE(exporter->Add(sample));
    EXPECT_TRUE(exporter->Export());
  }
}

TEST_F(ProfileExporterExportTests, TagPreparation) {
  // Test that we can prepare tags without errors
  ASSERT_TRUE(exporter->Initialize());

  // This is more of an integration test - if initialization succeeds,
  // tag preparation worked correctly during exporter creation
  EXPECT_TRUE(exporter->IsInitialized());
}

TEST_F(ProfileExporterExportTests, ConfigurationIntegration) {
  // Test that exporter correctly uses Configuration data
  ASSERT_TRUE(exporter->Initialize());

  // Verify that configuration data is being used
  // The exporter should read service name, environment, version, etc. from config
  EXPECT_TRUE(exporter->IsInitialized());

  // Create a sample to ensure we have something to export
  std::chrono::nanoseconds timestamp =
      std::chrono::nanoseconds(std::chrono::system_clock::now().time_since_epoch());

  // Create a valid ThreadInfo with a real handle
  HANDLE hThread;
  ::DuplicateHandle(
      ::GetCurrentProcess(),
      ::GetCurrentThread(),
      ::GetCurrentProcess(),
      &hThread,
      0,
      FALSE,
      DUPLICATE_SAME_ACCESS
  );
  auto threadInfo = std::make_shared<ThreadInfo>(::GetCurrentThreadId(), hThread);

  // Create frames array
  uint64_t frames[] = {0x1000, 0x2000};

  // Create sample using constructor
  auto sample = std::make_shared<Sample>(timestamp, threadInfo, frames, 2);

  // Add sample values for both types
  sample->AddValue(1000000, 0);  // 1ms CPU time in nanoseconds
  sample->AddValue(1, 1);        // 1 sample count

  EXPECT_TRUE(exporter->Add(sample));

  // Export should use the configuration data for tags, URLs, etc.
  EXPECT_TRUE(exporter->Export());
}

// ===========================================================================
// noEnvVars scenarios -- exporter with defaults-only and overridden config
// ===========================================================================

static std::shared_ptr<Sample> CreateTestSample() {
  auto timestamp =
      std::chrono::nanoseconds(std::chrono::system_clock::now().time_since_epoch());
  HANDLE hThread;
  ::DuplicateHandle(
      ::GetCurrentProcess(),
      ::GetCurrentThread(),
      ::GetCurrentProcess(),
      &hThread,
      0,
      FALSE,
      DUPLICATE_SAME_ACCESS
  );
  auto threadInfo = std::make_shared<ThreadInfo>(::GetCurrentThreadId(), hThread);
  uint64_t frames[] = {0x1000, 0x2000};
  auto sample = std::make_shared<Sample>(timestamp, threadInfo, frames, 2);
  sample->AddValue(1000000, 0);
  sample->AddValue(1, 1);
  return sample;
}

TEST_F(ProfileExporterExportTests, ExporterWithDefaultsOnlyConfig) {
  auto defaultsConfig = std::make_unique<Configuration>();
  defaultsConfig->ResetToDefaults();
  defaultsConfig->SetExportEnabled(false);

  std::vector<SampleValueType> types = {
      {"cpu-time", "nanoseconds"}, {"cpu-samples", "count"}
  };
  Sample::SetValuesCount(types.size());

  auto exp = std::make_unique<ProfileExporter>(defaultsConfig.get(), types);
  ASSERT_TRUE(exp->Initialize());
  EXPECT_TRUE(exp->IsInitialized());

  EXPECT_TRUE(exp->Add(CreateTestSample()));
  EXPECT_TRUE(exp->Export());
}

TEST_F(ProfileExporterExportTests, ExporterWithApiOverriddenConfig) {
  auto overriddenConfig = std::make_unique<Configuration>();
  overriddenConfig->ResetToDefaults();
  overriddenConfig->SetServiceName("my-svc");
  overriddenConfig->SetEnvironmentName("staging");
  overriddenConfig->SetVersion("2.0");
  overriddenConfig->SetUserTags(TagsHelper::Parse("team:infra"));
  overriddenConfig->SetUploadInterval(std::chrono::seconds(30));
  overriddenConfig->SetExportEnabled(false);

  std::vector<SampleValueType> types = {
      {"cpu-time", "nanoseconds"}, {"cpu-samples", "count"}
  };
  Sample::SetValuesCount(types.size());

  auto exp = std::make_unique<ProfileExporter>(overriddenConfig.get(), types);
  ASSERT_TRUE(exp->Initialize());
  EXPECT_TRUE(exp->IsInitialized());

  EXPECT_TRUE(exp->Add(CreateTestSample()));
  EXPECT_TRUE(exp->Export());
}

TEST_F(ProfileExporterExportTests, ExporterWithDebugOutputDir) {
  // Use a temp directory for pprof output
  auto tempDir = fs::temp_directory_path() / "dd-win-prof-test-pprof";
  if (fs::exists(tempDir)) {
    fs::remove_all(tempDir);
  }

  auto debugConfig = std::make_unique<Configuration>();
  debugConfig->ResetToDefaults();
  debugConfig->SetProfilesOutputDirectory(tempDir);
  debugConfig->SetExportEnabled(false);

  std::vector<SampleValueType> types = {
      {"cpu-time", "nanoseconds"}, {"cpu-samples", "count"}
  };
  Sample::SetValuesCount(types.size());

  auto exp = std::make_unique<ProfileExporter>(debugConfig.get(), types);
  ASSERT_TRUE(exp->Initialize());
  EXPECT_TRUE(exp->Add(CreateTestSample()));
  EXPECT_TRUE(exp->Export());

  // Verify at least one .pprof file was created
  bool foundPprof = false;
  if (fs::exists(tempDir)) {
    for (auto const& entry : fs::directory_iterator(tempDir)) {
      if (entry.path().extension() == ".pprof") {
        foundPprof = true;
        break;
      }
    }
  }
  EXPECT_TRUE(foundPprof) << "Expected a .pprof file in " << tempDir.string();

  // Cleanup
  if (fs::exists(tempDir)) {
    fs::remove_all(tempDir);
  }
}

TEST_F(ProfileExporterExportTests, StandaloneExportInitializesWithoutConnecting) {
  // Initialization opens a WinHTTP session, not a network connection.
  auto agentConfig = std::make_unique<Configuration>();
  agentConfig->ResetToDefaults();
  agentConfig->SetExportEnabled(true);

  std::vector<SampleValueType> types = {
      {"cpu-time", "nanoseconds"}, {"cpu-samples", "count"}
  };
  Sample::SetValuesCount(types.size());

  auto exp = std::make_unique<ProfileExporter>(agentConfig.get(), types);
  EXPECT_TRUE(exp->Initialize());
  EXPECT_TRUE(exp->IsInitialized());

  // Explicit cleanup before destruction to avoid potential deadlocks
  exp->Cleanup();
}
TEST(ProfileExporterEndpointTests, AgentlessCopiesEndpointInputs) {
  pe_exporter* created = nullptr;
  pe_result result{};
  {
    const std::string site = "example.invalid";
    const std::string key = "local-test-key";
    pe_config config{
        {site.data(), site.size()},
        {key.data(), key.size()},
        {"endpoint-test", 13},
        {"1", 1},
        10000,
        1
    };
    ASSERT_EQ(pe_exporter_create(&config, &created, &result), PE_OK);
  }
  ASSERT_NE(created, nullptr);
  pe_exporter_destroy(created);
}

namespace {
Configuration HttpConfiguration(const LocalHttpServer& server) {
  ScopedEnvironment agent("DD_TRACE_AGENT_URL", server.Url());
  ScopedEnvironment pipe("DD_TRACE_PIPE_NAME", "");
  Configuration config;
  config.SetApiKey("");
  config.SetExportEnabled(true);
  config.SetProfilesOutputDirectory({});
  config.SetServiceName("exporter-test");
  return config;
}

std::string Tag(const std::string& body, const std::string& name) {
  auto start = body.find(name + ":");
  if (start == std::string::npos) return {};
  start += name.size() + 1;
  return body.substr(start, body.find_first_of(",\"", start) - start);
}

class ExportRumRecords : public IRumRecordProvider {
 public:
  void ConsumeViewRecords(std::vector<RumViewRecord>& records) override {
    records = {{1700000000000, 1000, "test-view", "Test view", {1000000, 3000000}}};
  }
  void ConsumeSessionRecords(std::vector<RumSessionRecord>& records) override {
    records = {{1700000000000, 1000, "test-session"}};
  }
  std::string GetCurrentSessionId() const override { return "test-session"; }
};
}  // namespace

TEST_F(
    ProfileExporterExportTests, HttpFinalThreadPreservesSequenceIdentityAndRumMetadata
) {
  LocalHttpServer server;
  auto httpConfig = HttpConfiguration(server);
  ExportRumRecords records;
  ProfileExporter exp(&httpConfig, sampleTypes, &records);
  exp.SetRumApplicationId("test-application");
  ASSERT_TRUE(exp.Initialize());
  std::thread periodic([&] {
    ASSERT_TRUE(exp.Add(CreateTestSample()));
    EXPECT_TRUE(exp.Export());
  });
  periodic.join();
  ASSERT_TRUE(exp.Add(CreateTestSample()));
  EXPECT_TRUE(exp.Export(true));
  exp.Cleanup();

  EXPECT_TRUE(server.Error().empty()) << server.Error();
  const auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 2u);
  for (size_t i = 0; i < requests.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(requests[i].headers.find("POST /profiling/v1/input HTTP/1.1\r\n"), 0u);
    const auto& body = requests[i].body;
    for (const auto* field :
         {"\"family\":\"windows\"",
          "\"version\":\"4\"",
          "language:native",
          "runtime_os:windows",
          "service:exporter-test",
          "test-application",
          "\"internal\":{",
          "\"viewId\":\"test-view\"",
          "\"viewName\":\"Test view\"",
          "\"cpuTimeNs\":1000000",
          "\"waitTimeNs\":3000000",
          "\"sessionId\":\"test-session\"",
          "\"timeStamp\":1700000000000",
          "\"duration\":1000"}) {
      EXPECT_NE(body.find(field), std::string::npos) << field;
    }
    EXPECT_EQ(Tag(body, "profile_seq"), std::to_string(i));
    EXPECT_FALSE(Tag(body, "runtime-id").empty());
    EXPECT_EQ(Tag(body, "runtime-id"), Tag(requests[0].body, "runtime-id"));
    EXPECT_EQ(Tag(body, "pid"), std::to_string(GetCurrentProcessId()));
    EXPECT_NE(body.find("filename=\"profile.pprof\""), std::string::npos);
    EXPECT_NE(body.find(std::string("\x28\xb5\x2f\xfd", 4)), std::string::npos);
  }
}

TEST_F(ProfileExporterExportTests, HttpResponsePolicyAcrossFinalThreadTransition) {
  for (const auto status : {200, 202, 204, 403, 404, 500, 504}) {
    SCOPED_TRACE(status);
    LocalHttpServer server({status});
    auto httpConfig = HttpConfiguration(server);
    ProfileExporter exp(&httpConfig, sampleTypes);
    ASSERT_TRUE(exp.Initialize());
    const bool expected = status != 403 && status != 404;
    std::thread periodic([&] {
      EXPECT_TRUE(exp.Add(CreateTestSample()));
      EXPECT_EQ(exp.Export(), expected);
    });
    periodic.join();
    ASSERT_TRUE(exp.Add(CreateTestSample()));
    EXPECT_EQ(exp.Export(true), expected);
    exp.Cleanup();
    EXPECT_TRUE(server.Error().empty()) << server.Error();
    const auto requests = server.Requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(Tag(requests.back().body, "profile_seq"), "1");
  }
}

TEST_F(
    ProfileExporterExportTests,
    HttpTransportErrorCountSurvivesThreadChangeAndResetsOnResponse
) {
  // Any HTTP response, including 500, clears the consecutive transport error count.
  LocalHttpServer server({0, 0, 0, 500, 0, 0, 0});
  auto httpConfig = HttpConfiguration(server);
  ProfileExporter exp(&httpConfig, sampleTypes);
  ASSERT_TRUE(exp.Initialize());
  auto send = [&](bool last, bool expected) {
    ASSERT_TRUE(exp.Add(CreateTestSample()));
    EXPECT_EQ(exp.Export(last), expected);
  };
  std::thread periodic([&] {
    send(false, true);
    send(false, true);
  });
  periodic.join();
  send(true, false);
  send(false, true);
  send(false, true);
  send(false, true);
  send(true, false);
  exp.Cleanup();
  EXPECT_TRUE(server.Error().empty()) << server.Error();
  const auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 7u);
  for (size_t i = 0; i < requests.size(); ++i) {
    EXPECT_EQ(Tag(requests[i].body, "profile_seq"), std::to_string(i));
  }
}

TEST_F(ProfileExporterExportTests, HttpTimeoutIsBoundedAndRecoverableTransportError) {
  LocalHttpServer server({-1, 202});
  auto httpConfig = HttpConfiguration(server);
  ProfileExporter exp(&httpConfig, sampleTypes);
  ASSERT_TRUE(exp.Initialize());
  ASSERT_TRUE(exp.Add(CreateTestSample()));
  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(exp.Export(true));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(elapsed, std::chrono::seconds(8));
  EXPECT_LT(elapsed, std::chrono::seconds(15));
  EXPECT_TRUE(server.Error().empty()) << server.Error();
  EXPECT_TRUE(exp.Add(CreateTestSample()));
  EXPECT_TRUE(exp.Export(true));
  exp.Cleanup();
  EXPECT_EQ(server.Requests().size(), 2u);
}

TEST_F(ProfileExporterExportTests, HttpFailureStillResetsAndAllowsNextExport) {
  WSADATA data{};
  ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
  struct WinsockCleanup {
    ~WinsockCleanup() { WSACleanup(); }
  } winsock;
  SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_NE(socket, INVALID_SOCKET);
  struct SocketCleanup {
    SOCKET s;
    ~SocketCleanup() { closesocket(s); }
  } close{socket};
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  int size = sizeof(address);
  ASSERT_EQ(getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size), 0);
  const char* variable = "DD_TRACE_AGENT_URL";
  DWORD length = GetEnvironmentVariableA(variable, nullptr, 0);
  std::string previous(length, '\0');
  if (length) ASSERT_NE(GetEnvironmentVariableA(variable, previous.data(), length), 0u);
  struct RestoreEnvironment {
    const char* name;
    const std::string& value;
    ~RestoreEnvironment() {
      SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    }
  } restore{variable, previous};
  auto endpoint = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
  ASSERT_TRUE(SetEnvironmentVariableA(variable, endpoint.c_str()));
  Configuration httpConfig;
  httpConfig.SetApiKey("");
  httpConfig.SetExportEnabled(true);
  int resetCalls = 0;
  // The encoding API has C function pointers, so track resets on this test thread.
  static thread_local int* resets;
  resets = &resetCalls;
  dd_win_prof::ProfileEncodingApi api;
  api.reset = [](pa_profile* p) {
    ++*resets;
    return pa_reset(p);
  };
  ProfileExporter http(&httpConfig, sampleTypes, nullptr, {}, api);
  ASSERT_TRUE(http.Initialize());
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(http.Add(CreateTestSample()));
    EXPECT_EQ(http.Export(i == 2), i < 2);
    EXPECT_EQ(resetCalls, i + 1);
  }
  http.Cleanup();
  resets = nullptr;
}

TEST_F(ProfileExporterExportTests, HttpCustomDefinitionsSurviveReset) {
  LocalHttpServer server;
  auto httpConfig = HttpConfiguration(server);
  const std::vector<SampleValueType> definitions{
      {std::string("wait\0time", 9), std::string("nano\0seconds", 12)},
      {"cpu-time", "count"}
  };
  ProfileExporter exp(&httpConfig, definitions);
  ASSERT_TRUE(exp.Initialize());
  for (int cycle = 0; cycle < 2; ++cycle) {
    ASSERT_TRUE(exp.Add(CreateTestSample()));
    ASSERT_TRUE(exp.Export(cycle == 1));
  }
  exp.Cleanup();
  ASSERT_TRUE(server.Error().empty()) << server.Error();
  const auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 2u);
  for (size_t i = 0; i < requests.size(); ++i) {
    EXPECT_EQ(Tag(requests[i].body, "profile_seq"), std::to_string(i));
    EXPECT_NE(requests[i].body.find("service:exporter-test"), std::string::npos);
    WriteHttpTestProfile(
        "http-custom-" + std::to_string(i + 1) + ".pprof", requests[i].body
    );
  }
}
