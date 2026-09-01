ps_5_0
dcl_globalFlags refactoringAllowed
dcl_sampler s0
dcl_resource_texture2d (float,float,float,float) t0
dcl_input_ps linear v1.xy
dcl_output o0.xyzw
dcl_temps 2
sample_indexable(texture2d) r0.xyzw, v1.xyxx, t0.xyzw, s0
add r1.x, r0.w, l(-5.00000000e-01)
mov o0.xyzw, r0.xyzw
lt r0.x, r1.x, l(0.00000000e+00)
discard_nz r0.x
ret
