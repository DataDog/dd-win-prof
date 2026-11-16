#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include "internal/dd-win-rum-private.h"

using clock_type = std::chrono::steady_clock;
__declspec(noinline) uint64_t work(uint64_t value, unsigned depth) {
  if (depth) return work(value + depth, depth - 1) ^ value;
  for (unsigned i = 0; i < 10000; ++i) value = value * 6364136223846793005ULL + 1;
  return value;
}
int main(int argc, char** argv) {
  if (argc != 4) return 2;
  const bool enabled = std::atoi(argv[1]) != 0;
  const bool zstd = std::atoi(argv[2]) != 0;
  const std::filesystem::path out = argv[3];
  std::filesystem::create_directories(out);
  std::ofstream memory(out / "memory.csv");
  memory << "elapsed_ms,phase,working_set_bytes,private_committed_bytes,callbacks,queued_bytes\n";
  std::atomic<size_t> callbacks{0}, errors{0}, final{0}, queued{0};
  std::mutex mutex;
  std::vector<uint8_t> pending;
  ProfilingDataCallback callback = [&](ProfilingData data) {
    if (data.result != 0) { ++errors; return; }
    if (data.isLast) ++final;
    const unsigned char zmagic[] = {0x28, 0xb5, 0x2f, 0xfd};
    const unsigned char lmagic[] = {0x04, 0x22, 0x4d, 0x18};
    if (data.pprof.size() < 4 || std::memcmp(data.pprof.data(), zstd ? zmagic : lmagic, 4)) { ++errors; return; }
    const size_t sequence = callbacks.fetch_add(1);
    std::ofstream profile(out / (std::to_string(sequence) + (zstd ? ".zst.pprof" : ".lz4.pprof")), std::ios::binary);
    profile.write(reinterpret_cast<const char*>(data.pprof.data()), data.pprof.size());
    std::lock_guard<std::mutex> lock(mutex);
    pending = std::move(data.pprof);
    queued = pending.size();
  };
#ifndef SMALL_APP_NO_PROFILER
  if (enabled) {
    if (!SetupProfilingDataCallback(callback)) return 3;
    ProfilerConfig config{};
    config.size = sizeof(config);
    config.noEnvVars = true;
    config.serviceName = "prof-16455-small-app";
    config.serviceEnvironment = "local";
    config.serviceVersion = "experiment";
    config.cpuWallTimeSamplingPeriodNs = 20000000;
    config.uploadIntervalSeconds = 5;
    config.symbolizeCallstacks = false;
    if (!SetupProfiler(&config) || !StartProfiler()) return 4;
  }
#endif
  const auto start = clock_type::now();
  auto last = start;
  uint64_t result = 1;
  while (clock_type::now() - start < std::chrono::seconds(45)) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start).count();
    const char* phase = elapsed < 15000 ? "small" : (elapsed < 30000 ? "churn" : "recovery");
    if (elapsed >= 15000 && elapsed < 30000) {
      std::thread t([&] { uint64_t n = 1; for (int i = 0; i < 100; ++i) n = work(n, static_cast<unsigned>(i % 64)); result ^= n; });
      t.join();
    } else { result = work(result, 8); }
    if (clock_type::now() - last >= std::chrono::milliseconds(100)) {
      PROCESS_MEMORY_COUNTERS_EX usage{};
      if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&usage), sizeof(usage))) return 5;
      memory << elapsed << ',' << phase << ',' << usage.WorkingSetSize << ',' << usage.PrivateUsage << ',' << callbacks << ',' << queued << '\n';
      last = clock_type::now();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
#ifndef SMALL_APP_NO_PROFILER
  if (enabled) {
    StopProfiler();
    if (!SetupProfilingDataCallback({})) return 6;
    if (!callbacks || final != 1 || errors) return 7;
  }
#endif
  std::printf("small-app enabled=%d callbacks=%zu final=%zu errors=%zu retained=%zu checksum=%llu\n", enabled, callbacks.load(), final.load(), errors.load(), queued.load(), static_cast<unsigned long long>(result));
  return 0;
}
