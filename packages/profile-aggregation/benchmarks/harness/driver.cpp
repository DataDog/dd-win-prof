// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
// One workload driver, compiled unchanged against either supported public API.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#endif
#if defined(PA_BENCH_V19) || defined(PA_BENCH_V47)
#include <datadog/profiling.h>
#else
#include "profile_aggregation.h"
#endif

#ifdef _WIN32
static uint64_t clock_frequency() {
  static const uint64_t frequency = [] { LARGE_INTEGER f; if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) std::abort(); return uint64_t(f.QuadPart); }();
  return frequency;
}
#endif
static uint64_t tick() {
#ifdef _WIN32
  const uint64_t frequency = clock_frequency();
  LARGE_INTEGER t; QueryPerformanceCounter(&t);
  // Split to avoid overflowing an absolute QPC tick count * 1e9.
  const auto n = uint64_t(t.QuadPart);
  return n / frequency * 1000000000ULL + n % frequency * 1000000000ULL / frequency;
#else
  timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t);
  return uint64_t(t.tv_sec) * 1000000000ULL + uint64_t(t.tv_nsec);
#endif
}
#ifdef PA_BENCH_COMPRESSION_HOOKS
#include "compression_hooks.h"
static uint64_t compression_ns = 0, compression_calls = 0, protobuf_bytes = 0;
extern "C" uint64_t pa_bench_compression_begin() { return tick(); }
extern "C" void pa_bench_compression_end(uint64_t start) { compression_ns += tick() - start; ++compression_calls; }
extern "C" void pa_bench_serialized_bytes(uint64_t bytes) { protobuf_bytes = bytes; }
#endif
static void require(bool b, const char* message) { if (!b) throw std::runtime_error(message); }
struct Memory { std::string current_ws = "NA", peak_ws = "NA", private_commit = "NA", peak_process_commit = "NA"; };
static Memory memory() {
  Memory m;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX c{}; c.cb = sizeof(c);
  require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof(c)) != 0, "GetProcessMemoryInfo");
  m.current_ws = std::to_string(c.WorkingSetSize); m.peak_ws = std::to_string(c.PeakWorkingSetSize);
  m.private_commit = std::to_string(c.PrivateUsage); m.peak_process_commit = std::to_string(c.PeakPagefileUsage);
#else
  rusage r{}; getrusage(RUSAGE_SELF, &r);
#ifdef __APPLE__
  m.peak_ws = std::to_string(r.ru_maxrss);
  mach_task_basic_info_data_t info{}; mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &n) == KERN_SUCCESS)
    m.current_ws = std::to_string(info.resident_size);
#else
  m.peak_ws = std::to_string(uint64_t(r.ru_maxrss) * 1024);
#endif
#endif
  return m;
}
struct Event { size_t cycle, sample; const char* phase; size_t bytes, live, peak, highwater_growth; bool allocation; };
struct Allocator {
  bool enabled = false;
  size_t allocations = 0, bytes = 0, frees = 0, live = 0, peak = 0, cycle = 0, sample = 0;
  const char* phase = "setup";
  std::vector<Event> events;
  size_t dropped_events = 0;
  void event(size_t n, size_t growth, bool allocation) noexcept {
    if(events.size() < events.capacity()) events.push_back({cycle,sample,phase,n,live,peak,growth,allocation});
    else ++dropped_events; // Never allocate or throw through a production C callback.
  }
  static void* alloc(void* context, size_t n) {
    auto& a = *static_cast<Allocator*>(context); void* p = std::malloc(n);
    if (p) { ++a.allocations; a.bytes += n; a.live += n; auto previous_peak=a.peak; a.peak = std::max(a.peak, a.live);
      a.event(n,a.peak-previous_peak,true); }
    return p;
  }
  static void release(void* context, void* p, size_t n) {
    auto& a = *static_cast<Allocator*>(context); a.live -= n; ++a.frees;
    a.event(n,0,false); std::free(p);
  }
};
#if defined(PA_BENCH_V19) || defined(PA_BENCH_V47)
// Only public result unions and API-owned opaque handles are used.
template<class R, class Tag> static auto unwrap(R r, Tag success) {
  if (r.tag != success) { ddog_Error_drop(&r.err); throw std::runtime_error("libdatadog public API error"); }
  return r.ok;
}
static void check(ddog_VoidResult r) { if (r.tag != DDOG_VOID_RESULT_OK) { ddog_Error_drop(&r.err); throw std::runtime_error("libdatadog sample error"); } }
using String = ddog_prof_StringId; using Function = ddog_prof_FunctionId; using Location = ddog_prof_LocationId;
using Stack = ddog_prof_StackTraceId; using Labels = ddog_prof_LabelSetId;
struct Backend {
  ddog_prof_Profile p{}; ddog_prof_EncodedProfile encoded{}; bool has_output = false;
  String empty = ddog_prof_Profile_interned_empty_string();
  explicit Backend(Allocator&) {
#ifdef PA_BENCH_V47
    const ddog_prof_SampleType types[] = {DDOG_PROF_SAMPLE_TYPE_CPU_TIME, DDOG_PROF_SAMPLE_TYPE_WALL_TIME};
#else
    ddog_prof_ValueType types[] = {{{"cpu-time",8},{"nanoseconds",11}},{{"wall-time",9},{"nanoseconds",11}}};
#endif
    ddog_prof_Period period{types[0],10000000};
    p = unwrap(ddog_prof_Profile_new({types,2}, &period), DDOG_PROF_PROFILE_NEW_RESULT_OK);
  }
  String string(const char* s) { return unwrap(ddog_prof_Profile_intern_string(&p, {s,std::strlen(s)}), DDOG_PROF_STRING_ID_RESULT_OK_GENERATIONAL_ID_STRING_ID); }
  Function function() { return unwrap(ddog_prof_Profile_intern_function(&p,empty,empty,empty), DDOG_PROF_FUNCTION_ID_RESULT_OK_GENERATIONAL_ID_FUNCTION_ID); }
  Location location(Function f, uint64_t address) { return unwrap(ddog_prof_Profile_intern_location(&p,f,address,0), DDOG_PROF_LOCATION_ID_RESULT_OK_GENERATIONAL_ID_LOCATION_ID); }
  Stack stack(const Location* ids, size_t n) { return unwrap(ddog_prof_Profile_intern_stacktrace(&p,{ids,n}), DDOG_PROF_STACK_TRACE_ID_RESULT_OK_GENERATIONAL_ID_STACK_TRACE_ID); }
  Labels labels() { ddog_prof_LabelId dummy{}; return unwrap(ddog_prof_Profile_intern_labelset(&p,{&dummy,0}), DDOG_PROF_LABEL_SET_ID_RESULT_OK_GENERATIONAL_ID_LABEL_SET_ID); }
  Labels labels(String key, String value) { auto l = unwrap(ddog_prof_Profile_intern_label_str(&p,key,value), DDOG_PROF_LABEL_ID_RESULT_OK_GENERATIONAL_ID_LABEL_ID); return unwrap(ddog_prof_Profile_intern_labelset(&p,{&l,1}), DDOG_PROF_LABEL_SET_ID_RESULT_OK_GENERATIONAL_ID_LABEL_SET_ID); }
  void record(Stack s, Labels l, bool timestamped, size_t i) { int64_t values[] = {10000000,20000000}; check(ddog_prof_Profile_intern_sample(&p,s,{values,2},l,timestamped ? int64_t(i+1) : 0)); }
  void encode(size_t end) { ddog_Timespec start{0,0}, stop{int64_t(end/1000000000ULL),uint32_t(end%1000000000ULL)};
    encoded = unwrap(ddog_prof_Profile_serialize(&p,&start,&stop), DDOG_PROF_PROFILE_SERIALIZE_RESULT_OK); has_output = true; }
  auto bytes() { return unwrap(ddog_prof_EncodedProfile_bytes(&encoded), DDOG_PROF_RESULT_BYTE_SLICE_OK_BYTE_SLICE); }
  void reset() { (void)unwrap(ddog_prof_Profile_reset(&p), DDOG_PROF_PROFILE_RESULT_OK); }
  void free_output() { if (has_output) { ddog_prof_EncodedProfile_drop(&encoded); has_output = false; } }
  ~Backend() { free_output(); ddog_prof_Profile_drop(&p); }
};
#ifdef PA_BENCH_V47
static const char* backend_name = "v47_cffi";
#else
static const char* backend_name = "v19_cffi";
#endif
#else
static void check(pa_status s) { require(s == PA_OK, "C API status failure"); }
using String = pa_id; using Function = pa_id; using Location = pa_id; using Stack = pa_id; using Labels = pa_id;
struct Backend {
  pa_profile* p = nullptr; std::vector<uint8_t> output;
  explicit Backend(Allocator& a) { pa_value_type types[] = {{"cpu-time","nanoseconds"},{"wall-time","nanoseconds"}};
    pa_allocator allocator{&a,Allocator::alloc,Allocator::release}; check(pa_create(types,2,10000000,a.enabled ? &allocator : nullptr,&p)); }
  String string(const char* s) { pa_id id; check(pa_intern_string(p,s,std::strlen(s),&id)); return id; }
  Function function() { pa_function f{}; pa_id id; check(pa_intern_function(p,&f,&id)); return id; }
  Location location(Function f, uint64_t address) { pa_location l{}; l.function=f; l.address=address; pa_id id; check(pa_intern_location(p,&l,&id)); return id; }
  Stack stack(const Location* ids, size_t n) { pa_id id; check(pa_intern_stack(p,ids,n,&id)); return id; }
  Labels labels() { pa_id id; check(pa_intern_labels(p,nullptr,0,&id)); return id; }
  Labels labels(String key, String value) { pa_label l{}; l.key=key; l.str=value; pa_id id; check(pa_intern_labels(p,&l,1,&id)); return id; }
  void record(Stack s, Labels l, bool timestamped, size_t i) { int64_t values[] = {10000000,20000000}; check(pa_record(p,s,l,values,2,timestamped,int64_t(i+1))); }
  static pa_status sink(void* context, const uint8_t* bytes, size_t n) { auto& out = static_cast<Backend*>(context)->output;
    try { out.insert(out.end(),bytes,bytes+n); return PA_OK; } catch (...) { return PA_OUTPUT; } }
  void encode(size_t end) { check(pa_serialize(p,0,int64_t(end),sink,this)); }
  struct Bytes { const uint8_t* ptr; size_t len; };
  Bytes bytes() { return {output.data(),output.size()}; }
  void reset() { check(pa_reset(p)); }
  void free_output() { std::vector<uint8_t>().swap(output); }
  ~Backend() { pa_destroy(p); }
};
#ifdef PA_BENCH_COMPRESSION_HOOKS
static const char* backend_name = "c_timed";
#else
static const char* backend_name = "c";
#endif
#endif
struct State { std::array<Location,256> locations{}; size_t depth; };
static State seed(Backend& b, bool deep) {
  State s{}; s.depth = deep ? 256 : 16;
  b.string("thread_name");
  // Identical function interning call count for both backends, including setup.
  for (size_t j=0;j<256;++j) s.locations[j] = b.location(b.function(),j);
  b.stack(s.locations.data(),s.depth); b.labels(); return s;
}
static void sample(Backend& b, State& s, const std::string& workload, bool timestamped, size_t i) {
  if (workload == "unique" || workload == "churn") s.locations[0] = b.location(b.function(),i+256);
  auto stack = b.stack(s.locations.data(),s.depth);
  Labels labels;
  if (workload == "labels") {
    char name[64]; std::snprintf(name,sizeof(name),"worker-%zu",i);
    auto key = b.string("thread_name"); auto value = b.string(name); labels = b.labels(key,value);
  } else labels = b.labels();
  b.record(stack,labels,timestamped,i);
}
struct Latency { uint64_t ns; size_t index; };
static Latency percentile(const std::vector<Latency>& sorted, unsigned p) { return sorted[(sorted.size()*p+99)/100-1]; }
struct Options {
  std::string workload="repeated", mode="throughput", output="run", fixture;
  size_t samples=100000, flush_every=1000, cycles=90;
  uint64_t affinity_mask=0;
  size_t allocation_event_limit=100000;
  bool timestamped=true, alloc_stats=false;
};
static Options options(int argc,char** argv) {
  Options o;
  for(int i=1;i<argc;++i) {
    std::string k=argv[i]; require(i+1<argc,"missing option value"); std::string v=argv[++i];
    if(k=="--workload") o.workload=v; else if(k=="--mode") o.mode=v; else if(k=="--output") o.output=v;
    else if(k=="--fixture") o.fixture=v; else if(k=="--samples") o.samples=std::stoull(v);
    else if(k=="--allocation-event-limit") o.allocation_event_limit=std::stoull(v);
    else if(k=="--affinity-mask") o.affinity_mask=std::stoull(v);
    else if(k=="--flush-every") o.flush_every=std::stoull(v); else if(k=="--cycles") o.cycles=std::stoull(v);
    else if(k=="--timestamped") { require(v=="0" || v=="1","timestamped must be 0/1"); o.timestamped=v=="1"; }
    else if(k=="--alloc-stats") { require(v=="0" || v=="1","alloc-stats must be 0/1"); o.alloc_stats=v=="1"; }
    else throw std::runtime_error("unknown option: "+k);
  }
  require(o.samples>0 && o.samples<=10000000 && o.flush_every>0 && o.cycles>0 && o.cycles<=1000,"counts out of range");
  require(o.workload=="repeated" || o.workload=="unique" || o.workload=="deep256" || o.workload=="labels" || o.workload=="churn" || o.workload=="recovery","invalid workload");
  require(o.mode=="throughput" || o.mode=="latency" || o.mode=="discard","invalid mode");
#if defined(PA_BENCH_V19) || defined(PA_BENCH_V47)
  require(!o.alloc_stats,"baseline allocator totals unavailable; cannot enable instrumentation");
#endif
  require(!o.alloc_stats || o.mode=="throughput","allocation diagnostics must be a separate throughput run");
  require(o.allocation_event_limit<=100000,"allocation event limit exceeds 100000");
  return o;
}
static std::ofstream csv(const std::string& path,const char* header) {
  std::ofstream f(path); require(bool(f),"cannot open output CSV"); f << header << '\n'; return f;
}
int main(int argc,char** argv) try {
  auto o=options(argc,argv); (void)tick(); // QPC frequency cached before measurements.
  auto platform=csv(o.output+"-platform.csv","clock,frequency_hz,affinity_requested,affinity_apply_success,process_affinity_mask,system_affinity_mask");
#ifdef _WIN32
  const bool affinity_ok=!o.affinity_mask || SetProcessAffinityMask(GetCurrentProcess(),DWORD_PTR(o.affinity_mask));
  DWORD_PTR process_mask=0,system_mask=0;
  require(GetProcessAffinityMask(GetCurrentProcess(),&process_mask,&system_mask)!=0,"GetProcessAffinityMask");
  platform<<"QPC,"<<clock_frequency()<<','<<o.affinity_mask<<','<<affinity_ok<<','<<process_mask<<','<<system_mask<<'\n'; platform.flush();
  require(affinity_ok,"requested affinity could not be applied");
#else
  require(o.affinity_mask==0,"affinity option only supported on Windows");
  platform<<"CLOCK_MONOTONIC,1000000000,0,NA,NA,NA\n";
#endif
  auto stages=csv(o.output+"-stages.csv","cycle,phase,samples,ns,compression_ns,noncompression_serialization_ns,compression_calls,protobuf_bytes,compressed_bytes,core_allocations,core_allocated_bytes,core_frees,core_live_bytes,core_peak_bytes");
  auto mem=csv(o.output+"-memory.csv","cycle,stage,samples,current_working_set_bytes,peak_working_set_bytes,private_committed_bytes,peak_process_commit_bytes");
  auto lat=csv(o.output+"-latency.csv","sample_index,ns");
  auto totals=csv(o.output+"-summary.csv","result_version,baseline_version,backend,workload,mode,timestamped,allocator_instrumented,samples,flushes,setup_ns,ingest_ns,serialize_ns,explicit_reset_ns,output_free_ns,destroy_ns,compressed_bytes,allocation_events_dropped,clock_p50_ns,clock_p95_ns,clock_p99_ns,clock_max_ns,record_p50_ns,record_p95_ns,record_p99_ns,record_max_ns,record_max_index,compression_ns,noncompression_serialization_ns,protobuf_bytes");
  // Measure paired empty reads; do not subtract this noisy estimate from samples.
  std::vector<Latency> empty; empty.reserve(10000);
  for(size_t i=0;i<10000;++i) { auto t=tick(); auto d=tick()-t; empty.push_back({d,i}); }
  std::sort(empty.begin(),empty.end(),[](auto a,auto b){return a.ns<b.ns;});
  std::vector<Latency> latency;
  size_t expected=o.workload=="recovery" ? o.cycles*o.samples*6 : o.samples;
  if(o.mode=="latency") latency.reserve(expected);
  Allocator alloc; alloc.enabled=o.alloc_stats;
  if(o.alloc_stats) alloc.events.reserve(o.allocation_event_limit); // diagnostic only, never primary runs.
  auto snapshot=[&](size_t cycle,const char* stage,size_t n) { auto m=memory(); mem << cycle<<','<<stage<<','<<n<<','<<m.current_ws<<','<<m.peak_ws<<','<<m.private_commit<<','<<m.peak_process_commit<<'\n'; };
  uint64_t setup_ns=0,ingest_ns=0,encode_ns=0,reset_ns=0,free_ns=0,destroy_ns=0;
#ifdef PA_BENCH_COMPRESSION_HOOKS
  uint64_t compress_total=0,protobuf_total=0;
#endif
  size_t total=0,flushes=0,compressed=0;
  Backend* b=nullptr;
  auto phase=[&](size_t cycle,const char* name,size_t n,auto&& fn) {
    alloc.phase=name; alloc.cycle=cycle;
    auto a=alloc.allocations,bytes=alloc.bytes,frees=alloc.frees;
    auto t=tick(); fn(); auto ns=tick()-t;
    stages<<cycle<<','<<name<<','<<n<<','<<ns<<',';
#ifdef PA_BENCH_COMPRESSION_HOOKS
    if(std::strcmp(name,"serialize")==0) { require(ns>=compression_ns,"compression exceeds serialize interval"); stages<<compression_ns<<','<<ns-compression_ns<<','<<compression_calls<<','<<protobuf_bytes; compress_total+=compression_ns; protobuf_total+=protobuf_bytes; }
    else stages<<"NA,NA,NA,NA";
#else
    stages<<"NA,NA,NA,NA";
#endif
    if(std::strcmp(name,"serialize")==0) stages<<','<<b->bytes().len; else stages<<",NA";
    if(o.alloc_stats) stages<<','<<alloc.allocations-a<<','<<alloc.bytes-bytes<<','<<alloc.frees-frees<<','<<alloc.live<<','<<alloc.peak;
    else stages<<",NA,NA,NA,NA,NA";
    stages<<'\n'; return ns;
  };
  snapshot(0,"beforecreate",0);
  setup_ns+=phase(0,"create",0,[&]{ b=new Backend(alloc); });
  State state{};
  const size_t cycles=o.workload=="recovery" ? o.cycles : (o.workload=="churn" ? (o.samples+o.flush_every-1)/o.flush_every : 1);
  for(size_t cycle=0;cycle<cycles;++cycle) {
    auto workload=o.workload; size_t n=o.samples;
    if(workload=="churn") n=std::min(o.flush_every,o.samples-total);
    if(workload=="recovery") { size_t section=cycle*3/cycles; workload=section==1 ? "unique" : "repeated"; n=section==1 ? o.samples*6 : o.samples; }
    setup_ns+=phase(cycle,"seed",n,[&]{state=seed(*b,workload=="deep256");});
    snapshot(cycle,"aftersetup",n);
    // Only batch clocks in throughput. Latency mode includes two reads per whole
    // matched record operation (interning + formatting + sample), logged after it.
    alloc.phase="ingest";
    auto ai=alloc.allocations, ab=alloc.bytes, af=alloc.frees;
    auto start=tick();
    if(o.mode=="latency") {
      for(size_t j=0;j<n;++j) { size_t i=total+j; auto t=tick(); sample(*b,state,workload,o.timestamped,i); auto elapsed=tick()-t; latency.push_back({elapsed,i}); }
    } else if(o.alloc_stats) {
      for(size_t j=0;j<n;++j) { alloc.sample=total+j; sample(*b,state,workload,o.timestamped,total+j); }
    } else {
      for(size_t j=0;j<n;++j) sample(*b,state,workload,o.timestamped,total+j);
    }
    auto elapsed=tick()-start; ingest_ns+=elapsed;
    stages<<cycle<<",ingest,"<<n<<','<<elapsed<<",NA,NA,NA,NA,NA";
    if(o.alloc_stats) stages<<','<<alloc.allocations-ai<<','<<alloc.bytes-ab<<','<<alloc.frees-af<<','<<alloc.live<<','<<alloc.peak;
    else stages<<",NA,NA,NA,NA,NA";
    stages<<'\n'; total+=n;
    snapshot(cycle,"beforeflush",n);
    if(o.mode!="discard") {
#ifdef PA_BENCH_COMPRESSION_HOOKS
      compression_ns=0; compression_calls=0; protobuf_bytes=0;
#endif
      encode_ns+=phase(cycle,"serialize",n,[&]{b->encode(total);});
      auto bytes=b->bytes(); compressed+=bytes.len; // Both backends retain full output.
      snapshot(cycle,"afterencode",n);
      if(!o.fixture.empty()) { std::ofstream file(o.fixture+"-"+std::to_string(cycle)+".pprof",std::ios::binary); require(bool(file),"fixture open failed"); file.write(reinterpret_cast<const char*>(bytes.ptr),bytes.len); require(bool(file),"fixture write failed"); }
    }
    // libdatadog serialize already drains/resets. This redundant reset models the
    // adapter, and is deliberately reported separately from its implicit reset.
    reset_ns+=phase(cycle,"explicitreset",n,[&]{b->reset();});
    free_ns+=phase(cycle,"output_free",n,[&]{b->free_output();});
    snapshot(cycle,"afterreset+free",n); ++flushes;
  }
  destroy_ns=phase(cycles,"destroy",0,[&]{delete b;}); require(alloc.live==0,"core allocation leak");
  snapshot(cycles,"afterdestroy",0);
  for(auto v:latency) lat<<v.index<<','<<v.ns<<'\n';
  std::sort(latency.begin(),latency.end(),[](auto a,auto b){return a.ns<b.ns || (a.ns==b.ns && a.index<b.index);});
  totals<<"v2-v"<<PA_BENCH_BASELINE_VERSION<<','<<PA_BENCH_BASELINE_VERSION<<','<<backend_name<<','<<o.workload<<','<<o.mode<<','<<o.timestamped<<','<<o.alloc_stats<<','<<total<<','<<flushes<<','<<setup_ns<<','<<ingest_ns<<','<<encode_ns<<','<<reset_ns<<','<<free_ns<<','<<destroy_ns<<','<<compressed<<',';
  if(o.alloc_stats) totals<<alloc.dropped_events<<','; else totals<<"NA,";
  totals<<percentile(empty,50).ns<<','<<percentile(empty,95).ns<<','<<percentile(empty,99).ns<<','<<empty.back().ns<<',';
  if(latency.empty()) totals<<"NA,NA,NA,NA,NA";
  else totals<<percentile(latency,50).ns<<','<<percentile(latency,95).ns<<','<<percentile(latency,99).ns<<','<<latency.back().ns<<','<<latency.back().index;
#ifdef PA_BENCH_COMPRESSION_HOOKS
  totals<<','<<compress_total<<','<<encode_ns-compress_total<<','<<protobuf_total;
#else
  totals<<",NA,NA,NA";
#endif
  totals<<'\n';
  if(o.alloc_stats) { auto events=csv(o.output+"-allocations.csv","cycle,sample_index,phase,operation,requested_bytes,live_bytes,peak_bytes,highwater_growth_bytes");
    for(auto& e:alloc.events) events<<e.cycle<<','<<e.sample<<','<<e.phase<<','<<(e.allocation ? "alloc" : "free")<<','<<e.bytes<<','<<e.live<<','<<e.peak<<','<<e.highwater_growth<<'\n'; }
  std::printf("PASS backend=%s workload=%s mode=%s samples=%zu flushes=%zu\n",backend_name,o.workload.c_str(),o.mode.c_str(),total,flushes);
  return 0;
} catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
