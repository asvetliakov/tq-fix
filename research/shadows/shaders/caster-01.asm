vs_5_0
dcl_globalFlags refactoringAllowed
dcl_constantBuffer cb0[4], immediateIndexed
dcl_input v0.xyz
dcl_input v1.xy
dcl_output_siv o0.xyzw, position
dcl_output o1.xy
dcl_temps 1
mov r0.xyz, v0.xyzx
mov r0.w, l(1.00000000e+00)
dp4 o0.x, r0.xyzw, cb0[0].xyzw
dp4 o0.y, r0.xyzw, cb0[1].xyzw
dp4 o0.z, r0.xyzw, cb0[2].xyzw
dp4 o0.w, r0.xyzw, cb0[3].xyzw
mov o1.xy, v1.xyxx
ret
