/*
 * spudgpu_image_first_use_draft.h - what the first use of an attachment
 * image must be, for every image, committed or placed (card S38, decided
 * 2026-11-05 with the user, a; the workspace's DECISIONS.md, "Decided").
 *
 * APPROVED 2026-11-05 by the user, as written here; built the same day.
 *
 * DRAFT, 2026-11-05. Built into spudgpu.h. Not compilable: it
 * repeats two declarations of spudgpu.h to change their comments. Every
 * declaration below is marked [ADDED], [CHANGED] or [REMOVED]; nothing
 * unmarked changes. Once approved it is built into spudgpu.h, and where
 * building finds this draft wrong or incomplete, a card says so and this
 * file is not edited to match.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * The D3D12 backend creates every resource with
 * D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, because a new image's contents are
 * undefined until written on every backend (spudgpu_create_image()). D3D12
 * has one rule more for a resource with render target or depth/stencil
 * flags made that way, committed or placed: "must be initialized with a
 * Discard/Clear/Copy operation before other operations are supported"
 * (debug layer message 1422). A write through a storage view does not count.
 * spudgpu.h states that rule only for an image placed in an
 * attachment-images heap. A committed image with an attachment usage is
 * under it too, and nothing tells the caller: on 2026-11-05 on Windows a
 * caller's test made an image with color attachment, storage and sampled
 * usage, wrote it first by a compute pass and copied it back, and D3D12's
 * debug layer reported 1422 on every adapter. Vulkan and Metal have no such
 * rule, so it ran clean there.
 *
 * Decided (a): the header states the narrowest rule every API keeps, for
 * every image with an attachment usage, on every backend, as it does for
 * placed ones; the caller clears or copies first. Not (b), zeroed
 * committed attachments on D3D12 (a cost at every creation, hidden behind a
 * flag no other backend has, and placed images keeping the rule anyway),
 * nor (c), a discard command (a new call, built if a caller needs to skip
 * the clear's cost).
 *
 * ABI: none. Two comments change; no declaration, type, value or layout.
 * Behaviour: none in SpudGPU. A caller that writes an attachment image
 * first by anything but a clearing pass or a copy is outside the contract,
 * as one using a placed attachment image that way already was.
 *
 * Compared: D3D12 (the rule, for committed and placed resources made not
 * zeroed: "ID3D12Device::CreateCommittedResource" and the 1422 message),
 * Vulkan (an image starts in VK_IMAGE_LAYOUT_UNDEFINED: its contents are
 * undefined, any first use allowed), Metal (a new texture's contents are
 * undefined, any first use allowed). The rule is D3D12's, held on all three
 * so a caller that follows it behaves the same everywhere (spudlib's
 * CLAUDE.md, "one rule that holds on all of them").
 *
 * Tests: spudgpu_objects_test makes a committed image with color attachment
 * and storage usage, clears it in a rendering pass, then writes it by a
 * compute dispatch and reads it back, under D3D12's debug layer with no
 * message (the rule's path), on every adapter. What a caller does outside
 * the rule is undefined and not tested. The caller found by the run
 * (a test's reflection texture) clears it first.
 */

#include "spudgpu.h" /* for the types; this file is never compiled */

/**
 * [CHANGED] Its comment: the second paragraph is new.
 *
 * @brief Allocates a GPU image (texture).
 *
 * The image's contents are undefined until written or cleared, on every
 * backend: what is read from a texel nothing has written is not zero or any
 * other value the caller can rely on.
 *
 * An image whose `desc->usage` has SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT or
 * SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT has one rule more, on every
 * backend: the first thing done with it writes all of every subresource, by
 * a rendering pass that clears each attachment over the whole image (depth
 * and stencil both, for a format with the two) or by copies into all of
 * every subresource. What anything else done first (a write through a
 * storage view, a draw that loads, a copy out, a read in a shader) reads or
 * leaves behind is undefined. (D3D12 requires a render target or
 * depth/stencil resource whose memory isn't zeroed to be initialized by a
 * clear, a full copy or a discard before any other use.)
 *
 * (Parameters, results and the rest unchanged.)
 */
SPUDRESULT spudgpu_create_image(
    spudgpu_device device,
    const spudgpu_image_desc *desc,
    spudgpu_image *out_image);

/**
 * [CHANGED] Its comment: the attachment-images paragraph refers to
 * spudgpu_create_image()'s rule instead of stating its own.
 *
 * @brief Creates an image in `heap`'s memory at `offset`.
 *
 * The result is a spudgpu_image like any other, destroyed with
 * spudgpu_destroy_image(), with a placed buffer's rules for what destroying
 * it frees and what overlapping placements are, and spudgpu_create_image()'s
 * rules for its contents and for the first use of an attachment image. An
 * image in a SPUDGPU_HEAP_RESOURCE_CLASS_ATTACHMENT_IMAGES heap has an
 * attachment usage, so that first-use rule is always its.
 *
 * (Parameters, results and the rest unchanged.)
 */
SPUDRESULT spudgpu_create_placed_image(
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_image_desc *desc,
    spudgpu_image *out_image);
