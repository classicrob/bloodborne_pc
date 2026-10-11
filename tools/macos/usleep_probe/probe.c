#define _GNU_SOURCE
#ifndef BB_USLEEP_HOST_ONLY
#include "../../../src/runtime_kernel.c"
#else
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#endif
#include <assert.h>
#include <inttypes.h>
#include <mach/mach_time.h>

#ifndef BB_USLEEP_HOST_ONLY
static _Thread_local int32_t probe_errno;
int32_t *runtime_errno(void) { return &probe_errno; }
#endif
static mach_timebase_info_data_t probe_timebase;
static uint64_t probe_ns(void) {
    return (uint64_t)(((__uint128_t)mach_absolute_time()*probe_timebase.numer)/probe_timebase.denom);
}
static int compare_u64(const void *a,const void *b) {
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;
    return (x>y)-(x<y);
}
static int host_sleep(uint32_t usec) {
    struct timespec request={usec/1000000,(long)(usec%1000000)*1000};
    while(nanosleep(&request,&request))if(errno!=EINTR)return errno;
    return 0;
}
int main(int argc,char **argv) {
    char *end=NULL;errno=0;
    unsigned long requested_samples=argc>1 ? strtoul(argv[1],&end,10) : 64;
    if(argc>2||errno||(end&&*end)||!requested_samples||requested_samples>10000){fputs("samples must be 1..10000\n",stderr);return 2;}
    unsigned samples=(unsigned)requested_samples;
    assert(!mach_timebase_info(&probe_timebase));
    uint64_t *values=calloc(samples,sizeof(*values));assert(values);
    const uint32_t requested[]={0,1,10,100,500,1000,5000,10000,20000,50000};
    puts("implementation,requested_us,samples,min_us,p50_us,p95_us,p99_us,max_us,mean_us,mean_excess_us,early_count");
    for(unsigned row=0;row<sizeof(requested)/sizeof(requested[0]);row++) {
        uint32_t usec=requested[row];
#ifdef BB_USLEEP_HOST_ONLY
        const unsigned first_mode=1;
#else
        const unsigned first_mode=0;
#endif
        for(unsigned mode=first_mode;mode<2;mode++) {
            uint64_t total=0;unsigned early=0;
            for(unsigned i=0;i<samples+2;i++) {
                uint64_t start=probe_ns();
#ifdef BB_USLEEP_HOST_ONLY
                int result=host_sleep(usec);
#else
                int result=mode ? host_sleep(usec) : kernel_usleep(usec);
#endif
                uint64_t elapsed=probe_ns()-start;
                if(result){fprintf(stderr,"sleep failed: mode=%u result=%d\n",mode,result);return 1;}
                if(i<2)continue;
                values[i-2]=elapsed;total+=elapsed;early+=elapsed<(uint64_t)usec*1000;
            }
            qsort(values,samples,sizeof(*values),compare_u64);
            double mean=(double)total/samples/1000;
            printf("%s,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u\n",
                mode ? "host_nanosleep" : "sceKernelUsleep",usec,samples,values[0]/1000.0,
                values[(samples-1)*50/100]/1000.0,values[(samples-1)*95/100]/1000.0,
                values[(samples-1)*99/100]/1000.0,values[samples-1]/1000.0,mean,mean-usec,early);
            fflush(stdout);
        }
    }
    free(values);return 0;
}
