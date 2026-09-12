# Live paint binding point

At commit `b6bfdd7299c`, the actual EEVEE material texture path is:

```text
ShaderModule::material_shader_get()
  -> GPU_material_from_nodetree()
  -> GPUCodegen::generate_resources()
  -> PassBase::material_set() in draw/intern/draw_pass.hh
  -> GPU_material_textures() / bind_texture()
```

`PassBase::material_set()` only knows the generated `GPUMaterial`; it does not
know the EEVEE `Material` value that currently carries
`GolemicsPaintOverride`. The generated shader only declares samplers for image
nodes returned by `GPU_material_textures()`.

Adding `bind_texture("golemics_paint_base_color", ...)` at that point would
not work because the sampler is absent from the shader interface. Adding the
sampler unconditionally in `ShaderModule::material_create_info_amend()` would
change every material's resource layout and still would not affect the
generated base-color expression. Replacing the first generated image texture
would also be incorrect for materials with multiple image nodes.

The current patch provides the resource and permutation boundary:

1. `ShaderModule::material_create_info_amend()` reserves the named sampler and
   `use_golemics_paint` specialization constant.
2. `PassBase::material_set()` binds the live texture and keeps it alive through
   submission.
3. `GolemicsPaintGpuResources` owns seven MRT channels and exposes a material
   override with stable channel indices.

The remaining buildable implementation work is to replace the temporary
screen-coordinate lookup with the generated mesh UV varying, then apply the
remaining six channels to their corresponding PBR closure inputs.

That work is branch-specific shader codegen, not a resource ownership change.
It should be done only with a configured Blender build because shader interface
validation and generated GLSL/Metal/Vulkan resource layouts must be tested.
