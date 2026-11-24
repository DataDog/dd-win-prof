// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../dd-win-prof/Configuration.h"
#include "../dd-win-prof/ProfileExporter.h"
#include "../dd-win-prof/SamplesCollector.h"
#include "pch.h"

namespace {
struct CallbackCounts {
  std::atomic<int> calls{0};
  std::atomic<int> finalCalls{0};
};

void DD_WIN_PROF_CALL CountCallback(const ProfilingData* data, void* userData) {
  auto& counts = *static_cast<CallbackCounts*>(userData);
  ++counts.calls;
  if (data->is_last != 0) {
    ++counts.finalCalls;
  }
}

class CountingProvider : public ISamplesProvider {
 public:
  size_t MoveSamples(std::vector<Sample>& destination) override {
    ++moveCalls;
    destination.clear();
    return 0;
  }

  const char* GetName() override { return "counting-provider"; }

  std::atomic<int> moveCalls{0};
};
}  // namespace

class SamplesCollectorTests : public ::testing::Test {
 protected:
  void SetUp() override {
    _previousValuesCount = Sample::ValuesCount;
    _configuration.ResetToDefaults();
    // no periodic export during a test: only the final export reaches the callback
    _configuration.SetUploadInterval(std::chrono::hours(1));
    Sample::SetValuesCount(_sampleTypes.size());
    _exporter = std::make_unique<ProfileExporter>(
        &_configuration, _sampleTypes, nullptr, CountCallback, &_callbackCounts
    );
    ASSERT_TRUE(_exporter->Initialize());
  }

  void TearDown() override {
    _exporter->Cleanup();
    _exporter.reset();
    Sample::SetValuesCount(_previousValuesCount);
  }

  static bool HasJoinableThread(SamplesCollector& collector) {
    return collector._workerThread.joinable() || collector._exporterThread.joinable();
  }

  size_t _previousValuesCount = 0;
  Configuration _configuration;
  std::vector<SampleValueType> _sampleTypes{{"samples", "count"}};
  CallbackCounts _callbackCounts;
  std::unique_ptr<ProfileExporter> _exporter;
};

TEST_F(SamplesCollectorTests, SecondWorkerFailureRollsBackFirstWorker) {
  std::mutex eventsLock;
  std::vector<std::string> events;
  auto record = [&](const char* event) {
    std::lock_guard lock(eventsLock);
    events.push_back(event);
  };

  int createCalls = 0;
  std::promise<void> firstWorkerStarted;
  SamplesCollector::ThreadFactory threadFactory = [&](std::function<void()> routine) {
    if (++createCalls == 2) {
      record("create exporter: failed");
      throw std::runtime_error("injected exporter thread creation failure");
    }
    record("create worker");
    std::thread thread([&, routine = std::move(routine)] {
      record("worker started");
      firstWorkerStarted.set_value();
      routine();
      record("worker returned");
    });
    // the worker runs before the second creation fails
    firstWorkerStarted.get_future().wait();
    return thread;
  };

  CountingProvider provider;
  SamplesCollector collector(
      &_configuration, _exporter.get(), std::move(threadFactory)
  );
  collector.Register(&provider);

  EXPECT_THROW(collector.Start(), std::runtime_error);
  EXPECT_EQ(
      events,
      (std::vector<std::string>{
          "create worker",
          "worker started",
          "create exporter: failed",
          "worker returned"
      })
  );
  EXPECT_FALSE(HasJoinableThread(collector));
  EXPECT_EQ(provider.moveCalls, 0);
  EXPECT_EQ(_callbackCounts.calls, 0);

  // a failed collector stays stopped
  EXPECT_EQ(collector.Stop(), std::nullopt);
  EXPECT_EQ(collector.Stop(), std::nullopt);
  collector.Start();
  EXPECT_EQ(createCalls, 2);
  EXPECT_EQ(provider.moveCalls, 0);
  EXPECT_EQ(_callbackCounts.calls, 0);
}

TEST_F(SamplesCollectorTests, FreshCollectorStartsAfterFailedStart) {
  {
    SamplesCollector failed(&_configuration, _exporter.get(), [](auto) -> std::thread {
      throw std::runtime_error("injected thread creation failure");
    });
    EXPECT_THROW(failed.Start(), std::runtime_error);
  }

  CountingProvider provider;
  SamplesCollector collector(&_configuration, _exporter.get());
  collector.Register(&provider);
  collector.Start();
  EXPECT_EQ(collector.Stop(), FinalExportStatus::Delivered);

  EXPECT_FALSE(HasJoinableThread(collector));
  EXPECT_GE(provider.moveCalls, 1);
  EXPECT_EQ(_callbackCounts.calls, 1);
  EXPECT_EQ(_callbackCounts.finalCalls, 1);
}

TEST_F(SamplesCollectorTests, StopBeforeStartIsIdempotentAndPreventsStart) {
  CountingProvider provider;
  SamplesCollector collector(&_configuration, _exporter.get());
  collector.Register(&provider);

  EXPECT_EQ(collector.Stop(), std::nullopt);
  EXPECT_EQ(collector.Stop(), std::nullopt);
  collector.Start();

  EXPECT_FALSE(HasJoinableThread(collector));
  EXPECT_EQ(provider.moveCalls, 0);
  EXPECT_EQ(_callbackCounts.calls, 0);
}

TEST_F(SamplesCollectorTests, FirstStopExportsOnceAndLaterCallsAreIgnored) {
  CountingProvider provider;
  SamplesCollector collector(&_configuration, _exporter.get());
  collector.Register(&provider);

  collector.Start();
  collector.Start();
  EXPECT_EQ(collector.Stop(), FinalExportStatus::Delivered);
  EXPECT_FALSE(HasJoinableThread(collector));
  EXPECT_EQ(_callbackCounts.calls, 1);
  EXPECT_EQ(_callbackCounts.finalCalls, 1);
  const int moveCallsAfterStop = provider.moveCalls;
  EXPECT_GE(moveCallsAfterStop, 1);

  EXPECT_EQ(collector.Stop(), std::nullopt);
  collector.Start();
  EXPECT_EQ(collector.Stop(), std::nullopt);
  EXPECT_FALSE(HasJoinableThread(collector));
  EXPECT_EQ(provider.moveCalls, moveCallsAfterStop);
  EXPECT_EQ(_callbackCounts.calls, 1);
}

TEST_F(SamplesCollectorTests, ShutdownStateDoesNotLeakToFreshCollector) {
  {
    SamplesCollector first(&_configuration, _exporter.get());
    first.Start();
    first.SignalShutdown();
    EXPECT_TRUE(first.IsShutdownReceived());
    EXPECT_EQ(first.Stop(true), std::nullopt);
  }
  EXPECT_EQ(_callbackCounts.calls, 0);

  SamplesCollector second(&_configuration, _exporter.get());
  EXPECT_FALSE(second.IsShutdownReceived());
  second.Start();
  EXPECT_EQ(second.Stop(), FinalExportStatus::Delivered);
  EXPECT_EQ(_callbackCounts.calls, 1);
  EXPECT_EQ(_callbackCounts.finalCalls, 1);
}

TEST_F(SamplesCollectorTests, DestructionJoinsWorkersWithoutFinalExport) {
  std::atomic<int> runningWorkers{0};
  {
    SamplesCollector collector(
        &_configuration, _exporter.get(), [&](std::function<void()> routine) {
          return std::thread([&, routine = std::move(routine)] {
            ++runningWorkers;
            routine();
            --runningWorkers;
          });
        }
    );
    collector.Start();
  }

  EXPECT_EQ(runningWorkers, 0);
  EXPECT_EQ(_callbackCounts.calls, 0);
}
