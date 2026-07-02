# flash.ps1
# Script to compile and upload PalaOne firmware with a strict timeout limit to prevent hangs.

param(
    [string]$Port = "COM12",
    [string]$Fqbn = "esp32:esp32:heltec_wireless_paper",
    [int]$TimeoutSec = 60,
    [switch]$CompileOnly,
    [switch]$SkipCompile
)

$SketchPath = Join-Path $PSScriptRoot "PalaOne1_7_1_HeltecV1_2.ino"

# Check if sketch exists
if (-not (Test-Path $SketchPath)) {
    Write-Error "Sketch not found at $SketchPath"
    exit 1
}

# Step 1: Compilation (unless skipped)
if (-not $SkipCompile) {
    Write-Host "=========================================="
    Write-Host "Compiling sketch: $SketchPath"
    Write-Host "=========================================="
    
    $compileStart = Get-Date
    $pinfo = New-Object System.Diagnostics.ProcessStartInfo
    $pinfo.FileName = "arduino-cli"
    $pinfo.Arguments = "compile --fqbn $Fqbn `"$SketchPath`""
    $pinfo.UseShellExecute = $false
    $pinfo.RedirectStandardOutput = $true
    $pinfo.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $pinfo
    $process.Start() | Out-Null
    
    # Wait for compile (up to 5 minutes / 300 seconds)
    if ($process.WaitForExit(300000)) {
        $stdout = $process.StandardOutput.ReadToEnd()
        $stderr = $process.StandardError.ReadToEnd()
        if ($process.ExitCode -ne 0) {
            Write-Error "Compilation failed!"
            Write-Host $stdout
            Write-Error $stderr
            exit $process.ExitCode
        }
        Write-Host $stdout
        Write-Host "Compilation succeeded in $(([Math]::Round(((Get-Date) - $compileStart).TotalSeconds, 1))) seconds."
    } else {
        Write-Warning "Compilation timed out after 5 minutes! Terminating compiler process..."
        $process.Kill()
        exit 1
    }
}

if ($CompileOnly) {
    exit 0
}

# Step 2: Upload (Flash)
Write-Host "=========================================="
Write-Host "Uploading sketch to $Port"
Write-Host "Timeout limit set to: $TimeoutSec seconds"
Write-Host "=========================================="

$uploadStart = Get-Date
$pinfo = New-Object System.Diagnostics.ProcessStartInfo
$pinfo.FileName = "arduino-cli"
$pinfo.Arguments = "upload -v -p $Port --fqbn $Fqbn `"$SketchPath`""
$pinfo.UseShellExecute = $false
$pinfo.RedirectStandardOutput = $true
$pinfo.RedirectStandardError = $true
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $pinfo
$process.Start() | Out-Null

# Wait for upload with strict timeout
if ($process.WaitForExit($TimeoutSec * 1000)) {
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    if ($process.ExitCode -ne 0) {
        Write-Error "Upload/Flash failed with exit code $($process.ExitCode)!"
        Write-Host $stdout
        Write-Error $stderr
        exit $process.ExitCode
    }
    Write-Host $stdout
    Write-Host "Upload/Flash completed successfully in $(([Math]::Round(((Get-Date) - $uploadStart).TotalSeconds, 1))) seconds!"
} else {
    Write-Warning "Upload/Flash timed out after $TimeoutSec seconds! Killing process..."
    $process.Kill()
    Write-Error "Upload failed due to timeout. Make sure COM port is not locked, or put ESP32 in bootloader mode (hold Boot button, press Reset)."
    exit 1
}
