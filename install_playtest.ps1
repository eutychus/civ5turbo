param(
    [ValidateSet("Install", "Restore", "Status")]
    [string]$Action = "Status",
    [string]$Civ5Dir = "C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V"
)

$ErrorActionPreference = "Stop"
$TargetDll = Join-Path $Civ5Dir "lua51_Win32.dll"
$BackupDll = Join-Path $Civ5Dir "lua51_Win32.dll.vanilla.bak"
$BuiltDll  = Join-Path $PSScriptRoot "dist\lua51_Win32.dll"

$VanillaSha256 = "F58CEFFEB317E42B7691D823B0DB4DA63DA0B988D2B381CCEC9C3C18A58C0E35"

if (-not (Test-Path $Civ5Dir)) {
    Write-Error "Civilization V directory not found at: $Civ5Dir"
}

switch ($Action) {
    "Status" {
        $currentHash = (Get-FileHash $TargetDll -Algorithm SHA256).Hash
        $builtHash   = (Get-FileHash $BuiltDll -Algorithm SHA256).Hash
        Write-Host "Target DLL : $TargetDll"
        Write-Host "SHA256     : $currentHash"
        if ($currentHash -eq $VanillaSha256) {
            Write-Host "State      : VANILLA (Firaxis Lua 5.1.4)" -ForegroundColor Cyan
        } elseif ($currentHash -eq $builtHash) {
            Write-Host "State      : ACTIVE (Hybrid LuaJIT 2.1 + Civ 5 Native C Accelerator)" -ForegroundColor Green
        } else {
            Write-Host "State      : CUSTOM / UNKNOWN" -ForegroundColor Yellow
        }
        Write-Host "Backup Exists: $(Test-Path $BackupDll)"
    }
    "Install" {
        $currentHash = (Get-FileHash $TargetDll -Algorithm SHA256).Hash
        if ($currentHash -eq $VanillaSha256 -and -not (Test-Path $BackupDll)) {
            Copy-Item -Path $TargetDll -Destination $BackupDll -Force
            Write-Host "Backed up vanilla lua51_Win32.dll -> $BackupDll" -ForegroundColor Cyan
        }
        Copy-Item -Path $BuiltDll -Destination $TargetDll -Force
        Write-Host "Installed Hybrid LuaJIT 2.1 + Civ 5 Native C Accelerator -> $TargetDll" -ForegroundColor Green
    }
    "Restore" {
        if (-not (Test-Path $BackupDll)) {
            Write-Error "Backup file not found at: $BackupDll"
        }
        Copy-Item -Path $BackupDll -Destination $TargetDll -Force
        Write-Host "Restored vanilla Firaxis lua51_Win32.dll from backup." -ForegroundColor Cyan
    }
}
