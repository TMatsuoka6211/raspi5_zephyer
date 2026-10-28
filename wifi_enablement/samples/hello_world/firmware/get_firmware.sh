#!/bin/bash
#
# Download CYW43455 Wi-Fi Firmware and CLM BLOB for Raspberry Pi 5
# Official upstream repository: Raspberry Pi firmware-nonfree (bookworm branch)
#
set -e

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TARGET_DIR="${SCRIPT_DIR}"
BASE_URL="https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/bookworm/brcm"

echo "=== Downloading Cypress/Infineon CYW43455 Wi-Fi Firmware ==="

curl -L -f -o "${TARGET_DIR}/brcmfmac43455-sdio.bin" "${BASE_URL}/brcmfmac43455-sdio.bin"
echo "  [OK] brcmfmac43455-sdio.bin"

curl -L -f -o "${TARGET_DIR}/brcmfmac43455-sdio.clm_blob" "${BASE_URL}/brcmfmac43455-sdio.clm_blob"
echo "  [OK] brcmfmac43455-sdio.clm_blob"

curl -L -f -o "${TARGET_DIR}/brcmfmac43455-sdio.txt" "${BASE_URL}/brcmfmac43455-sdio.txt"
echo "  [OK] brcmfmac43455-sdio.txt"

echo "Download completed successfully."

