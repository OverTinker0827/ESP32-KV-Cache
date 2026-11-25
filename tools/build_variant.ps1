# Build (and optionally flash) one ESP32 KV Cache variant in its own build directory.
# Usage: .\tools\build_variant.ps1 -Name basic -Cfg basic[,netopt,...] [-Flash] [-Port COM5]
# Each variant = sdkconfig.defaults + the listed variants\*.cfg fragments (later ones win).
param(
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][string[]]$Cfg,
    [switch]$Flash,
    [string]$Port = "COM5"
)
$ErrorActionPreference = "Continue"  # IDF tools write progress to stderr; check exit codes instead
$root = Split-Path -Parent $PSScriptRoot
$proj = Join-Path $root "kvcache"
$bdir = Join-Path $root "build\cc_$Name"
. C:\Users\preet\esp\esp-idf\export.ps1 *> $null
$frags = @("sdkconfig.defaults") + ($Cfg | ForEach-Object { "variants/$_.cfg" })
$defaults = $frags -join ";"
New-Item -ItemType Directory -Force $bdir | Out-Null
# ESP-IDF applies SDKCONFIG_DEFAULTS only when sdkconfig is first created: a fragment edited later
# is silently ignored (this made the first Entry 5 loopback rerun use a stale 80 KB reserve and an
# 8-packet loopback queue). Every setting lives in the fragments, so always regenerate.
Remove-Item (Join-Path $bdir "sdkconfig") -ErrorAction SilentlyContinue
$args_ = @("-C", $proj, "-B", $bdir, "-D", "SDKCONFIG=$bdir\sdkconfig", "-D", "SDKCONFIG_DEFAULTS=$defaults")
# global options (-p) must precede the actions
# 230400 baud: this board's USB/power dropped out at 91 % at the default 460800
if ($Flash) { $args_ += @("-p", $Port, "-b", "230400", "build", "flash") } else { $args_ += @("build") }
idf.py @args_
if ($LASTEXITCODE -ne 0) { throw "idf.py failed ($LASTEXITCODE)" }
