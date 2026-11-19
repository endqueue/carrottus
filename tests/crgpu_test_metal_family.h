/*
 * crgpu_test_metal_family - what a listed Metal GPU answers, for
 * crgpu_device_test's checks against Metal itself: whether it is in
 * MTLGPUFamilyApple10 (the image limits Metal's feature set tables give by
 * family), whether it samples BC and ASTC LDR (card S48's two feature
 * bits), and what decides its format capabilities (card S51).
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

/* The same GPU's facts behind its format capabilities: the highest of
 * MTLGPUFamilyApple7 to Apple10 it answers to (7 to 10; 0 for none of
 * them), supports32BitFloatFiltering and readWriteTextureSupport (0 to 2).
 * False, the outputs 0, when no such GPU is found. */
bool crgpu_test_metal_format_facts(
    const uint8_t adapter_id[16],
    uint32_t *out_apple_family,
    bool *out_float32_filtering,
    uint32_t *out_read_write_tier);

#endif
