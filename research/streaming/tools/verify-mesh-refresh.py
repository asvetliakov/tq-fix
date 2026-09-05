#!/usr/bin/env python3
"""Verify mesh refresh patch, native layout accessors, and executable test bodies."""
import hashlib,os,re,sys
from pathlib import Path
from pe import PE
root=Path(__file__).resolve().parents[3]
pe=PE(Path(os.environ['TQ_GAME_DIR'])/'Engine.dll')
assert hashlib.sha256(pe.data).hexdigest()=='0aedbb1805b4a5616f74e34d4f609f392e2c2dd4561c64c118f4772ab4f694f6'
source=(root/'src/mesh_preload.cpp').read_text()
assert 'bool g_requested = true' in source
assert 'g_requested = !path || !path[0]' in source
assert 'L"mesh_preload_refresh", 1, path' in source
for name,rva in [('kActorWindow',0x114f07),('kEntityHead',0x148050),('kEntityTail',0x1480c3),('kTouched',0x2130c0),('kFrame',0x146cd0),('kTextureIdle',0x1418cb),('kMeshIdle',0x1418e4)]:
 body=re.search(r'const BYTE '+name+r'\[\] = \{(.*?)\};',source,re.S)[1]
 data=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]+)',body))
 assert data==pe.read(rva,len(data)),name
exports=pe.exports()
for name,rva in [('?PreLoad@Entity@GAME@@UAE_NH_N@Z',0x148050),('?GetLastTouchedFrame@Resource@GAME@@QBEIXZ',0x2130c0),('?GetFrameCount@Engine@GAME@@QBEIXZ',0x146cd0),('?gEngine@GAME@@3PAVEngine@1@A',0x3743f0)]:assert exports[name]==rva
out='// Audited native bodies for executable off-game tests; see verify-mesh-refresh.py.\n'
for name,rva in [('?EvictOldResources@Engine@GAME@@QAEXXZ',0x1418a0),('?EvictOldResources@BaseResourceManager@GAME@@QAEXIIII@Z',0x11f830)]:assert exports[name]==rva
for name,rva,size in [('kTestActor',0x114f00,49),('kTestEntity',0x148050,131),('kTestIdleEviction',0x1418a0,160)]:
 out+='const BYTE '+name+'[] = {\n'
 data=pe.read(rva,size)
 for i in range(0,len(data),12):out+='    '+','.join(f'0x{b:02x}' for b in data[i:i+12])+',\n'
 out+='};\n'
fixture=root/'test/fixtures/mesh-preload-native.inc'
if '--write' in sys.argv:fixture.write_text(out)
else:assert fixture.read_text()==out
print('PASS: mesh preload and idle requeue sites, native accessors, exports and executable fixtures')
