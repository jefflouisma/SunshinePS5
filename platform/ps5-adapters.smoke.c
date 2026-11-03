#include "capture/ps5.h"
#include "encode/ps5.h"
#include "firmware/13.60.h"
#include "privileged/access.h"
#include "ps5_media_global.h"

/* Compile-only smoke TU for the provisional console adapters. It instantiates
 * caller-owned, preallocated contexts and checks the contract wiring. It never
 * opens a console service and is not linked into portable core targets. */
int main(void)
{
    static sp5_ps5_capture capture;
    static sp5_ps5_encoder encoder;
    sp5_priv_access access;
    sp5_platform_provenance provenance;

    sp5_ps5_capture_init(&capture);
    sp5_ps5_encoder_init(&encoder);
    sp5_priv_access_init(&access);

    sp5_capture_ops capture_ops = sp5_ps5_capture_operations(&capture);
    sp5_encode_ops encode_ops = sp5_ps5_encoder_operations(&encoder);
    if (capture_ops.context != &capture || encode_ops.context != &encoder ||
        capture_ops.open == NULL || capture_ops.source_state == NULL ||
        capture_ops.acquire == NULL || capture_ops.release == NULL || capture_ops.close == NULL ||
        encode_ops.open == NULL || encode_ops.encode == NULL || encode_ops.close == NULL)
        return 1;
    if (capture_ops.close(capture_ops.context).status != SP5_OK ||
        capture_ops.close(capture_ops.context).status != SP5_OK ||
        encode_ops.close(encode_ops.context).status != SP5_OK ||
        encode_ops.close(encode_ops.context).status != SP5_OK)
        return 2;

    sp5_ps5_capture_provenance(&capture, &provenance);
    if (provenance.synthetic || provenance.validity != SP5_VALIDITY_INVALID)
        return 2;
    sp5_ps5_encoder_provenance(&encoder, &provenance);
    if (provenance.synthetic || provenance.validity != SP5_VALIDITY_INVALID)
        return 3;
    if (sp5_fw_13_60_profile()->exact_version != (uint32_t)SP5_FIRMWARE_13_60_EXACT ||
        !sp5_fw_13_60_supported((uint32_t)SP5_FIRMWARE_13_60_EXACT) ||
        sp5_fw_13_60_supported(SP5_FIRMWARE_13_60_EXACT - 1) ||
        sp5_fw_13_60_profile()->privileged_hook_count != 0)
        return 4;

    if (sp5_priv_probe_raw(&access, (uint32_t)SP5_FIRMWARE_13_60_EXACT, &provenance).status !=
            SP5_OK ||
        provenance.validity != SP5_VALIDITY_VALID || sp5_priv_prepare(&access).status != SP5_OK ||
        !sp5_priv_prepared(&access) || sp5_priv_rollback(&access).status != SP5_OK ||
        sp5_priv_prepared(&access) || sp5_priv_rollback(&access).status != SP5_OK)
        return 5;
    if (sp5_ps5_media_global_refcount() != 0 || sp5_ps5_media_global_status() == NULL)
        return 6;
    return 0;
}
