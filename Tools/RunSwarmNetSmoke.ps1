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

    # 阴性对照：给 Client1 注入故障，仅当位置断言 FAIL 时脚本才成功（exit 0）
    [ValidateSet('None', 'FreezeBugs')]
    [string]$ExpectFailure = 'None',

    [string]$OutputRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) (Join-Path 'Saved\SwarmNetSmoke' (Get-Date -Format 'yyyyMMdd-HHmmss')))
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---- 断言阈值（与计划 Task B 一致） ----
$MinVisible = 27
$VisibleWithinMs = 10000
$MaxMeanOffsetUu = 300.0
$MaxSpreadDiffUu = 300.0
$KillObserveMs = 5000
# 滑行检测需要足够帧率：帧间隔过大时连续 3 帧 >150uu 的判据失真
$MinDetectorFps = 25.0
# 回收后、杀 Client1 前收集的 Client1 样本数（随后立即杀，保证仍在追逐中）
$PostRecycleSamples = 6
$PositionCheck = 'Client1 mean pos vs host mean (same second)'
# 仅当位置/散布实测值越过阈值时置位（阴性对照的唯一成功依据）
$ThresholdBreached = $false

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

Write-Host "Swarm net smoke Run=$runId ExpectFailure=$ExpectFailure Output=$OutputRoot"
try {
    $hostInfo = Start-SmokeProcess -Label 'Host' -Role 'Host' -RunId $runId -OutputDir $OutputRoot -ExtraArguments @(
        "-WeaponSwarmHarnessRecycleDelay=$RecycleDelaySeconds",
        # 等晚加入的 Client2 进入后再回收，保证杀 Client1 时 Client2 已在场且虫群仍在追逐
        '-WeaponSwarmHarnessRecyclePlayers=3')
    $processes += $hostInfo
    Wait-SmokeMarker -ProcessInfo $hostInfo -Event 'DriverReady' -TimeoutSeconds $StartupTimeoutSeconds | Out-Null
    Wait-SmokeMarker -ProcessInfo $hostInfo -Event 'HostAdvertised' -TimeoutSeconds $StartupTimeoutSeconds | Out-Null
    Wait-SmokeMarker -ProcessInfo $hostInfo -Event 'HostReady' -TimeoutSeconds $StartupTimeoutSeconds | Out-Null
    Write-Host 'Host ready (listen + swarm backend + pawn)'

    # 1. Client1 加入 → Host 达到 2 名玩家后生成 30 只 BasicGrunt
    $client1Args = if ($ExpectFailure -ne 'None') { @("-WeaponSwarmHarnessFault=$ExpectFailure") } else { @() }
    $client1 = Start-SmokeProcess -Label 'Client1' -Role 'Client' -RunId $runId -OutputDir $OutputRoot -ExtraArguments $client1Args
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
    $c2Join = Wait-SmokeMarker -ProcessInfo $client2 -Event 'ClientInWorld' -TimeoutSeconds $StartupTimeoutSeconds
    $c2JoinMs = [long]$c2Join['T']
    Write-Host 'Client2 in world'
    Wait-SmokeMarker -ProcessInfo $client2 -Event 'ClientSample' -TimeoutSeconds ($VisibleWithinMs / 1000 + 5) -AllowTimeout -Where {
        param($m) ([long]$m['T'] -ge $c2JoinMs) -and ([int]$m['Visible'] -ge $MinVisible)
    } | Out-Null

    # 3. 回收：SPAWNED + RecycleDelay 且 3 名玩家在场后 DespawnAll 并在 ≥3000uu 外重生
    $recycled = Wait-SmokeMarker -ProcessInfo $hostInfo -Event @('RECYCLED', 'RECYCLE_FAILED') -TimeoutSeconds ($RecycleDelaySeconds + 60)
    if ($recycled['Event'] -ne 'RECYCLED') {
        throw "Host could not respawn BasicGrunt >= 3000 uu away (Spawned=$($recycled['Spawned']))"
    }
    $recycleMs = [long]$recycled['T']
    Write-Host "RECYCLED Spawned=$($recycled['Spawned']) MinDist=$($recycled['MinDist'])"

    # 回收后收集 $PostRecycleSamples 个 Client1 样本，随即杀掉（虫群仍在追逐途中）
    Wait-SmokeMarker -ProcessInfo $client1 -Event 'ClientSample' -TimeoutSeconds 30 -Where {
        param($m) @($client1.Markers | Where-Object { $_['Event'] -eq 'ClientSample' -and [long]$_['T'] -gt $recycleMs }).Count -ge $PostRecycleSamples
    } | Out-Null

    # 5. 断线：先取时间再杀（taskkill + WaitForExit 期间 Host 可能已经收到断线）
    $killMs = Get-NowMs
    Stop-SmokeProcess -ProcessInfo $client1
    Write-Host 'Client1 killed'
    Wait-SmokeMarker -ProcessInfo $hostInfo -Event 'HostSample' -TimeoutSeconds 30 -Where {
        param($m) [long]$m['T'] -ge ($killMs + $KillObserveMs + 1000)
    } | Out-Null
    Wait-SmokeMarker -ProcessInfo $client2 -Event 'ClientSample' -TimeoutSeconds 30 -Where {
        param($m) [long]$m['T'] -ge ($killMs + $KillObserveMs + 1000)
    } | Out-Null
    # 信息项：Host 侧移除断线玩家的耗时
    Wait-SmokeMarker -ProcessInfo $hostInfo -Event 'PlayerCount' -TimeoutSeconds 30 -AllowTimeout -Where {
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
    $hasRecycle = ($null -ne $recycled -and $recycled['Event'] -eq 'RECYCLED')
    $recycleMs = if ($hasRecycle) { [long]$recycled['T'] } else { [long]::MaxValue }
    $endMs = if ($null -ne $killMs) { [long]$killMs } else { [long]::MaxValue }
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

    # A2: 同一 UTC 秒的 Host/Client1 平均位置与散布偏差。窗口 SPAWNED→杀 Client1（含回收后的追逐），
    #     跳过回收所在秒前后（两侧可能分别处于回收前/后），仅计数一致且 ≥ 27 的样本。
    $recycleSec = [long][Math]::Floor($recycleMs / 1000.0)
    $hostBySec = @{}
    foreach ($h in $hostSamples) { $hostBySec[$h['Sec']] = $h }
    $offsets = @(); $spreadDiffs = @(); $prePairs = 0; $postPairs = 0
    foreach ($c in $c1Samples) {
        $t = [long]$c['T']
        $sec = [long]$c['Sec']
        if ($t -le $spawnMs -or $t -ge $endMs) { continue }
        if ($hasRecycle -and $sec -ge $recycleSec - 1 -and $sec -le $recycleSec + 2) { continue }
        $h = $hostBySec[$c['Sec']]
        if ($null -eq $h -or [long]$h['T'] -ge $endMs) { continue }
        if ([int]$h['Alive'] -lt $MinVisible -or [int]$h['Alive'] -ne [int]$c['Visible']) { continue }
        $offsets += (Get-Dist3 -A $h -B $c)
        $spreadDiffs += [Math]::Abs([double]$h['Spread'] - [double]$c['Spread'])
        if ($t -lt $recycleMs) { $prePairs++ } else { $postPairs++ }
    }
    if ($offsets.Count -ge 3) {
        $stats = $offsets | Measure-Object -Maximum -Average
        $spreadStats = $spreadDiffs | Measure-Object -Maximum -Average
        # 阴性对照只认"实测值越过阈值"，不认样本不足之类的覆盖性失败
        $script:ThresholdBreached = ($stats.Maximum -gt $MaxMeanOffsetUu) -or ($spreadStats.Maximum -gt $MaxSpreadDiffUu)
        # 回收后的追逐段必须至少有 1 个配对样本，否则追逐覆盖会悄悄消失
        $chaseCovered = (-not $hasRecycle) -or ($postPairs -ge 1)
        $measured = "max {0:N1} / avg {1:N1} uu over {2} samples ({3} pre + {4} post recycle)" -f $stats.Maximum, $stats.Average, $offsets.Count, $prePairs, $postPairs
        if (-not $chaseCovered) { $measured = "no post-recycle pairs: $measured" }
        Add-Result -Check $PositionCheck -Measured $measured -Threshold "max <= $MaxMeanOffsetUu uu, >= 1 post-recycle pair" -Passed (($stats.Maximum -le $MaxMeanOffsetUu) -and $chaseCovered)
        Add-Result -Check 'Client1 spread vs host spread (same second)' -Measured ("max {0:N1} / avg {1:N1} uu" -f $spreadStats.Maximum, $spreadStats.Average) -Threshold "max <= $MaxSpreadDiffUu uu" -Passed ($spreadStats.Maximum -le $MaxSpreadDiffUu)
    } else {
        Add-Result -Check $PositionCheck -Measured "only $($offsets.Count) paired samples" -Threshold '>= 3 paired samples' -Passed $false
    }

    # A3: RECYCLED 之后 Client1 滑行计数恒为 0；前提是虫在视野内且帧率足够让检测器有效
    if ($hasRecycle) {
        $afterRecycle = @($c1Samples | Where-Object { [long]$_['T'] -gt $recycleMs -and [long]$_['T'] -lt $endMs })
        $visibleAfter = @($afterRecycle | Where-Object { [int]$_['Visible'] -ge $MinVisible })
        $slides = ($afterRecycle | ForEach-Object { [int]$_['Slides'] } | Measure-Object -Sum).Sum
        if ($null -eq $slides) { $slides = 0 }
        $avgDt = ($visibleAfter | ForEach-Object { [double]$_['AvgDtMs'] } | Measure-Object -Average).Average
        $maxDt = ($visibleAfter | ForEach-Object { [double]$_['MaxDtMs'] } | Measure-Object -Maximum).Maximum
        $fps = if ($avgDt -gt 0) { 1000.0 / $avgDt } else { 0.0 }
        $measured = "{0} over {1} samples ({2} with Visible>={3}); avg {4:N1} fps, max dt {5:N1} ms; MinDist={6} uu" -f $slides, $afterRecycle.Count, $visibleAfter.Count, $MinVisible, $fps, $maxDt, $recycled['MinDist']
        if ($visibleAfter.Count -lt 3) {
            Add-Result -Check 'Client1 slides after RECYCLED' -Measured "precondition: $measured" -Threshold ">= 3 samples Visible>=$MinVisible" -Passed $false
        } elseif ($fps -lt $MinDetectorFps) {
            Add-Result -Check 'Client1 slides after RECYCLED' -Measured "detector blind (<$MinDetectorFps fps): $measured" -Threshold "0, fps >= $MinDetectorFps" -Passed $false
        } else {
            Add-Result -Check 'Client1 slides after RECYCLED' -Measured $measured -Threshold "0, fps >= $MinDetectorFps" -Passed ($slides -eq 0)
        }
        $beforeSlides = ($c1Samples | Where-Object { [long]$_['T'] -le $recycleMs } | ForEach-Object { [int]$_['Slides'] } | Measure-Object -Sum).Sum
        Add-Result -Check 'Client1 slides before RECYCLED' -Measured "$beforeSlides" -Threshold 'info' -Passed $null
    }

    # A4: Client2 加入后 10 s 内可见 ≥ 27
    $c2Samples = @()
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

    # 信息项：Client1 入站带宽（SPAWNED 之后、被杀之前）
    $kb = $c1Samples | Where-Object { [long]$_['T'] -gt $spawnMs + 1000 } | ForEach-Object { [double]$_['InKBps'] } | Measure-Object -Average -Maximum
    if ($kb.Count -gt 0) {
        Add-Result -Check 'Client1 inbound KB/s (30 bugs)' -Measured ("avg {0:N1} / max {1:N1} over {2} s" -f $kb.Average, $kb.Maximum, $kb.Count) -Threshold 'info' -Passed $null
    }

    # A5/A6: 强杀 Client1 后 Host 无 Swarm/网络 Error/Ensure、存活数 5 s 内恒 > 0；Client2 持续可见且有入站流量
    if ($null -ne $killMs) {
        $lastBeforeKill = $hostSamples | Where-Object { [long]$_['T'] -le $killMs } | Select-Object -Last 1
        if ($null -ne $lastBeforeKill) {
            Add-Result -Check 'Mean bug distance to host pawn at kill' -Measured ("{0:N0} uu (Alive={1})" -f [double]$lastBeforeKill['MeanDistToHost'], $lastBeforeKill['Alive']) -Threshold 'info' -Passed $null
        }

        $windowEnd = $killMs + $KillObserveMs + 1000
        $afterKill = @($hostSamples | Where-Object { [long]$_['T'] -ge $killMs -and [long]$_['T'] -le $windowEnd })
        $minAlive = ($afterKill | ForEach-Object { [int]$_['Alive'] } | Measure-Object -Minimum).Minimum
        Add-Result -Check 'Host alive count after Client1 kill' -Measured "min $minAlive over $($afterKill.Count) samples" -Threshold '> 0 for 5 s' -Passed (($afterKill.Count -ge 5) -and ($minAlive -gt 0))

        $c2AfterKill = @($c2Samples | Where-Object { [long]$_['T'] -ge $killMs -and [long]$_['T'] -le $windowEnd })
        $c2MinVisible = ($c2AfterKill | ForEach-Object { [int]$_['Visible'] } | Measure-Object -Minimum).Minimum
        $c2MinKb = ($c2AfterKill | ForEach-Object { [double]$_['InKBps'] } | Measure-Object -Minimum).Minimum
        $c2Passed = ($c2AfterKill.Count -ge 5) -and ($c2MinVisible -ge $MinVisible) -and ($c2MinKb -gt 0)
        Add-Result -Check 'Client2 after Client1 kill' -Measured ("min Visible {0}, min {1:N2} KB/s over {2} samples" -f $c2MinVisible, $c2MinKb, $c2AfterKill.Count) -Threshold "Visible >= $MinVisible, KB/s > 0 for 5 s" -Passed $c2Passed

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
        $dropText = if ($null -ne $drop) { "$([long]$drop['T'] - $killMs) ms" } else { 'not within 30 s' }
        Add-Result -Check 'Host removed killed Client1' -Measured $dropText -Threshold 'info' -Passed $null
    }

    # 回归项：任何客户端玩家脚下没有地面即失败（Slot 回退出生点曾悬空，玩家掉出地图）
    $groundless = @($hostInfo.Markers | Where-Object { $_['Event'] -eq 'PawnNoGround' })
    $groundText = if ($groundless.Count -gt 0) { "$($groundless.Count) (first at X=$($groundless[0]['X']) Y=$($groundless[0]['Y']) Z=$($groundless[0]['Z']))" } else { '0' }
    Add-Result -Check 'All players spawn on ground' -Measured $groundText -Threshold '0 groundless players' -Passed ($groundless.Count -eq 0)

    # 回归项：客户端激活的虫应显示 HD2 模型而非回退锥体（仅当本机有 HD2 资产时断言）
    $meshSamples = @(@($client1.Markers) + @($client2.Markers) | Where-Object { $_['Event'] -eq 'ClientSample' -and $null -ne $_['FallbackMeshes'] -and [int]$_['Visible'] -gt 0 })
    if ($meshSamples.Count -eq 0) {
        Add-Result -Check 'Client bug meshes (no cone fallback)' -Measured 'no samples' -Threshold '0 fallback' -Passed $false
    }
    elseif (@($meshSamples | Where-Object { $_['HasHD2'] -eq '1' }).Count -eq 0) {
        Add-Result -Check 'Client bug meshes (no cone fallback)' -Measured 'HD2 assets absent locally; skipped' -Threshold 'info' -Passed $null
    }
    else {
        $maxFallback = ($meshSamples | ForEach-Object { [int]$_['FallbackMeshes'] } | Measure-Object -Maximum).Maximum
        Add-Result -Check 'Client bug meshes (no cone fallback)' -Measured "max $maxFallback fallback over $($meshSamples.Count) samples" -Threshold '0 fallback' -Passed ($maxFallback -eq 0)
    }
}

$failed = @($results | Where-Object { $_.Result -eq 'FAIL' }).Count
$overall = if ($failed -eq 0 -and $results.Count -gt 0) { 'PASSED' } else { 'FAILED' }
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputRoot 'summary.json') -Encoding UTF8
Write-Host ''
Write-Host "Swarm net smoke $overall  Run=$runId  Output=$OutputRoot"
$results | Format-Table -AutoSize -Wrap | Out-String -Width 240 | Write-Host

if ($ExpectFailure -ne 'None') {
    # 阴性对照：场景必须跑完，且位置断言必须 FAIL，才证明检测器能发现客户端位置错误
    $detected = ($null -eq $fatal) -and $script:ThresholdBreached
    if ($detected) {
        Write-Host "NEGATIVE CONTROL PASSED: fault $ExpectFailure breached the position/spread threshold"
        exit 0
    }
    Write-Host "NEGATIVE CONTROL FAILED: fault $ExpectFailure did NOT breach the position/spread threshold (coverage failures do not count)"
    exit 1
}
if ($overall -ne 'PASSED') {
    exit 1
}
exit 0
