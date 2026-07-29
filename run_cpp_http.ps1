param(
    [string]$BaseUrl = "https://procon.ptit.edu.vn",
    [Parameter(Mandatory = $true)][string]$MatchId,
    [string]$Token = $env:HEX_TOKEN,
    [string]$BotPath = ".\bot.exe",
    [int]$PollMs = 120,
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
    throw "Bot executable not found at $BotPath. Build it first: g++ -std=c++17 -O2 -Wall -o bot.exe main.cpp"
}

$runtimeBins = @(
    "C:\msys64\ucrt64\bin",
    "C:\msys64\mingw64\bin",
    "C:\msys64\clang64\bin"
)
foreach ($bin in $runtimeBins) {
    if ((Test-Path -LiteralPath $bin) -and $env:PATH -notlike "*$bin*") {
        $env:PATH = "$bin;$env:PATH"
        break
    }
}

$api = "$BaseUrl/api/v1/matches/$MatchId"
$headers = @{
    Authorization = "Bearer $Token"
    "Content-Type" = "application/json"
}

$matchDir = Join-Path "matches" $MatchId
New-Item -ItemType Directory -Force -Path $matchDir | Out-Null
$httpLogPath = Join-Path $matchDir "http.log"
$replayPath = Join-Path $matchDir "replay.jsonl"
if (Test-Path -LiteralPath $httpLogPath) { Remove-Item -LiteralPath $httpLogPath }
if (Test-Path -LiteralPath $replayPath) { Remove-Item -LiteralPath $replayPath }

function Write-RunLog {
    param([string]$Message)
    $line = "$(Get-Date -Format o) $Message"
    Add-Content -LiteralPath $httpLogPath -Value $line
    Write-Host $Message
}

function Save-Json {
    param([string]$Path, [object]$Value)
    $Value | ConvertTo-Json -Compress -Depth 80 | Set-Content -LiteralPath $Path
}

function Append-Replay {
    param([object]$Value)
    $Value | ConvertTo-Json -Compress -Depth 80 | Add-Content -LiteralPath $replayPath
}

function Invoke-MatchApi {
    param(
        [string]$Method,
        [string]$Path,
        [object]$Body = $null
    )

    $params = @{
        Method = $Method
        Uri = "$api$Path"
        Headers = $headers
        TimeoutSec = $TimeoutSec
    }
    if ($null -ne $Body) {
        $params.Body = ($Body | ConvertTo-Json -Compress -Depth 50)
    }
    $started = Get-Date
    try {
        $response = Invoke-WebRequest @params
        $elapsed = [int]((Get-Date) - $started).TotalMilliseconds
        $content = $response.Content
        Write-RunLog "HTTP $Method $Path -> $($response.StatusCode) ${elapsed}ms"
        if ([string]::IsNullOrWhiteSpace($content)) { return $null }
        return $content | ConvertFrom-Json
    } catch {
        $elapsed = [int]((Get-Date) - $started).TotalMilliseconds
        $status = "ERR"
        $body = ""
        if ($_.Exception.Response) {
            try {
                $status = [int]$_.Exception.Response.StatusCode
                $stream = $_.Exception.Response.GetResponseStream()
                if ($stream) {
                    $reader = New-Object System.IO.StreamReader($stream)
                    $body = $reader.ReadToEnd()
                }
            } catch {
                $body = ""
            }
        }
        Write-RunLog "HTTP $Method $Path -> $status ${elapsed}ms $($_.Exception.Message) $body"
        throw
    }
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = (Resolve-Path -LiteralPath $BotPath).Path
$psi.WorkingDirectory = (Get-Location).Path
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$bot = New-Object System.Diagnostics.Process
$bot.StartInfo = $psi
[void]$bot.Start()

try {
    $setup = $null
    while ($null -eq $setup) {
        try {
            $setup = Invoke-MatchApi -Method GET -Path "/setup"
        } catch {
            Start-Sleep -Milliseconds $PollMs
        }
    }

    $setupJson = $setup | ConvertTo-Json -Compress -Depth 50
    Save-Json -Path (Join-Path $matchDir "setup.json") -Value $setup
    Append-Replay -Value $setup
    $bot.StandardInput.WriteLine($setupJson)
    $botStarted = Get-Date
    $assignmentLine = $bot.StandardOutput.ReadLine()
    $botMs = [int]((Get-Date) - $botStarted).TotalMilliseconds
    if (-not $assignmentLine) {
        throw "C++ bot did not return assignment."
    }
    Set-Content -LiteralPath (Join-Path $matchDir "assignment.raw.json") -Value $assignmentLine
    $assignment = $assignmentLine | ConvertFrom-Json
    Invoke-MatchApi -Method POST -Path "/assignment" -Body $assignment | Out-Null
    Write-RunLog "Assignment submitted for match $MatchId bot_ms=$botMs"

    $currentDay = -1
    $stateErrorCount = 0
    while (-not $bot.HasExited) {
        try {
            $state = Invoke-MatchApi -Method GET -Path "/state"
            $stateErrorCount = 0
            if ($null -ne $state -and $state.day -ne $currentDay) {
                if ($currentDay -ge 0 -and $state.day -ne ($currentDay + 1)) {
                    Write-RunLog "WARNING day jump $currentDay -> $($state.day)"
                }
                $stateJson = $state | ConvertTo-Json -Compress -Depth 50
                Save-Json -Path (Join-Path $matchDir "day-$($state.day)-state.json") -Value $state
                Append-Replay -Value $state
                $bot.StandardInput.WriteLine($stateJson)
                $botStarted = Get-Date
                $actionsLine = $bot.StandardOutput.ReadLine()
                $botMs = [int]((Get-Date) - $botStarted).TotalMilliseconds
                if (-not $actionsLine) {
                    throw "C++ bot did not return actions for day $($state.day)."
                }
                Set-Content -LiteralPath (Join-Path $matchDir "day-$($state.day)-actions.raw.json") -Value $actionsLine
                $actions = $actionsLine | ConvertFrom-Json
                Invoke-MatchApi -Method POST -Path "/actions" -Body $actions | Out-Null
                $currentDay = $state.day
                Write-RunLog "Submitted day $currentDay bot_ms=$botMs"
            }
        } catch {
            $stateErrorCount++
            try {
                if ($stateErrorCount -ge 4) {
                    $result = Invoke-MatchApi -Method GET -Path "/result"
                    if ($null -ne $result) {
                        Save-Json -Path (Join-Path $matchDir "result.json") -Value $result
                        $bot.StandardInput.WriteLine(($result | ConvertTo-Json -Compress -Depth 50))
                        Write-RunLog "Match finished"
                        $result | ConvertTo-Json -Depth 50
                        break
                    }
                }
            } catch {
                Start-Sleep -Milliseconds $PollMs
            }
        }
        Start-Sleep -Milliseconds $PollMs
    }
} finally {
    if (-not $bot.HasExited) {
        $bot.Kill()
    }
    $stderr = $bot.StandardError.ReadToEnd()
    if ($stderr) {
        $stderr | Set-Content -LiteralPath "run_$MatchId.err.log"
        $stderr | Set-Content -LiteralPath (Join-Path $matchDir "bot.stderr.log")
    }
}
