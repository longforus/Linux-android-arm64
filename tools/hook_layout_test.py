"""Decode production trampoline words to validate shifted control/data references."""
from pathlib import Path
import re, struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
s=(Path(__file__).resolve().parents[1]/'lsdriver/inline_hook_frame.h').read_text()
part=s.split('static const uint32_t tramp_template[TRAMP_WORDS] = {',1)[1].split('};',1)[0]
words=[int(x,16) for x in re.findall(r'^\s*(0x[0-9A-Fa-f]+),',part,re.M)]
assert len(words)==120
md=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN)
def decode(i):return next(md.disasm(struct.pack('<I',words[i]),i*4))
for i,target in [(19,116),(26,118),(30,116),(62,116),(64,116)]:
 inst=decode(i); assert inst.mnemonic=='ldr' and int(inst.op_str.split('#')[1],0)==target*4,(i,inst.op_str)
for i,target in [(29,64),(34,92),(68,92)]:
 inst=decode(i);assert inst.mnemonic in ('b.eq','b.ne') and int(inst.op_str.lstrip('#'),0)==target*4,(i,inst.op_str)
for i in (20,31,65): assert decode(i).op_str.endswith('#0x14')
assert words[57:62]==[0]*5
assert decode(63).mnemonic=='ret' and decode(91).mnemonic=='ret' and decode(115).mnemonic=='ret'
def constant(name):return int(re.search(r'#define '+name+r'\s+(\d+)',s)[1])
assert constant('HOOK_STUB_WORDS')==5
assert constant('TRAMP_BRANCH_SLOT_INDEX')>=120
assert constant('TRAMP_WORDS')>=constant('TRAMP_BRANCH_SLOT_INDEX')+5
assert constant('TRAMP_WORDS')*4%8==0
print('Production trampoline control flow/data layout PASS')
