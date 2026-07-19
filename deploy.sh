#!/bin/bash
set -e

# Check if required environment variables are set
if [ -z "$VPS_USER" ] || [ -z "$VPS_IP" ] || [ -z "$VPS_PATH" ]; then
    echo "❌ Error: Missing configuration variables."
    echo "Ensure VPS_USER, VPS_IP, and VPS_PATH are defined."
    exit 1
fi

echo "🔨 Building ESP-IDF Project..."
idf.py build

echo "🚚 Transferring firmware to VPS..."
scp build/bootloader/bootloader.bin \
    build/partition_table/partition-table.bin \
    build/*.bin \
    ${VPS_USER}@${VPS_IP}:${VPS_PATH}

echo "🎉 Done! Your ESP32 can now pull the update."
