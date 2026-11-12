# Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [string]$BuildRoot = 'C:\Repos\dd-win-prof-c-aggregation\build-perf-v2-v47',
    [ValidateSet('19','47')][string]$BaselineVersion = '47',
    [string]$BaselineRoot = 'C:\Repos\libdatadog-v47.0.0\libdatadog-x64-windows',
    [string]$BaselineLibrary = '',
    [ValidateSet('MultiThreaded','MultiThreadedDLL')][string]$MsvcRuntime = 'MultiThreaded',
    [string]$Output = 'Z:\winshare\artifacts\prof-16455-perf-v2-v47',
    [int]$Replicates = 7,
    [int]$Samples = 100000,
    [int]$LatencySamples = 100000,
    [int]$LatencyReplicates = 3,
    [int]$DiagnosticSamples = 0,
    [int]$FixtureSamples = 6000,
    [int]$Seed = 16455,
    [uint64]$AffinityMask = 0
)
$ErrorActionPreference = 'Stop'
if ($DiagnosticSamples -eq 0) { $DiagnosticSamples = $Samples }
if ($Replicates -lt 1 -or $Samples -lt 1 -or $LatencySamples -lt 1 -or $LatencyReplicates -lt 1 -or $DiagnosticSamples -lt 1 -or $FixtureSamples -lt 1) { throw 'Counts must be positive' }
$PackageRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ($PackageRoot.StartsWith('Z:', [StringComparison]::OrdinalIgnoreCase) -or $BuildRoot.StartsWith('Z:', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Run/build from the isolated C:\Repos tree, not SMB'
}
if (-not $BaselineLibrary) { $BaselineLibrary = Join-Path $BaselineRoot 'release\static\datadog_profiling_ffi.lib' }
if (-not (Test-Path $BaselineLibrary)) { throw "Missing explicitly selected baseline library: $BaselineLibrary" }
$baselineBackend = "v$BaselineVersion"
$baselineName = "v$BaselineVersion.0.0"
$header = Join-Path $BaselineRoot 'include\datadog\profiling.h'
if (-not (Test-Path $header)) { throw "Missing public release header: $header" }
if ($BaselineVersion -eq '47' -and (Get-Content $header -Raw) -notmatch 'ddog_prof_Profile_set_custom_sample_type') {
    throw 'v47 header API missing; refusing a stale v19 dependency'
}
if ($BaselineVersion -eq '19' -and $BaselineRoot -match 'v47') { throw 'Explicitly select the historical v19 package root' }
if ($BaselineVersion -eq '47') {
    $expectedLibrary = '27f6577e54d74dc32c8ce97c903786249a48045cf2bfa929ce6963527aa5f34f'
    $expectedHeader = '7f90cc0670fbc9da7c4d56b09c5db60fa5f1619a0486dff33bf470bb72b22a2f'
    $expectedCommon = '273d994bcc8e5c1905f81b4f4023ec6de8dd79690f804a6360f826efce331a41'
    foreach ($pair in @(@($BaselineLibrary,$expectedLibrary), @($header,$expectedHeader), @((Join-Path $BaselineRoot 'include\datadog\common.h'),$expectedCommon))) {
        if ((Get-FileHash -LiteralPath $pair[0] -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pair[1]) { throw "Not the pinned v47.0.0 Windows release asset: $($pair[0])" }
    }
}
$cache = Join-Path $BuildRoot 'CMakeCache.txt'
if (Test-Path $cache) {
    $cached = Get-Content $cache -Raw
    $other = if ($BaselineVersion -eq '47') { '19' } else { '47' }
    if ($cached -match "PA_BENCH_V${other}:BOOL=ON") { throw 'Use a fresh version-specific build directory' }
}
$runId = 'run-' + (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$local = Join-Path $BuildRoot $runId
New-Item -ItemType Directory -Force $local, (Join-Path $local 'fixtures') | Out-Null
$watch = [Diagnostics.Stopwatch]::StartNew()
try {
    $metadata = [ordered]@{
        resultVersion = "v2-$baselineBackend"; baselineVersion = $baselineName; recipe = 'shared-driver-v2'
        baselineSourceCommit = $(if ($BaselineVersion -eq '47') { 'ef6371243c020319e74e8531035f0a5a2fcb8858' } else { $null })
        msvcRuntime = $MsvcRuntime
        lifecycle = 'Primary whole_flush_and_free: C serialize+reset+output_free versus libdatadog serialize+output_free; redundant baseline explicitreset reported separately'
        compression = $(if ($BaselineVersion -eq '47') { 'C: Zstd 1.5.7 level1 windowLog17; v47: default native Zstd codec level1, default window' } else { 'C: Zstd 1.5.7 level1 windowLog17; v19: LZ4' })
        run = $runId; utc = (Get-Date).ToUniversalTime().ToString('o'); seed = $Seed
        requestedAffinityMask = $AffinityMask; affinityAndQpcResults = 'Per-process *-platform.csv; zero mask inherits OS affinity, no pinning'
        replicates = $Replicates; samples = $Samples; latencySamples = $LatencySamples; latencyReplicates = $LatencyReplicates; diagnosticSamples = $DiagnosticSamples; fixtureSamples = $FixtureSamples
        computer = $env:COMPUTERNAME; processorCount = [Environment]::ProcessorCount
        os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber, OSArchitecture
        cpu = Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed
        memory = Get-CimInstance Win32_ComputerSystem | Select-Object TotalPhysicalMemory
        powershell = $PSVersionTable.PSVersion.ToString(); cmake = (& cmake --version | Out-String).Trim()
        powerScheme = (& powercfg /getactivescheme | Out-String).Trim()
        packageRoot = $PackageRoot; buildRoot = $BuildRoot; baselineRoot = $BaselineRoot; baselineLibrary = $BaselineLibrary
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) { $metadata.visualStudio = (& $vswhere -latest -products '*' -format json | Out-String) }
    $hashes = @((Join-Path $PSScriptRoot 'driver.cpp'), (Join-Path $PSScriptRoot 'CMakeLists.txt'), (Join-Path $PSScriptRoot 'run-windows.ps1'), (Join-Path $PSScriptRoot 'compression_hooks.h'), (Join-Path $PackageRoot 'include\profile_aggregation.h'), (Join-Path $PackageRoot 'src\profile_aggregation.c'), (Join-Path $PackageRoot 'vendor\zstd.c'), (Join-Path $BaselineRoot 'include\datadog\profiling.h'), (Join-Path $BaselineRoot 'include\datadog\common.h'), $BaselineLibrary) | ForEach-Object { Get-FileHash -LiteralPath $_ -Algorithm SHA256 }
    $metadata.hashes = $hashes | Select-Object Path, Hash
    $metadata | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $local 'metadata.json') -Encoding UTF8
    & cmake -S $PSScriptRoot -B $BuildRoot -G 'Visual Studio 17 2022' -A x64 "-DPA_BENCH_V${BaselineVersion}=ON" "-DPA_V${BaselineVersion}_ROOT=$BaselineRoot" "-DPA_V${BaselineVersion}_LIBRARY=$BaselineLibrary" "-DPA_BENCH_MSVC_RUNTIME=$MsvcRuntime" *> (Join-Path $local 'configure.log')
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    & cmake --build $BuildRoot --config Release --parallel 4 *> (Join-Path $local 'build.log')
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
    Copy-Item (Join-Path $BuildRoot 'CMakeCache.txt') $local
    $runtimeDefinitions = foreach ($project in Get-ChildItem $BuildRoot -Filter '*.vcxproj') {
        foreach ($match in Select-String -LiteralPath $project.FullName -Pattern '<RuntimeLibrary>') {
            [pscustomobject]@{project=$project.Name; line=$match.LineNumber; value=$match.Line.Trim()}
        }
    }
    $runtimeDefinitions | Export-Csv (Join-Path $local 'runtime-config.csv') -NoTypeInformation
    # Exact compiler version and effective flags are also recorded in build logs.
    Get-ChildItem (Join-Path $BuildRoot 'CMakeFiles') -Filter CMakeCXXCompiler.cmake -Recurse | ForEach-Object { Copy-Item $_.FullName (Join-Path $local 'CMakeCXXCompiler.cmake') }
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'Visual Studio tools required for baseline CRT audit' }
    $dumpbin = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe') | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $dumpbin) { throw 'dumpbin not found' }
    $directives = & $dumpbin.FullName /directives $BaselineLibrary
    if ($LASTEXITCODE -ne 0) { throw 'Baseline CRT directive audit failed' }
    $directives | Set-Content (Join-Path $local 'baseline-library-directives.txt') -Encoding UTF8
    $crt = @($directives | Select-String -Pattern 'DEFAULTLIB:.*(LIBCMT|MSVCRT)|FAILIFMISMATCH:.*RuntimeLibrary' | ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique)
    if (-not $crt.Count) { throw 'No baseline CRT evidence found' }
    if (($MsvcRuntime -eq 'MultiThreaded' -and ($crt -join ' ') -match '(?i)MSVCRT|MD_DynamicRelease') -or
        ($MsvcRuntime -eq 'MultiThreadedDLL' -and ($crt -join ' ') -match '(?i)LIBCMT|MT_StaticRelease')) { throw 'Selected benchmark CRT conflicts with baseline directives' }
    $metadata.baselineCrtDirectives = $crt
    $metadata.executableHashes = @('core','core_timed',$baselineBackend) | ForEach-Object {
        Get-FileHash -LiteralPath (Join-Path $BuildRoot "Release\bench_$_.exe") -Algorithm SHA256 | Select-Object Path, Hash
    }
    $metadata | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $local 'metadata.json') -Encoding UTF8
    $jobs = [Collections.Generic.List[object]]::new()
    function Add-JobSpec($group, $backend, $workload, $timestamped, $n, $replicate, $mode = 'throughput', $allocation = 0, $fixture = $false) {
        $jobs.Add([pscustomobject]@{group=$group; backend=$backend; workload=$workload; timestamped=$timestamped; samples=$n; replicate=$replicate; mode=$mode; allocation=$allocation; fixture=$fixture})
    }
    $workloads = @('repeated', 'unique', 'deep256', 'labels', 'churn')
    foreach ($timestamped in @(0,1)) {
        foreach ($workload in $workloads) {
            for ($r=1; $r -le $Replicates; $r++) {
                foreach ($backend in @('core',$baselineBackend)) { Add-JobSpec 'primary' $backend $workload $timestamped $Samples $r }
            }
            foreach ($backend in @('core',$baselineBackend)) {
                Add-JobSpec 'small' $backend $workload $timestamped 6000 1
                for ($r=1; $r -le $LatencyReplicates; $r++) {
                    Add-JobSpec 'latency' $backend $workload $timestamped $LatencySamples $r 'latency'
                }
                # Fresh process, populated profile reset without serialization.
                Add-JobSpec 'discard' $backend $workload $timestamped $Samples 1 'discard'
            }
            foreach ($backend in @('core','core_timed',$baselineBackend)) { Add-JobSpec 'fixture' $backend $workload $timestamped $FixtureSamples 1 'throughput' 0 $true }
            Add-JobSpec 'allocator' 'core' $workload $timestamped $DiagnosticSamples 1 'throughput' 1
        }
        foreach ($workload in @('deep256','churn')) {
            for ($r=1; $r -le $Replicates; $r++) { Add-JobSpec 'compression' 'core_timed' $workload $timestamped $Samples $r }
        }
        foreach ($backend in @('core',$baselineBackend)) { Add-JobSpec 'recovery' $backend 'recovery' $timestamped 1000 1 }
    }
    # Stable Fisher-Yates, never Get-Random with process-global implicit state.
    $rng = [Random]::new($Seed)
    for ($i=$jobs.Count-1; $i -gt 0; $i--) { $j=$rng.Next($i+1); $tmp=$jobs[$i]; $jobs[$i]=$jobs[$j]; $jobs[$j]=$tmp }
    $manifest = [Collections.Generic.List[object]]::new()
    for ($order=0; $order -lt $jobs.Count; $order++) {
        $job=$jobs[$order]
        $id = '{0:D3}-{1}-{2}-{3}-t{4}-r{5}' -f $order,$job.group,$job.backend,$job.workload,$job.timestamped,$job.replicate
        $exe = Join-Path $BuildRoot ('Release\bench_' + $job.backend + '.exe')
        $arguments = @('--output',(Join-Path $local $id),'--workload',$job.workload,'--samples',"$($job.samples)",'--timestamped',"$($job.timestamped)",'--mode',$job.mode,'--alloc-stats',"$($job.allocation)",'--affinity-mask',"$AffinityMask")
        if ($job.fixture) { $arguments += @('--fixture',(Join-Path $local ('fixtures\' + $job.backend + '-' + $job.workload + '-t' + $job.timestamped))) }
        Write-Host "[$($order+1)/$($jobs.Count)] $id"
        $start=(Get-Date).ToUniversalTime().ToString('o')
        & $exe @arguments *> (Join-Path $local ($id + '.log'))
        $code=$LASTEXITCODE
        $manifest.Add([pscustomobject]@{order=$order; id=$id; utc=$start; group=$job.group; backend=$job.backend; workload=$job.workload; timestamped=$job.timestamped; samples=$job.samples; replicate=$job.replicate; mode=$job.mode; allocator=$job.allocation; exitCode=$code; command=$exe + ' ' + ($arguments -join ' ')})
        $manifest | Export-Csv (Join-Path $local 'jobs.csv') -NoTypeInformation
        if ($code -ne 0) { throw "Benchmark failed: $id ($code)" }
    }
    [pscustomobject]@{result='PASS'; resultVersion="v2-$baselineBackend"; baselineVersion=$baselineName; seconds=$watch.Elapsed.TotalSeconds; jobs=$jobs.Count; powerSchemeAfter=(& powercfg /getactivescheme | Out-String).Trim()} | ConvertTo-Json | Set-Content (Join-Path $local 'result.json') -Encoding UTF8
} finally {
    # All timed work and log/fixture writes used local disk. Copy only afterwards.
    New-Item -ItemType Directory -Force $Output | Out-Null
    Copy-Item $local (Join-Path $Output $runId) -Recurse -Force
    Write-Host "Artifacts: $(Join-Path $Output $runId)"
}
