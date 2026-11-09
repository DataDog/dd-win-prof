// Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
use datadog_profiling::{api, internal::Profile};
use std::{io::Read, num::NonZeroI64, time::{Duration, UNIX_EPOCH}};
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let types = [api::ValueType::new("cpu-time", "nanoseconds"), api::ValueType::new("wall-time", "nanoseconds")];
    let mut p = Profile::new(&types, Some(api::Period { r#type: types[0], value: 10000000 })).with_start_time(UNIX_EPOCH + Duration::from_secs(1))?;
    let mut locations = Vec::new();
    for _ in 0..2 {
        let filename = p.intern_string("native.dll")?;
        let build = p.intern_string("ABCDEF01234567891")?;
        let m = p.intern_mapping(0x140000000, 0x140100000, 4096, filename, build)?;
        let name = p.intern_string("Worker")?;
        let system = p.intern_string("Worker")?;
        let file = p.intern_string("worker.cpp")?;
        let f = p.intern_function(name, system, file)?;
        locations.push(p.intern_location(Some(m), f, 0x123456789, 43)?);
    }
    for timestamp in [Some(1234567890123456789), Some(1234567890123456789), None, None, None] {
        let stack = p.intern_stacktrace(&locations)?;
        let key = p.intern_string("process_id")?;
        let value = p.intern_string("123")?;
        let process = p.intern_label_str(key, value)?;
        let key = p.intern_string("thread id")?;
        let unit = p.intern_string("count")?;
        let thread = p.intern_label_num(key, 0, unit)?;
        let key = p.intern_string("rum.view_id")?;
        let value = p.intern_string("view-1")?;
        let view = p.intern_label_str(key, value)?;
        let labels = p.intern_labelset(&[process, thread, view])?;
        p.intern_sample(stack, &[7, 11], labels, timestamp.and_then(NonZeroI64::new))?;
    }
    let encoded = p.serialize_into_compressed_pprof(Some(UNIX_EPOCH + Duration::from_secs(2)), Some(Duration::from_secs(1)))?;
    let mut raw = Vec::new();
    lz4_flex::frame::FrameDecoder::new(encoded.buffer.as_slice()).read_to_end(&mut raw)?;
    std::fs::write(std::env::args().nth(1).expect("raw pprof output path"), &raw)?;
    println!("v19 native fixture: {} LZ4 bytes, {} protobuf bytes", encoded.buffer.len(), raw.len());
    Ok(())
}
