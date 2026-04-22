// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include "pch.h"
#include "../dd-win-prof/RumContext.h"
#include "../dd-win-prof/Profiler.h"
#include "../dd-win-prof/ProfileExporter.h"
#include "../dd-win-prof/Configuration.h"
#include "../dd-win-prof/Sample.h"
#include "../dd-win-prof/SampleValueType.h"
#include "../dd-win-prof/ThreadInfo.h"
#include "../dd-win-prof/dd-win-prof.h"
#include "../dd-win-prof/dd-win-rum-private.h"
#include <gtest/gtest.h>
#include <sstream>
#include <thread>

// Helper: build a RumViewRecord with optional per-vital values.
static RumViewRecord MakeViewRecord(
    int64_t timestampMs, int64_t durationMs,
    std::string viewId, std::string viewName,
    int64_t cpuTimeNs = 0, int64_t waitTimeNs = 0)
{
    RumViewRecord r;
    r.timestamp_ms = timestampMs;
    r.duration_ms  = durationMs;
    r.view_id      = std::move(viewId);
    r.view_name    = std::move(viewName);
    r.vitals_ns[static_cast<size_t>(ViewVitalKind::CpuTime)]  = cpuTimeNs;
    r.vitals_ns[static_cast<size_t>(ViewVitalKind::WaitTime)] = waitTimeNs;
    return r;
}

// ---------------------------------------------------------------------------
// RumViewContext struct tests
// ---------------------------------------------------------------------------

TEST(RumViewContextTests, DefaultConstructedIsEmpty) {
    RumViewContext ctx;
    EXPECT_TRUE(ctx.view_id.empty());
    EXPECT_TRUE(ctx.view_name.empty());
}

TEST(RumViewContextTests, CopySemantics) {
    RumViewContext ctx;
    ctx.view_id = "view-1";
    ctx.view_name = "HomePage";

    RumViewContext copy = ctx;
    EXPECT_EQ(copy.view_id, "view-1");
    EXPECT_EQ(copy.view_name, "HomePage");

    // Modifying the copy doesn't affect the original
    copy.view_id = "view-2";
    EXPECT_EQ(ctx.view_id, "view-1");
}

TEST(RumViewContextTests, MoveSemantics) {
    RumViewContext ctx;
    ctx.view_id = "view-1";
    ctx.view_name = "HomePage";

    RumViewContext moved = std::move(ctx);
    EXPECT_EQ(moved.view_id, "view-1");
    EXPECT_EQ(moved.view_name, "HomePage");
}

// ---------------------------------------------------------------------------
// Profiler RUM context tests
// ---------------------------------------------------------------------------

class ProfilerRumContextTest : public ::testing::Test {
protected:
    void SetUp() override {
        _profiler = std::make_unique<Profiler>();
    }

    void TearDown() override {
        _profiler.reset();
    }

    std::unique_ptr<Profiler> _profiler;
};

TEST_F(ProfilerRumContextTest, SetRumSessionReturnsFalseOnNull) {
    EXPECT_FALSE(_profiler->SetRumSession(nullptr));
}

TEST_F(ProfilerRumContextTest, SetRumSessionReturnsFalseOnEmptyAppId) {
    RumSessionContext ctx = {};
    ctx.application_id = "";
    ctx.session_id = "S1";
    EXPECT_FALSE(_profiler->SetRumSession(&ctx));
}

TEST_F(ProfilerRumContextTest, SetRumSessionReturnsFalseOnNullAppId) {
    RumSessionContext ctx = {};
    ctx.application_id = nullptr;
    ctx.session_id = "S1";
    EXPECT_FALSE(_profiler->SetRumSession(&ctx));
}

TEST_F(ProfilerRumContextTest, SetRumSessionReturnsFalseOnEmptySessionId) {
    RumSessionContext ctx = {};
    ctx.application_id = "app-1";
    ctx.session_id = "";
    EXPECT_FALSE(_profiler->SetRumSession(&ctx));
}

TEST_F(ProfilerRumContextTest, SetRumSessionReturnsFalseOnNullSessionId) {
    RumSessionContext ctx = {};
    ctx.application_id = "app-1";
    ctx.session_id = nullptr;
    EXPECT_FALSE(_profiler->SetRumSession(&ctx));
}

TEST_F(ProfilerRumContextTest, SetViewContextAndReadBack) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-id-1";
    sessionCtx.session_id = "session-id-1";
    EXPECT_TRUE(_profiler->SetRumSession(&sessionCtx));

    RumViewValues viewVals = {};
    viewVals.view_id = "view-id-1";
    viewVals.view_name = "HomePage";
    EXPECT_TRUE(_profiler->SetRumView(&viewVals));

    RumViewContext viewCtx;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-id-1");
    EXPECT_EQ(viewCtx.view_name, "HomePage");
}

TEST_F(ProfilerRumContextTest, ClearViewWithNullViewId) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-id-1";
    sessionCtx.session_id = "session-id-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-id-1";
    viewVals.view_name = "HomePage";
    _profiler->SetRumView(&viewVals);

    viewVals.view_id = nullptr;
    viewVals.view_name = nullptr;
    EXPECT_TRUE(_profiler->SetRumView(&viewVals));

    RumViewContext viewCtx;
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));
}

TEST_F(ProfilerRumContextTest, ClearViewWithEmptyViewId) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-id-1";
    sessionCtx.session_id = "session-id-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-id-1";
    viewVals.view_name = "HomePage";
    _profiler->SetRumView(&viewVals);

    viewVals.view_id = "";
    viewVals.view_name = "";
    EXPECT_TRUE(_profiler->SetRumView(&viewVals));

    RumViewContext viewCtx;
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));
}

TEST_F(ProfilerRumContextTest, AppIdSetOnce) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    EXPECT_TRUE(_profiler->SetRumSession(&sessionCtx));

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);

    // Same app_id with different session_id should succeed (session is mutable)
    sessionCtx.session_id = "session-2";
    EXPECT_TRUE(_profiler->SetRumSession(&sessionCtx));

    viewVals.view_id = "view-1b";
    viewVals.view_name = "Page1b";
    _profiler->SetRumView(&viewVals);

    RumViewContext viewCtx;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-1b");
}

TEST_F(ProfilerRumContextTest, DifferentAppIdRejected) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    EXPECT_TRUE(_profiler->SetRumSession(&sessionCtx));

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);

    // Different application_id should be rejected (view unchanged)
    RumSessionContext sessionCtx2 = {};
    sessionCtx2.application_id = "app-2";
    sessionCtx2.session_id = "session-1";
    EXPECT_FALSE(_profiler->SetRumSession(&sessionCtx2));

    RumViewContext viewCtx;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-1");
    EXPECT_EQ(viewCtx.view_name, "Page1");
}

TEST_F(ProfilerRumContextTest, NoActiveViewByDefault) {
    RumViewContext viewCtx;
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));
}

TEST_F(ProfilerRumContextTest, ViewTransitionPattern) {
    // Simulates the real callback pattern: set view -> clear -> set new view
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};

    // Set first view
    viewVals.view_id = "view-1";
    viewVals.view_name = "HomePage";
    _profiler->SetRumView(&viewVals);

    RumViewContext viewCtx;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-1");

    // Clear view (between views)
    viewVals.view_id = nullptr;
    viewVals.view_name = nullptr;
    _profiler->SetRumView(&viewVals);
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));

    // Set second view
    viewVals.view_id = "view-2";
    viewVals.view_name = "SettingsPage";
    _profiler->SetRumView(&viewVals);
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-2");
    EXPECT_EQ(viewCtx.view_name, "SettingsPage");
}

TEST_F(ProfilerRumContextTest, ViewContextCopyIsIndependent) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "HomePage";
    _profiler->SetRumView(&viewVals);

    // Get a copy of the view context
    RumViewContext snapshot;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(snapshot));

    // Update to a new view
    viewVals.view_id = "view-2";
    viewVals.view_name = "SettingsPage";
    _profiler->SetRumView(&viewVals);

    // The snapshot should still hold the old values
    EXPECT_EQ(snapshot.view_id, "view-1");
    EXPECT_EQ(snapshot.view_name, "HomePage");
}

// ---------------------------------------------------------------------------
// Sample RUM view context tests
// ---------------------------------------------------------------------------

TEST(SampleRumViewContextTests, DefaultSampleHasEmptyRumView) {
    HANDLE hThread;
    ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(),
                      ::GetCurrentProcess(), &hThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    auto threadInfo = std::make_shared<ThreadInfo>(::GetCurrentThreadId(), hThread);
    uint64_t frames[] = {0x1000};
    Sample sample(std::chrono::nanoseconds(0), threadInfo, frames, 1);

    const auto& rumView = sample.GetRumViewContext();
    EXPECT_TRUE(rumView.view_id.empty());
    EXPECT_TRUE(rumView.view_name.empty());
}

TEST(SampleRumViewContextTests, SetAndGetRumViewContext) {
    HANDLE hThread;
    ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(),
                      ::GetCurrentProcess(), &hThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    auto threadInfo = std::make_shared<ThreadInfo>(::GetCurrentThreadId(), hThread);
    uint64_t frames[] = {0x1000};
    Sample sample(std::chrono::nanoseconds(0), threadInfo, frames, 1);

    RumViewContext ctx;
    ctx.view_id = "view-abc";
    ctx.view_name = "TestPage";
    sample.SetRumViewContext(std::move(ctx));

    const auto& rumView = sample.GetRumViewContext();
    EXPECT_EQ(rumView.view_id, "view-abc");
    EXPECT_EQ(rumView.view_name, "TestPage");
}

// ---------------------------------------------------------------------------
// ProfileExporter RUM tag and label tests
// ---------------------------------------------------------------------------

class ProfileExporterRumTests : public ::testing::Test {
protected:
    void SetUp() override {
        config = std::make_unique<Configuration>();
        config->SetExportEnabled(false);

        sampleTypes = {
            {"cpu-time", "nanoseconds"},
            {"cpu-samples", "count"}
        };
        Sample::SetValuesCount(sampleTypes.size());

        exporter = std::make_unique<ProfileExporter>(config.get(), sampleTypes);
    }

    void TearDown() override {
        exporter.reset();
        config.reset();
    }

    std::shared_ptr<Sample> CreateTestSample(const char* viewId = nullptr, const char* viewName = nullptr) {
        auto timestamp = std::chrono::nanoseconds(std::chrono::system_clock::now().time_since_epoch());
        HANDLE hThread;
        ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(),
                          ::GetCurrentProcess(), &hThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
        auto threadInfo = std::make_shared<ThreadInfo>(::GetCurrentThreadId(), hThread);
        uint64_t frames[] = {0x1000, 0x2000};
        auto sample = std::make_shared<Sample>(timestamp, threadInfo, frames, 2);
        sample->AddValue(1000000, 0);
        sample->AddValue(1, 1);

        if (viewId != nullptr) {
            RumViewContext rumView;
            rumView.view_id = viewId;
            if (viewName != nullptr) {
                rumView.view_name = viewName;
            }
            sample->SetRumViewContext(std::move(rumView));
        }

        return sample;
    }

    std::unique_ptr<Configuration> config;
    std::vector<SampleValueType> sampleTypes;
    std::unique_ptr<ProfileExporter> exporter;
};

TEST_F(ProfileExporterRumTests, SetRumApplicationId) {
    ASSERT_TRUE(exporter->Initialize());

    exporter->SetRumApplicationId("app-id-123");

    EXPECT_TRUE(exporter->Add(CreateTestSample()));
    EXPECT_TRUE(exporter->Export());
}

TEST_F(ProfileExporterRumTests, AddSampleWithRumViewLabels) {
    ASSERT_TRUE(exporter->Initialize());

    auto sample = CreateTestSample("view-id-1", "HomePage");
    EXPECT_TRUE(exporter->Add(sample));
    EXPECT_TRUE(exporter->Export());
}

TEST_F(ProfileExporterRumTests, AddSampleWithoutRumViewLabels) {
    ASSERT_TRUE(exporter->Initialize());

    auto sample = CreateTestSample();
    EXPECT_TRUE(exporter->Add(sample));
    EXPECT_TRUE(exporter->Export());
}

TEST_F(ProfileExporterRumTests, MixedSamplesWithAndWithoutRum) {
    ASSERT_TRUE(exporter->Initialize());

    EXPECT_TRUE(exporter->Add(CreateTestSample("view-1", "Page1")));
    EXPECT_TRUE(exporter->Add(CreateTestSample()));
    EXPECT_TRUE(exporter->Add(CreateTestSample("view-2", "Page2")));
    EXPECT_TRUE(exporter->Export());
}

TEST_F(ProfileExporterRumTests, MultipleExportsWithRumTags) {
    ASSERT_TRUE(exporter->Initialize());
    exporter->SetRumApplicationId("app-id");

    for (int i = 0; i < 3; i++) {
        std::string viewId = "view-" + std::to_string(i);
        std::string viewName = "Page" + std::to_string(i);
        EXPECT_TRUE(exporter->Add(CreateTestSample(viewId.c_str(), viewName.c_str())));
        EXPECT_TRUE(exporter->Export());
    }
}

// ---------------------------------------------------------------------------
// RumViewRecord struct tests
// ---------------------------------------------------------------------------

TEST(RumViewRecordTests, DefaultConstruction) {
    RumViewRecord record;
    EXPECT_EQ(record.timestamp_ms, 0);
    EXPECT_EQ(record.duration_ms, 0);
    EXPECT_TRUE(record.view_id.empty());
    EXPECT_TRUE(record.view_name.empty());
    for (size_t i = 0; i < MaxViewVitalKind; ++i)
        EXPECT_EQ(record.vitals_ns[i], 0);
}

TEST(RumViewRecordTests, ValueInitialization) {
    RumViewRecord record;
    record.timestamp_ms = 1773058873970;
    record.duration_ms = 2000;
    record.view_id = "view-abc";
    record.view_name = "HomePage";
    record.vitals_ns[static_cast<size_t>(ViewVitalKind::CpuTime)] = 500000;
    record.vitals_ns[static_cast<size_t>(ViewVitalKind::WaitTime)] = 300000;
    EXPECT_EQ(record.timestamp_ms, 1773058873970);
    EXPECT_EQ(record.duration_ms, 2000);
    EXPECT_EQ(record.view_id, "view-abc");
    EXPECT_EQ(record.view_name, "HomePage");
    EXPECT_EQ(record.vitals_ns[static_cast<size_t>(ViewVitalKind::CpuTime)], 500000);
    EXPECT_EQ(record.vitals_ns[static_cast<size_t>(ViewVitalKind::WaitTime)], 300000);
}

// ---------------------------------------------------------------------------
// Profiler ConsumeViewRecords tests
// ---------------------------------------------------------------------------

TEST_F(ProfilerRumContextTest, ConsumeViewRecordsEmptyByDefault) {
    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    EXPECT_TRUE(records.empty());
}

TEST_F(ProfilerRumContextTest, SetAndClearViewProducesOneRecord) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "HomePage";

    auto beforeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    _profiler->SetRumView(&viewVals);
    ::Sleep(50);

    // Clear the view
    viewVals.view_id = "";
    viewVals.view_name = "";
    _profiler->SetRumView(&viewVals);

    auto afterMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].view_id, "view-1");
    EXPECT_EQ(records[0].view_name, "HomePage");
    EXPECT_GE(records[0].timestamp_ms, beforeMs);
    EXPECT_LE(records[0].timestamp_ms, afterMs);
    EXPECT_GE(records[0].duration_ms, 40);  // at least ~50ms minus tolerance
    EXPECT_LE(records[0].duration_ms, afterMs - beforeMs + 50);
}

TEST_F(ProfilerRumContextTest, ConsumeSwapsBufferSecondCallEmpty) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);

    viewVals.view_id = "";
    viewVals.view_name = "";
    _profiler->SetRumView(&viewVals);

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    ASSERT_EQ(records.size(), 1u);

    std::vector<RumViewRecord> records2;
    _profiler->ConsumeViewRecords(records2);
    EXPECT_TRUE(records2.empty());
}

TEST_F(ProfilerRumContextTest, PendingViewProducesNoRecord) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    EXPECT_TRUE(records.empty());
}

TEST_F(ProfilerRumContextTest, ClearWithoutSetProducesNoRecord) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "";
    viewVals.view_name = "";
    _profiler->SetRumView(&viewVals);

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    EXPECT_TRUE(records.empty());
}

TEST_F(ProfilerRumContextTest, ViewTransitionsProduceMultipleRecords) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};

    // View 1
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);
    ::Sleep(20);

    // Clear view 1
    viewVals.view_id = "";
    viewVals.view_name = "";
    _profiler->SetRumView(&viewVals);

    // View 2
    viewVals.view_id = "view-2";
    viewVals.view_name = "Page2";
    _profiler->SetRumView(&viewVals);
    ::Sleep(20);

    // Clear view 2
    viewVals.view_id = "";
    viewVals.view_name = "";
    _profiler->SetRumView(&viewVals);

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].view_id, "view-1");
    EXPECT_EQ(records[0].view_name, "Page1");
    EXPECT_GT(records[0].duration_ms, 0);
    EXPECT_EQ(records[1].view_id, "view-2");
    EXPECT_EQ(records[1].view_name, "Page2");
    EXPECT_GT(records[1].duration_ms, 0);
}

// ---------------------------------------------------------------------------
// JSON serialization tests
// ---------------------------------------------------------------------------

TEST(RumRecordJsonTests, EmptyBothProducesEmptyJson) {
    std::vector<RumViewRecord> views;
    std::vector<RumSessionRecord> sessions;
    auto json = ProfileExporter::SerializeRumRecordsToJson(views, sessions);
    EXPECT_EQ(json, "{\"views\":[],\"sessions\":[]}");
}

TEST(RumRecordJsonTests, ViewsOnlyJson) {
    std::vector<RumViewRecord> views = {
        MakeViewRecord(1773058873970, 2000, "view-1", "HomePage")
    };
    std::vector<RumSessionRecord> sessions;
    auto json = ProfileExporter::SerializeRumRecordsToJson(views, sessions);
    EXPECT_EQ(json,
        "{\"views\":[{\"startClocks\":{\"relative\":0,\"timeStamp\":1773058873970}"
        ",\"duration\":2000"
        ",\"viewId\":\"view-1\""
        ",\"viewName\":\"HomePage\""
        ",\"vitals\":{\"cpuTimeNs\":0,\"waitTimeNs\":0}}]"
        ",\"sessions\":[]}");
}

TEST(RumRecordJsonTests, SessionsOnlyJson) {
    std::vector<RumViewRecord> views;
    std::vector<RumSessionRecord> sessions = {
        {1773058870000, 5000, "session-1"}
    };
    auto json = ProfileExporter::SerializeRumRecordsToJson(views, sessions);
    EXPECT_EQ(json,
        "{\"views\":[]"
        ",\"sessions\":[{\"startClocks\":{\"relative\":0,\"timeStamp\":1773058870000}"
        ",\"duration\":5000"
        ",\"sessionId\":\"session-1\"}]}");
}

TEST(RumRecordJsonTests, MixedViewsAndSessionsJson) {
    std::vector<RumViewRecord> views = {
        MakeViewRecord(1773058873970, 2000, "view-1", "HomePage"),
        MakeViewRecord(1773058876000, 1500, "view-2", "Settings")
    };
    std::vector<RumSessionRecord> sessions = {
        {1773058870000, 8000, "session-1"}
    };
    auto json = ProfileExporter::SerializeRumRecordsToJson(views, sessions);

    EXPECT_NE(json.find("\"viewId\":\"view-1\""), std::string::npos);
    EXPECT_NE(json.find("\"viewId\":\"view-2\""), std::string::npos);
    EXPECT_NE(json.find("\"sessionId\":\"session-1\""), std::string::npos);
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
}

TEST(RumRecordJsonTests, ZeroDuration) {
    std::vector<RumViewRecord> views = {
        MakeViewRecord(1773058873970, 0, "view-1", "Quick")
    };
    std::vector<RumSessionRecord> sessions;
    auto json = ProfileExporter::SerializeRumRecordsToJson(views, sessions);
    EXPECT_NE(json.find("\"duration\":0"), std::string::npos);
}

// ---------------------------------------------------------------------------
// EscapeJsonString tests
// ---------------------------------------------------------------------------

TEST(EscapeJsonStringTests, PlainString) {
    std::ostringstream ss;
    ProfileExporter::EscapeJsonString(ss, "hello world");
    EXPECT_EQ(ss.str(), "hello world");
}

TEST(EscapeJsonStringTests, Backslash) {
    std::ostringstream ss;
    ProfileExporter::EscapeJsonString(ss, R"(path\to\file)");
    EXPECT_EQ(ss.str(), R"(path\\to\\file)");
}

TEST(EscapeJsonStringTests, DoubleQuote) {
    std::ostringstream ss;
    ProfileExporter::EscapeJsonString(ss, R"(say "hi")");
    EXPECT_EQ(ss.str(), R"(say \"hi\")");
}

TEST(EscapeJsonStringTests, ControlCharacters) {
    std::ostringstream ss;
    ProfileExporter::EscapeJsonString(ss, "line1\nline2\ttab");
    EXPECT_EQ(ss.str(), "line1\\nline2\\ttab");
}

TEST(EscapeJsonStringTests, LowControlChar) {
    std::ostringstream ss;
    std::string input(1, '\x01');
    ProfileExporter::EscapeJsonString(ss, input);
    EXPECT_EQ(ss.str(), "\\u0001");
}

TEST(EscapeJsonStringTests, EmptyString) {
    std::ostringstream ss;
    ProfileExporter::EscapeJsonString(ss, "");
    EXPECT_EQ(ss.str(), "");
}

// ---------------------------------------------------------------------------
// RumSessionRecord struct tests
// ---------------------------------------------------------------------------

TEST(RumSessionRecordTests, DefaultConstruction) {
    RumSessionRecord record;
    EXPECT_EQ(record.timestamp_ms, 0);
    EXPECT_EQ(record.duration_ms, 0);
    EXPECT_TRUE(record.session_id.empty());
}

TEST(RumSessionRecordTests, ValueInitialization) {
    RumSessionRecord record{1773058870000, 5000, "session-abc"};
    EXPECT_EQ(record.timestamp_ms, 1773058870000);
    EXPECT_EQ(record.duration_ms, 5000);
    EXPECT_EQ(record.session_id, "session-abc");
}

// ---------------------------------------------------------------------------
// Profiler session tracking tests
// ---------------------------------------------------------------------------

TEST_F(ProfilerRumContextTest, SessionIdCanChange) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    EXPECT_TRUE(_profiler->SetRumSession(&sessionCtx));
    EXPECT_EQ(_profiler->GetCurrentSessionId(), "S1");

    sessionCtx.session_id = "S2";
    EXPECT_TRUE(_profiler->SetRumSession(&sessionCtx));
    EXPECT_EQ(_profiler->GetCurrentSessionId(), "S2");
}

TEST_F(ProfilerRumContextTest, SessionIdChangeCompletesRecord) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";

    auto beforeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    _profiler->SetRumSession(&sessionCtx);
    ::Sleep(50);

    sessionCtx.session_id = "S2";
    _profiler->SetRumSession(&sessionCtx);

    std::vector<RumSessionRecord> records;
    _profiler->ConsumeSessionRecords(records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].session_id, "S1");
    EXPECT_GE(records[0].timestamp_ms, beforeMs);
    EXPECT_GE(records[0].duration_ms, 40);
}

TEST_F(ProfilerRumContextTest, MultipleSessionTransitions) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";

    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);
    ::Sleep(20);

    sessionCtx.session_id = "S2";
    _profiler->SetRumSession(&sessionCtx);
    ::Sleep(20);

    sessionCtx.session_id = "S3";
    _profiler->SetRumSession(&sessionCtx);

    std::vector<RumSessionRecord> records;
    _profiler->ConsumeSessionRecords(records);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].session_id, "S1");
    EXPECT_GT(records[0].duration_ms, 0);
    EXPECT_EQ(records[1].session_id, "S2");
    EXPECT_GT(records[1].duration_ms, 0);
    EXPECT_EQ(_profiler->GetCurrentSessionId(), "S3");
}

TEST_F(ProfilerRumContextTest, ConsumeSessionRecordsEmptyByDefault) {
    std::vector<RumSessionRecord> records;
    _profiler->ConsumeSessionRecords(records);
    EXPECT_TRUE(records.empty());
}

TEST_F(ProfilerRumContextTest, ConsumeSessionRecordsSwapsBuffer) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    sessionCtx.session_id = "S2";
    _profiler->SetRumSession(&sessionCtx);

    std::vector<RumSessionRecord> records;
    _profiler->ConsumeSessionRecords(records);
    ASSERT_EQ(records.size(), 1u);

    std::vector<RumSessionRecord> records2;
    _profiler->ConsumeSessionRecords(records2);
    EXPECT_TRUE(records2.empty());
}

TEST_F(ProfilerRumContextTest, SameSessionIdDoesNotCreateRecord) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);

    viewVals.view_id = "view-2";
    viewVals.view_name = "Page2";
    _profiler->SetRumView(&viewVals);

    std::vector<RumSessionRecord> records;
    _profiler->ConsumeSessionRecords(records);
    EXPECT_TRUE(records.empty());
}

// ---------------------------------------------------------------------------
// View vitals accumulation tests
// ---------------------------------------------------------------------------

TEST_F(ProfilerRumContextTest, VitalsAccumulateDuringActiveView) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "HomePage";
    _profiler->SetRumView(&viewVals);

    EXPECT_TRUE(_profiler->AccumulateViewVitals(ViewVitalKind::WaitTime, 1000));
    EXPECT_TRUE(_profiler->AccumulateViewVitals(ViewVitalKind::CpuTime, 2000));
    EXPECT_TRUE(_profiler->AccumulateViewVitals(ViewVitalKind::WaitTime, 500));
    EXPECT_TRUE(_profiler->AccumulateViewVitals(ViewVitalKind::CpuTime, 3000));

    // End the view
    viewVals.view_id = "";
    viewVals.view_name = "";
    _profiler->SetRumView(&viewVals);

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].vitals_ns[static_cast<size_t>(ViewVitalKind::WaitTime)], 1500);
    EXPECT_EQ(records[0].vitals_ns[static_cast<size_t>(ViewVitalKind::CpuTime)], 5000);
}

TEST_F(ProfilerRumContextTest, VitalsResetOnViewEnd) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};

    // First view with some vitals
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);
    _profiler->AccumulateViewVitals(ViewVitalKind::WaitTime, 100);
    _profiler->AccumulateViewVitals(ViewVitalKind::CpuTime, 200);

    viewVals.view_id = "";
    viewVals.view_name = "";
    _profiler->SetRumView(&viewVals);

    // Second view without any vitals
    viewVals.view_id = "view-2";
    viewVals.view_name = "Page2";
    _profiler->SetRumView(&viewVals);

    viewVals.view_id = "";
    viewVals.view_name = "";
    _profiler->SetRumView(&viewVals);

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].vitals_ns[static_cast<size_t>(ViewVitalKind::WaitTime)], 100);
    EXPECT_EQ(records[0].vitals_ns[static_cast<size_t>(ViewVitalKind::CpuTime)], 200);
    EXPECT_EQ(records[1].vitals_ns[static_cast<size_t>(ViewVitalKind::WaitTime)], 0);
    EXPECT_EQ(records[1].vitals_ns[static_cast<size_t>(ViewVitalKind::CpuTime)], 0);
}

TEST_F(ProfilerRumContextTest, VitalsResetOnNewViewStart) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};

    // Start view and accumulate vitals
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);
    _profiler->AccumulateViewVitals(ViewVitalKind::WaitTime, 1000);
    _profiler->AccumulateViewVitals(ViewVitalKind::CpuTime, 2000);

    // Switch directly to a new view (without clearing)
    viewVals.view_id = "view-2";
    viewVals.view_name = "Page2";
    _profiler->SetRumView(&viewVals);

    // Accumulate for the second view
    _profiler->AccumulateViewVitals(ViewVitalKind::WaitTime, 300);
    _profiler->AccumulateViewVitals(ViewVitalKind::CpuTime, 400);

    viewVals.view_id = "";
    viewVals.view_name = "";
    _profiler->SetRumView(&viewVals);

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].view_id, "view-1");
    EXPECT_EQ(records[0].vitals_ns[static_cast<size_t>(ViewVitalKind::WaitTime)], 1000);
    EXPECT_EQ(records[0].vitals_ns[static_cast<size_t>(ViewVitalKind::CpuTime)], 2000);
    EXPECT_EQ(records[1].view_id, "view-2");
    EXPECT_EQ(records[1].vitals_ns[static_cast<size_t>(ViewVitalKind::WaitTime)], 300);
    EXPECT_EQ(records[1].vitals_ns[static_cast<size_t>(ViewVitalKind::CpuTime)], 400);
}

TEST_F(ProfilerRumContextTest, VitalsRejectOutOfRangeKind) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);

    EXPECT_FALSE(_profiler->AccumulateViewVitals(ViewVitalKind::Unknown, 100));
    EXPECT_FALSE(_profiler->AccumulateViewVitals(static_cast<ViewVitalKind>(255), 100));

    // End view and verify nothing was accumulated from the rejected calls
    viewVals.view_id = "";
    viewVals.view_name = "";
    _profiler->SetRumView(&viewVals);

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    ASSERT_EQ(records.size(), 1u);
    for (size_t i = 0; i < MaxViewVitalKind; ++i)
        EXPECT_EQ(records[0].vitals_ns[i], 0);
}

TEST_F(ProfilerRumContextTest, VitalsZeroWhenNoAccumulation) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "session-1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);

    // End the view without any AccumulateViewVitals calls
    viewVals.view_id = "";
    viewVals.view_name = "";
    _profiler->SetRumView(&viewVals);

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    ASSERT_EQ(records.size(), 1u);
    for (size_t i = 0; i < MaxViewVitalKind; ++i)
        EXPECT_EQ(records[0].vitals_ns[i], 0);
}

// ---------------------------------------------------------------------------
// JSON vitals serialization tests
// ---------------------------------------------------------------------------

TEST(RumRecordJsonTests, VitalsInJson) {
    std::vector<RumViewRecord> views = {
        MakeViewRecord(1000, 500, "view-1", "Home", 123456789, 987654321)
    };
    std::vector<RumSessionRecord> sessions;
    auto json = ProfileExporter::SerializeRumRecordsToJson(views, sessions);
    EXPECT_NE(json.find("\"vitals\":{\"cpuTimeNs\":123456789,\"waitTimeNs\":987654321}"), std::string::npos);
}

TEST(RumRecordJsonTests, VitalsZeroInJson) {
    std::vector<RumViewRecord> views = {
        MakeViewRecord(1000, 500, "view-1", "Home")
    };
    std::vector<RumSessionRecord> sessions;
    auto json = ProfileExporter::SerializeRumRecordsToJson(views, sessions);
    EXPECT_NE(json.find("\"vitals\":{\"cpuTimeNs\":0,\"waitTimeNs\":0}"), std::string::npos);
}

TEST(RumRecordJsonTests, MultipleViewsWithDifferentVitals) {
    std::vector<RumViewRecord> views = {
        MakeViewRecord(1000, 500, "view-1", "Home", 100, 200),
        MakeViewRecord(2000, 300, "view-2", "Settings", 400, 500)
    };
    std::vector<RumSessionRecord> sessions;
    auto json = ProfileExporter::SerializeRumRecordsToJson(views, sessions);
    EXPECT_NE(json.find("\"vitals\":{\"cpuTimeNs\":100,\"waitTimeNs\":200}"), std::string::npos);
    EXPECT_NE(json.find("\"vitals\":{\"cpuTimeNs\":400,\"waitTimeNs\":500}"), std::string::npos);
}

// Note: the RUM records JSON built by SerializeRumRecordsToJson is also the
// payload passed to libdatadog through optional_internal_metadata_json. The
// tests above (RumRecordJsonTests) therefore double-cover that payload's
// shape; no dedicated internal-metadata suite is needed.

// ---------------------------------------------------------------------------
// ClearRumContext tests
// ---------------------------------------------------------------------------

TEST_F(ProfilerRumContextTest, ClearRumContextCompletesRecords) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);

    ::Sleep(50);

    _profiler->ClearRumContext();

    std::vector<RumViewRecord> viewRecords;
    _profiler->ConsumeViewRecords(viewRecords);
    ASSERT_EQ(viewRecords.size(), 1u);
    EXPECT_EQ(viewRecords[0].view_id, "view-1");
    EXPECT_GE(viewRecords[0].duration_ms, 40);

    std::vector<RumSessionRecord> sessionRecords;
    _profiler->ConsumeSessionRecords(sessionRecords);
    ASSERT_EQ(sessionRecords.size(), 1u);
    EXPECT_EQ(sessionRecords[0].session_id, "S1");

    // Verify state is clean
    RumViewContext viewCtx;
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_TRUE(_profiler->GetCurrentSessionId().empty());
}

TEST_F(ProfilerRumContextTest, ClearRumContextClearsAppId) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    EXPECT_TRUE(_profiler->SetRumSession(&sessionCtx));

    _profiler->ClearRumContext();

    // After clearing, a different app_id should be accepted
    RumSessionContext sessionCtx2 = {};
    sessionCtx2.application_id = "app-2";
    sessionCtx2.session_id = "S2";
    EXPECT_TRUE(_profiler->SetRumSession(&sessionCtx2));
}

TEST_F(ProfilerRumContextTest, ClearRumContextAfterOnlySession) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    _profiler->ClearRumContext();

    std::vector<RumSessionRecord> sessionRecords;
    _profiler->ConsumeSessionRecords(sessionRecords);
    ASSERT_EQ(sessionRecords.size(), 1u);
    EXPECT_EQ(sessionRecords[0].session_id, "S1");

    std::vector<RumViewRecord> viewRecords;
    _profiler->ConsumeViewRecords(viewRecords);
    EXPECT_TRUE(viewRecords.empty());
}

// ---------------------------------------------------------------------------
// Consistency tests
// ---------------------------------------------------------------------------

TEST_F(ProfilerRumContextTest, SetRumViewWithoutSessionReturnsFalse) {
    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    EXPECT_FALSE(_profiler->SetRumView(&viewVals));

    RumViewContext viewCtx;
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));
}

TEST_F(ProfilerRumContextTest, EnterViewWithoutSessionReturnsFalse) {
    EXPECT_FALSE(_profiler->EnterView("Page1"));

    RumViewContext viewCtx;
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));
}

TEST_F(ProfilerRumContextTest, SetRumViewSucceedsAfterSession) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    EXPECT_TRUE(_profiler->SetRumSession(&sessionCtx));

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    EXPECT_TRUE(_profiler->SetRumView(&viewVals));

    RumViewContext viewCtx;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-1");
}

TEST_F(ProfilerRumContextTest, EnterViewSucceedsAfterSession) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    EXPECT_TRUE(_profiler->SetRumSession(&sessionCtx));

    EXPECT_TRUE(_profiler->EnterView("Page1"));

    RumViewContext viewCtx;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_FALSE(viewCtx.view_id.empty());
    EXPECT_EQ(viewCtx.view_name, "Page1");
}

TEST_F(ProfilerRumContextTest, ViewUpdatesBetweenConsecutiveSetRumViewCalls) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);

    RumViewContext viewCtx;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-1");
    EXPECT_EQ(viewCtx.view_name, "Page1");

    viewVals.view_id = "view-2";
    viewVals.view_name = "Page2";
    _profiler->SetRumView(&viewVals);

    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-2");
    EXPECT_EQ(viewCtx.view_name, "Page2");
}

TEST_F(ProfilerRumContextTest, ViewUpdatesBetweenConsecutiveEnterViewCalls) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    _profiler->EnterView("Page1");
    RumViewContext viewCtx1;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx1));
    EXPECT_EQ(viewCtx1.view_name, "Page1");

    _profiler->EnterView("Page2");
    RumViewContext viewCtx2;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx2));
    EXPECT_EQ(viewCtx2.view_name, "Page2");
    EXPECT_NE(viewCtx1.view_id, viewCtx2.view_id);
}

TEST_F(ProfilerRumContextTest, StackedSetRumViewCompletesFirstView) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);
    ::Sleep(50);

    viewVals.view_id = "view-2";
    viewVals.view_name = "Page2";
    _profiler->SetRumView(&viewVals);
    ::Sleep(50);

    viewVals.view_id = "";
    viewVals.view_name = "";
    _profiler->SetRumView(&viewVals);

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].view_id, "view-1");
    EXPECT_EQ(records[0].view_name, "Page1");
    EXPECT_GE(records[0].duration_ms, 40);
    EXPECT_EQ(records[1].view_id, "view-2");
    EXPECT_EQ(records[1].view_name, "Page2");
    EXPECT_GE(records[1].duration_ms, 40);
}

TEST_F(ProfilerRumContextTest, StackedEnterViewCompletesFirstView) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    _profiler->EnterView("Page1");
    ::Sleep(50);

    _profiler->EnterView("Page2");
    ::Sleep(50);

    _profiler->LeaveCurrentView();

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].view_name, "Page1");
    EXPECT_GE(records[0].duration_ms, 40);
    EXPECT_EQ(records[1].view_name, "Page2");
    EXPECT_GE(records[1].duration_ms, 40);
    EXPECT_NE(records[0].view_id, records[1].view_id);
}

TEST_F(ProfilerRumContextTest, SessionChangeDoesNotAffectActiveView) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);

    sessionCtx.session_id = "S2";
    _profiler->SetRumSession(&sessionCtx);

    RumViewContext viewCtx;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-1");
    EXPECT_EQ(viewCtx.view_name, "Page1");
}

// ---------------------------------------------------------------------------
// View lifecycle tests
// ---------------------------------------------------------------------------

TEST_F(ProfilerRumContextTest, ViewEndsWithNullViewId) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);

    viewVals.view_id = nullptr;
    viewVals.view_name = nullptr;
    EXPECT_TRUE(_profiler->SetRumView(&viewVals));

    RumViewContext viewCtx;
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].view_id, "view-1");
}

TEST_F(ProfilerRumContextTest, ViewEndsWithEmptyViewId) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    _profiler->SetRumView(&viewVals);

    viewVals.view_id = "";
    viewVals.view_name = "";
    EXPECT_TRUE(_profiler->SetRumView(&viewVals));

    RumViewContext viewCtx;
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].view_id, "view-1");
}

TEST_F(ProfilerRumContextTest, LeaveCurrentViewCompletesView) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    _profiler->EnterView("Page1");
    ::Sleep(50);

    EXPECT_TRUE(_profiler->LeaveCurrentView());

    RumViewContext viewCtx;
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));

    std::vector<RumViewRecord> records;
    _profiler->ConsumeViewRecords(records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].view_name, "Page1");
    EXPECT_GE(records[0].duration_ms, 40);
}

TEST_F(ProfilerRumContextTest, LeaveCurrentViewWithNoViewReturnsFalse) {
    EXPECT_FALSE(_profiler->LeaveCurrentView());
}

TEST_F(ProfilerRumContextTest, SetRumViewAfterClearRumContextReturnsFalse) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    _profiler->ClearRumContext();

    RumViewValues viewVals = {};
    viewVals.view_id = "view-1";
    viewVals.view_name = "Page1";
    EXPECT_FALSE(_profiler->SetRumView(&viewVals));
}

TEST_F(ProfilerRumContextTest, EnterViewAfterClearRumContextReturnsFalse) {
    RumSessionContext sessionCtx = {};
    sessionCtx.application_id = "app-1";
    sessionCtx.session_id = "S1";
    _profiler->SetRumSession(&sessionCtx);

    _profiler->ClearRumContext();

    EXPECT_FALSE(_profiler->EnterView("Page1"));
}
