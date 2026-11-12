#ifndef CRSTALE_H
#define CRSTALE_H

// A stale or double-freed handle, caught in Debug builds (decision X16 in
// the workspace's DECISIONS.md): a tag word in every handle's
// cr_object_head (crallocator.h) that reads CR_LIVE while the object
// lives and CR_DEAD from its destroy call on.
//
// - Set to CR_LIVE where a create (or the owner of an object with no
//   destroy call of its own) fills the head: CR_BIRTH.
// - Written CR_DEAD at the destroy call, before the memory is freed, and
//   by an owner for each object it releases: CR_KILL.
// - Checked in every public function that takes a handle, right after that
//   handle's null check, as its own `if` that refuses exactly as the null
//   check does (the same result, the same fallback for a getter, a plain
//   return for a void call): CR_STALE. A Debug build prints one line on
//   stderr when it refuses, so a stale handle is seen even where the result
//   is ignored.
//
// One LIVE and one DEAD value for the whole library, not one per type:
// enough for a call after a destroy and for a second destroy. Anything but
// CR_LIVE refuses, so memory the allocator has written over since is
// caught too. Memory reused by a new object of the library can still pass;
// AddressSanitizer covers that case.
//
// Release builds have no tag, no check and no call: CR_STALE is the
// constant false, so the `if` it is in disappears, and CR_BIRTH and
// CR_KILL are nothing. Only _DEBUG decides it.
//
// The quarantine (card X27): the tag is only worth reading while the
// memory under it is still the dead handle's. So in a Debug build a
// destroy call does everything it did before (releases what the object
// holds, writes the tag dead), then hands the object's block to its
// owner's quarantine instead of the allocator, and the owner frees every
// block it holds when it is destroyed itself, through the same allocator,
// with the size and alignment each was allocated with. Until then no new
// object can be made in that memory and the allocator can't give its page
// back, so the tag stays CR_DEAD. Release frees at once, as before: no
// quarantine, no field, no call (cr_quarantine, crallocator.h).
//
// The owner is the object at the root of the handle's tree that the
// caller must destroy after it, one rule per kind:
//
// - Carrottus GPU: everything made on a device (buffers and their views, images
//   and their views, samplers, heaps, shader modules, pipelines, descriptor
//   set layouts, pools and the sets a pool reset or destroy releases,
//   command allocators and lists, fences, semaphores, timestamp sets,
//   acceleration structures, swap chains and the back-buffer images and
//   views a swap chain releases): the device. A device and a surface: their
//   instance. (Every backend: Vulkan, D3D12 and Metal.)
// - Carrottus Files: a file, a listing, a mapping and a library: their instance.
// - Carrottus Net: a TCP listener or socket (an accepted one too), an HTTP
//   client and its transfers, a WebSocket, a wait set: their instance.
// - None, freed at once as before: the roots (every module's instance, and
//   Carrottus Input's, whose gamepads are records inside it); objects an owner
//   releases in its own destroy (a GPU instance's adapters, a device's
//   queues and bindless layout, an audio instance's devices), since the
//   owner is going too; a Carrottus Net resolver, whose memory may outlast its
//   instance while its lookup runs (crnet.h); a Carrottus Audio stream, which
//   craudio.h doesn't say is destroyed before its instance; and a block
//   a create frees on its failure path, which no caller ever held.
//
// CR_BIRTH_IN(obj, quarantine) is CR_BIRTH that also names the owner's
// quarantine (a `cr_quarantine *` that only exists in Debug: Release
// drops the argument unread); CR_BIRTH names none. A destroy call frees
// with CR_RETIRE (crallocator.h) where it freed with CR_DELETE.
// The quarantine allocates nothing and can't fail: a retired block's own
// memory, ahead of the tag, holds its link, its size and its alignment.
// crfiles_test and crgpu_objects_test count, through their counting
// allocator, that the blocks are held until the owner goes and all freed
// then, on every platform; AddressSanitizer's leak check (Linux, macOS)
// covers the rest.

#include <stdbool.h>
#include <stdint.h>

// "CRLIVE" and "CRDEAD" in ASCII, as a memory view shows the word on a
// little-endian machine.
#define CR_LIVE UINT64_C(0x4556494C44555053)
#define CR_DEAD UINT64_C(0x4441454444555053)

#if _DEBUG
#include <stdio.h>

// True, with a line on stderr, when [tag] isn't CR_LIVE.
static inline bool cr_stale_tag(uint64_t tag, const char *function, const char *param) {
	if (tag == CR_LIVE)
		return false;
	fprintf(stderr, "carrottus: %s: called with a destroyed %s\n", function, param);
	return true;
}

// [obj] is a handle, never NULL. Every handle's struct starts with its
// cr_object_head (crallocator.h), so the head is reached through the
// pointer itself, also where the struct is a backend's and isn't complete
// (a front end checking a handle it passes on).
#define CR_HEAD_OF(obj) ((cr_object_head *)(void *)(obj))
#define CR_BIRTH(obj) (CR_HEAD_OF(obj)->debug_tag = CR_LIVE, CR_HEAD_OF(obj)->debug_quarantine = NULL)
#define CR_BIRTH_IN(obj, quarantine) \
	(CR_HEAD_OF(obj)->debug_tag = CR_LIVE, CR_HEAD_OF(obj)->debug_quarantine = (quarantine))
#define CR_KILL(obj) (CR_HEAD_OF(obj)->debug_tag = CR_DEAD)
#define CR_STALE(obj, function, param) cr_stale_tag(CR_HEAD_OF(obj)->debug_tag, (function), (param))
// The same check through a cr_object_head pointer.
#define CR_STALE_HEAD(head_ptr, function, param) cr_stale_tag((head_ptr)->debug_tag, (function), (param))
#else
#define CR_BIRTH(obj) ((void)0)
#define CR_BIRTH_IN(obj, quarantine) ((void)0)
#define CR_KILL(obj) ((void)0)
#define CR_STALE(obj, function, param) false
#define CR_STALE_HEAD(head_ptr, function, param) false
#endif

#endif // CRSTALE_H
