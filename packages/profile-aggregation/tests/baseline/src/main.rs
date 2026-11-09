// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
use datadog_profiling::{api, internal::Profile};
use std::alloc::{GlobalAlloc, Layout, System};
use std::num::NonZeroI64;
use std::sync::atomic::{AtomicUsize, Ordering::Relaxed};
use std::time::{Duration, Instant, UNIX_EPOCH};
static ALLOCS: AtomicUsize = AtomicUsize::new(0);
static BYTES: AtomicUsize = AtomicUsize::new(0);
static LIVE: AtomicUsize = AtomicUsize::new(0);
static PEAK: AtomicUsize = AtomicUsize::new(0);
struct Counting;
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        let p = System.alloc(l);
        if !p.is_null() {
            ALLOCS.fetch_add(1, Relaxed);
            BYTES.fetch_add(l.size(), Relaxed);
            let live = LIVE.fetch_add(l.size(), Relaxed) + l.size();
            PEAK.fetch_max(live, Relaxed);
        }
        p
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        LIVE.fetch_sub(l.size(), Relaxed);
        System.dealloc(p, l);
    }
}
#[global_allocator]
static ALLOCATOR: Counting = Counting;
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let count: usize = std::env::args().nth(1).unwrap_or("100000".into()).parse()?;
    let types = [api::ValueType::new("cpu-time", "nanoseconds"), api::ValueType::new("wall-time", "nanoseconds")];
    println!("mode,samples,ingest_ms,max_record_us,serialize_ms,allocations,allocated_bytes,profile_peak_delta,live_before_flush_delta,after_reset_delta,compressed_bytes");
    for (mode, name) in ["repeated", "unique", "deep", "labels", "churn"].iter().enumerate() {
        let initial_live = LIVE.load(Relaxed);
        PEAK.store(initial_live, Relaxed);
        let initial_bytes = BYTES.load(Relaxed);
        let setup = || -> Result<_, Box<dyn std::error::Error>> {
            let mut p = Profile::new(&types, Some(api::Period { r#type: types[0], value: 10000000 }))
                .with_start_time(UNIX_EPOCH)?;
            p.intern_string("thread_name")?;
            let mut locs = Vec::with_capacity(256);
            for j in 0..256 {
                let f = p.intern_function(Profile::INTERNED_EMPTY_STRING, Profile::INTERNED_EMPTY_STRING, Profile::INTERNED_EMPTY_STRING)?;
                locs.push(p.intern_location(None, f, j, 0)?);
            }
            Ok((p, locs))
        };
        let (mut p, mut locs) = setup()?;
        let depth = if mode == 2 { 256 } else { 16 };
        let warm_allocs = ALLOCS.load(Relaxed);
        let start = Instant::now();
        let mut max_record = Duration::ZERO;
        let mut serialization = Duration::ZERO;
        let mut compressed = 0;
        for i in 0..count {
            let tick = Instant::now();
            if mode == 1 || mode == 4 {
                let f = p.intern_function(Profile::INTERNED_EMPTY_STRING, Profile::INTERNED_EMPTY_STRING, Profile::INTERNED_EMPTY_STRING)?;
                locs[0] = p.intern_location(None, f, (i + 256) as u64, 0)?;
            }
            let stack = p.intern_stacktrace(&locs[..depth])?;
            let labels = if mode == 3 {
                use std::io::Write;
                let mut buffer = [0u8; 64];
                let mut cursor = std::io::Cursor::new(buffer.as_mut_slice());
                write!(cursor, "worker-{i}")?;
                let len = cursor.position() as usize;
                let value = p.intern_string(std::str::from_utf8(&buffer[..len])?)?;
                let key = p.intern_string("thread_name")?;
                let label = p.intern_label_str(key, value)?;
                p.intern_labelset(&[label])?
            } else { p.intern_labelset(&[])? };
            p.intern_sample(stack, &[10000000, 20000000], labels, NonZeroI64::new((i + 1) as i64))?;
            max_record = max_record.max(tick.elapsed());
            if mode == 4 && i % 1000 == 999 {
                let tick = Instant::now();
                let old = p.reset_and_return_previous()?;
                compressed += old.serialize_into_compressed_pprof(Some(UNIX_EPOCH + Duration::from_nanos((i+1) as u64)), Some(Duration::from_nanos((i+1) as u64)))?.buffer.len();
                serialization += tick.elapsed();
                (p, locs) = setup()?;
            }
        }
        let ingest = start.elapsed() - serialization;
        let live_before = LIVE.load(Relaxed).saturating_sub(initial_live);
        let tick = Instant::now();
        let old = p.reset_and_return_previous()?;
        compressed += old.serialize_into_compressed_pprof(Some(UNIX_EPOCH + Duration::from_nanos(count as u64)), Some(Duration::from_nanos(count as u64)))?.buffer.len();
        serialization += tick.elapsed();
        let after_reset = LIVE.load(Relaxed).saturating_sub(initial_live);
        println!("{name},{count},{:.3},{:.3},{:.3},{},{},{},{},{},{compressed}",
            ingest.as_secs_f64()*1000.0, max_record.as_secs_f64()*1e6, serialization.as_secs_f64()*1000.0,
            ALLOCS.load(Relaxed)-warm_allocs, BYTES.load(Relaxed)-initial_bytes,
            PEAK.load(Relaxed).saturating_sub(initial_live), live_before, after_reset);
        drop((p,locs));
    }
    Ok(())
}
