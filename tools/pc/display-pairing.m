// Display-pairing experiment (notes/DISPLAY-PAIRING.md Q5). Userspace only, NO GPU submission.
// Compares CGDirectDisplayCopyCurrentMetalDevice (what SkyLight composites the display on) with
// MTLCopyAllDevices, by registryID/name. Creating an MTLDevice opens Apple's accelerator user
// client, so run only where it cannot disturb a blit measurement.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
int main(void){ @autoreleasepool {
  uint32_t n=0; CGGetOnlineDisplayList(16,NULL,&n);
  CGDirectDisplayID ids[16]={0}; CGGetOnlineDisplayList(16,ids,&n);
  printf("online displays: %u\n", n);
  for(uint32_t i=0;i<n;i++){
    CGDirectDisplayID d=ids[i];
    printf("display[%u] id=%u main=%d builtin=%d vendor=0x%x model=0x%x\n", i, d,
           CGDisplayIsMain(d), CGDisplayIsBuiltin(d), CGDisplayVendorNumber(d), CGDisplayModelNumber(d));
    id<MTLDevice> md = CGDirectDisplayCopyCurrentMetalDevice(d);
    if(md) printf("  CGDirectDisplayCopyCurrentMetalDevice -> name=%s regID=0x%llx headless=%d removable=%d low=%d\n",
                  md.name.UTF8String,(unsigned long long)md.registryID,(int)md.isHeadless,(int)md.isRemovable,(int)md.isLowPower);
    else printf("  CGDirectDisplayCopyCurrentMetalDevice -> nil\n");
  }
  NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
  printf("MTLCopyAllDevices count=%lu\n",(unsigned long)all.count);
  for(id<MTLDevice> m in all)
    printf("  all: name=%s regID=0x%llx headless=%d removable=%d low=%d\n",
           m.name.UTF8String,(unsigned long long)m.registryID,(int)m.isHeadless,(int)m.isRemovable,(int)m.isLowPower);
  id<MTLDevice> sys = MTLCreateSystemDefaultDevice();
  if(sys) printf("MTLCreateSystemDefaultDevice -> name=%s regID=0x%llx\n", sys.name.UTF8String,(unsigned long long)sys.registryID);
  else printf("MTLCreateSystemDefaultDevice -> nil\n");
  return 0;
} }
