param(
    [Parameter(Mandatory=$true)]
    [string]$Replay,
    [int[]]$Budgets = @(200, 1000, 5000),
    [string]$Bot = ".\bot.exe"
)

if (!(Test-Path $Replay)) {
    throw "Replay not found: $Replay"
}
if (!(Test-Path $Bot)) {
    throw "Bot not found: $Bot"
}

$results = @()
foreach ($budget in $Budgets) {
    $tmpIn = New-TemporaryFile
    $tmpOut = New-TemporaryFile
    $tmpErr = New-TemporaryFile
    try {
        Get-Content $Replay | ForEach-Object {
            if ([string]::IsNullOrWhiteSpace($_)) { return }
            $line = $_
            if ($line -match '"day"\s*:' -and $line -match '"agents"\s*:') {
                $meta = ',"_deadlineMarginMs":1000000,"_hardBudgetMs":' + $budget + ',"_daySeconds":5,"_ultraFastMode":0'
                $line = $line -replace '}\s*$', ($meta + '}')
            }
            $line
        } | Set-Content -Encoding ASCII $tmpIn

        $botPath = (Resolve-Path $Bot).Path
        cmd /c "`"$botPath`" < `"$tmpIn`" > `"$tmpOut`" 2> `"$tmpErr`""
        if ($LASTEXITCODE -ne 0) {
            throw "Bot failed for budget ${budget}ms"
        }

        $server = 0
        $brands = 0
        $negativeSlack = 0
        foreach ($line in Get-Content $tmpErr) {
            if ($line -match "final_plan_summary .*final_server_est=(\d+).*final_negative_slack=(-?\d+)") {
                $server += [int]$Matches[1]
                $negativeSlack += [int]$Matches[2]
            }
            if ($line -match "final_plan_summary .*final_daily_brands=(\d+)") {
                $brands += [int]$Matches[1]
            }
        }
        $results += [pscustomobject]@{
            budget_ms = $budget
            server_sum = $server
            daily_brands_sum = $brands
            negative_slack_sum = $negativeSlack
        }
    } finally {
        Remove-Item -Force $tmpIn,$tmpOut,$tmpErr -ErrorAction SilentlyContinue
    }
}

$results | Format-Table -AutoSize

$baseline = $results | Where-Object { $_.budget_ms -eq $Budgets[0] } | Select-Object -First 1
foreach ($row in $results) {
    if ($row.server_sum -lt $baseline.server_sum) {
        throw "Budget monotonic failed: $($row.budget_ms)ms server_sum $($row.server_sum) < baseline $($baseline.server_sum)"
    }
    if ($row.negative_slack_sum -ne 0) {
        throw "Feasibility failed: $($row.budget_ms)ms negative_slack_sum=$($row.negative_slack_sum)"
    }
}
