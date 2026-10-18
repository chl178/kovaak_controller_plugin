# build.ps1 — 用 w64devkit (MinGW GCC) 编译 kovaak_controller.exe
# 与 APEX SIMULATOR gdext_rawinput 使用同一工具链。
$ErrorActionPreference = "Stop"

$mingwBin = "C:\Users\liu\.codex\tools\w64devkit\w64devkit\bin"
if (-not (Test-Path "$mingwBin\g++.exe")) {
    # 回退：PATH 上的 g++
    $gxx = "g++"
} else {
    $gxx = "$mingwBin\g++.exe"
}

$root = $PSScriptRoot
New-Item -ItemType Directory -Force -Path "$root\build" | Out-Null

& $gxx -O2 -std=c++17 -Wall -Wextra `
    -I"$root\src" `
    "$root\src\main.cpp" "$root\src\rawinput_reader.cpp" "$root\src\kovaak_config.cpp" `
    -o "$root\build\kovaak_controller.exe" `
    -static -luser32 -lhid -lsetupapi -lwinmm
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "OK -> $root\build\kovaak_controller.exe"
& "$root\build\kovaak_controller.exe" --selftest
