/* Optional Zydis-assisted scan of a plaintext executable segment, never executes it. */
#include <Zydis/Zydis.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t counts[ZYDIS_ISA_SET_MAX_VALUE+1];
int main(int argc,char **argv) {
    if (argc<2) { fprintf(stderr,"usage: scan_x86 segment.bin [start [end]]\n"); return 2; }
    FILE *f=fopen(argv[1],"rb"); if (!f) return 2;
    fseek(f,0,SEEK_END); long length=ftell(f); rewind(f);
    if (length<=0) return 2;
    unsigned char *data=malloc((size_t)length); if (!data) return 2;
    if (fread(data,1,(size_t)length,f)!=(size_t)length) return 2;
    fclose(f);
    size_t start=argc>2 ? strtoull(argv[2],NULL,0) : 0;
    size_t end=argc>3 ? strtoull(argv[3],NULL,0) : (size_t)length;
    if (end>(size_t)length) end=(size_t)length;
    ZydisDecoder dec; ZydisDecoderInit(&dec,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64);
    ZydisFormatter fmt; ZydisFormatterInit(&fmt,ZYDIS_FORMATTER_STYLE_INTEL);
    size_t failed=0,total=0;
    for (size_t at=start;at<end;) {
        ZydisDecodedInstruction ins; ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec,data+at,end-at,&ins,ops))) { failed++; at++; continue; }
        counts[ins.meta.isa_set]++; total++;
        int show=argc>2 || ins.meta.isa_set==ZYDIS_ISA_SET_SSE4A || ins.meta.isa_set==ZYDIS_ISA_SET_XOP || ins.meta.isa_set==ZYDIS_ISA_SET_FMA4;
        const char *reason="ISA";
        const char *isa=ZydisISASetGetString(ins.meta.isa_set);
        if (at<0x2bbe000 && (strstr(isa,"AVX2") || strstr(isa,"AVX512") || strstr(isa,"FMA") ||
            strstr(isa,"3DNOW") || strstr(isa,"XOP") || strstr(isa,"CLZERO") || strstr(isa,"MONITOR") ||
            strstr(isa,"RDSEED") || strstr(isa,"RTM"))) { show=1;reason="rare-ISA"; }
        for (unsigned i=0;i<ins.operand_count_visible;i++) {
            if (ops[i].type==ZYDIS_OPERAND_TYPE_IMMEDIATE && !ops[i].imm.is_relative && ops[i].imm.value.u>=UINT64_C(0xfc0000000) && ops[i].imm.value.u<UINT64_C(0x7000000000)) { show=1;reason="guard-constant"; }
            if ((ins.mnemonic==ZYDIS_MNEMONIC_CALL || ins.mnemonic==ZYDIS_MNEMONIC_JMP) && ops[i].type==ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[i].imm.is_relative) {
                uint64_t target=at+ins.length+ops[i].imm.value.s;
                if (target==45869752 || target==45869768 || target==45875704 || target==45878600) { show=1;reason="map-call"; }
            }
        }
        if (show) { char text[256]; ZydisFormatterFormatInstruction(&fmt,&ins,ops,ins.operand_count_visible,text,sizeof(text),at,NULL); printf("%08zx %-16s %-12s %s\n",at,ZydisISASetGetString(ins.meta.isa_set),reason,text); }
        at+=ins.length;
    }
    fprintf(stderr,"linear decode: %zu instructions, %zu undecodable bytes\n",total,failed);
    for (unsigned i=0;i<=ZYDIS_ISA_SET_MAX_VALUE;i++) if (counts[i]) fprintf(stderr,"%-20s %zu\n",ZydisISASetGetString(i),counts[i]);
    /* Every possible start catches candidates hidden by linear data disassembly. */
    if (argc==2) for (size_t at=0;at+4<(size_t)length;at++) {
        if (data[at]!=0x66 && data[at]!=0xf2) continue;
        size_t op=at+1; if ((data[op]&0xf0)==0x40) op++;
        if (data[op]!=0x0f || (data[op+1]!=0x78 && data[op+1]!=0x79)) continue;
        ZydisDecodedInstruction ins; ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec,data+at,(size_t)length-at,&ins,ops)) && ins.meta.isa_set==ZYDIS_ISA_SET_SSE4A) {
            char text[256]; ZydisFormatterFormatInstruction(&fmt,&ins,ops,ins.operand_count_visible,text,sizeof(text),at,NULL);
            printf("%08zx SSE4a-candidate %s\n",at,text);
        }
    }
    free(data);return 0;
}
