#ifndef SPUDSTALE_H
#define SPUDSTALE_H

// A stale or double-freed handle, caught in Debug builds (decision X16 in
// the workspace's DECISIONS.md): a tag word in every handle's
// spud_object_head (spudallocator.h) that reads SPUD_LIVE while the object
// lives and SPUD_DEAD from its destroy call on.
//
// - Set to SPUD_LIVE where a create (or the owner of an object with no
//   destroy call of its own) fills the head: SPUD_BIRTH.
// - Written SPUD_DEAD at the destroy call, before the memory is freed, and
//   by an owner for each object it releases: SPUD_KILL.
// - Checked in every public function that takes a handle, right after that
//   handle's null check, as its own `if` that refuses exactly as the null
//   check does (the same result, the same fallback for a getter, a plain
//   return for a void call): SPUD_STALE. A Debug build prints one line on
//   stderr when it refuses, so a stale handle is seen even where the result
//   is ignored.
//
// One LIVE and one DEAD value for the whole library, not one per type:
// enough for a call after a destroy and for a second destroy. Anything but
// SPUD_LIVE refuses, so memory the allocator has written over since is
// caught too. Memory reused by a new object of the library can still pass;
// AddressSanitizer covers that case.
//
// Release builds have no tag, no check and no call: SPUD_STALE is the
// constant false, so the `if` it is in disappears, and SPUD_BIRTH and
// SPUD_KILL are nothing. Only _DEBUG decides it.
//
// The quarantine (card X27): the tag is only worth reading while the
// memory under it is still the dead handle's. So in a Debug build a
// destroy call does everything it did before (releases what the object
// holds, writes the tag dead), then hands the object's block to its
// owner's quarantine instead of the allocator, and the owner frees every
// block it holds when it is destroyed itself, through the same allocator,
// with the size and alignment each was allocated with. Until then no new
// object can be made in that memory and the allocator can't give its page
// back, so the tag stays SPUD_DEAD. Release frees at once, as before: no
// quarantine, no field, no call (spud_quarantine, spudallocator.h).
//
// The owner is the object at the root of the handle's tree that the
// caller must destroy after it, one rule per kind:
//
// - SpudGPU: everything made on a device (buffers and their views, images
//   and their views, samplers, heaps, shader modules, pipelines, descriptor
//   set layouts, pools and the sets a pool reset or destroy releases,
//   command allocators and lists, fences, semaphores, timestamp sets,
//   acceleration structures, swap chains and the back-buffer images and
//   views a swap chain releases): the device. A device and a surface: their
//   instance. (Vulkan and D3D12; the Metal backend still frees at once.)
// - SpudFiles: a file, a listing and a mapping: their instance.
// - SpudNet: a TCP listener or socket (an accepted one too), an HTTP
//   client and its transfers, a WebSocket, a wait set: their instance.
// - None, freed at once as before: the roots (every module's instance, and
//   SpudInput's, whose gamepads are records inside it); objects an owner
//   releases in its own destroy (a GPU instance's adapters, a device's
//   queues and bindless layout, an audio instance's devices), since the
//   owner is going too; a SpudNet resolver, whose memory may outlast its
//   instance while its lookup runs (spudnet.h); a SpudAudio stream, which
//   spudaudio.h doesn't say is destroyed before its instance; and a block
//   a create frees on its failure path, which no caller ever held.
//
// SPUD_BIRTH_IN(obj, quarantine) is SPUD_BIRTH that also names the owner's
// quarantine (a `spud_quarantine *` that only exists in Debug: Release
// drops the argument unread); SPUD_BIRTH names none. A destroy call frees
// with SPUD_RETIRE (spudallocator.h) where it freed with SPUD_DELETE.
// The quarantine allocates nothing and can't fail: a retired block's own
// memory, ahead of the tag, holds its link, its size and its alignment.
// spudfiles_test and spudgpu_objects_test count, through their counting
// allocator, that the blocks are held until the owner goes and all freed
// then, on every platform; AddressSanitizer's leak check (Linux, macOS)
// covers the rest.

#include <stdbool.h>
#include <stdint.h>

// "SPUDLIVE" and "SPUDDEAD" in ASCII, as a memory view shows the word on a
// little-endian machine.
#define SPUD_LIVE UINT64_C(0x4556494C44555053)
#define SPUD_DEAD UINT64_C(0x4441454444555053)

#if _DEBUG
#include <stdio.h>

// True, with a line on stderr, when [tag] isn't SPUD_LIVE.
static inline bool spud_stale_tag(uint64_t tag, const char *function, const char *param) {
	if (tag == SPUD_LIVE)
		return false;
	fprintf(stderr, "spudlib: %s: called with a destroyed %s\n", function, param);
	return true;
}

// [obj] is a handle, never NULL. Every handle's struct starts with its
// spud_object_head (spudallocator.h), so the head is reached through the
// pointer itself, also where the struct is a backend's and isn't complete
// (a front end checking a handle it passes on).
#define SPUD_HEAD_OF(obj) ((spud_object_head *)(void *)(obj))
#define SPUD_BIRTH(obj) (SPUD_HEAD_OF(obj)->debug_tag = SPUD_LIVE, SPUD_HEAD_OF(obj)->debug_quarantine = NULL)
#define SPUD_BIRTH_IN(obj, quarantine) \
	(SPUD_HEAD_OF(obj)->debug_tag = SPUD_LIVE, SPUD_HEAD_OF(obj)->debug_quarantine = (quarantine))
#define SPUD_KILL(obj) (SPUD_HEAD_OF(obj)->debug_tag = SPUD_DEAD)
#define SPUD_STALE(obj, function, param) spud_stale_tag(SPUD_HEAD_OF(obj)->debug_tag, (function), (param))
// The same check through a spud_object_head pointer.
#define SPUD_STALE_HEAD(head_ptr, function, param) spud_stale_tag((head_ptr)->debug_tag, (function), (param))
#else
#define SPUD_BIRTH(obj) ((void)0)
#define SPUD_BIRTH_IN(obj, quarantine) ((void)0)
#define SPUD_KILL(obj) ((void)0)
#define SPUD_STALE(obj, function, param) false
#define SPUD_STALE_HEAD(head_ptr, function, param) false
#endif

#endif // SPUDSTALE_H
