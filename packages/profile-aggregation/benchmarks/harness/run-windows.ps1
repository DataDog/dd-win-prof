# Copyright 2026 Datadog, Inc. SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [string]$BuildRoot = 'C:\Repos\dd-win-prof-c-aggregation\build-perf-v2',
    [string]$V19Root = 'C:\Repos\dd-win-prof\build\_deps\libdatadog-src',
    [string]$V19Library = '',
    [string]$Output = 'Z:\winshare\artifacts\prof-16455-perf-v2',
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
if (-not $V19Library) { $V19Library = Join-Path $V19Root 'release\static\datadog_profiling_ffi.lib' }
if (-not (Test-Path $V19Library)) { throw "Missing prebuilt v19 library: $V19Library" }
$runId = 'run-' + (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$local = Join-Path $BuildRoot $runId
New-Item -ItemType Directory -Force $local, (Join-Path $local 'fixtures') | Out-Null
$watch = [Diagnostics.Stopwatch]::StartNew()
try {
    $metadata = [ordered]@{
        run = $runId; utc = (Get-Date).ToUniversalTime().ToString('o'); seed = $Seed
        requestedAffinityMask = $AffinityMask; affinityAndQpcResults = 'Per-process *-platform.csv; zero mask inherits OS affinity, no pinning'
        replicates = $Replicates; samples = $Samples; latencySamples = $LatencySamples; latencyReplicates = $LatencyReplicates; diagnosticSamples = $DiagnosticSamples; fixtureSamples = $FixtureSamples
        computer = $env:COMPUTERNAME; processorCount = [Environment]::ProcessorCount
        os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber, OSArchitecture
        cpu = Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed
        memory = Get-CimInstance Win32_ComputerSystem | Select-Object TotalPhysicalMemory
        powershell = $PSVersionTable.PSVersion.ToString(); cmake = (& cmake --version | Out-String).Trim()
        powerScheme = (& powercfg /getactivescheme | Out-String).Trim()
        packageRoot = $PackageRoot; buildRoot = $BuildRoot; baselineRoot = $V19Root; baselineLibrary = $V19Library
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) { $metadata.visualStudio = (& $vswhere -latest -products '*' -format json | Out-String) }
    $hashes = @((Join-Path $PSScriptRoot 'driver.cpp'), (Join-Path $PSScriptRoot 'CMakeLists.txt'), (Join-Path $PSScriptRoot 'run-windows.ps1'), (Join-Path $PSScriptRoot 'compression_hooks.h'), (Join-Path $PackageRoot 'include\profile_aggregation.h'), (Join-Path $PackageRoot 'src\profile_aggregation.c'), (Join-Path $PackageRoot 'vendor\zstd.c'), (Join-Path $V19Root 'include\datadog\profiling.h'), (Join-Path $V19Root 'include\datadog\common.h'), $V19Library) | ForEach-Object { Get-FileHash -LiteralPath $_ -Algorithm SHA256 }
    $metadata.hashes = $hashes | Select-Object Path, Hash
    $metadata | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $local 'metadata.json') -Encoding UTF8
    & cmake -S $PSScriptRoot -B $BuildRoot -G 'Visual Studio 17 2022' -A x64 '-DPA_BENCH_V19=ON' "-DPA_V19_ROOT=$V19Root" "-DPA_V19_LIBRARY=$V19Library" *> (Join-Path $local 'configure.log')
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
    $jobs = [Collections.Generic.List[object]]::new()
    function Add-JobSpec($group, $backend, $workload, $timestamped, $n, $replicate, $mode = 'throughput', $allocation = 0, $fixture = $false) {
        $jobs.Add([pscustomobject]@{group=$group; backend=$backend; workload=$workload; timestamped=$timestamped; samples=$n; replicate=$replicate; mode=$mode; allocation=$allocation; fixture=$fixture})
    }
    $workloads = @('repeated', 'unique', 'deep256', 'labels', 'churn')
    foreach ($timestamped in @(0,1)) {
        foreach ($workload in $workloads) {
            for ($r=1; $r -le $Replicates; $r++) {
                foreach ($backend in @('core','v19')) { Add-JobSpec 'primary' $backend $workload $timestamped $Samples $r }
            }
            foreach ($backend in @('core','v19')) {
                Add-JobSpec 'small' $backend $workload $timestamped 6000 1
                for ($r=1; $r -le $LatencyReplicates; $r++) {
                    Add-JobSpec 'latency' $backend $workload $timestamped $LatencySamples $r 'latency'
                }
                # Fresh process, populated profile reset without serialization.
                Add-JobSpec 'discard' $backend $workload $timestamped $Samples 1 'discard'
            }
            foreach ($backend in @('core','core_timed','v19')) { Add-JobSpec 'fixture' $backend $workload $timestamped $FixtureSamples 1 'throughput' 0 $true }
            Add-JobSpec 'allocator' 'core' $workload $timestamped $DiagnosticSamples 1 'throughput' 1
        }
        foreach ($workload in @('deep256','churn')) {
            for ($r=1; $r -le $Replicates; $r++) { Add-JobSpec 'compression' 'core_timed' $workload $timestamped $Samples $r }
        }
        foreach ($backend in @('core','v19')) { Add-JobSpec 'recovery' $backend 'recovery' $timestamped 1000 1 }
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
    [pscustomobject]@{result='PASS'; seconds=$watch.Elapsed.TotalSeconds; jobs=$jobs.Count; powerSchemeAfter=(& powercfg /getactivescheme | Out-String).Trim()} | ConvertTo-Json | Set-Content (Join-Path $local 'result.json') -Encoding UTF8
} finally {
    # All timed work and log/fixture writes used local disk. Copy only afterwards.
    New-Item -ItemType Directory -Force $Output | Out-Null
    Copy-Item $local (Join-Path $Output $runId) -Recurse -Force
    Write-Host "Artifacts: $(Join-Path $Output $runId)"
}
