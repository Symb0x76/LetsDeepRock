[CmdletBinding()]
param(
    [string]$EditorCmd = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',

    [string]$UProject = (Join-Path (Split-Path $PSScriptRoot -Parent) 'LetsDeepRock.uproject'),

    [string]$Map = '/Game/GridMap/Maps/L_GridMapStage4Test',

    [int]$StartupTimeoutSeconds = 300,

    # Host 在 SPAWNED 之后多少秒执行回收（DespawnAll + 远处重生）
    [int]$RecycleDelaySeconds = 15,

    # 限帧，避免 3 个 -nullrhi 进程空转抢 CPU
    [int]$MaxFps = 60,

    [string]$OutputRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) (Join-Path 'Saved\SwarmNetSmoke' (Get-Date -Format 'yyyyMMdd-HHmmss')))
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---- 断言阈值（与计划 Task B 一致） ----
$MinVisible = 27
$VisibleWithinMs = 10000
$MaxMeanOffsetUu = 300.0
$KillObserveMs = 5000

function Get-NowMs { [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() }

function ConvertFrom-SmokeMarker {
    param([string]$Line)

    $markerIndex = $Line.IndexOf('WSH|')
    if ($markerIndex -lt 0) {
        return $null
    }
    $fields = @{}
    foreach ($part in $Line.Substring($markerIndex).Split('|')) {
        $separator = $part.IndexOf('=')
        if ($separator -gt 0) {
            $fields[$part.Substring(0, $separator)] = $part.Substring($separator + 1).Trim()
        }
    }
    return $fields
}

# 增量读取日志（UE 持有写句柄，需 FileShare.ReadWrite），缓存全部行与解析出的标记
function Update-SmokeLog {
    param([pscustomobject]$ProcessInfo)

    if (-not (Test-Path -LiteralPath $ProcessInfo.LogPath)) {
        return
    }
    $stream = [IO.File]::Open($ProcessInfo.LogPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        if ($stream.Length -le $ProcessInfo.Offset) {
            return
        }
        $null = $stream.Seek($ProcessInfo.Offset, [IO.SeekOrigin]::Begin)
        $buffer = New-Object byte[] ($stream.Length - $ProcessInfo.Offset)
        $read = $stream.Read($buffer, 0, $buffer.Length)
        $ProcessInfo.Offset += $read
        $text = $ProcessInfo.Partial + [Text.Encoding]::UTF8.GetString($buffer, 0, $read)
    } finally {
        $stream.Dispose()
    }
    $parts = $text -split "`r?`n"
    $ProcessInfo.Partial = $parts[-1]
    for ($i = 0; $i -lt $parts.Count - 1; $i++) {
        $line = $parts[$i]
        $ProcessInfo.Lines.Add($line)
        if ($line.Contains("WSH|Run=$($ProcessInfo.RunId)|Process=$($ProcessInfo.Label)|")) {
            $fields = ConvertFrom-SmokeMarker -Line $line
            if ($null -ne $fields) {
                $fields['LineIndex'] = $ProcessInfo.Lines.Count - 1
                $ProcessInfo.Markers.Add($fields)
            }
        }
    }
}

function Get-SmokeMarkers {
    param([pscustomobject]$ProcessInfo, [string]$Event)

    Update-SmokeLog -ProcessInfo $ProcessInfo
    return @($ProcessInfo.Markers | Where-Object { $_['Event'] -eq $Event })
}

function Wait-SmokeMarker {
    param(
        [pscustomobject]$ProcessInfo,
        [string[]]$Event,
        [int]$TimeoutSeconds,
        [scriptblock]$Where = { $true },
        [switch]$AllowTimeout
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ($true) {
        Update-SmokeLog -ProcessInfo $ProcessInfo
        foreach ($marker in $ProcessInfo.Markers) {
            if ($Event -contains $marker['Event'] -and (& $Where $marker)) {
                return $marker
            }
        }
        $ProcessInfo.Process.Refresh()
        if ($ProcessInfo.Process.HasExited) {
            $tail = ($ProcessInfo.Lines | Select-Object -Last 30) -join [Environment]::NewLine
            throw "$($ProcessInfo.Label) exited before Event=$($Event -join ',') ExitCode=$($ProcessInfo.Process.ExitCode)`n$tail"
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            if ($AllowTimeout) {
                return $null
            }
            throw "Timed out after $TimeoutSeconds s waiting for $($ProcessInfo.Label) Event=$($Event -join ',')"
        }
        Start-Sleep -Milliseconds 250
    }
}

function Start-SmokeProcess {
    param(
        [string]$Label,
        [string]$Role,
        [string]$RunId,
        [string]$OutputDir,
        [string[]]$ExtraArguments = @()
    )

    $logPath = Join-Path $OutputDir "$Label.log"
    $arguments = @(
        "`"$UProject`"",
        '-game', '-nullrhi', '-unattended', '-nosplash', '-nosound', '-NoLiveCoding', '-NoLoadingScreen',
        # 不拉起常驻的 UnrealTraceServer，确保脚本结束后不留下进程
        '-notraceserver',
        '-log', '-FORCELOGFLUSH', "-abslog=`"$logPath`"",
        "-ExecCmds=`"t.MaxFPS $MaxFps`"",
        "-WeaponSwarmHarnessRole=$Role",
        "-WeaponSwarmHarnessRun=$RunId",
        "-WeaponSwarmHarnessLabel=$Label",
        "-WeaponSwarmHarnessMap=$Map"
    ) + $ExtraArguments
    $process = Start-Process -FilePath $EditorCmd -ArgumentList $arguments -PassThru -WindowStyle Hidden
    Write-Host "Started $Label PID=$($process.Id) log=$logPath"
    return [pscustomobject]@{
        Label = $Label
        RunId = $RunId
        LogPath = $logPath
        Process = $process
        Offset = [long]0
        Partial = ''
        Lines = [Collections.Generic.List[string]]::new()
        Markers = [Collections.Generic.List[hashtable]]::new()
    }
}

function Stop-SmokeProcess {
    param([pscustomobject]$ProcessInfo)

    if ($null -eq $ProcessInfo) {
        return
    }
    $ProcessInfo.Process.Refresh()
    if (-not $ProcessInfo.Process.HasExited) {
        # /T 连同子进程（ShaderCompileWorker 等）一起结束
        & taskkill.exe /PID $ProcessInfo.Process.Id /T /F | Out-Null
        $ProcessInfo.Process.WaitForExit(15000) | Out-Null
    }
}

function Get-Dist3 {
    param([hashtable]$A, [hashtable]$B)
    $dx = [double]$A['MeanX'] - [double]$B['MeanX']
    $dy = [double]$A['MeanY'] - [double]$B['MeanY']
    $dz = [double]$A['MeanZ'] - [double]$B['MeanZ']
    return [Math]::Sqrt($dx * $dx + $dy * $dy + $dz * $dz)
}

$results = [Collections.Generic.List[object]]::new()
function Add-Result {
    param([string]$Check, [string]$Measured, [string]$Threshold, [Nullable[bool]]$Passed)
    $status = if ($null -eq $Passed) { 'INFO' } elseif ($Passed) { 'PASS' } else { 'FAIL' }
    $results.Add([pscustomobject]@{ Check = $Check; Measured = $Measured; Threshold = $Threshold; Result = $status })
}

New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$OutputRoot = (Resolve-Path -LiteralPath $OutputRoot).Path
$UProject = (Resolve-Path -LiteralPath $UProject).Path
$runId = [Guid]::NewGuid().ToString('N').Substring(0, 12)
$processes = @()
$fatal = $null
$hostInfo = $null; $client1 = $null; $client2 = $null
$spawned = $null; $recycled = $null; $c2Join = $null; $killMs = $null

Write-Host "Swarm net smoke Run=$runId Output=$OutputRoot"
try {
    $hostInfo = Start-SmokeProcess -Label 'Host' -Role 'Host' -RunId $runId -OutputDir $OutputRoot -ExtraArguments @("-WeaponSwarmHarnessRecycleDelay=$RecycleDelaySeconds")
    $processes += $hostInfo
    Wait-SmokeMarker -ProcessInfo $hostInfo -Event 'DriverReady' -TimeoutSeconds $StartupTimeoutSeconds | Out-Null
    Wait-SmokeMarker -ProcessInfo $hostInfo -Event 'HostAdvertised' -TimeoutSeconds $StartupTimeoutSeconds | Out-Null
    Wait-SmokeMarker -ProcessInfo $hostInfo -Event 'HostReady' -TimeoutSeconds $StartupTimeoutSeconds | Out-Null
    Write-Host 'Host ready (listen + swarm backend + pawn)'

    # 1. Client1 加入 → Host 达到 2 名玩家后生成 30 只 BasicGrunt
    $client1 = Start-SmokeProcess -Label 'Client1' -Role 'Client' -RunId $runId -OutputDir $OutputRoot
    $processes += $client1
    Wait-SmokeMarker -ProcessInfo $client1 -Event 'ClientInWorld' -TimeoutSeconds $StartupTimeoutSeconds | Out-Null
    Write-Host 'Client1 in world'
    $spawned = Wait-SmokeMarker -ProcessInfo $hostInfo -Event @('SPAWNED', 'SPAWN_FAILED') -TimeoutSeconds 60
    if ($spawned['Event'] -ne 'SPAWNED') {
        throw "Host could not spawn $MinVisible+ BasicGrunt near its pawn (Spawned=$($spawned['Spawned']))"
    }
    $spawnMs = [long]$spawned['T']
    Write-Host "SPAWNED Spawned=$($spawned['Spawned'])"

    # 2. 等 Client1 看到虫群（条件等待，断言在后面统一计算）
    Wait-SmokeMarker -ProcessInfo $client1 -Event 'ClientSample' -TimeoutSeconds ($VisibleWithinMs / 1000 + 5) -AllowTimeout -Where {
        param($m) ([long]$m['T'] -ge $spawnMs) -and ([int]$m['Visible'] -ge $MinVisible)
    } | Out-Null

    # 4. 晚加入：Client2 在 SPAWNED 之后启动
    $client2 = Start-SmokeProcess -Label 'Client2' -Role 'Client' -RunId $runId -OutputDir $OutputRoot
    $processes += $client2

    # 3. 回收：Host 在 SPAWNED + RecycleDelay 后 DespawnAll 并在 ≥3000uu 外重生
    $recycled = Wait-SmokeMarker -ProcessInfo $hostInfo -Event @('RECYCLED', 'RECYCLE_FAILED') -TimeoutSeconds ($RecycleDelaySeconds + 60)
    if ($recycled['Event'] -ne 'RECYCLED') {
        throw "Host could not respawn BasicGrunt >= 3000 uu away (Spawned=$($recycled['Spawned']))"
    }
    $recycleMs = [long]$recycled['T']
    Write-Host "RECYCLED Spawned=$($recycled['Spawned']) MinDist=$($recycled['MinDist'])"

    $c2Join = Wait-SmokeMarker -ProcessInfo $client2 -Event 'ClientInWorld' -TimeoutSeconds $StartupTimeoutSeconds
    $c2JoinMs = [long]$c2Join['T']
    Write-Host 'Client2 in world'
    Wait-SmokeMarker -ProcessInfo $client2 -Event 'ClientSample' -TimeoutSeconds ($VisibleWithinMs / 1000 + 5) -AllowTimeout -Where {
        param($m) ([long]$m['T'] -ge $c2JoinMs) -and ([int]$m['Visible'] -ge $MinVisible)
    } | Out-Null
    # 回收后至少收集 5 秒的 Client1 滑行样本
    Wait-SmokeMarker -ProcessInfo $client1 -Event 'ClientSample' -TimeoutSeconds 30 -Where {
        param($m) [long]$m['T'] -ge ($recycleMs + 5000)
    } | Out-Null

    # 5. 断线：追逐中强杀 Client1，Host 继续运行
    # 先取时间再杀：taskkill + WaitForExit 期间 Host 可能已经收到断线
    $killMs = Get-NowMs
    Stop-SmokeProcess -ProcessInfo $client1
    Write-Host 'Client1 killed'
    Wait-SmokeMarker -ProcessInfo $hostInfo -Event 'HostSample' -TimeoutSeconds 30 -Where {
        param($m) [long]$m['T'] -ge ($killMs + $KillObserveMs + 1000)
    } | Out-Null
    # 信息项：Host 侧移除断线玩家的耗时（等待最多 60 s，不作断言）
    Wait-SmokeMarker -ProcessInfo $hostInfo -Event 'PlayerCount' -TimeoutSeconds 60 -AllowTimeout -Where {
        param($m) ([long]$m['T'] -ge $killMs) -and ([int]$m['Players'] -lt [int]$m['Previous'])
    } | Out-Null
} catch {
    $fatal = $_.Exception.Message
} finally {
    foreach ($processInfo in $processes) {
        if ($null -ne $processInfo) {
            Stop-SmokeProcess -ProcessInfo $processInfo
            Update-SmokeLog -ProcessInfo $processInfo
        }
    }
}

# ---------------- 断言 ----------------
if ($null -ne $fatal) {
    Add-Result -Check 'Scenario completed' -Measured $fatal -Threshold 'no timeout/exit' -Passed $false
}

if ($null -ne $spawned -and $spawned['Event'] -eq 'SPAWNED') {
    $spawnMs = [long]$spawned['T']
    $recycleMs = if ($null -ne $recycled) { [long]$recycled['T'] } else { [long]::MaxValue }
    $c1Samples = @(Get-SmokeMarkers -ProcessInfo $client1 -Event 'ClientSample')
    $hostSamples = @(Get-SmokeMarkers -ProcessInfo $hostInfo -Event 'HostSample')

    # A1: Client1 在 SPAWNED 后 10 s 内可见 ≥ 27
    $first = $c1Samples | Where-Object { [long]$_['T'] -ge $spawnMs -and [int]$_['Visible'] -ge $MinVisible } | Select-Object -First 1
    if ($null -ne $first) {
        $delta = [long]$first['T'] - $spawnMs
        Add-Result -Check "Client1 visible >= $MinVisible after SPAWNED" -Measured "$delta ms (Visible=$($first['Visible']))" -Threshold "<= $VisibleWithinMs ms" -Passed ($delta -le $VisibleWithinMs)
    } else {
        $maxVisible = ($c1Samples | Where-Object { [long]$_['T'] -ge $spawnMs } | ForEach-Object { [int]$_['Visible'] } | Measure-Object -Maximum).Maximum
        Add-Result -Check "Client1 visible >= $MinVisible after SPAWNED" -Measured "never (max Visible=$maxVisible)" -Threshold "<= $VisibleWithinMs ms" -Passed $false
    }

    # A2: 同一 UTC 秒的 Host/Client1 平均位置偏差（仅计数一致的样本，SPAWNED~RECYCLED 窗口）
    $hostBySec = @{}
    foreach ($h in $hostSamples) { $hostBySec[$h['Sec']] = $h }
    $offsets = @()
    foreach ($c in $c1Samples) {
        $t = [long]$c['T']
        if ($t -le $spawnMs -or $t -ge $recycleMs) { continue }
        $h = $hostBySec[$c['Sec']]
        if ($null -eq $h -or [long]$h['T'] -ge $recycleMs) { continue }
        if ([int]$h['Alive'] -lt $MinVisible -or [int]$h['Alive'] -ne [int]$c['Visible']) { continue }
        $offsets += (Get-Dist3 -A $h -B $c)
    }
    if ($offsets.Count -ge 3) {
        $stats = $offsets | Measure-Object -Maximum -Average
        Add-Result -Check 'Client1 mean pos vs host mean (same second)' -Measured ("max {0:N1} / avg {1:N1} uu over {2} samples" -f $stats.Maximum, $stats.Average, $offsets.Count) -Threshold "max <= $MaxMeanOffsetUu uu" -Passed ($stats.Maximum -le $MaxMeanOffsetUu)
    } else {
        Add-Result -Check 'Client1 mean pos vs host mean (same second)' -Measured "only $($offsets.Count) paired samples" -Threshold '>= 3 paired samples' -Passed $false
    }

    # A3: RECYCLED 之后 Client1 滑行计数恒为 0
    if ($null -ne $recycled -and $recycled['Event'] -eq 'RECYCLED') {
        $afterRecycle = @($c1Samples | Where-Object { [long]$_['T'] -gt $recycleMs })
        $slides = ($afterRecycle | ForEach-Object { [int]$_['Slides'] } | Measure-Object -Sum).Sum
        if ($null -eq $slides) { $slides = 0 }
        $passed = ($afterRecycle.Count -ge 3) -and ($slides -eq 0)
        Add-Result -Check 'Client1 slides after RECYCLED' -Measured "$slides over $($afterRecycle.Count) samples (MinDist=$($recycled['MinDist']) uu)" -Threshold '0 (>= 3 samples)' -Passed $passed
        $beforeSlides = ($c1Samples | Where-Object { [long]$_['T'] -le $recycleMs } | ForEach-Object { [int]$_['Slides'] } | Measure-Object -Sum).Sum
        Add-Result -Check 'Client1 slides before RECYCLED' -Measured "$beforeSlides" -Threshold 'info' -Passed $null
    }

    # A4: Client2 加入后 10 s 内可见 ≥ 27
    if ($null -ne $c2Join) {
        $c2JoinMs = [long]$c2Join['T']
        $c2Samples = @(Get-SmokeMarkers -ProcessInfo $client2 -Event 'ClientSample')
        $first2 = $c2Samples | Where-Object { [long]$_['T'] -ge $c2JoinMs -and [int]$_['Visible'] -ge $MinVisible } | Select-Object -First 1
        if ($null -ne $first2) {
            $delta2 = [long]$first2['T'] - $c2JoinMs
            Add-Result -Check "Client2 (late join) visible >= $MinVisible" -Measured "$delta2 ms (Visible=$($first2['Visible']))" -Threshold "<= $VisibleWithinMs ms" -Passed ($delta2 -le $VisibleWithinMs)
        } else {
            $maxVisible2 = ($c2Samples | ForEach-Object { [int]$_['Visible'] } | Measure-Object -Maximum).Maximum
            Add-Result -Check "Client2 (late join) visible >= $MinVisible" -Measured "never (max Visible=$maxVisible2)" -Threshold "<= $VisibleWithinMs ms" -Passed $false
        }
        $kb2 = $c2Samples | Where-Object { [double]$_['InKBps'] -gt 0 } | ForEach-Object { [double]$_['InKBps'] } | Measure-Object -Average -Maximum
        if ($kb2.Count -gt 0) {
            Add-Result -Check 'Client2 inbound KB/s' -Measured ("avg {0:N1} / max {1:N1}" -f $kb2.Average, $kb2.Maximum) -Threshold 'info' -Passed $null
        }
    }

    # 信息项：Client1 入站带宽（追逐窗口 = SPAWNED 之后、被杀之前）
    $kb = $c1Samples | Where-Object { [long]$_['T'] -gt $spawnMs + 1000 } | ForEach-Object { [double]$_['InKBps'] } | Measure-Object -Average -Maximum
    if ($kb.Count -gt 0) {
        Add-Result -Check 'Client1 inbound KB/s (30 bugs)' -Measured ("avg {0:N1} / max {1:N1} over {2} s" -f $kb.Average, $kb.Maximum, $kb.Count) -Threshold 'info' -Passed $null
    }

    # A5: 强杀 Client1 后 Host 无 Swarm/网络 Error/Ensure，存活数 5 s 内恒 > 0
    if ($null -ne $killMs) {
        $afterKill = @($hostSamples | Where-Object { [long]$_['T'] -ge $killMs -and [long]$_['T'] -le $killMs + $KillObserveMs + 1000 })
        $minAlive = ($afterKill | ForEach-Object { [int]$_['Alive'] } | Measure-Object -Minimum).Minimum
        $alivePassed = ($afterKill.Count -ge 5) -and ($minAlive -gt 0)
        Add-Result -Check 'Host alive count after Client1 kill' -Measured "min $minAlive over $($afterKill.Count) samples" -Threshold '> 0 for 5 s' -Passed $alivePassed

        $beforeKill = @($hostInfo.Markers | Where-Object { [long]$_['T'] -lt $killMs })
        $startLine = if ($beforeKill.Count -gt 0) { [int]$beforeKill[-1]['LineIndex'] + 1 } else { 0 }
        $netCategory = '(?i)\b(LogNet\w*|LogSwarm\w*|LogRep\w*|LogIris\w*|LogHandshake|LogPacketHandler\w*|LogSockets|LogOnline\w*|LogWeaponSwarmHarness|LogWeaponRelay\w*)\b|\[Swarm\]'
        $badLines = @()
        $allErrors = 0
        for ($i = $startLine; $i -lt $hostInfo.Lines.Count; $i++) {
            $line = $hostInfo.Lines[$i]
            $isEnsure = $line -match '(?i)Ensure condition failed|=== Handled ensure'
            $isError = $line -match ': Error: '
            if ($isError) { $allErrors++ }
            if ($isEnsure -or ($isError -and $line -match $netCategory)) {
                $badLines += $line.Trim()
            }
        }
        Add-Result -Check 'Host Swarm/net Error+Ensure after kill' -Measured "$($badLines.Count) (all-category errors: $allErrors)" -Threshold '0' -Passed ($badLines.Count -eq 0)
        foreach ($bad in ($badLines | Select-Object -First 10)) { Write-Host "  host: $bad" }

        $drop = $hostInfo.Markers | Where-Object { $_['Event'] -eq 'PlayerCount' -and [long]$_['T'] -ge $killMs -and [int]$_['Players'] -lt [int]$_['Previous'] } | Select-Object -First 1
        $dropText = if ($null -ne $drop) { "$([long]$drop['T'] - $killMs) ms" } else { 'not within 60 s' }
        Add-Result -Check 'Host removed killed Client1' -Measured $dropText -Threshold 'info' -Passed $null
    }
}

$failed = @($results | Where-Object { $_.Result -eq 'FAIL' }).Count
$overall = if ($failed -eq 0 -and $results.Count -gt 0) { 'PASSED' } else { 'FAILED' }
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputRoot 'summary.json') -Encoding UTF8
Write-Host ''
Write-Host "Swarm net smoke $overall  Run=$runId  Output=$OutputRoot"
$results | Format-Table -AutoSize -Wrap | Out-String -Width 220 | Write-Host
if ($overall -ne 'PASSED') {
    exit 1
}
exit 0
