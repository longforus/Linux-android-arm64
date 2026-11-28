"""Exercise production relocation/encoder/decoder together on the host.
Kprobe lookup is excluded: it requires a live kernel. All relocation code is unchanged.
"""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'lsdriver/inline_hook_frame.h').read_text()
start=s.index('static int hook_relocate_cond_branch')
end=s.index('// 批量 patch',start)
code=s[start:end]
a=code.index('        if (insts[i] == kprobe_brk_inst)');b=code.index('        // 对解包后的真实原指令',a)
code=code[:a]+'        if (insts[i] == kprobe_brk_inst) return -EBUSY;\n'+code[b:]
code=re.sub(r'__attribute__\(\(no_sanitize\("cfi", "kcfi"\)\)\)', '',code)
macros='\n'.join(re.findall(r'^#define (?:HOOK_STUB_WORDS|TRAMP_REPLAY_INST_INDEX|TRAMP_BRANCH_SLOT_INDEX)\s+\d+',s,re.M))
main=r'''
int main(void) {
 uint32_t w[69]; struct arm64_decoded_instruction d;
 const uint64_t src=0x10000000, dst=src+0x200000;
 #define RESET() do { memset(w,0,sizeof(w)); for(int i=0;i<5;i++)w[i]=0xD503201F; } while(0)
 RESET(); assert(!arm64_encode_b(src,src+16,&w[0])); assert(!hook_relocate_replay_insts(src,dst,w,5));
 assert(!arm64_decode_instruction(w[0],&d)); assert(dst+d.offset==dst+16);
 RESET(); assert(!arm64_encode_tbz(src,src+0x100,1,0,&w[0])); assert(!hook_relocate_replay_insts(src,dst,w,5));
 assert(!arm64_decode_instruction(w[0],&d)); uint64_t island=dst+d.offset;
 assert(island==dst+(TRAMP_BRANCH_SLOT_INDEX-TRAMP_REPLAY_INST_INDEX)*4);
 assert(!arm64_decode_instruction(w[(island-dst)/4],&d));assert(island+d.offset==src+0x100);
 RESET(); assert(!arm64_encode_ldr_literal(ARM64_ENCODE_LS_GPR64,0,src,src+0x80000,&w[0]));
 assert(hook_relocate_replay_insts(src,dst,w,5)==-ERANGE);
 RESET(); assert(!arm64_encode_ldr_literal(ARM64_ENCODE_LS_GPR64,0,src,src+4,&w[0]));
 assert(hook_relocate_replay_insts(src,src+0x1000,w,5)==-EOPNOTSUPP);
 RESET(); assert(!arm64_encode_ldr_literal(ARM64_ENCODE_LS_GPR64,0,src,src+0x80000,&w[0]));
 assert(!hook_relocate_replay_insts(src,src+0x1000,w,5));assert(!arm64_decode_instruction(w[0],&d));assert(src+0x1000+d.offset==src+0x80000);
 puts("Production branch-island and literal relocation contracts PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'test.c'
 p.write_text('#include <assert.h>\n#include <stdio.h>\n#include <string.h>\n#include <errno.h>\n#include "arm64_decode/arm64_decode.h"\n#include "arm64_encode/arm64_encode.h"\n#define ls_log_always_tag(...) ((void)0)\n'+macros+'\n'+code+main)
 subprocess.run(['cc','-std=gnu11','-O2','-I'+str(root/'lsdriver'),str(p),str(root/'lsdriver/arm64_encode/arm64_encode.c'),*[str(p) for p in (root/'lsdriver/arm64_decode').glob('*.c')],'-o',tmp+'/test'],check=True)
 subprocess.run([tmp+'/test'],check=True)
