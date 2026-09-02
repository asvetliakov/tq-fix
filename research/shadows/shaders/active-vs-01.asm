vs_5_0
dcl_globalFlags refactoringAllowed
dcl_input v0.xyz
dcl_input v1.xy
dcl_output_siv o0.xyzw, position
dcl_output o1.xy
mov o0.xyz, v0.xyzx
mov o0.w, l(1.00000000e+00)
mov o1.xy, v1.xyxx
ret
