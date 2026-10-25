# Modern existing-weaver attachment (0.6.1)

Date: 2026-10-21. Target: already-running DX11/DX12 LeiaSR clients, including
the modern DX12 weaver retained by LikeADragonGaiden/YakuzaVRMod.

## Boundary and implementation

The public modern interface has input setters but no input getter. Hooking
CreateDX11Weaver/CreateDX12Weaver alone misses already-created objects, and
switching stereo off/on need not recreate them. Replaying a setter would mutate
the vendor object's state and is deliberately not used.

`ModernDiscovery` reads the actual loaded PE's MSVC RTTI and infers unique
virtual-base mappings from that image. It identifies public typed/base/destroy
tables and validates real object aliases at the original weave callback.
`SetterAnalysis` then supplies bounded read expressions derived from the actual
setter instructions before detours are installed. All matching fields must
agree. The recovery integration validates COM interface type and device, then
uses the ordinary capture path. Later real setter calls remain authoritative.

No private offset/RVA or runtime version in this note is compiled into capture.
Unsupported RTTI/code paths are rejected. This does not prove compatibility
with arbitrary builds, stripped RTTI, wrappers or enhanced-barrier-only DX12.

## Static vendor evidence

Official `C:\Program Files\LeiaSR\Platform\bin\SimulatedRealityDirectX.dll`:
SHA256 `6B3BE0BC1166D2C3566890DED5F7B494EB60CC8B91132E310CEA6DF1794B4C28`.
The installed signed build reports `57c9f88a7d v1.0.3.0`.

- CDX11Weaver: input setter RVA 0x15EC0, vtordisp adjustment then pimpl input
  storage; immediate context setter RVA 0x15DAC has two agreeing fields.
- CDX12Weaver: input setter RVA 0x1F930, vtordisp adjustment then pimpl input and
  format fields; command-list setter RVA 0x1F800.
- Both typed interfaces are themselves virtual bases. Assuming the SDK fixture's
  complete-object offset would therefore be incorrect for this binary.

Titan `F:\开发项目\TitanLeiaBridge\dist\TitanLeiaBridge-0.8.1-win64\runtime\TitanLeiaRuntime.dll`:
modern DX12 input setter RVA 0x2B5C0 stores both a ComPtr and request.inputSbs;
both must agree. Format is also recovered. List setter RVA 0x2B3D0.
Titan's DX11 path remains on the legacy public interface.

`ModernRuntimeLayoutTests` obtains all addresses from discovered tables, not
these example RVAs. It uses DONT_RESOLVE_DLL_REFERENCES: static mapping only,
with no runtime factory/context or physical display side effects.

## GPU and restart invariants

- A newly observed DX12 list may contain barriers recorded before attachment.
  That recording is skipped until an unambiguous transition is observed.
- Stop invalidates pending frame selection but preserves allocations referenced
  by GPU recordings. Start cannot republish those old candidates.
- A project-specific public PrivateData token links transparent command-list
  wrappers with the native list observed by ExecuteCommandLists. Capture waits
  for an actual matching submission before recording any readback. Missing or
  changed identities are rejected; unconfirmed retired copies remain held under
  the existing allocation limit rather than assuming that Reset implies GPU idle.
- All native GPU probing runs in the sender process. The injected DLL creates
  no probe window, device, factory, queue, allocator, list or swap chain. The
  profile contains module paths, PE/PDB identities and RVAs; hooks require a
  matching already-loaded executable image and cannot trigger profile DLL loads.
- The new DX12 fixture initially exited with 2173. A debug-event monitor proved
  D3D12 error #921 OBJECT_DELETED_WHILE_STILL_IN_USE at final swapchain release:
  its last fence preceded its final Present. The fixture now waits after that
  Present, retains natural COM destruction order, and passes with zero debug
  errors. This was not evidence of an input-recovery failure.

## Validation records

The first real 0.6.0 attachment exposed a host side effect despite the offline
checks below. `build/test-results/live-hotattach/game-before-restart.log` records
`queue changed after backend setup` followed by `NR stopped` and retained old
frames. `RecoverModernWeaver -> AttachDevice -> ProbePresent` created a temporary
swap chain with a new queue inside the game. YakuzaVRMod's CreateSwapChain hooks
then passed that queue to NRRuntime::SetQueue, whose post-initialization queue
change protection permanently set deviceFault. Stopping capture and disabling
its hooks could not reset the Mod's fault state. The user authorized one game
restart. 0.6.1 removes this in-host probe path; current validation is recorded
separately from the old 0.6.0 passing fixture results.

`build/test-results/modern-analysis-final.xml`: 3/3 analysis/runtime tests pass.
`build/test-results/modern-capture-final.xml`: 28/28 affected capture, injector,
GUI and ordinary-desktop integration checks pass, including all 8 modern
late-attach cases. Each late-attach fixture renders before injection and checks
pixels plus unchanged factory/setter counts after attach and Stop/Start.

Real-game acceptance is recorded separately by `tests/LiveHotAttach.ps1`; its
0.6.1 result is under `build/test-results/live-hotattach-061/`. It uses an
already-running source, two independent sender sessions, no desktop fallback,
hardware encoding/decoding and hidden preview. It does not launch/close the game
or test the remote physical Leia display.

0.6.1 affected results: 25/25 injection/direct/forward integration checks in
`native-probe-integration.xml`, the extended Core check in `native-core-final.xml`,
and the unchanged GUI/desktop passes from `native-probe-final.xml`. The first
modern12 creation-observer run caught DXGI's first Present1 allocator creation;
its PDB-resolved stack was NoFrameCheck -> Present1 thunk -> DXGI. The fixture now
warms up Present1 and fences before measuring, retaining the zero-creation rule.

The real game (PID 38040, started 19:59:37 local) passed the observation-only
phase and both 30-unique-frame NVENC AV1/NVDEC sessions. No additional restart
occurred. Raw SBS is 7680x2160 and encoded video is 3840x1080. Three independent
SBS/GDI samples per session show changing scene pixels, not only UI counters;
`pixel-comparison.json` records a character ROI excluding overlays. No new
queue-change/NR-stopped/device-loss messages were found. Short-run receive rate
was 11-14 FPS: this is a functionality result, not 4K60 performance acceptance.
GDI snapshots are composed desktop images, not raw Present textures. Desktop
Duplication returned black images in this environment and was not used to
decide whether the game scene was frozen.

## Primary API references

- [Microsoft DXGI best practices](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/dxgi-best-practices): synchronous window messages and DXGI threading.
- [Microsoft D3D12 descriptors](https://learn.microsoft.com/en-us/windows/win32/direct3d12/descriptors): descriptor handles are opaque; a previously supplied descriptor is not a public input-resource getter.
