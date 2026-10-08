# Environment lighting preprocessing

`ProcessEnvironmentImage` is the deterministic CPU preprocessing step for managed Radiance HDR environments. It produces a linear RGBA32F environment cubemap, a cosine-weighted diffuse irradiance cubemap, a GGX importance-sampled specular cubemap mip chain, and a split-sum BRDF integration LUT. It performs no gamma conversion, tone mapping, or display-range clamp.

The equirectangular mapping is `u = atan2(z, x) / (2π) + 0.5` with horizontal wrap, and `v = acos(y) / π` with vertical clamp. Cubemap faces use the fixed `+X, -X, +Y, -Y, +Z, -Z` order, with the Vulkan/NVRHI-compatible face directions documented by `TextureSubresourceData`. Mip zero of the prefiltered specular cube has roughness zero; the final mip has roughness one.

Default output sizes are 256 environment, 32 irradiance, 128 specular, and 256 BRDF LUT, with 128 deterministic Hammersley samples per convolution/integration operation. The processor bounds output storage to 64 MiB, each cube face to 1024, the LUT to 512, and sampling to 1024 per output texel. It is CPU-side groundwork; renderer caching and authored scene integration are added in the environment-rendering milestone.
