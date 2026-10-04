#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Receives standard-codec-compatible device and tablet-pad dictionaries.
typedef void (^StyletWacomPacketHandler)(NSDictionary<NSString *, id> *packet);

/// Optional bridge to Wacom's macOS Driver Request Interface.
///
/// The bridge sends Apple Events only after explicit activation. This is
/// important because taking control replaces the user's driver mappings while
/// the application is active and can trigger the macOS Automation permission.
@interface StyletWacomControls : NSObject

- (instancetype)initWithPacketHandler:(StyletWacomPacketHandler)packetHandler
    NS_DESIGNATED_INITIALIZER;

- (instancetype)init NS_UNAVAILABLE;

/// Enables or releases every available Wacom tablet-control override.
- (BOOL)setTabletPadOverrideEnabled:(BOOL)enabled;

/// Controls whether packets can currently be delivered to Dart.
- (void)setListening:(BOOL)listening;

@end

NS_ASSUME_NONNULL_END
