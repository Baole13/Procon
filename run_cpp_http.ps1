param(
    [string]$BaseUrl = "https://procon.ptit.edu.vn",
    [Parameter(Mandatory = $true)][string]$MatchId,
    [string]$Token = $env:HEX_TOKEN,
    [string]$BotPath = ".\bot.exe",
    [int]$PollMs = 250,
    [int]$TimeoutSec = 2
)

if (-not $Token -and (Test-Path -LiteralPath ".env")) {
    Get-Content -LiteralPath ".env" | ForEach-Object {
        if ($_ -match '^\s*HEX_TOKEN\s*=\s*(.+?)\s*$') {
            $Token = $matches[1]
        }
    }
}

if (-not $Token) {
    throw "Missing token. Pass -Token or set HEX_TOKEN in the environment."
}

if (-not (Test-Path -LiteralPath $BotPath)) {
    throw "Bot executable not found at $BotPath. Build it first: .\build_cpp.ps1"
}

python .\run_cpp_http.py `
    --url $BaseUrl `
    --match $MatchId `
    --token $Token `
    --bot $BotPath `
    --poll-ms $PollMs `
    --short-poll-ms 250 `
    --timeout $TimeoutSec `
    --action-timeout ([Math]::Min(1.0, [double]$TimeoutSec)) `
    --min-request-interval-ms 250

if ($LASTEXITCODE -ne 0) {
    throw "run_cpp_http.py failed with exit code $LASTEXITCODE"
}
