package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"strings"

	"github.com/DataDog/prof-correctness/analysis"
	"github.com/google/pprof/profile"
)

func require(ok bool, format string, args ...any) {
	if !ok {
		fmt.Fprintf(os.Stderr, format+"\n", args...)
		os.Exit(1)
	}
}

func read(path string) *profile.Profile {
	p, err := analysis.ReadPprofFile(path)
	require(err == nil, "%s: %v", path, err)
	require(p.CheckValid() == nil, "invalid profile: %s", path)
	return p
}

func capture(directory string) {
	var metadata struct {
		Callbacks int
		Final     bool
		StartMS   int64 `json:"start_ms"`
		EndMS     int64 `json:"end_ms"`
		ThreadID  int64 `json:"thread_id"`
	}
	data, err := os.ReadFile(filepath.Join(directory, "callback.json"))
	require(err == nil, "%v", err)
	require(json.Unmarshal(data, &metadata) == nil, "invalid callback metadata")
	require(metadata.Callbacks == 1 && metadata.Final, "expected one final callback")
	p := read(filepath.Join(directory, "callback.zst.pprof"))
	expected := [][2]string{{"cpu-time", "nanoseconds"}, {"cpu-samples", "count"}, {"wall-time", "nanoseconds"}, {"wait-time", "nanoseconds"}}
	var definitions [][2]string
	for _, definition := range p.SampleType {
		definitions = append(definitions, [2]string{definition.Type, definition.Unit})
	}
	require(reflect.DeepEqual(definitions, expected), "unexpected ordered sample definitions: %v", definitions)
	require(p.PeriodType != nil && p.PeriodType.Type == expected[0][0] && p.PeriodType.Unit == expected[0][1] && p.Period == 10_000_000, "unexpected period")
	require(p.TimeNanos == metadata.StartMS*1_000_000, "callback/profile start mismatch")
	require(p.DurationNanos == (metadata.EndMS-metadata.StartMS)*1_000_000 && p.DurationNanos > 0, "callback/profile duration mismatch")
	require(len(p.Sample) > 0 && len(p.Mapping) > 0 && len(p.Function) > 0 && len(p.Location) > 0, "missing profile tables")
	workload := 0
	for _, sample := range p.Sample {
		require(len(sample.Location) > 0 && len(sample.Location) <= 256, "capture depth outside 1..256")
		for _, location := range sample.Location {
			for _, line := range location.Line {
				if line.Function != nil && strings.Contains(line.Function.Name, "LibddCIntegrationWorkload") {
					require(reflect.DeepEqual(sample.NumLabel["thread id"], []int64{metadata.ThreadID}), "callback/profile thread mismatch")
					require(location.Mapping != nil && strings.Contains(strings.ToLower(location.Mapping.File), "consumer"), "missing workload module")
					workload++
				}
			}
		}
	}
	require(workload > 0, "missing sampled workload")
	report, _ := json.MarshalIndent(map[string]any{"decoder": "prof-correctness v1.0.0 / google/pprof", "samples": len(p.Sample), "workload_samples": workload, "period": p.Period, "time_nanos": p.TimeNanos, "duration_nanos": p.DurationNanos, "metadata": metadata}, "", "  ")
	require(os.WriteFile(filepath.Join(directory, "independent-decode.json"), append(report, '\n'), 0644) == nil, "cannot write report")
	fmt.Println(string(report))
}

func compare(before, after string) {
	files, err := filepath.Glob(filepath.Join(before, "*.pprof"))
	require(err == nil && len(files) == 6, "expected six baseline fixtures")
	other, err := filepath.Glob(filepath.Join(after, "*.pprof"))
	require(err == nil && len(other) == len(files), "fixture count mismatch")
	for _, path := range files {
		var a, b bytes.Buffer
		require(read(path).WriteUncompressed(&a) == nil, "cannot canonicalize baseline")
		require(read(filepath.Join(after, filepath.Base(path))).WriteUncompressed(&b) == nil, "cannot canonicalize candidate")
		require(bytes.Equal(a.Bytes(), b.Bytes()), "decoded protobuf differs: %s", filepath.Base(path))
		fmt.Printf("PASS: prof-correctness decoded baseline/candidate equality: %s\n", filepath.Base(path))
	}
}

func main() {
	require(len(os.Args) >= 3, "usage: profile-check capture DIR | compare BEFORE AFTER")
	switch os.Args[1] {
	case "capture":
		capture(os.Args[2])
	case "compare":
		require(len(os.Args) == 4, "compare requires BEFORE AFTER")
		compare(os.Args[2], os.Args[3])
	default:
		panic("unknown mode")
	}
}
