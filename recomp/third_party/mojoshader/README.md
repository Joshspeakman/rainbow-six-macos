# MojoShader (vendored)

Direct3D 9 shader bytecode (vs/ps 1.1-3.0) to GLSL (Linux) and MSL (macOS), by Ryan C. Gordon; zlib licence
(LICENSE.txt). From https://github.com/icculus/mojoshader at ad5dff84830c2863c841f4b1f4e3df78c705b383, only the parser
and the GLSL/Metal profiles, with one change: `profiles/mojoshader_profile_metal.c` declares centroid-sampled pixel
shader inputs `[[centroid_perspective]]` instead of failing (Fallout NV's shaders use `dcl_centroid`).

`profiles/mojoshader_profile_glsl.c`: centroid-sampled inputs are interpolated normally instead of failing (the
built-in varyings this profile uses cannot take the qualifier; the difference is only at multisampled edges).
`mov a0, ...` in vertex shaders (shader model 1.1) converts like `mova` instead of assigning a float to the integer
address register (which GLSL rejects).

`mojoshader.c`: a relatively addressed float constant array runs to the end of the register file (c255 in vertex
shaders, c223 in pixel shaders) instead of stopping where the constant table's declaration ends, and relative
addressing outside every declared array gets such an array of its own instead of failing. Direct3D reads the whole
register file; Fallout NV's skinning shaders declare 18 bones and index 26 or more, and reading past a GLSL array is
undefined (on NVIDIA: whatever an earlier draw left there, which threw skinned meshes across the screen).

`profiles/mojoshader_profile_glsl.c`: `nrm`, `pow` and `log` follow Direct3D where GLSL gives NaN: a zero vector
normalises to zero, `pow(0, 0)` is 1, and `log` is of the absolute value. The NaNs showed as flashing specks on
skinned characters' specular (Doc Mitchell's gloves in the opening).
