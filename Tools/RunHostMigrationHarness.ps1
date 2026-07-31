[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [ValidateSet('HostCrashToA', 'HostThenBackupACrashToB', 'All')]
    [string]$Scenario = 'All',

    [int]$StartupTimeoutSeconds = 120,

    [int]$RecoveryTimeoutSeconds = 210,

    [string]$OutputRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) (Join-Path 'Saved\HostMigrationHarness' (Get-Date -Format 'yyyyMMdd-HHmmss')))
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function ConvertFrom-HarnessMarker {
    param([string]$Line)

    $markerIndex = $Line.IndexOf('WMH|')
    if ($markerIndex -lt 0) {
        return $null
    }

    $fields = @{}
    $marker = $Line.Substring($markerIndex)
    foreach ($part in $marker.Split('|')) {
        $separator = $part.IndexOf('=')
        if ($separator -gt 0) {
            $fields[$part.Substring(0, $separator)] = $part.Substring($separator + 1)
        }
    }
    return $fields
}

function Get-HarnessMarkers {
    param(
        [pscustomobject]$ProcessInfo,
        [string]$RunId,
        [string]$Event
    )

    if (-not (Test-Path -LiteralPath $ProcessInfo.LogPath)) {
        return @()
    }

    $markers = @()
    foreach ($line in Get-Content -LiteralPath $ProcessInfo.LogPath -ErrorAction SilentlyContinue) {
        if ($line -notlike "*WMH|Run=$RunId|Process=$($ProcessInfo.Label)|Event=$Event*") {
            continue
        }
        $fields = ConvertFrom-HarnessMarker -Line $line
        if ($null -ne $fields -and $fields['Run'] -eq $RunId -and
            $fields['Process'] -eq $ProcessInfo.Label -and $fields['Event'] -eq $Event) {
            $markers += $fields
        }
    }
    return $markers
}

function Wait-HarnessMarker {
    param(
        [pscustomobject]$ProcessInfo,
        [string]$RunId,
        [string]$Event,
        [int]$TimeoutSeconds,
        [hashtable]$Expected = @{}
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        foreach ($marker in (Get-HarnessMarkers -ProcessInfo $ProcessInfo -RunId $RunId -Event $Event)) {
            $matches = $true
            foreach ($entry in $Expected.GetEnumerator()) {
                if ($marker[$entry.Key] -ne [string]$entry.Value) {
                    $matches = $false
                    break
                }
            }
            if ($matches) {
                return $marker
            }
        }

        $ProcessInfo.Process.Refresh()
        if ($ProcessInfo.Process.HasExited) {
            $tail = if (Test-Path -LiteralPath $ProcessInfo.LogPath) {
                (Get-Content -LiteralPath $ProcessInfo.LogPath -Tail 30) -join [Environment]::NewLine
            } else {
                '<log not created>'
            }
            throw "$($ProcessInfo.Label) exited before Event=$Event. ExitCode=$($ProcessInfo.Process.ExitCode)`n$tail"
        }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out after $TimeoutSeconds seconds waiting for $($ProcessInfo.Label) Event=$Event Expected=$($Expected | ConvertTo-Json -Compress)"
}

function Start-HarnessProcess {
    param(
        [string]$ExecutablePath,
        [string]$Label,
        [string]$RunId,
        [string]$ServerName,
        [string]$LogPath
    )

    $arguments = @(
        '-nullrhi',
        '-nosound',
        '-unattended',
        '-NoSplash',
        '-NoLoadingScreen',
        '-log',
        "-abslog=`"$LogPath`"",
        "-WeaponMigrationHarnessRole=$Label",
        "-WeaponMigrationHarnessRunId=$RunId",
        "-WeaponMigrationHarnessServer=$ServerName"
    )
    $process = Start-Process -FilePath $ExecutablePath -ArgumentList $arguments -PassThru -WindowStyle Hidden
    return [pscustomobject]@{
        Label = $Label
        LogPath = $LogPath
        Process = $process
    }
}

function Stop-HarnessProcess {
    param([pscustomobject]$ProcessInfo)

    if ($null -eq $ProcessInfo) {
        return
    }
    $ProcessInfo.Process.Refresh()
    if (-not $ProcessInfo.Process.HasExited) {
        Stop-Process -Id $ProcessInfo.Process.Id -Force
        $ProcessInfo.Process.WaitForExit(10000) | Out-Null
    }
}

function Assert-EqualField {
    param(
        [hashtable]$Expected,
        [hashtable]$Actual,
        [string]$Field,
        [string]$Context
    )

    if ($Expected[$Field] -ne $Actual[$Field]) {
        throw "$Context field $Field differs. Expected=$($Expected[$Field]) Actual=$($Actual[$Field])"
    }
}

function Invoke-HarnessScenario {
    param(
        [string]$ExecutablePath,
        [string]$ScenarioName,
        [string]$ScenarioOutput,
        [int]$StartupTimeout,
        [int]$RecoveryTimeout
    )

    New-Item -ItemType Directory -Path $ScenarioOutput -Force | Out-Null
    $runId = [Guid]::NewGuid().ToString('N')
    $serverName = "WMH_$runId"
    $processes = @()
    $result = [ordered]@{
        Scenario = $ScenarioName
        RunId = $runId
        ServerName = $serverName
        Status = 'Running'
        StartedUtc = [DateTime]::UtcNow.ToString('o')
    }

    try {
        $hostProcessInfo = Start-HarnessProcess -ExecutablePath $ExecutablePath -Label 'Host' -RunId $runId -ServerName $serverName -LogPath (Join-Path $ScenarioOutput 'Host.log')
        $processes += $hostProcessInfo
        Wait-HarnessMarker -ProcessInfo $hostProcessInfo -RunId $runId -Event 'DriverReady' -TimeoutSeconds $StartupTimeout | Out-Null
        Wait-HarnessMarker -ProcessInfo $hostProcessInfo -RunId $runId -Event 'HostAdvertised' -TimeoutSeconds $StartupTimeout | Out-Null

        $client1 = Start-HarnessProcess -ExecutablePath $ExecutablePath -Label 'Client1' -RunId $runId -ServerName $serverName -LogPath (Join-Path $ScenarioOutput 'Client1.log')
        $processes += $client1
        Wait-HarnessMarker -ProcessInfo $client1 -RunId $runId -Event 'InitialConnectionObserved' -TimeoutSeconds $StartupTimeout | Out-Null

        $client2 = Start-HarnessProcess -ExecutablePath $ExecutablePath -Label 'Client2' -RunId $runId -ServerName $serverName -LogPath (Join-Path $ScenarioOutput 'Client2.log')
        $processes += $client2
        Wait-HarnessMarker -ProcessInfo $client2 -RunId $runId -Event 'InitialConnectionObserved' -TimeoutSeconds $StartupTimeout | Out-Null

        $hostBaseline = Wait-HarnessMarker -ProcessInfo $hostProcessInfo -RunId $runId -Event 'BaselineReady' -TimeoutSeconds $StartupTimeout
        $client1Baseline = Wait-HarnessMarker -ProcessInfo $client1 -RunId $runId -Event 'BaselineReady' -TimeoutSeconds $StartupTimeout
        $client2Baseline = Wait-HarnessMarker -ProcessInfo $client2 -RunId $runId -Event 'BaselineReady' -TimeoutSeconds $StartupTimeout
        foreach ($baseline in @($client1Baseline, $client2Baseline)) {
            foreach ($field in @('Match', 'Epoch', 'Roster', 'Snapshot', 'Hash')) {
                Assert-EqualField -Expected $hostBaseline -Actual $baseline -Field $field -Context 'Initial CommonCommitted baseline'
            }
        }
        $playerIds = @($hostBaseline['Player'], $client1Baseline['Player'], $client2Baseline['Player']) | Select-Object -Unique
        if ($playerIds.Count -ne 3) {
            throw "Packaged processes did not receive three unique OSS identities: $($playerIds -join ',')"
        }

        $backupA = if ($client1Baseline['Role'] -eq 'BackupA') { $client1 } elseif ($client2Baseline['Role'] -eq 'BackupA') { $client2 } else { $null }
        $backupB = if ($client1Baseline['Role'] -eq 'BackupB') { $client1 } elseif ($client2Baseline['Role'] -eq 'BackupB') { $client2 } else { $null }
        if ($null -eq $backupA -or $null -eq $backupB) {
            throw "Expected one BackupA and one BackupB. Client1=$($client1Baseline['Role']) Client2=$($client2Baseline['Role'])"
        }

        Stop-HarnessProcess -ProcessInfo $hostProcessInfo
        if ($ScenarioName -eq 'HostThenBackupACrashToB') {
            Wait-HarnessMarker -ProcessInfo $backupA -RunId $runId -Event 'MigrationState' -TimeoutSeconds $StartupTimeout -Expected @{ State = 'WaitingForReadyAcks' } | Out-Null
            Wait-HarnessMarker -ProcessInfo $backupB -RunId $runId -Event 'MigrationState' -TimeoutSeconds $StartupTimeout -Expected @{ State = 'WaitingForReadyAcks' } | Out-Null
            Stop-HarnessProcess -ProcessInfo $backupA
            $survivingCandidate = $backupB
            $expectedEpoch = ([UInt64]$hostBaseline['Epoch'] + 2).ToString()
        } else {
            $survivingCandidate = $backupA
            $expectedEpoch = ([UInt64]$hostBaseline['Epoch'] + 1).ToString()
        }

        $recoveryLive = Wait-HarnessMarker -ProcessInfo $survivingCandidate -RunId $runId -Event 'RecoveryLive' -TimeoutSeconds $RecoveryTimeout -Expected @{ Role = 'ActiveHost'; NetMode = 'ListenServer' }
        foreach ($field in @('Match', 'Snapshot', 'Hash')) {
            Assert-EqualField -Expected $hostBaseline -Actual $recoveryLive -Field $field -Context 'Recovered authority baseline'
        }
        if ($recoveryLive['Epoch'] -ne $expectedEpoch) {
            throw "Recovered authority Epoch differs. Expected=$expectedEpoch Actual=$($recoveryLive['Epoch'])"
        }

        if ($ScenarioName -eq 'HostCrashToA') {
            $survivingClient = if ($backupA.Label -eq $client1.Label) { $client2 } else { $client1 }
            $clientRecovery = Wait-HarnessMarker -ProcessInfo $survivingClient -RunId $runId -Event 'RecoveryLive' -TimeoutSeconds $RecoveryTimeout -Expected @{ NetMode = 'Client' }
            foreach ($field in @('Match', 'Epoch', 'Snapshot', 'Hash')) {
                Assert-EqualField -Expected $recoveryLive -Actual $clientRecovery -Field $field -Context 'Recovered client convergence'
            }
        }

        $result.Status = 'Passed'
        $result.Baseline = $hostBaseline
        $result.Recovery = $recoveryLive
        $result.CompletedUtc = [DateTime]::UtcNow.ToString('o')
        $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $ScenarioOutput 'result.json') -Encoding UTF8
        Write-Host "PASS $ScenarioName Run=$runId Epoch=$($recoveryLive['Epoch']) Snapshot=$($recoveryLive['Snapshot'])"
        return $result
    } catch {
        $result.Status = 'Failed'
        $result.Error = $_.Exception.Message
        $result.CompletedUtc = [DateTime]::UtcNow.ToString('o')
        $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $ScenarioOutput 'result.json') -Encoding UTF8
        throw
    } finally {
        foreach ($processInfo in $processes) {
            Stop-HarnessProcess -ProcessInfo $processInfo
        }
    }
}

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$bootstrapDirectory = Split-Path $resolvedExecutable -Parent
$executableName = [IO.Path]::GetFileName($resolvedExecutable)
$projectName = [IO.Path]::GetFileNameWithoutExtension($resolvedExecutable)
$innerExecutable = Join-Path $bootstrapDirectory (Join-Path $projectName (Join-Path 'Binaries\Win64' $executableName))
if (Test-Path -LiteralPath $innerExecutable) {
    $resolvedExecutable = (Resolve-Path -LiteralPath $innerExecutable).Path
}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$resolvedOutputRoot = (Resolve-Path -LiteralPath $OutputRoot).Path
$scenarios = if ($Scenario -eq 'All') {
    @('HostCrashToA', 'HostThenBackupACrashToB')
} else {
    @($Scenario)
}

$results = @()
foreach ($scenarioName in $scenarios) {
    $results += Invoke-HarnessScenario -ExecutablePath $resolvedExecutable -ScenarioName $scenarioName -ScenarioOutput (Join-Path $resolvedOutputRoot $scenarioName) -StartupTimeout $StartupTimeoutSeconds -RecoveryTimeout $RecoveryTimeoutSeconds
}
$results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $resolvedOutputRoot 'summary.json') -Encoding UTF8
Write-Host "Host Migration harness completed: $resolvedOutputRoot"
