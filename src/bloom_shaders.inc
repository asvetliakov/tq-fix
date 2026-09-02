// Shared by the production renderer and the off-game shader-compiler test.
const char* kBloomExtractSource =
"Texture2D Source:register(t0);SamplerState LinearClamp:register(s0);"
"cbuffer BloomConstants:register(b0){float2 sourceTexel;float threshold;float knee;"
"float invRange;float saturation;float strength;float scatter;}"
"float3 hot(float2 uv){float3 c=max(Source.SampleLevel(LinearClamp,uv,0).rgb,0);"
"float b=max(c.r,max(c.g,c.b));float s=clamp(b-threshold+knee,0,2*knee);"
"s=s*s/(4*knee+1e-6);float contribution=max(s,b-threshold)/max(b,1e-6);"
"float3 h=c*contribution*invRange;float l=dot(h,float3(1.0/3,1.0/3,1.0/3));"
"return max(lerp(l.xxx,h,saturation),0);}"
"float3 filter13(float2 u){float2 t=sourceTexel;"
"float3 A=hot(u+t*float2(-2,-2)),B=hot(u+t*float2(0,-2));"
"float3 C=hot(u+t*float2(2,-2)),D=hot(u+t*float2(-1,-1));"
"float3 E=hot(u+t*float2(1,-1)),F=hot(u+t*float2(-2,0));"
"float3 G=hot(u),H=hot(u+t*float2(2,0));"
"float3 I=hot(u+t*float2(-1,1)),J=hot(u+t*float2(1,1));"
"float3 K=hot(u+t*float2(-2,2)),L=hot(u+t*float2(0,2));"
"float3 M=hot(u+t*float2(2,2));"
"return (D+E+I+J)*.125+(A+B+F+G+B+C+G+H+F+G+K+L+G+H+L+M)*.03125;}"
"float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD0):SV_Target{"
"return float4(filter13(u),1);}";

const char* kBloomDownsampleSource =
"Texture2D Source:register(t0);SamplerState LinearClamp:register(s0);"
"cbuffer BloomConstants:register(b0){float2 sourceTexel;float threshold;float knee;"
"float invRange;float saturation;float strength;float scatter;}"
"float3 s(float2 uv){return max(Source.SampleLevel(LinearClamp,uv,0).rgb,0);}"
"float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD0):SV_Target{float2 t=sourceTexel;"
"float3 A=s(u+t*float2(-2,-2)),B=s(u+t*float2(0,-2));"
"float3 C=s(u+t*float2(2,-2)),D=s(u+t*float2(-1,-1));"
"float3 E=s(u+t*float2(1,-1)),F=s(u+t*float2(-2,0));"
"float3 G=s(u),H=s(u+t*float2(2,0));"
"float3 I=s(u+t*float2(-1,1)),J=s(u+t*float2(1,1));"
"float3 K=s(u+t*float2(-2,2)),L=s(u+t*float2(0,2));"
"float3 M=s(u+t*float2(2,2));"
"float3 c=(D+E+I+J)*.125+(A+B+F+G+B+C+G+H+F+G+K+L+G+H+L+M)*.03125;"
"return float4(c,1);}";

const char* kBloomUpsampleSource =
"Texture2D Lower:register(t0);Texture2D Higher:register(t1);"
"SamplerState LinearClamp:register(s0);"
"cbuffer BloomConstants:register(b0){float2 sourceTexel;float threshold;float knee;"
"float invRange;float saturation;float strength;float scatter;}"
"float3 tent(float2 u){float2 t=sourceTexel;"
"return (Lower.SampleLevel(LinearClamp,u+t*float2(-1,-1),0).rgb+"
"2*Lower.SampleLevel(LinearClamp,u+t*float2(0,-1),0).rgb+"
"Lower.SampleLevel(LinearClamp,u+t*float2(1,-1),0).rgb+"
"2*Lower.SampleLevel(LinearClamp,u+t*float2(-1,0),0).rgb+"
"4*Lower.SampleLevel(LinearClamp,u,0).rgb+"
"2*Lower.SampleLevel(LinearClamp,u+t*float2(1,0),0).rgb+"
"Lower.SampleLevel(LinearClamp,u+t*float2(-1,1),0).rgb+"
"2*Lower.SampleLevel(LinearClamp,u+t*float2(0,1),0).rgb+"
"Lower.SampleLevel(LinearClamp,u+t*float2(1,1),0).rgb)/16;}"
"float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD0):SV_Target{"
"float3 low=tent(u),high=max(Higher.SampleLevel(LinearClamp,u,0).rgb,0);"
"return float4(lerp(high,low,scatter),1);}";

const char* kBloomCompositeSource =
"Texture2D Bloom:register(t0);SamplerState LinearClamp:register(s0);"
"cbuffer BloomConstants:register(b0){float2 sourceTexel;float threshold;float knee;"
"float invRange;float saturation;float strength;float scatter;}"
"float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD0):SV_Target{"
"return float4(max(Bloom.SampleLevel(LinearClamp,u,0).rgb,0)*strength,0);}";
