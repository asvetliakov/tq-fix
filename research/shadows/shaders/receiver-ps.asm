ps_5_0
dcl_globalFlags refactoringAllowed
dcl_constantBuffer cb0[12], immediateIndexed
dcl_sampler s0
dcl_sampler s1
dcl_sampler s2
dcl_resource_texture2d (float,float,float,float) t0
dcl_resource_texture2d (float,float,float,float) t1
dcl_resource_texture2d (float,float,float,float) t2
dcl_input_ps linear v1.xy
dcl_input_ps linear v2.xyz
dcl_input_ps linear v3.xyz
dcl_input_ps linear v4.xyz
dcl_input_ps linear v5.xyz
dcl_output o0.xyzw
dcl_temps 4
mad r0.xyzw, cb0[0].x, l(-5.00000000e-01, 0.00000000e+00, 5.00000000e-01, 0.00000000e+00), v5.xyxy
sample_indexable(texture2d) r0.x, r0.xyxx, t2.xyzw, s2
sample_indexable(texture2d) r0.y, r0.zwzz, t2.yxzw, s2
lt r0.xy, r0.xyxx, v5.z
movc r0.xy, r0.xyxx, l(0, 0, 0, 0), l(2.50000000e-01, 2.50000000e-01, 0, 0)
add r0.x, r0.y, r0.x
mad r1.xyzw, cb0[0].x, l(0.00000000e+00, -5.00000000e-01, 0.00000000e+00, 5.00000000e-01), v5.xyxy
sample_indexable(texture2d) r0.y, r1.xyxx, t2.yxzw, s2
sample_indexable(texture2d) r0.z, r1.zwzz, t2.yzxw, s2
lt r0.yz, r0.yyzy, v5.z
movc r0.yz, r0.yyzy, l(0, 0, 0, 0), l(0, 2.50000000e-01, 2.50000000e-01, 0)
add r0.x, r0.y, r0.x
add r0.x, r0.z, r0.x
add r0.x, r0.x, l(-1.00000000e+00)
add r0.yzw, -v5.xxyz, l(0.00000000e+00, 1.00000000e+00, 1.00000000e+00, 1.00000000e+00)
min r0.yz, r0.yyzy, v5.xxyx
min r0.y, r0.z, r0.y
min r0.y, r0.w, r0.y
mul_sat r0.y, r0.y, l(2.00000000e+01)
mad r0.x, r0.y, r0.x, l(1.00000000e+00)
dp3 r0.y, v2.xyzx, v2.xyzx
rsq r0.y, r0.y
mul r0.yzw, r0.y, v2.xxyz
sample_indexable(texture2d) r1.xyzw, v1.xyxx, t1.xyzw, s1
add r1.xyz, r1.xyzx, l(-5.00000000e-01, -5.00000000e-01, -5.00000000e-01, 0.00000000e+00)
add r2.xyz, r1.xyzx, r1.xyzx
dp3 r2.w, r2.xyzx, r0.yzwy
mul r1.xyz, r1.xyzx, r2.w
mov_sat r2.w, r2.w
mad r0.yzw, r1.xxyz, l(0.00000000e+00, 4.00000000e+00, 4.00000000e+00, 4.00000000e+00), -r0.yyzw
dp3_sat r0.y, v3.xyzx, r0.yzwy
log r0.y, r0.y
mul r0.y, r0.y, cb0[9].w
exp r0.y, r0.y
min r0.y, r0.y, l(1.00000000e+00)
mul r0.yzw, r0.y, cb0[9].xxyz
sample_indexable(texture2d) r1.xyz, v1.xyxx, t0.xyzw, s0
mul r3.xyz, r1.xyzx, r2.w
mul r3.xyz, r3.xyzx, cb0[8].xyzx
mad r0.yzw, r0.yyzw, r1.w, r3.xxyz
mul r0.yzw, r0.yyzw, cb0[11].xxyz
mul r0.xyz, r0.x, r0.yzwy
dp3 r0.w, v4.xyzx, v4.xyzx
rsq r0.w, r0.w
mul r3.xyz, r0.w, v4.xyzx
dp3 r0.w, r2.xyzx, r3.xyzx
mad r0.w, r0.w, l(5.00000000e-01), l(5.00000000e-01)
add r2.xyz, cb0[6].xyzx, -cb0[7].xyzx
mad r2.xyz, r0.w, r2.xyzx, cb0[7].xyzx
add r2.xyz, r2.xyzx, cb0[8].w
mad o0.xyz, r2.xyzx, r1.xyzx, r0.xyzx
mov o0.w, l(1.00000000e+00)
ret
