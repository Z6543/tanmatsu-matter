#include "paa_certs_embedded.h"

#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/Span.h>

CHIP_ERROR EmbeddedAttestationTrustStore::GetProductAttestationAuthorityCert(
    const chip::ByteSpan &skid,
    chip::MutableByteSpan &outPaaDerBuffer) const
{
    const EmbeddedPAACert *certs = embedded_paa_certs();
    size_t count = embedded_paa_certs_count();

    for (size_t i = 0; i < count; i++) {
        chip::ByteSpan certSpan(certs[i].data, certs[i].len);
        uint8_t skidBuf[chip::Crypto::kSubjectKeyIdentifierLength] = {};
        chip::MutableByteSpan skidSpan(skidBuf);

        if (chip::Crypto::ExtractSKIDFromX509Cert(certSpan, skidSpan)
                != CHIP_NO_ERROR) {
            continue;
        }
        if (skid.data_equal(skidSpan)) {
            return chip::CopySpanToMutableSpan(certSpan, outPaaDerBuffer);
        }
    }
    return CHIP_ERROR_CA_CERT_NOT_FOUND;
}
