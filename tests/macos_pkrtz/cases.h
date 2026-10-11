// SPDX-License-Identifier: GPL-2.0-or-later
#include <math.h>
static Case cases[65500];
static size_t case_count;

static uint32_t half_as_float_bits(uint32_t h) {
    const unsigned exponent=(h>>10)&31, fraction=h&1023;
    const float f=exponent ? ldexpf((float)(1024+fraction),(int)exponent-25)
                           : ldexpf((float)fraction,-24);
    uint32_t bits; memcpy(&bits,&f,4); return bits;
}

// Independent reference: find the greatest finite half magnitude <= the input magnitude.
static uint32_t reference_half(uint32_t bits,int mode) {
    const uint32_t sign=(bits>>16)&0x8000, magnitude=bits&0x7fffffff;
    if(magnitude>=0x7f800000) return sign | (magnitude==0x7f800000 ? 0x7c00 : 0x7e00);
    unsigned low=0, high=0x7bff;
    while(low<high) {
        const unsigned mid=(low+high+1)/2;
        if(half_as_float_bits(mid)<=magnitude) low=mid; else high=mid-1;
    }
    if((mode==0 || mode==1) && low<0x400) low=0;
    return sign|low;
}
static int half_matches(uint32_t actual,uint32_t expected) {
    return (expected&0x7fff)>0x7c00 ? (actual&0x7fff)>0x7c00 : actual==expected;
}
static void pair(uint32_t a,uint32_t b) {
    assert(case_count<sizeof(cases)/sizeof(*cases));
    cases[case_count++]=(Case){"pkrtz",0,a,b,0};
}
static void make_cases(void) {
    for(unsigned half=0;half<=0x7bff;++half) {
        const uint32_t exact=half_as_float_bits(half);
        const uint32_t upper=half<0x7bff ? half_as_float_bits(half+1)-1 : 0x7f7fffff;
        pair(exact,upper|0x80000000);
        pair(exact|0x80000000,upper);
    }
    static const uint32_t edges[]={0,0x80000000,1,0x80000001,0x33000000,0x337fffff,
        0x33800000,0x33800001,0x387fffff,0x38800000,0x3f803000,0xbf803000,
        0x477fe000,0x477ff000,0x47800000,0xc7800000,0x7f800000,0xff800000,
        0x7f800001,0x7fc12345,0xff800001,0xffc12345};
    for(size_t i=0;i<sizeof(edges)/sizeof(*edges);++i)
        pair(edges[i],edges[(i+7)%(sizeof(edges)/sizeof(*edges))]);
}
