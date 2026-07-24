Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process

& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64

cmake --workflow --preset windows-msvc-debug
cmake --workflow --preset windows-msvc-debug-asan
cmake --workflow --preset windows-msvc-release
cmake --workflow --preset windows-msvc-release-asan
cmake --workflow --preset windows-ninja-debug
cmake --workflow --preset windows-ninja-release
cmake --workflow --preset windows-ninja-debug-asan
cmake --workflow --preset windows-ninja-release-asan

New-Item -ItemType Junction -Path "build" -Target "build-windows-ninja-debug-asan"
