# Build + flash + run the on-device engine benchmark for a list of variants.
# Saves the RESULT lines of each variant to results\<Prefix>_<name>.jsonl
# Usage: .\tools\run_engine_matrix.ps1 -Prefix entry2 [-Only opt,basic]
param([string]$Prefix = "entry2", [string[]]$Only)
$root = Split-Path -Parent $PSScriptRoot
$py = Join-Path $root ".venv\Scripts\python.exe"
$matrix = [ordered]@{
    "basic"          = @("basic", "nowifi")
    "basic_fastitoa" = @("basic", "fastitoa", "nowifi")
    "opt"            = @("opt", "nowifi")
    "opt_forcelock"  = @("opt", "forcelock", "nowifi")
    "opt_noiram"     = @("opt", "noiram", "nowifi")
}
foreach ($name in $matrix.Keys) {
    if ($Only -and -not ($Only -contains $name)) { continue }
    Write-Output "=== $name : $($matrix[$name] -join ',')"
    & (Join-Path $PSScriptRoot "build_variant.ps1") -Name "eng_$name" -Cfg $matrix[$name] -Flash *> (Join-Path $root "build\eng_$name.buildlog")
    if ($LASTEXITCODE -ne 0) { Write-Output "BUILD/FLASH FAILED for $name (see build\eng_$name.buildlog)"; continue }
    $out = & $py (Join-Path $PSScriptRoot "serial_wait.py") COM5 "DONE|Guru|abort|panic" 180
    $res = $out | Where-Object { $_ -like "RESULT *" } | ForEach-Object { $_.Substring(7) }
    $file = Join-Path $root "results\${Prefix}_$name.jsonl"
    $res | Set-Content -Encoding utf8 $file
    Write-Output "$name -> $($res.Count) result lines -> $file"
    $out | Where-Object { $_ -match "Guru|abort|panic|error" } | ForEach-Object { Write-Output "  !! $_" }
}
Write-Output "MATRIX DONE"
