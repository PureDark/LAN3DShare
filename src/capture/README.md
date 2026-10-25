# Capture implementation and verified scope

`LAN3DCapture.dll` is injected into an explicitly selected x64 process. The
sender probes native GPU entry points in its own process and the injector calls
`LAN3DSetGpuHooks`, `LAN3DConfigure` and then `LAN3DStart` outside `DllMain`. Nothing is
installed in the Leia runtime or a game's mod directory.

The DLL hooks exports of the actually loaded `SimulatedRealityDirectX.dll`.
`GetProcAddress` follows PE export forwarders, including the factory boundary
used by TitanLeiaBridge. Factory results retain their original object identity,
query interfaces and destroy operation. Virtual-base adjustments and method
slots are derived using the public SDK headers and the compiling MSVC ABI.
The supported interface contract is `IDX11Weaver1` / `IDX12Weaver1` /
`IWeaverBase1`, plus the public legacy DX11/DX12 constructor, setter, getter and
weave exports. An incompatible future ABI requires another adapter; no private
runtime layout is guessed from a version number or hardcoded private offset.

LoadLibrary hooks only signal the capture worker after a successful load; they
never scan or take the capture control lock on the loading thread. A nested
load can return while an outer DLL entry point still holds the loader lock,
so synchronous scanning there could deadlock with the worker's module queries.
The event wakes asynchronous discovery; a 25 ms poll also covers later arrivals.
Attachment is not guaranteed before the loading application immediately calls
the new module's factory or setters. Modern objects created before attachment are found
at their next original weave call. The loaded implementation's MSVC RTTI,
complete-object locators and uniquely inferred virtual-base tables identify
the public interface tables. Live object pointers, all base displacements and
vptrs are checked before registering aliases; no heap scan or dummy runtime
object is used. PE forwarders are followed to the actual implementation module.

A bounded x64 analysis of each actual setter derives read-only expressions for
the stored SBS, immediate context/direct list and DX12 format. It handles
direct fields, pimpl indirection and this-adjust/vtordisp thunks. Candidate
stores must be unambiguous and agree, including null. Unsupported code, partial
overwrites and candidate addresses escaping to unknown calls are rejected.
The setter is never replayed. Fields are read on the original weave thread;
COM type, device, command-list/context type and resource properties are checked
before capture. A later real setter is authoritative even if old-field recovery
is unsupported. Discovery depends on intact MSVC RTTI and the supported public
ABI, not a particular runtime version; arbitrary future layouts are not promised.

Existing legacy weavers can be
recognized at their next public setter/getter/weave call. The weave hook uses
the implementation's public `getFrameBuffer` only when no input was observed;
a null getter does not expose an externally supplied texture, so capture waits
for a later input setter.

Present and native queue hooks use the external probe's module path, PE identity,
PDB identity and RVA. Only matching already-loaded executable images are pinned
and hooked; the injected DLL does not load modules named by the profile or create
probe windows, devices, factories, queues, allocators, lists or swap chains.
Missing modules are retried when loaded; unverified hooks pause capture.
Device/list hooks observe actual objects supplied by the application. DX11 can recover
the device's immediate context from a valid SRV/RTV, consistent with the legacy
weaver's immediate-context contract. An explicitly observed context, including
a deferred or null context, is authoritative and is never replaced by this
fallback. DX12 still requires a subsequently observed command list and resource
transition; a list discovered only at weave cannot recover barriers recorded
before the hook was attached and that recording is skipped safely.

Only one stream is selected. IDs are assigned when weavers are observed and
reported through the frame bus status. `streamId=0` follows the first live
weaver, including a recreated weaver. `weaveOrdinal=0` chooses the last weave
before the stream's window Presents; a positive ordinal explicitly selects the
first/second/etc weave in that presentation interval. Copies happen at weave,
so overwriting the same source texture for a mask and final image does not
corrupt the selected image. Both Present and Present1 are hooked. Applications
with unusual multiple-swapchain/offscreen presentation boundaries require an
explicit window/ordinal selection or a further adapter.

`maxFps` selects entire presentation intervals before allocating or recording
copies. Its next deadline advances by a fixed period, not `now + period` on
every accepted frame, so a 250 FPS source does not quantize a 60 FPS request
down to 50 FPS. Every weave within an eligible interval remains a candidate;
the last-weave and explicit-ordinal rules are unchanged.

DX11 copies the selected 2D SRV mip and single array slice into a six-slot
texture ring and polls event queries without flushing or waiting. CPU mode uses
staging textures; `CaptureConfig.reserved & CaptureGpuOutput` uses default-heap
shared NT textures instead. Deferred
contexts, multisampling, multiple visible array slices and unsupported formats
are rejected. DX12 accepts a direct command list and a non-MSAA 2D input with
one mip and one array slice. Only an explicit legacy ResourceBarrier observed
in that command recording establishes the source state. Split/enhanced/aliasing
barriers and bundles invalidate tracked state; capture resumes after a new
unambiguous transition. Unknown state is reported and skipped. Copy barriers
restore the exact observed state before the original weave runs.

DX12 lists carry a project-specific token through the public SetPrivateData /
GetPrivateData methods. A wrapper and its underlying submitted list are joined
only when the same token is observed in an actual ExecuteCommandLists call.
Before that observation no capture copy is recorded. Wrappers that do not forward
this metadata are reported and skipped; their private memory is never scanned.
DX12 submission associates each recorded copy with the actual command queue and
signals a GPU fence after ExecuteCommandLists. CPU readback waits for every
associated queue completion. A readback remains owned by its command recording
until Reset/destruction, even after completion, because lists may be executed
again. Reset retires old allocations until completion. An unobserved submission
is not evidence that the GPU did not execute a copy: such retired allocations
remain quarantined within the readback budget instead of being freed early.
Destroying a weaver
discards undecided candidates without releasing in-flight allocations. A full
readback budget drops the new candidate instead of blocking the application's
rendering. Private command queue wrappers that bypass the observed native COM
entry points are outside the verified scope.

GPU mode creates a shared default-heap DX12 texture on the application's
existing device. The selected original direct command list copies into it and
restores both the source's observed state and the shared destination's COMMON
state. No new device, queue, list, swap chain or shader is required in the game.
The source texture is retained until the recording's GPU work is complete.
Unlike CPU readback, GPU metadata is published only after that recording has
been retired by Reset/final destruction and all of its observed queue fences
have completed. This prevents another execution of the old recording from
overwriting a texture while the external encoder reads it.

`GpuFrame` carries the named texture, adapter LUID, dimensions, pixel format,
sequence and timestamp; it contains no pixels. DX11 publishes after its event
query completes. A successful `SetGpuOutput` callback transfers a consumer
lease: neither backend can overwrite or recycle that texture until
`released(sequence)` reports completion of every consumer GPU read. Stop,
weaver destruction, network disconnect and elapsed time do not revoke a lease.
A stalled consumer causes new captures to be dropped, never a render-thread
wait or an overwrite. DX12 keeps at most six completed, unleased textures in
its reuse pool, avoiding per-frame committed-resource creation. Each backend
has a 24-allocation / 512 MiB capture budget (DX11 counts pixel bytes); unknown
submissions and unacknowledged leases count toward that budget.

The image contract is SDR RGBA8/BGRA8 input, sent as top-down BGRA8 left|right
SBS using the actual resource/view dimensions. HDR, tone mapping, separate-eye
textures, arbitrary crops, protected textures, D3D11 deferred contexts and
enhanced-barrier-only DX12 rendering are not supported by this first version.
CPU mode retains top-down BGRA8 readback for diagnostics and compatibility.
GPU mode preserves source RGBA8/BGRA8 byte order in the shared texture and
normalizes the sRGB view format to UNORM without changing pixels. Its producer
does no Map/readback or CPU frame copy. GPU scaling, color conversion and NVENC
input belong to the external sender. This is a GPU copy into a reusable shared
texture, not direct ownership of the game's mutable input texture. Real-game
4K60 and game-frame-rate impact still require measured acceptance.

Stop disables capturing/publishing. Hooks, the capture DLL and hooked runtime
modules stay resident until process exit; unloading them while runtime objects
or GPU recordings still refer to them would be unsafe. A partial initialization
reports a persistent error rather than claiming that a retry succeeded.
Pending copies are invalidated across Stop/Start without freeing GPU-visible
allocations, so re-sharing the same running weaver cannot replay an old frame.

Offline tests use real WARP devices with D3D11/D3D12 debug layers. Core tests
verify pixel contents, mask/final selection, SRV mip/slice handling, explicit
ordinal selection, recreated automatic streams, unknown-state rejection,
GPU-gated publishing, in-flight Reset, repeated command-list execution, and
destroy-before-Present retirement. GPU-mode tests additionally open actual
DX11 and DX12 shared NT textures through a separate DX11 device, verify pixel
contents, require Reset plus fence completion before DX12 publication, reject
early re-execution publication, retain leased pixels under backpressure,
reuse the same named allocation only after release, enforce the allocation
cap, and confirm that no CPU frame callback runs. Fake runtime integration tests cover modern
DX11/DX12 factories, legacy DX11/DX12 exports, late loading, query/destroy
identity, Present1 and PE-forwarded modern factories. Modern late-attach cases
create/configure/render before loading capture, then verify changed pixels and
Stop/Start without another factory or setter call. Direct and pimpl layouts,
null input/context, deferred/bundle rejection and destroyed/replaced objects
are covered for direct and forwarded DLLs. Separate static tests map the
installed official runtime and Titan runtime without running DllMain/factories,
and validate discovered layouts and input access plans. These do not substitute
for real Leia/Titan runtime, game, physical display, dual-machine or performance
acceptance.
