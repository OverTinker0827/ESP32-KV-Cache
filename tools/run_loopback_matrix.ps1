# Build + flash + run the end-to-end loopback test (real server, no WiFi) for each server design.
# Saves RESULT lines to results\<Prefix>_<name>.jsonl and prints the functional-test summary.
param([string]$Prefix = "entry5", [string[]]$Only)
$root = Split-Path -Parent $PSScriptRoot
$py = Join-Path $root ".venv\Scripts\python.exe"
$lb = @("nowifi", "loopback")
$matrix = [ordered]@{
    "lb_func40_opt1"    = @("opt1", "netopt") + $lb + @("lbfunc40")  # Entry 5 engine, functional suite at the deployment reserve
    "lb_func40_opt2"    = @("opt", "netopt") + $lb + @("lbfunc40")   # v2 engine, same
    "lb_v2"             = @("opt", "netopt") + $lb                     # integrated server: v2 engine, 1 loop core 1, core locking
    "lb_v2_prefix"      = @("opt", "netopt", "prefix") + $lb          # same + prefix compression
    "lb_basic"          = @("basic") + $lb                        # basic engine + task-per-client, stock lwIP
    "lb_single"         = @("opt", "netopt") + $lb                # one loop on core 1 (default)
    "lb_single0"        = @("opt", "netopt", "single0") + $lb     # one loop on core 0, next to lwIP
    "lb_shared_seq"     = @("opt", "netopt", "shared") + $lb      # two loops, one table, seqlock
    "lb_sharded_seq"    = @("opt", "netopt", "sharded") + $lb     # two loops, 8 shards, seqlock
    "lb_single_cl"      = @("opt", "netopt", "corelock") + $lb    # one loop core 1 + lwIP core locking
}
foreach ($name in $matrix.Keys) {
    if ($Only -and -not ($Only -contains $name)) { continue }
    Write-Output "=== $name : $($matrix[$name] -join ',')"
    & (Join-Path $PSScriptRoot "build_variant.ps1") -Name $name -Cfg $matrix[$name] -Flash *> (Join-Path $root "build\$name.buildlog")
    if ($LASTEXITCODE -ne 0) {
        # one retry: this board's USB/power sometimes drops mid-flash
        & (Join-Path $PSScriptRoot "build_variant.ps1") -Name $name -Cfg $matrix[$name] -Flash *> (Join-Path $root "build\$name.buildlog")
        if ($LASTEXITCODE -ne 0) { Write-Output "BUILD/FLASH FAILED for $name"; continue }
    }
    $out = & $py (Join-Path $PSScriptRoot "serial_wait.py") COM5 "DONE|Guru|abort|panic" 300
    $res = $out | Where-Object { $_ -like "RESULT *" } | ForEach-Object { $_.Substring(7) }
    $res | Set-Content -Encoding utf8 (Join-Path $root "results\${Prefix}_$name.jsonl")
    $out | Where-Object { $_ -match "^TEST |LBDIAG|Guru|abort|panic" } | Select-Object -First 12 | ForEach-Object { Write-Output "  $_" }
    Write-Output "  $($res.Count) result lines"
}
Write-Output "MATRIX DONE"
