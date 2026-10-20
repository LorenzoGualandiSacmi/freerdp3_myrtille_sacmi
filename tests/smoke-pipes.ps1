param([Parameter(Mandatory)][string]$BinaryDirectory)
$ErrorActionPreference = 'Stop'
$bin = (Resolve-Path $BinaryDirectory).Path
$hostExe = Join-Path $bin 'MyrtilleNativeHost.exe'
& "$env:WINDIR/Microsoft.NET/Framework/v4.0.30319/csc.exe" /nologo /platform:x86 "/out:$hostExe" (Join-Path $PSScriptRoot 'MyrtilleNativeHost.cs')
if ($LASTEXITCODE) { throw 'Native test host compilation failed' }
$sid = [Guid]::NewGuid().ToString()
$pipes = @{}
$connections = @{}
$process = $null
try {
    foreach ($name in 'inputs','updates','audio','video') {
        $mode = if ($name -eq 'video') { [IO.Pipes.PipeTransmissionMode]::Message } else { [IO.Pipes.PipeTransmissionMode]::Byte }
        $pipe = [IO.Pipes.NamedPipeServerStream]::new("remotesession_${sid}_$name", [IO.Pipes.PipeDirection]::InOut, 1, $mode, [IO.Pipes.PipeOptions]::Asynchronous)
        $pipes[$name] = $pipe
        $connections[$name] = $pipe.WaitForConnectionAsync()
    }
    $process = Start-Process -FilePath $hostExe -WorkingDirectory $bin -WindowStyle Hidden -PassThru -ArgumentList @("/myrtille-sid:$sid", '/w:1280', '/h:720', '/gfx-h264:AVC444', '-async-input', '-async-update', '-async-channels', '+clipboard', '/sound', '/audio-mode:0')
    foreach ($name in 'inputs','updates','audio','video') {
        if (!$connections[$name].Wait(15000)) { throw "Timeout connecting $name; process exited: $($process.HasExited)" }
    }
    foreach ($command in 'SRV127.0.0.1:1','DOMtest','USRtest','PWDdummy','CLPprova è € 漢字','STA','CLO') {
        $bytes = [Text.Encoding]::UTF8.GetBytes($command)
        $header = [BitConverter]::GetBytes([int]$bytes.Length)
        $pipes.inputs.Write($header, 0, $header.Length)
        $pipes.inputs.Write($bytes, 0, $bytes.Length)
        $pipes.inputs.Flush()
    }
    if (!$process.WaitForExit(15000)) { throw 'Native client did not close after CLO' }
    if ($process.ExitCode -ne 0) { throw "Native client exit code: $($process.ExitCode)" }
    'PASS: x86 .NET P/Invoke, four native pipes, input commands and clean process exit'
}
finally {
    if ($process -and !$process.HasExited) { $process.Kill(); $process.WaitForExit() }
    foreach ($pipe in $pipes.Values) { $pipe.Dispose() }
}
