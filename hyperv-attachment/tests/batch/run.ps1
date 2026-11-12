# Run after loading the project's dependency environment. All CPU/memory
# operations are mocked or emulated; this script never deploys or reboots.
param([ValidateSet('Intel','AMD')][string[]]$Architecture = @('Intel','AMD'),
      [string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
foreach ($cpuArchitecture in $Architecture) {
    & MSBuild.exe (Join-Path $PSScriptRoot 'allocator-check.vcxproj') /m:3 /t:Build /p:Configuration=Release /p:Platform=x64 "/p:HypervisorArchitecture=$cpuArchitecture" /nr:false /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw "$cpuArchitecture allocator build failed" }
    $outputRoot = Join-Path $projectRoot "build\isolated-tests\batch\$cpuArchitecture"
    & (Join-Path $outputRoot 'allocator-check.exe') | Tee-Object -FilePath (Join-Path $outputRoot 'allocator-results.txt')
    if ($LASTEXITCODE -ne 0) { throw "$cpuArchitecture real allocator failed" }
    & MSBuild.exe (Join-Path $PSScriptRoot 'map-bounds.vcxproj') /m:3 /t:Build /p:Configuration=Release /p:Platform=x64 "/p:HypervisorArchitecture=$cpuArchitecture" /nr:false /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw "$cpuArchitecture real mapper build failed" }
    $outputRoot = Join-Path $projectRoot "build\isolated-tests\batch\$cpuArchitecture"
    & (Join-Path $outputRoot 'map-bounds.exe') | Tee-Object -FilePath (Join-Path $outputRoot 'map-results.txt')
    if ($LASTEXITCODE -ne 0) { throw "$cpuArchitecture real mapper bounds failed" }
    & MSBuild.exe (Join-Path $PSScriptRoot 'batch-tests.vcxproj') /m:3 /t:Build /p:Configuration=Release /p:Platform=x64 "/p:HypervisorArchitecture=$cpuArchitecture" /nr:false /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw "$cpuArchitecture test build failed" }
    $outputRoot = Join-Path $projectRoot "build\isolated-tests\batch\$cpuArchitecture"
    & (Join-Path $outputRoot 'batch-tests.exe') | Tee-Object -FilePath (Join-Path $outputRoot 'results.txt')
    if ($LASTEXITCODE -ne 0) { throw "$cpuArchitecture tests failed" }
    & $Python (Join-Path $PSScriptRoot 'stub_check.py') (Join-Path $outputRoot 'stub-model.dll') | Tee-Object -FilePath (Join-Path $outputRoot 'stub-results.json')
    if ($LASTEXITCODE -ne 0) { throw "$cpuArchitecture assembly checks failed" }
}
