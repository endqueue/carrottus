/*
 * crgpu_test_metal_family - whether a listed Metal GPU is in
 * MTLGPUFamilyApple10, for crgpu_device_test's check of the image limits
 * Metal's feature set tables give by family.
 */
#ifndef CRGPU_TEST_METAL_FAMILY_H
#define CRGPU_TEST_METAL_FAMILY_H

#include <stdbool.h>
#include <stdint.h>

/* The GPU whose registryID is the first 8 bytes of [adapter_id]
 * (CRGPU_ADAPTER_PROPERTIES::adapter_id on Metal): true when it answers
 * supportsFamily:MTLGPUFamilyApple10. False when no such GPU is found. */
bool crgpu_test_metal_is_apple10(const uint8_t adapter_id[16]);

#endif
