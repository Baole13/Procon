param(
    [string]$Output = "bot.exe"
)

$candidates = @()

$cmd = Get-Command g++ -ErrorAction SilentlyContinue
if ($cmd) { $candidates += $cmd.Source }

$knownPaths = @(
    "C:\msys64\ucrt64\bin\g++.exe",
    "C:\msys64\mingw64\bin\g++.exe",
    "C:\msys64\clang64\bin\g++.exe",
    "C:\mingw64\bin\g++.exe",
    "C:\MinGW\bin\g++.exe"
)

foreach ($path in $knownPaths) {
    if (Test-Path -LiteralPath $path) { $candidates += $path }
}

if ($candidates.Count -eq 0) {
    throw @"
No C++ compiler found.

Install MSYS2/MinGW-w64, then reopen PowerShell:
  winget install MSYS2.MSYS2
  C:\msys64\usr\bin\bash.exe -lc "pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc make"

Then build:
  .\build_cpp.ps1
"@
}

$compiler = $candidates[0]
Write-Host "Using compiler: $compiler"
$compilerBin = Split-Path -Parent $compiler
if ($env:PATH -notlike "*$compilerBin*") {
    $env:PATH = "$compilerBin;$env:PATH"
}
& $compiler -std=c++17 -O2 -Wall -static -static-libgcc -static-libstdc++ -o $Output main.cpp
if ($LASTEXITCODE -ne 0) {
    throw "C++ build failed."
}

Write-Host "Built $Output"
