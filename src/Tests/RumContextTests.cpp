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
#include <gtest/gtest.h>
#include <thread>

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

TEST_F(ProfilerRumContextTest, UpdateRumContextReturnsfalseOnNull) {
    EXPECT_FALSE(_profiler->UpdateRumContext(nullptr));
}

TEST_F(ProfilerRumContextTest, SetViewContextAndReadBack) {
    RumContextValues ctx = {};
    ctx.application_id = "app-id-1";
    ctx.session_id = "session-id-1";
    ctx.view_id = "view-id-1";
    ctx.view_name = "HomePage";

    EXPECT_TRUE(_profiler->UpdateRumContext(&ctx));

    RumViewContext viewCtx;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-id-1");
    EXPECT_EQ(viewCtx.view_name, "HomePage");
}

TEST_F(ProfilerRumContextTest, ClearViewWithNullViewId) {
    // Set a view first
    RumContextValues ctx = {};
    ctx.application_id = "app-id-1";
    ctx.session_id = "session-id-1";
    ctx.view_id = "view-id-1";
    ctx.view_name = "HomePage";
    _profiler->UpdateRumContext(&ctx);

    // Clear view by passing null view_id
    RumContextValues clearCtx = {};
    clearCtx.application_id = "app-id-1";
    clearCtx.session_id = "session-id-1";
    clearCtx.view_id = nullptr;
    clearCtx.view_name = nullptr;
    EXPECT_TRUE(_profiler->UpdateRumContext(&clearCtx));

    RumViewContext viewCtx;
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));
}

TEST_F(ProfilerRumContextTest, ClearViewWithEmptyViewId) {
    // Set a view first
    RumContextValues ctx = {};
    ctx.application_id = "app-id-1";
    ctx.session_id = "session-id-1";
    ctx.view_id = "view-id-1";
    ctx.view_name = "HomePage";
    _profiler->UpdateRumContext(&ctx);

    // Clear view by passing empty view_id
    RumContextValues clearCtx = {};
    clearCtx.application_id = "app-id-1";
    clearCtx.session_id = "session-id-1";
    clearCtx.view_id = "";
    clearCtx.view_name = "";
    EXPECT_TRUE(_profiler->UpdateRumContext(&clearCtx));

    RumViewContext viewCtx;
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));
}

TEST_F(ProfilerRumContextTest, AppIdsSetOnce) {
    // Set app IDs with first call
    RumContextValues ctx1 = {};
    ctx1.application_id = "app-1";
    ctx1.session_id = "session-1";
    ctx1.view_id = "view-1";
    ctx1.view_name = "Page1";
    EXPECT_TRUE(_profiler->UpdateRumContext(&ctx1));

    // Second call with same app IDs should succeed
    RumContextValues ctx1b = {};
    ctx1b.application_id = "app-1";
    ctx1b.session_id = "session-1";
    ctx1b.view_id = "view-1b";
    ctx1b.view_name = "Page1b";
    EXPECT_TRUE(_profiler->UpdateRumContext(&ctx1b));

    RumViewContext viewCtx;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-1b");
}

TEST_F(ProfilerRumContextTest, DifferentAppIdsRejected) {
    // Set app IDs with first call
    RumContextValues ctx1 = {};
    ctx1.application_id = "app-1";
    ctx1.session_id = "session-1";
    ctx1.view_id = "view-1";
    ctx1.view_name = "Page1";
    EXPECT_TRUE(_profiler->UpdateRumContext(&ctx1));

    // Second call with different app IDs should be rejected
    RumContextValues ctx2 = {};
    ctx2.application_id = "app-2";
    ctx2.session_id = "session-2";
    ctx2.view_id = "view-2";
    ctx2.view_name = "Page2";
    EXPECT_FALSE(_profiler->UpdateRumContext(&ctx2));

    // View should still hold the first values (rejected call had no effect)
    RumViewContext viewCtx;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-1");
    EXPECT_EQ(viewCtx.view_name, "Page1");
}

TEST_F(ProfilerRumContextTest, DifferentSessionIdOnlyRejected) {
    RumContextValues ctx1 = {};
    ctx1.application_id = "app-1";
    ctx1.session_id = "session-1";
    ctx1.view_id = "view-1";
    ctx1.view_name = "Page1";
    EXPECT_TRUE(_profiler->UpdateRumContext(&ctx1));

    // Same app ID but different session ID should be rejected
    RumContextValues ctx2 = {};
    ctx2.application_id = "app-1";
    ctx2.session_id = "session-OTHER";
    ctx2.view_id = "view-2";
    ctx2.view_name = "Page2";
    EXPECT_FALSE(_profiler->UpdateRumContext(&ctx2));
}

TEST_F(ProfilerRumContextTest, NoActiveViewByDefault) {
    RumViewContext viewCtx;
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));
}

TEST_F(ProfilerRumContextTest, ViewTransitionPattern) {
    // Simulates the real callback pattern: set view -> clear -> set new view
    RumContextValues ctx = {};
    ctx.application_id = "app-1";
    ctx.session_id = "session-1";

    // Set first view
    ctx.view_id = "view-1";
    ctx.view_name = "HomePage";
    _profiler->UpdateRumContext(&ctx);

    RumViewContext viewCtx;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-1");

    // Clear view (between views)
    ctx.view_id = nullptr;
    ctx.view_name = nullptr;
    _profiler->UpdateRumContext(&ctx);
    EXPECT_FALSE(_profiler->GetCurrentViewContext(viewCtx));

    // Set second view
    ctx.view_id = "view-2";
    ctx.view_name = "SettingsPage";
    _profiler->UpdateRumContext(&ctx);
    EXPECT_TRUE(_profiler->GetCurrentViewContext(viewCtx));
    EXPECT_EQ(viewCtx.view_id, "view-2");
    EXPECT_EQ(viewCtx.view_name, "SettingsPage");
}

TEST_F(ProfilerRumContextTest, ViewContextCopyIsIndependent) {
    RumContextValues ctx = {};
    ctx.application_id = "app-1";
    ctx.session_id = "session-1";
    ctx.view_id = "view-1";
    ctx.view_name = "HomePage";
    _profiler->UpdateRumContext(&ctx);

    // Get a copy of the view context
    RumViewContext snapshot;
    EXPECT_TRUE(_profiler->GetCurrentViewContext(snapshot));

    // Update to a new view
    ctx.view_id = "view-2";
    ctx.view_name = "SettingsPage";
    _profiler->UpdateRumContext(&ctx);

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

TEST_F(ProfileExporterRumTests, SetRumApplicationTags) {
    ASSERT_TRUE(exporter->Initialize());

    exporter->SetRumApplicationTags("app-id-123", "session-id-456");

    // Export a sample; the tags should be included in the export.
    // Since export is disabled, this mainly verifies no crash.
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
    exporter->SetRumApplicationTags("app-id", "session-id");

    for (int i = 0; i < 3; i++) {
        std::string viewId = "view-" + std::to_string(i);
        std::string viewName = "Page" + std::to_string(i);
        EXPECT_TRUE(exporter->Add(CreateTestSample(viewId.c_str(), viewName.c_str())));
        EXPECT_TRUE(exporter->Export());
    }
}
