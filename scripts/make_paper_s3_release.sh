#!/usr/bin/env bash
set -euo pipefail

# make_paper_s3_release.sh
# Helper script to build and package a Paper S3 release.
#
# Usage:
#   ./scripts/make_paper_s3_release.sh v0.1.0
#
# This will:
#   - Run `pio run -e paper_s3_release`
#   - Collect the Paper S3 firmware images (bootloader, partitions, app)
#   - Copy them into releases/paper_s3_<version>/
#   - Copy docs/USER_GUIDE_PAPER_S3.md into the same folder
#   - Zip the folder as releases/paper_s3_<version>.zip

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <version> (e.g. v0.1.0)" >&2
  exit 1
fi

VERSION="$1"

# Resolve project root (script is in scripts/)
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

BUILD_ENV="paper_s3_release"
BUILD_DIR=".pio/build/${BUILD_ENV}"
RELEASES_DIR="${ROOT_DIR}/releases"
RELEASE_DIR="${RELEASES_DIR}/paper_s3_${VERSION}"
ZIP_NAME="paper_s3_${VERSION}.zip"

mkdir -p "${RELEASE_DIR}"

echo "[make_paper_s3_release] Building environment: ${BUILD_ENV}" >&2
pio run -e "${BUILD_ENV}"

echo "[make_paper_s3_release] Collecting build artifacts from ${BUILD_DIR}" >&2

BOOTLOADER_BIN="${BUILD_DIR}/bootloader.bin"
FIRMWARE_BIN="${BUILD_DIR}/firmware.bin"
PARTITIONS_BIN="${BUILD_DIR}/partitions.bin"
PARTITION_TABLE_BIN="${BUILD_DIR}/partition-table.bin"

if [[ ! -f "${BOOTLOADER_BIN}" ]]; then
  echo "ERROR: bootloader.bin not found at ${BOOTLOADER_BIN}" >&2
  exit 1
fi

if [[ ! -f "${FIRMWARE_BIN}" ]]; then
  echo "ERROR: firmware.bin not found at ${FIRMWARE_BIN}" >&2
  exit 1
fi

PARTITIONS_SRC=""
if [[ -f "${PARTITIONS_BIN}" ]]; then
  PARTITIONS_SRC="${PARTITIONS_BIN}"
elif [[ -f "${PARTITION_TABLE_BIN}" ]]; then
  PARTITIONS_SRC="${PARTITION_TABLE_BIN}"
else
  echo "ERROR: No partition image found (expected partitions.bin or partition-table.bin in ${BUILD_DIR})" >&2
  exit 1
fi

cp "${BOOTLOADER_BIN}" "${RELEASE_DIR}/bootloader.bin"
cp "${FIRMWARE_BIN}" "${RELEASE_DIR}/firmware.bin"
cp "${PARTITIONS_SRC}" "${RELEASE_DIR}/$(basename "${PARTITIONS_SRC}")"

# Include the Paper S3 end-user guide with esptool/M5 burning instructions
GUIDE_SRC="${ROOT_DIR}/docs/USER_GUIDE_PAPER_S3.md"
if [[ -f "${GUIDE_SRC}" ]]; then
  cp "${GUIDE_SRC}" "${RELEASE_DIR}/USER_GUIDE_PAPER_S3.md"
else
  echo "WARNING: ${GUIDE_SRC} not found; release will not include the user guide" >&2
fi

cd "${RELEASES_DIR}"

# Remove any existing zip with the same name to avoid appending
if [[ -f "${ZIP_NAME}" ]]; then
  rm -f "${ZIP_NAME}"
fi

echo "[make_paper_s3_release] Creating archive ${ZIP_NAME}" >&2
zip -r "${ZIP_NAME}" "$(basename "${RELEASE_DIR}")" >/dev/null

echo "[make_paper_s3_release] Done. Release contents:" >&2
ls -1 "${RELEASE_DIR}" >&2

echo "[make_paper_s3_release] Release folder: ${RELEASE_DIR}" >&2
echo "[make_paper_s3_release] Release archive: ${RELEASES_DIR}/${ZIP_NAME}" >&2
