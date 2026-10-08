// Compile the production bridge and exercise its helpers without a Wacom driver.
#import "../../macos/stylet/Sources/StyletWacomBridge/StyletWacomControls.m"

#include <assert.h>

/// Checks the Apple Event address and the monotonic packet timestamp.
int main(void) {
  @autoreleasepool {
    NSAppleEventDescriptor *target = DriverTarget();
    assert(target != nil);
    assert(target.descriptorType == typeApplSignature);
    assert(target.data.length == sizeof(OSType));
    OSType signature = 0;
    [target.data getBytes:&signature length:sizeof(signature)];
    assert(signature == kWacomDriverSignature);

    const int64_t before =
        (int64_t)llround(NSProcessInfo.processInfo.systemUptime * 1000000.0);
    const int64_t timestamp = TimestampMicros();
    const int64_t after =
        (int64_t)llround(NSProcessInfo.processInfo.systemUptime * 1000000.0);
    assert(timestamp > 0);
    assert(timestamp >= before);
    assert(timestamp <= after);
  }
  return 0;
}
