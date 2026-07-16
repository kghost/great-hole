param(
    [Parameter(Mandatory = $true)]
    [string]$GreatHoleDir,
    [Parameter(Mandatory = $true)]
    [string]$DesktopHoleDir
)

$SrcHeader = "$GreatHoleDir\src\interface\Interface.hpp"
$DestHeaderDir = "$DesktopHoleDir\src-tauri\src\cxxbridge"
$DestHeader = "$DestHeaderDir\Interface.hpp"

$BuildDir = "$GreatHoleDir\build-windows-msvc"
$DestBinDir = "$DesktopHoleDir\src-tauri\libs"

# Create directories if they do not exist
if (!(Test-Path -Path $DestHeaderDir)) {
    New-Item -ItemType Directory -Force -Path $DestHeaderDir | Out-Null
}

# Copy Interface.hpp
Write-Host "Copying Interface.hpp..."
Copy-Item -Path $SrcHeader -Destination $DestHeader -Force

# Configurations to process
$Configs = @("Debug", "Release")

foreach ($Config in $Configs) {
    $ConfigLower = $Config.ToLower()
    $ConfigDestDir = Join-Path $DestBinDir $ConfigLower

    if (!(Test-Path -Path $ConfigDestDir)) {
        New-Item -ItemType Directory -Force -Path $ConfigDestDir | Out-Null
    }

    # Explicit map of required binaries and their relative paths inside build-windows-msvc directory
    if ($Config -eq "Debug") {
        $BinariesMap = @{
            "great-hole-windows.dll"                    = "src/windows/Debug/great-hole-windows.dll"
            "great-hole-windows.lib"                    = "src/windows/Debug/great-hole-windows.lib"
            "great-hole-windows.pdb"                    = "src/windows/Debug/great-hole-windows.pdb"
            "great-hole-windows-asan.dll"               = "src/windows/Debug/great-hole-windows-asan.dll"
            "great-hole-windows-asan.lib"               = "src/windows/Debug/great-hole-windows-asan.lib"
            "great-hole-windows-asan.pdb"               = "src/windows/Debug/great-hole-windows-asan.pdb"
            "WinDivert.dll"                             = "_deps/windivert-src/x64/WinDivert.dll"
            "WinDivert64.sys"                           = "_deps/windivert-src/x64/WinDivert64.sys"
            "cares.dll"                                 = "src/windows/Debug/cares.dll"
            "boost_filesystem-vc145-mt-gd-x64-1_90.dll" = "src/windows/Debug/boost_filesystem-vc145-mt-gd-x64-1_90.dll"
            "boost_log-vc145-mt-gd-x64-1_90.dll"        = "src/windows/Debug/boost_log-vc145-mt-gd-x64-1_90.dll"
            "boost_thread-vc145-mt-gd-x64-1_90.dll"     = "src/windows/Debug/boost_thread-vc145-mt-gd-x64-1_90.dll"
        }
    }
    else {
        $BinariesMap = @{
            "great-hole-windows.dll"                 = "src/windows/Release/great-hole-windows.dll"
            "great-hole-windows.lib"                 = "src/windows/Release/great-hole-windows.lib"
            "great-hole-windows.pdb"                 = "src/windows/Release/great-hole-windows.pdb"
            "great-hole-windows-asan.dll"            = "src/windows/Release/great-hole-windows-asan.dll"
            "great-hole-windows-asan.lib"            = "src/windows/Release/great-hole-windows-asan.lib"
            "great-hole-windows-asan.pdb"            = "src/windows/Release/great-hole-windows-asan.pdb"
            "WinDivert.dll"                          = "_deps/windivert-src/x64/WinDivert.dll"
            "WinDivert64.sys"                        = "_deps/windivert-src/x64/WinDivert64.sys"
            "cares.dll"                              = "src/windows/Release/cares.dll"
            "boost_filesystem-vc145-mt-x64-1_90.dll" = "src/windows/Release/boost_filesystem-vc145-mt-x64-1_90.dll"
            "boost_log-vc145-mt-x64-1_90.dll"        = "src/windows/Release/boost_log-vc145-mt-x64-1_90.dll"
            "boost_thread-vc145-mt-x64-1_90.dll"     = "src/windows/Release/boost_thread-vc145-mt-x64-1_90.dll"
        }
    }

    # Copy DLL, LIB, PDB and SYS files
    Write-Host "Copying explicit binaries for $Config configuration to $ConfigDestDir..."
    foreach ($File in $BinariesMap.Keys) {
        $RelPath = $BinariesMap[$File]
        $SrcFile = Join-Path $BuildDir $RelPath
        $DestFile = Join-Path $ConfigDestDir $File
        if (Test-Path -Path $SrcFile) {
            Write-Host "Copying $File to $DestFile..."
            Copy-Item -Path $SrcFile -Destination $DestFile -Force
        }
        else {
            if ($File.EndsWith(".pdb") -and $Config -eq "Release") {
                # PDB is expectedly missing in Release build
                continue
            }
            Write-Warning "Source file not found: $SrcFile"
        }
    }
}

Write-Host "Sync complete!"
