#include "../src/runtime_ajm.c"
#include <assert.h>
#include <math.h>

uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static void bits(unsigned char *buffer,unsigned *at,unsigned value,unsigned count) {
    for (unsigned i=count;i;--i,++*at) buffer[*at/8]|=((value>>(i-1))&1)<<(7-*at%8);
}
static void spectral_frame(unsigned char frame[256]) {
    memset(frame,0,256); unsigned at=0;
    bits(frame,&at,0,2); /* first superframe block, fresh bands */
    bits(frame,&at,0,4); /* three bands / ten quantization units */
    bits(frame,&at,0,1); /* no band extension */
    bits(frame,&at,0,2); /* gradient mode zero */
    bits(frame,&at,0,6); bits(frame,&at,0,6); /* gradient [0,1) */
    bits(frame,&at,16,5); bits(frame,&at,16,5); bits(frame,&at,0,4);
    bits(frame,&at,0,1); /* no extension data */
    bits(frame,&at,1,2); bits(frame,&at,3,2); /* direct five-bit scale factors */
    for (unsigned i=0;i<10;++i) bits(frame,&at,31,5);
    /* sf 31 - gradient 16 = precision 15: signed 16-bit coefficients. */
    for (unsigned i=0;i<24;++i) bits(frame,&at,i==4 ? 10000 : 0,16);
    assert(at<=256*8);
}
static void batch(uint32_t ctx,unsigned char *start,void *end) {
    uint32_t id=0; BatchError error={0};
    assert(!ajm_batch_start(ctx,start,(uint32_t)((unsigned char *)end-start),0,&error,&id));
    assert(!ajm_batch_wait(ctx,id,0,&error));
}
static void initialize(uint32_t ctx,uint32_t instance,uint32_t total,uint16_t skip) {
    unsigned char commands[128],control[16]={0};
    const unsigned char config[]={0xfe,0x70,0x1f,0xe0}; /* 48kHz mono, 256-byte frames */
    uint64_t flags=UINT64_C(1)<<14;
    size_t offset=0;
    if (total || skip) {
        SidebandGapless gapless={total,skip,0}; memcpy(control,&gapless,sizeof(gapless));
        offset=8; flags|=UINT64_C(1)<<45;
    }
    memcpy(control+offset,config,4);
    SidebandResult result={-1,-1};
    batch(ctx,commands,job_control(commands,instance,flags,control,offset+8,&result,sizeof(result),NULL));
    assert(!result.result && !result.internal_result);
}
int main(int argc,char **argv) {
    assert(argc==2);
    unsigned char encoded[256]; spectral_frame(encoded);
    uint32_t ctx=0; assert(!ajm_initialize(0,&ctx)); assert(!ajm_module_register(ctx,1,0));
    int16_t reference[8*256];
    for (int format=0;format<3;++format) {
        uint32_t instance=0; assert(!ajm_instance_create(ctx,1,1|((uint64_t)format<<7),&instance));
        initialize(ctx,instance,0,0);
        for (unsigned frame=0;frame<8;++frame) {
            unsigned char commands[128],pcm[1024]; memset(pcm,0xa5,sizeof(pcm));
            struct { SidebandResult result; SidebandStream stream; } side={0};
            batch(ctx,commands,job_run(commands,instance,UINT64_C(1)<<47,encoded,sizeof(encoded),
                pcm,256*pcm_size(format),&side,sizeof(side),NULL));
            assert(!side.result.result && !side.result.internal_result);
            assert(side.stream.input_consumed==256 && side.stream.output_written==256*pcm_size(format));
            assert(side.stream.total_decoded_samples==(frame+1)*256);
            for (unsigned i=0;i<256;++i) {
                if (!format) memcpy(&reference[frame*256+i],pcm+i*2,2);
                else if (format==FORMAT_S32) { int32_t value; memcpy(&value,pcm+i*4,4); assert(value==(int32_t)reference[frame*256+i]*65536); }
                else { float value; memcpy(&value,pcm+i*4,4); assert(value==reference[frame*256+i]/32768.0f); }
            }
        }
        assert(!ajm_instance_destroy(ctx,instance));
    }
    FILE *f=fopen(argv[1],"wb"); assert(f); assert(fwrite(reference,1,sizeof(reference),f)==sizeof(reference)); assert(!fclose(f));
    unsigned nonzero=0; for (unsigned i=0;i<8*256;++i) nonzero+=reference[i]!=0; assert(nonzero>1900);
    uint32_t instance=0; assert(!ajm_instance_create(ctx,1,1,&instance)); initialize(ctx,instance,600,64);
    int16_t trimmed[600]; unsigned written=0;
    for (unsigned frame=0;frame<3;++frame) {
        unsigned char commands[128]; struct { SidebandResult result; SidebandStream stream; } side={0};
        batch(ctx,commands,job_run(commands,instance,UINT64_C(1)<<47,encoded,sizeof(encoded),
            trimmed+written,sizeof(trimmed)-written*2,&side,sizeof(side),NULL));
        assert(!side.result.result && side.stream.input_consumed==256);
        written+=(unsigned)side.stream.output_written/2;
    }
    assert(written==600 && !memcmp(trimmed,reference+64,sizeof(trimmed)));
    assert(!ajm_instance_destroy(ctx,instance));
    assert(!ajm_finalize(ctx));
    puts("PASS: synthesized ATRAC9 sequence through actual AJM batches, S16/S32/float and gapless trimming");
    runtime_ajm_report();
}
