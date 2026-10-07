// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <cstdio>
#include <sys/sysctl.h>
int main() {
    setbuf(stdout, nullptr);
    @autoreleasepool {
        int translated = 0; size_t size = sizeof(translated);
        if (sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) || translated != 1) {
            std::fprintf(stderr, "Requires translated x86_64 process\n"); return 1;
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return 2;
        const bool temporal = [MTLFXTemporalScalerDescriptor supportsDevice:device];
        const bool spatial = [MTLFXSpatialScalerDescriptor supportsDevice:device];
        std::printf("Rosetta=%d device=%s temporal=%d spatial=%d\n", translated,
                    device.name.UTF8String, temporal, spatial);
        if (!temporal || !spatial) return 3;
        auto* td = [MTLFXTemporalScalerDescriptor new];
        td.inputWidth = 2560; td.inputHeight = 1440;
        td.outputWidth = 3840; td.outputHeight = 2160;
        td.colorTextureFormat = MTLPixelFormatRGBA16Float;
        td.depthTextureFormat = MTLPixelFormatR32Float;
        td.motionTextureFormat = MTLPixelFormatRG16Float;
        td.outputTextureFormat = MTLPixelFormatRGBA16Float;
        td.autoExposureEnabled = YES;
        id<MTLFXTemporalScaler> ts = [td newTemporalScalerWithDevice:device];
        auto* sd = [MTLFXSpatialScalerDescriptor new];
        sd.inputWidth = 2560; sd.inputHeight = 1440;
        sd.outputWidth = 3840; sd.outputHeight = 2160;
        sd.colorTextureFormat = MTLPixelFormatRGBA16Float;
        sd.outputTextureFormat = MTLPixelFormatRGBA16Float;
        sd.colorProcessingMode = MTLFXSpatialScalerColorProcessingModeHDR;
        id<MTLFXSpatialScaler> ss = [sd newSpatialScalerWithDevice:device];
        std::printf("Created temporal=%d spatial=%d\n", ts != nil, ss != nil);
        if (!ts || !ss) return 4;
        std::printf("Usage temporal color=%lu depth=%lu motion=%lu output=%lu; spatial color=%lu output=%lu\n",
          (unsigned long)ts.colorTextureUsage, (unsigned long)ts.depthTextureUsage,
          (unsigned long)ts.motionTextureUsage, (unsigned long)ts.outputTextureUsage,
          (unsigned long)ss.colorTextureUsage, (unsigned long)ss.outputTextureUsage);
    }
}
