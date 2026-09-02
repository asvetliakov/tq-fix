ps_5_0
dcl_globalFlags refactoringAllowed
dcl_constantBuffer cb0[12], immediateIndexed
dcl_sampler s0
dcl_sampler s1
dcl_sampler s2
dcl_sampler s3
dcl_resource_texture2d (float,float,float,float) t0
dcl_resource_texture2d (float,float,float,float) t1
dcl_resource_texture2d (float,float,float,float) t2
dcl_resource_texture2d (float,float,float,float) t3
dcl_input_ps linear v1.xy
dcl_output o0.xyzw
dcl_temps 4
mad r0.xy, v1.xyxx, l(1.00000000e+00, -1.00000000e+00, 0.00000000e+00, 0.00000000e+00), l(0.00000000e+00, 1.00000000e+00, 0.00000000e+00, 0.00000000e+00)
mad r0.xy, r0.xyxx, l(2.00000000e+00, 2.00000000e+00, 0.00000000e+00, 0.00000000e+00), l(-1.00000000e+00, -1.00000000e+00, 0.00000000e+00, 0.00000000e+00)
sample_indexable(texture2d) r0.z, v1.xyxx, t2.yzxw, s2
mov r0.w, l(1.00000000e+00)
dp4 r1.x, r0.xyzw, cb0[8].xyzw
dp4 r1.y, r0.xyzw, cb0[9].xyzw
dp4 r1.z, r0.xyzw, cb0[10].xyzw
dp4 r0.x, r0.xyzw, cb0[11].xyzw
div r0.xyz, r1.xyzx, r0.x
mov r0.w, l(1.00000000e+00)
dp4 r1.x, r0.xyzw, cb0[4].xyzw
dp4 r1.y, r0.xyzw, cb0[5].xyzw
dp4 r1.z, r0.xyzw, cb0[6].xyzw
dp4 r0.w, r0.xyzw, cb0[7].xyzw
add r0.xyz, -r0.xyzx, cb0[3].xyzx
div r1.xyz, r1.zxyz, r0.w
mad r2.xyzw, cb0[3].w, l(-5.00000000e-01, 0.00000000e+00, 5.00000000e-01, 0.00000000e+00), r1.yzyz
sample_indexable(texture2d) r0.w, r2.xyxx, t3.yzwx, s3
sample_indexable(texture2d) r1.w, r2.zwzz, t3.yzwx, s3
mov_sat r1.x, r1.x
add r1.x, r1.x, l(-3.50000011e-03)
lt r0.w, r0.w, r1.x
movc r0.w, r0.w, l(0), l(2.50000000e-01)
lt r1.w, r1.w, r1.x
movc r1.w, r1.w, l(0), l(2.50000000e-01)
add r0.w, r0.w, r1.w
mad r2.xyzw, cb0[3].w, l(0.00000000e+00, -5.00000000e-01, 0.00000000e+00, 5.00000000e-01), r1.yzyz
sample_indexable(texture2d) r1.w, r2.xyxx, t3.yzwx, s3
sample_indexable(texture2d) r2.x, r2.zwzz, t3.xyzw, s3
lt r2.x, r2.x, r1.x
movc r2.x, r2.x, l(0), l(2.50000000e-01)
lt r1.w, r1.w, r1.x
add r1.x, -r1.x, l(1.00000000e+00)
movc r1.w, r1.w, l(0), l(2.50000000e-01)
add r0.w, r0.w, r1.w
add r0.w, r2.x, r0.w
add r0.w, r0.w, l(-1.00000000e+00)
add r2.xy, -r1.yzyy, l(1.00000000e+00, 1.00000000e+00, 0.00000000e+00, 0.00000000e+00)
min r1.yz, r1.yyzy, r2.xxyx
min r1.y, r1.z, r1.y
min r1.x, r1.x, r1.y
mul_sat r1.x, r1.x, l(2.00000000e+01)
mad r0.w, r1.x, r0.w, l(1.00000000e+00)
dp3 r1.x, r0.xyzx, r0.xyzx
rsq r1.x, r1.x
mul r0.xyz, r0.xyzx, r1.x
sample_indexable(texture2d) r1.xyz, v1.xyxx, t0.xyzw, s0
add r1.xyz, r1.xyzx, l(-5.00000000e-01, -5.00000000e-01, -5.00000000e-01, 0.00000000e+00)
add r2.xyz, r1.xyzx, r1.xyzx
dp3 r1.w, cb0[1].xyzx, cb0[1].xyzx
rsq r1.w, r1.w
mul r3.xyz, r1.w, cb0[1].xyzx
dp3 r1.w, r2.xyzx, r3.xyzx
mul r1.xyz, r1.xyzx, r1.w
mov_sat r1.w, r1.w
mul r2.xyz, r1.w, cb0[0].xyzx
mul o0.xyz, r0.w, r2.xyzx
mad r1.xyz, r1.xyzx, l(4.00000000e+00, 4.00000000e+00, 4.00000000e+00, 0.00000000e+00), -r3.xyzx
dp3_sat r0.x, r0.xyzx, r1.xyzx
log r0.x, r0.x
sample_indexable(texture2d) r1.xyzw, v1.xyxx, t1.xyzw, s1
mul r0.y, r1.w, l(6.40000000e+01)
mul r0.x, r0.x, r0.y
exp r0.x, r0.x
min r0.x, r0.x, l(1.00000000e+00)
mul r0.xyz, r1.xyzx, r0.x
mul r0.xyz, r0.xyzx, cb0[0].xyzx
mul r0.xy, r0.xyxx, l(5.00000000e-01, 5.00000000e-01, 0.00000000e+00, 0.00000000e+00)
add r0.x, r0.y, r0.x
mad r0.x, r0.z, l(5.00000000e-01), r0.x
mul r0.x, r0.w, r0.x
mul o0.w, r0.x, l(3.33299994e-01)
ret
