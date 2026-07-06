.\build_release.bat
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed!"
    exit $LASTEXITCODE
}
$vst3Path = ".\build-release\KickLock_artefacts\Release\VST3\KickLock.vst3"
if (Test-Path $vst3Path) {
    $zipPath = ".\KickLock_Release.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath }
    Compress-Archive -Path $vst3Path -DestinationPath $zipPath
    Write-Host "Successfully created $zipPath"
} else {
    Write-Host "Could not find $vst3Path"
}
