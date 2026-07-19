$ErrorActionPreference = "Stop"

if (-not $env:VPS_USER -or -not $env:VPS_IP -or -not $env:VPS_PATH) {
    Write-Error "Error: Missing configuration variables. Ensure VPS_USER, VPS_IP, and VPS_PATH are defined."
    Exit
}

Write-Host "Building ESP-IDF Project..." -ForegroundColor Cyan
idf.py build

Write-Host "Transferring firmware to VPS..." -ForegroundColor Cyan
scp build/bootloader/bootloader.bin `
    build/partition_table/partition-table.bin `
    build/*.bin `
    "${env:VPS_USER}@${env:VPS_IP}:${env:VPS_PATH}"

Write-Host "Done! Your ESP32 can now pull the update." -ForegroundColor Green
