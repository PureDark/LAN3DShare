# NVIDIA video codec interface headers

Source: https://github.com/FFmpeg/nv-codec-headers/tree/n13.0.19.0/include/ffnvcodec

Pinned release: `n13.0.19.0` (NVIDIA Video Codec SDK 13.0 interface).
The headers retain their NVIDIA MIT license notices; see `LICENSE`.
These are API declarations, not FFmpeg codec implementations. No FFmpeg binary,
CUDA toolkit, or Video Codec SDK installation is required by the client.
The NVIDIA display driver provides the loaded runtime libraries. API 13.0
requires a compatible NVIDIA Windows driver (570 or later).

Unmodified `nvEncodeAPI.h` SHA-256:
`4fe4094541ef0f8a13249d97a8692dc5f835a6e9dd42eeadb3e2f7321d54dc7e`.

Unmodified NVDEC/CUDA interface header SHA-256 values:
- `dynlink_cuda.h`: `2c74cf9b0ace0503ca18d2c43645d6cb1ec0dd7ea7163f7880bba5141586f275`
- `dynlink_nvcuvid.h`: `2141dade75f3178520092dce2c02df9b47c4b19036b56bb9316bd83a07bf16f1`
- `dynlink_cuviddec.h`: `1244165ea5521d4be865995eb1c49b8910f755d0ca35004e1c4010ef7f612143`

The decoder uses driver-loaded PTX for BT.709 limited-range NV12 to BGRA
conversion. The last BGRA download preserves the current CPU Frame API; this
path is hardware decoded and GPU converted, but is not a zero-copy pipeline.
