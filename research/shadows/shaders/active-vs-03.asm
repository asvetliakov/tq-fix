vs_5_0
dcl_globalFlags refactoringAllowed
dcl_constantBuffer cb0[28], immediateIndexed
dcl_input v0.xyz
dcl_input v1.xy
dcl_input v2.xyz
dcl_input v3.xyz
dcl_input v4.xyz
dcl_output_siv o0.xyzw, position
dcl_output o1.xy
dcl_output o2.xyzw
dcl_output o3.xyzw
dcl_output o4.xyzw
dcl_output o5.xyz
dcl_output o6.xyz
dcl_output_siv o7.x, clip_distance
dcl_temps 7
mov r0.xyz, v0.xyzx
mov r0.w, l(1.00000000e+00)
dp4 r1.x, r0.xyzw, cb0[18].xyzw
dp4 r1.y, r0.xyzw, cb0[19].xyzw
dp4 r1.z, r0.xyzw, cb0[20].xyzw
dp4 r1.w, r0.xyzw, cb0[21].xyzw
mov o0.xyzw, r1.xyzw
dp4 r1.x, cb0[27].xyzw, r1.xyzw
mov o1.xy, v1.xyxx
dp3 r2.x, v3.xyzx, cb0[22].xyzx
dp3 r2.z, v3.xyzx, cb0[24].xyzx
dp3 r2.y, v3.xyzx, cb0[23].xyzx
dp3 r3.x, cb0[14].xyzx, r2.xyzx
dp3 r4.x, -v4.xyzx, cb0[22].xyzx
dp3 r4.z, -v4.xyzx, cb0[24].xyzx
dp3 r4.y, -v4.xyzx, cb0[23].xyzx
dp3 r3.y, cb0[14].xyzx, r4.xyzx
dp3 r5.x, v2.xyzx, cb0[22].xyzx
dp3 r5.z, v2.xyzx, cb0[24].xyzx
dp3 r5.y, v2.xyzx, cb0[23].xyzx
dp3 r3.z, cb0[14].xyzx, r5.xyzx
dp3 r1.y, r3.xyzx, r3.xyzx
rsq r1.y, r1.y
mul o2.xyz, r1.y, r3.xyzx
dp4 r3.x, r0.xyzw, cb0[22].xyzw
dp4 r3.y, r0.xyzw, cb0[23].xyzw
dp4 r3.z, r0.xyzw, cb0[24].xyzw
dp4 r3.w, r0.xyzw, cb0[25].xyzw
add r0.xyz, -r3.xyzx, cb0[17].xyzx
dp3 r0.w, r0.xyzx, r0.xyzx
rsq r0.w, r0.w
mul r0.xyz, r0.w, r0.xyzx
dp3 r0.w, -r0.xyzx, r5.xyzx
add r0.w, r0.w, r0.w
mad r1.yzw, r5.xxyz, -r0.w, -r0.xxyz
dp3 r6.z, r0.xyzx, r5.xyzx
mov o4.z, r5.y
mov o2.w, r1.y
dp3 r6.x, r0.xyzx, r2.xyzx
mov o4.x, r2.y
dp3 r6.y, r0.xyzx, r4.xyzx
mov o4.y, r4.y
dp3 r0.x, r6.xyzx, r6.xyzx
rsq r0.x, r0.x
mul o3.xyz, r0.x, r6.xyzx
mov o3.w, r1.z
mov o4.w, r1.w
dp4 r0.x, r3.xyzw, cb0[1].xyzw
dp4 r0.y, r3.xyzw, cb0[2].xyzw
dp4 r0.z, r3.xyzw, cb0[3].xyzw
dp4 r0.w, r3.xyzw, cb0[4].xyzw
div r0.xyz, r0.xyzx, r0.w
add_sat r0.w, r0.z, -cb0[5].x
mov o5.xyz, r0.xywx
mov o6.xyz, cb0[16].xyzx
dp4 r0.x, cb0[27].xyzw, l(1.00000000e+00, 1.00000000e+00, 1.00000000e+00, 1.00000000e+00)
ne r0.x, r0.x, l(0.00000000e+00)
movc o7.x, r0.x, r1.x, l(1.00000000e+00)
ret
