# Inputs pipe framing regression for the Myrtille RDP client. No RDP server nor Myrtille service needed:
# the gateway pipes are created here, the client is started without the CON command (no RDP connection).
#   zero  : a zero size header must stop the client, not spin its inputs thread at 100% of a core
#   split : a size header split over two writes must be reassembled; a following CLO closes the client
param([string]$Exe = (Join-Path $PSScriptRoot '..\artifacts\bin\wfreerdp.exe'))
$ErrorActionPreference = 'Stop'

function Start-Client {
    $id = [guid]::NewGuid()
    $pipes = @{}
    foreach ($p in @(@('inputs', 'Byte', 0), @('updates', 'Byte', 0), @('audio', 'Byte', 196608), @('video', 'Message', 262144))) {
        $pipes[$p[0]] = New-Object System.IO.Pipes.NamedPipeServerStream("remotesession_$($id)_$($p[0])",
            [IO.Pipes.PipeDirection]::InOut, 1, [IO.Pipes.PipeTransmissionMode]$p[1], [IO.Pipes.PipeOptions]::Asynchronous, $p[2], $p[2])
    }
    $process = Start-Process -FilePath $Exe -ArgumentList "/myrtille-sid:$id /w:800 /h:600" -PassThru -WindowStyle Hidden
    foreach ($name in 'inputs', 'updates', 'audio', 'video') {
        if (-not $pipes[$name].WaitForConnectionAsync().Wait(15000)) { throw "client did not connect the $name pipe" }
    }
    return @{ Process = $process; Pipes = $pipes }
}

function Send([IO.Stream]$pipe, [byte[]]$bytes) {
    if (-not $pipe.WriteAsync($bytes, 0, $bytes.Length).Wait(3000)) { throw 'the client stopped reading its inputs pipe (gateway write blocked)' }
}

function Stop-Client($client) {
    if (-not $client.Process.HasExited) { $client.Process.Kill(); $client.Process.WaitForExit() }
    foreach ($pipe in $client.Pipes.Values) { $pipe.Dispose() }
}

$results = @()

# zero size header
$client = Start-Client
try {
    Send $client.Pipes.inputs ([byte[]](0, 0, 0, 0))
    # a zero byte read waits for data, then returns at once without consuming it: the next message is never read
    # (with no pipe buffer, the gateway write blocks) and the inputs thread spins
    $pulse = [Text.Encoding]::UTF8.GetBytes('PLS')
    $message = [BitConverter]::GetBytes([int]$pulse.Length) + $pulse
    $written = $false
    try { $written = $client.Pipes.inputs.WriteAsync($message, 0, $message.Length).Wait(3000) } catch { $written = $true }
    if ($client.Process.WaitForExit(5000)) { $results += 'PASS zero: client stopped on a zero size header' }
    elseif (-not $written) { $results += 'FAIL zero: client no longer reads its inputs pipe, the gateway write is blocked' }
    else {
        $cpu = $client.Process.TotalProcessorTime; Start-Sleep -Seconds 2; $client.Process.Refresh()
        $busy = ($client.Process.TotalProcessorTime - $cpu).TotalMilliseconds
        $results += "FAIL zero: client still running, $([int]$busy) ms CPU in 2 s"
    }
} finally { Stop-Client $client }

# size header split over two writes, then a close command
$client = Start-Client
try {
    try {
        $pulse = [Text.Encoding]::UTF8.GetBytes('PLS')
        $header = [BitConverter]::GetBytes([int]$pulse.Length)
        Send $client.Pipes.inputs $header[0..1]
        Start-Sleep -Milliseconds 300
        Send $client.Pipes.inputs ($header[2..3] + $pulse)
        Start-Sleep -Milliseconds 500
        if ($client.Process.HasExited) { $results += "FAIL split: client exited (code $($client.Process.ExitCode)) on a split header" }
        else {
            $close = [Text.Encoding]::UTF8.GetBytes('CLO')
            Send $client.Pipes.inputs ([BitConverter]::GetBytes([int]$close.Length) + $close)
            if ($client.Process.WaitForExit(10000)) { $results += 'PASS split: split header reassembled, close command honored' }
            else { $results += 'FAIL split: client ignored the close command after a split header' }
        }
    } catch { $results += "FAIL split: $($_.Exception.Message)" }
} finally { Stop-Client $client }

$results
if ($results -match '^FAIL') { exit 1 }
