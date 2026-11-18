/*
 * crgpu_test_metal_family - what a listed Metal GPU answers, for
 * crgpu_device_test's checks against Metal itself: whether it is in
 * MTLGPUFamilyApple10 (the image limits Metal's feature set tables give by
 * family), and whether it samples BC and ASTC LDR (card S48's two feature
 * bits).
 */
#ifndef CRGPU_TEST_METAL_FAMILY_H
#define CRGPU_TEST_METAL_FAMILY_H

#include <stdbool.h>
#include <stdint.h>

/* The GPU whose registryID is the first 8 bytes of [adapter_id]
 * (CRGPU_ADAPTER_PROPERTIES::adapter_id on Metal): true when it answers
 * supportsFamily:MTLGPUFamilyApple10. False when no such GPU is found. */
bool crgpu_test_metal_is_apple10(const uint8_t adapter_id[16]);

/* The same GPU's supportsBCTextureCompression into [out_bc], and whether
 * it answers supportsFamily:MTLGPUFamilyApple2 (ASTC LDR) into [out_astc].
 * False, both outputs false, when no such GPU is found. */
bool crgpu_test_metal_block_formats(
    const uint8_t adapter_id[16],
    bool *out_bc,
    bool *out_astc);

#endif
