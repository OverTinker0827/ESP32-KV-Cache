# Phase 3: engine tests (v2) + memory benchmark for basic, frozen v1 and v2 engines.
param([string]$Prefix = "entry6", [string[]]$Only)
$root = Split-Path -Parent $PSScriptRoot
$py = Join-Path $root ".venv\Scripts\python.exe"
$matrix = [ordered]@{
    "p3_basic" = @("basic", "p3")
    "p3_opt0"  = @("opt0", "p3")
    "p3_opt1"  = @("opt1", "p3")
    "p3_opt2"  = @("opt", "p3")
    "p3_opt2pfx" = @("opt", "prefix", "p3")
}
foreach ($name in $matrix.Keys) {
    if ($Only -and -not ($Only -contains $name)) { continue }
    Write-Output "=== $name : $($matrix[$name] -join ',')"
    & (Join-Path $PSScriptRoot "build_variant.ps1") -Name $name -Cfg $matrix[$name] -Flash *> (Join-Path $root "build\$name.buildlog")
    if ($LASTEXITCODE -ne 0) {
        & (Join-Path $PSScriptRoot "build_variant.ps1") -Name $name -Cfg $matrix[$name] -Flash *> (Join-Path $root "build\$name.buildlog")
        if ($LASTEXITCODE -ne 0) { Write-Output "BUILD/FLASH FAILED for $name"; continue }
    }
    $out = & $py (Join-Path $PSScriptRoot "serial_wait.py") COM5 "DONE|Guru|abort|panic" 600
    $res = $out | Where-Object { $_ -like "RESULT *" } | ForEach-Object { $_.Substring(7) }
    $res | Set-Content -Encoding utf8 (Join-Path $root "results\${Prefix}_$name.jsonl")
    $out | Where-Object { $_ -match "Guru|abort|panic|error" } | Select-Object -First 5 | ForEach-Object { Write-Output "  $_" }
    Write-Output "  $($res.Count) result lines"
}
Write-Output "MATRIX DONE"
