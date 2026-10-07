/* Diagnostic: decode supplied config + ATRAC9 superframes through actual AJM. */
#include "../src/runtime_ajm.c"
#include <assert.h>
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static void batch(uint32_t ctx,unsigned char *start,void *end) {
    uint32_t id=0; BatchError error={0};
    assert(!ajm_batch_start(ctx,start,(uint32_t)((unsigned char *)end-start),0,&error,&id));
    assert(!ajm_batch_wait(ctx,id,0,&error));
}
int main(int argc,char **argv) {
    assert(argc==3);
    FILE *source=fopen(argv[1],"rb"); assert(source);
    unsigned char config[8]={0}; assert(fread(config,1,4,source)==4);
    uint32_t ctx=0,instance=0;
    assert(!ajm_initialize(0,&ctx) && !ajm_module_register(ctx,1,0));
    assert(!ajm_instance_create(ctx,1,1,&instance));
    unsigned char commands[128]; SidebandResult init={0};
    batch(ctx,commands,job_control(commands,instance,UINT64_C(1)<<14,config,8,&init,sizeof(init),NULL));
    assert(!init.result && !init.internal_result);
    Atrac9CodecInfo info=contexts[ctx]->instances[instance & MAX_INSTANCES].info;
    printf("ATRAC9 channels=%d rate=%d frame_samples=%d superframe_bytes=%d frames_per_superframe=%d\n",
           info.channels,info.samplingRate,info.frameSamples,info.superframeSize,info.framesInSuperframe);
    assert(info.channels>0 && info.superframeSize>0 && info.frameSamples>0);
    size_t pcm_size=(size_t)info.frameSamples*info.framesInSuperframe*info.channels*2;
    unsigned char *encoded=malloc(info.superframeSize), *pcm=malloc(pcm_size); assert(encoded && pcm);
    FILE *output=fopen(argv[2],"wb"); assert(output);
    unsigned superframes=0;
    for (;;) {
        size_t n=fread(encoded,1,info.superframeSize,source);
        if (!n) { assert(!ferror(source)); break; }
        assert(n==(size_t)info.superframeSize);
        size_t consumed=0, written=0;
        for (int frame=0;frame<info.framesInSuperframe;++frame) {
            struct { SidebandResult result; SidebandStream stream; } side={0};
            batch(ctx,commands,job_run(commands,instance,UINT64_C(1)<<47,
                    encoded+consumed,n-consumed,pcm+written,pcm_size-written,&side,sizeof(side),NULL));
            if (side.result.result || side.result.internal_result)
                fprintf(stderr,"superframe %u frame %d: result=%x internal=%x consumed=%d written=%d\n",
                        superframes,frame,side.result.result,side.result.internal_result,
                        side.stream.input_consumed,side.stream.output_written);
            assert(!side.result.result && !side.result.internal_result);
            assert(side.stream.input_consumed>0 && side.stream.output_written==info.frameSamples*info.channels*2);
            consumed+=(size_t)side.stream.input_consumed; written+=(size_t)side.stream.output_written;
        }
        assert(consumed==n && written==pcm_size);
        assert(fwrite(pcm,1,pcm_size,output)==pcm_size); ++superframes;
    }
    assert(superframes>0);
    assert(!fclose(source) && !fclose(output)); free(encoded); free(pcm);
    assert(!ajm_instance_destroy(ctx,instance) && !ajm_finalize(ctx));
    printf("PASS: %u real ATRAC9 superframes decoded through actual AJM\n",superframes);
}
