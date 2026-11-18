/*
 * crgpu_test_metal_family - see crgpu_test_metal_family.h.
 */

#import <Metal/Metal.h>

#include <string.h>

#include "crgpu_test_metal_family.h"

bool crgpu_test_metal_is_apple10(const uint8_t adapter_id[16]) {
	uint64_t registry_id = 0;
	memcpy(&registry_id, adapter_id, sizeof(registry_id));
	bool found                   = false;
	bool apple10                 = false;
	NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
	for (id<MTLDevice> device in all) {
		if (device.registryID == registry_id) {
			found   = true;
			apple10 = [device supportsFamily:MTLGPUFamilyApple10];
			break;
		}
	}
	[all release];
	return found && apple10;
}

bool crgpu_test_metal_block_formats(
    const uint8_t adapter_id[16],
    bool *out_bc,
    bool *out_astc) {
	uint64_t registry_id = 0;
	memcpy(&registry_id, adapter_id, sizeof(registry_id));
	bool found                  = false;
	*out_bc                     = false;
	*out_astc                   = false;
	NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
	for (id<MTLDevice> device in all) {
		if (device.registryID == registry_id) {
			found     = true;
			*out_bc   = device.supportsBCTextureCompression;
			*out_astc = [device supportsFamily:MTLGPUFamilyApple2];
			break;
		}
	}
	[all release];
	return found;
}
