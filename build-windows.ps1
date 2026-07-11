Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process

& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64

cmake --workflow --preset windows-debug-msvc
cmake --workflow --preset windows-debug-ninja
