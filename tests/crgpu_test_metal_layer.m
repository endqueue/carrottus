/*
 * crgpu_test_metal_layer — a CAMetalLayer with no window, for
 * crgpu_objects_test's swap chain checks on Metal. A layer needs no view
 * or window to be a Carrottus GPU surface, so the test can make swap chains on it
 * and read back what was set on the layer.
 */

#import <QuartzCore/QuartzCore.h>

#include "crgpu_test_metal_layer.h"

void *crgpu_test_metal_layer_create(bool geometry_flipped) {
	CAMetalLayer *layer   = [[CAMetalLayer alloc] init];
	layer.bounds          = CGRectMake(0, 0, 64, 64);
	layer.geometryFlipped = geometry_flipped ? YES : NO;
	return (void *)layer;
}

void crgpu_test_metal_layer_destroy(void *layer) {
	[(CAMetalLayer *)layer release];
}

const char *crgpu_test_metal_layer_gravity(void *layer) {
	NSString *gravity = ((CAMetalLayer *)layer).contentsGravity;
	if ([gravity isEqualToString:kCAGravityTopLeft])
		return "topLeft";
	if ([gravity isEqualToString:kCAGravityBottomLeft])
		return "bottomLeft";
	if ([gravity isEqualToString:kCAGravityResize])
		return "resize";
	if ([gravity isEqualToString:kCAGravityResizeAspect])
		return "resizeAspect";
	return "other";
}

void crgpu_test_metal_layer_set_gravity_other(void *layer) {
	((CAMetalLayer *)layer).contentsGravity = kCAGravityCenter;
}
