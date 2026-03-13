#!/usr/bin/env bash
# Fetch PAA root certificates from the Matter DCL (prod + test) and copy them
# into paa_cert/ so they are bundled into the SPIFFS attestation partition.
#
# Usage:
#   ./scripts/fetch_paa_certs.sh
#
# Requires: Python 3 with: pip install click click-option-group requests cryptography
#
# Run this whenever you want to refresh the certificate bundle.
#
# SPIFFS note: filenames (including the leading '/') must fit in obj-name-len=32
# bytes (31 usable chars). Fetched DCL certs have long names so we rename them
# to dcl_NNNN.der (max 12 chars) after fetching.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${PROJECT_ROOT}/paa_cert"

# Locate the chip SDK fetch script. Prefer ESP_MATTER_PATH if set.
if [ -n "${ESP_MATTER_PATH:-}" ] && [ -f "${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip/credentials/fetch_paa_certs_from_dcl.py" ]; then
    FETCH_SCRIPT="${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip/credentials/fetch_paa_certs_from_dcl.py"
else
    FETCH_SCRIPT="/SSD/tanmatsu/esp-matter/connectedhomeip/connectedhomeip/credentials/fetch_paa_certs_from_dcl.py"
fi

if [ ! -f "${FETCH_SCRIPT}" ]; then
    echo "ERROR: Could not locate fetch_paa_certs_from_dcl.py" >&2
    echo "Set ESP_MATTER_PATH to the esp-matter root directory." >&2
    exit 1
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "${TMPDIR}"' EXIT

echo "==> Fetching production PAA certs from MainNet DCL..."
python3 "${FETCH_SCRIPT}" --use-main-net-http \
    --paa-trust-store-path "${TMPDIR}/prod" || true

echo "==> Fetching test PAA certs from TestNet DCL..."
python3 "${FETCH_SCRIPT}" --use-test-net-http \
    --paa-trust-store-path "${TMPDIR}/test" || true

echo "==> Installing DER files into ${OUT_DIR}/ with short names..."
mkdir -p "${OUT_DIR}"

# Remove previously installed DCL certs (dcl_NNNN.der pattern).
find "${OUT_DIR}" -maxdepth 1 -name "dcl_*.der" -delete

idx=0
for src_dir in "${TMPDIR}/prod" "${TMPDIR}/test"; do
    if [ ! -d "${src_dir}" ]; then
        continue
    fi
    while IFS= read -r -d '' f; do
        dest="${OUT_DIR}/$(printf 'dcl_%04d.der' "${idx}")"
        cp "${f}" "${dest}"
        idx=$((idx + 1))
    done < <(find "${src_dir}" -maxdepth 1 -name "*.der" -print0)
done

total=$(find "${OUT_DIR}" -maxdepth 1 -name "*.der" | wc -l)
echo "==> Done. ${idx} certs fetched from DCL, ${total} total in ${OUT_DIR}/"
echo "    Filenames are dcl_0000.der..dcl_$(printf '%04d' $((idx - 1))).der"
echo "    (SPIFFS ignores filenames; lookup uses SKID extracted from DER content)"
