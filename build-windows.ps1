Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process

& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64

cmake --workflow --preset windows-debug-msvc
cmake --workflow --preset windows-debug-msvc-asan
cmake --workflow --preset windows-release-msvc
cmake --workflow --preset windows-release-msvc-asan
cmake --workflow --preset windows-debug-ninja
cmake --workflow --preset windows-debug-ninja-asan
cmake --workflow --preset windows-release-ninja-asan
