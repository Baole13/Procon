param(
    [string]$MatchId,
    [string[]]$MatchIds = @("m-0598", "m-0600", "m-0603", "m-0604"),
    [int[]]$Budgets = @(20, 50, 100, 200, 500, 1000),
    [int]$DaySeconds = 5,
    [int]$DeadlineMarginMs = 4000
)

$bot = Join-Path $PSScriptRoot "..\bot.exe"
if (-not (Test-Path -LiteralPath $bot)) {
    throw "Bot not found: $bot"
}

if ($MatchId) {
    $MatchIds = @($MatchId)
}

foreach ($currentMatchId in $MatchIds) {
    $replay = Join-Path $PSScriptRoot "..\matches\$currentMatchId\replay.jsonl"
    if (-not (Test-Path -LiteralPath $replay)) {
        Write-Warning "Replay not found: $replay"
        continue
    }
    if ($MatchIds.Count -gt 1) {
        "--- $currentMatchId"
    }
foreach ($budget in $Budgets) {
    $output = Get-Content -LiteralPath $replay | ForEach-Object {
        $message = $_ | ConvertFrom-Json
        if ($null -ne $message.day) {
            $message | Add-Member -NotePropertyName _hardBudgetMs -NotePropertyValue $budget
            $message | Add-Member -NotePropertyName _daySeconds -NotePropertyValue $DaySeconds
            $message | Add-Member -NotePropertyName _deadlineMarginMs -NotePropertyValue $DeadlineMarginMs
        }
        $message | ConvertTo-Json -Compress -Depth 100
    } | & $bot 2>&1

    $selected = @{}
    $reasons = @{}
    $evals = @{}
    $maxCompute = 0
    foreach ($line in $output) {
        $text = "$line"
        if ($text -match "plan_compare day=(\d+) fast_brands=(\d+) fast_portions=(\d+) fast_server=(\d+).*strong_brands=(-?\d+) strong_portions=(-?\d+) strong_server=(-?\d+)") {
            $evals[[int]$Matches[1]] = @{
                FastBrands = [int]$Matches[2]
                FastPortions = [int]$Matches[3]
                FastServer = [int]$Matches[4]
                StrongBrands = [int]$Matches[5]
                StrongPortions = [int]$Matches[6]
                StrongServer = [int]$Matches[7]
            }
        }
        if ($text -match "plan_compare day=(\d+) selected=([a-z_]+)(?: selected_reason=([a-z_]+))?") {
            $selected[[int]$Matches[1]] = $Matches[2]
            if ($Matches[3]) {
                $reason = $Matches[3]
                if (-not $reasons.ContainsKey($reason)) { $reasons[$reason] = 0 }
                $reasons[$reason] += 1
            }
        }
        if ($text -match "planner_timing day=\d+ compute_ms=(\d+)") {
            $maxCompute = [Math]::Max($maxCompute, [int]$Matches[1])
        }
    }

    $brands = 0
    $portions = 0
    $server = 0
    $selectedCounts = @{}
    foreach ($day in $evals.Keys) {
        $eval = $evals[$day]
        $profile = $selected[$day]
        if (-not $profile) { $profile = "fast_baseline" }
        if (-not $selectedCounts.ContainsKey($profile)) { $selectedCounts[$profile] = 0 }
        $selectedCounts[$profile] += 1
        if ($profile -eq "strong") {
            $brands += $eval.StrongBrands
            $portions += $eval.StrongPortions
            $server += $eval.StrongServer
        } else {
            $brands += $eval.FastBrands
            $portions += $eval.FastPortions
            $server += $eval.FastServer
        }
    }
    $selectedText = ($selectedCounts.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name):$($_.Value)" }) -join ","
    $reasonText = ($reasons.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name):$($_.Value)" }) -join ","
    "budget_ms=$budget brands_sum=$brands portions_sum=$portions server_sum=$server max_compute_ms=$maxCompute selected=$selectedText reasons=$reasonText"
}
}
