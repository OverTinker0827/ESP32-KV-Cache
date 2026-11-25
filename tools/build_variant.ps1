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
$args_ = @("-C", $proj, "-B", $bdir, "-D", "SDKCONFIG=$bdir\sdkconfig", "-D", "SDKCONFIG_DEFAULTS=$defaults")
# global options (-p) must precede the actions
if ($Flash) { $args_ += @("-p", $Port, "build", "flash") } else { $args_ += @("build") }
idf.py @args_
if ($LASTEXITCODE -ne 0) { throw "idf.py failed ($LASTEXITCODE)" }
