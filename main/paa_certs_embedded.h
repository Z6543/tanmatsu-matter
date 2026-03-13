#pragma once

#include <credentials/attestation_verifier/DeviceAttestationVerifier.h>
#include <stddef.h>
#include <stdint.h>

// POD struct used by the generated paa_cert_data.cpp.
struct EmbeddedPAACert {
    const uint8_t *data;
    size_t len;
};

// Provided by generated paa_cert_data.cpp.
const EmbeddedPAACert *embedded_paa_certs();
size_t embedded_paa_certs_count();

// AttestationTrustStore backed by DER certs compiled into the firmware.
// Register it via chip::Credentials::set_custom_attestation_trust_store().
class EmbeddedAttestationTrustStore
    : public chip::Credentials::AttestationTrustStore {
public:
    CHIP_ERROR GetProductAttestationAuthorityCert(
        const chip::ByteSpan &skid,
        chip::MutableByteSpan &outPaaDerBuffer) const override;
};
