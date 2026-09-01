vs_5_0
dcl_globalFlags refactoringAllowed
dcl_constantBuffer cb0[3], immediateIndexed
dcl_input v0.xyz
dcl_input v1.xy
dcl_output_siv o0.xyzw, position
dcl_output o1.xy
dcl_temps 2
mov r0.xyz, v0.xyzx
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
