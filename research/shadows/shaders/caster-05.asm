vs_5_0
dcl_globalFlags refactoringAllowed
dcl_constantBuffer cb0[86], dynamicIndexed
dcl_input v0.xyz
dcl_input v1.xy
dcl_input v2.xyzw
dcl_input v3.xyzw
dcl_output_siv o0.xyzw, position
dcl_output o1.xy
dcl_temps 4
imul null, r0.xyzw, v2.xyzw, l(3, 3, 3, 3)
mov r1.xyz, v0.xyzx
mov r1.w, l(1.00000000e+00)
dp4 r2.x, r1.xyzw, cb0[r0.y + 5].xyzw
dp4 r2.y, r1.xyzw, cb0[r0.y + 6].xyzw
dp4 r2.z, r1.xyzw, cb0[r0.y + 7].xyzw
mul r2.xyz, r2.xyzx, v3.y
dp4 r3.x, r1.xyzw, cb0[r0.x + 5].xyzw
dp4 r3.y, r1.xyzw, cb0[r0.x + 6].xyzw
dp4 r3.z, r1.xyzw, cb0[r0.x + 7].xyzw
mad r2.xyz, r3.xyzx, v3.x, r2.xyzx
dp4 r3.x, r1.xyzw, cb0[r0.z + 5].xyzw
dp4 r3.y, r1.xyzw, cb0[r0.z + 6].xyzw
dp4 r3.z, r1.xyzw, cb0[r0.z + 7].xyzw
mad r0.xyz, r3.xyzx, v3.z, r2.xyzx
dp4 r2.x, r1.xyzw, cb0[r0.w + 5].xyzw
dp4 r2.y, r1.xyzw, cb0[r0.w + 6].xyzw
dp4 r2.z, r1.xyzw, cb0[r0.w + 7].xyzw
mad r0.xyz, r2.xyzx, v3.w, r0.xyzx
mov r0.w, l(1.00000000e+00)
dp4 r1.x, r0.xyzw, cb0[0].xyzw
dp4 r1.y, r0.xyzw, cb0[1].xyzw
dp4 r1.z, r0.xyzw, cb0[2].xyzw
dp3 r0.x, r1.xyzx, r1.xyzx
sqrt r0.x, r0.x
div r0.yzw, r1.xxyz, r0.x
add r0.x, r0.x, l(-1.00000001e-01)
mul o0.z, r0.x, l(3.34448144e-02)
add r0.x, r0.z, l(-1.00000000e+00)
div o0.xy, r0.ywyy, r0.x
mov o0.w, l(1.00000000e+00)
mov o1.xy, v1.xyxx
ret
