#import "include/StyletWacomControls.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <math.h>

static const SInt32 kDriverTimeoutTicks = 3600;

#define STYLET_FOUR_CC(a, b, c, d)                                        \
  (((DescType)(uint8_t)(a) << 24) | ((DescType)(uint8_t)(b) << 16) |      \
   ((DescType)(uint8_t)(c) << 8) | (DescType)(uint8_t)(d))

static DescType const kWacomDriverSignature =
    STYLET_FOUR_CC('W', 'a', 'C', 'M');
static DescType const kDriverClass = STYLET_FOUR_CC('D', 'r', 'v', 'r');
static DescType const kTabletClass = STYLET_FOUR_CC('T', 'b', 'l', 't');
static DescType const kContextClass = STYLET_FOUR_CC('C', 'T', 'x', 't');
static DescType const kTouchRingClass = STYLET_FOUR_CC('W', 'R', 'n', 'G');
static DescType const kExpressKeyClass = STYLET_FOUR_CC('W', 'E', 'x', 'K');
static DescType const kTouchStripClass = STYLET_FOUR_CC('W', 'T', 'c', 'S');
static DescType const kControlFunctionClass =
    STYLET_FOUR_CC('W', 'C', 't', 'F');
static DescType const kBlankContext = STYLET_FOUR_CC('B', 'l', 'n', 'k');
static DescType const kConnectedProperty = STYLET_FOUR_CC('C', 'n', 'c', 't');
static DescType const kFunctionAvailableProperty =
    STYLET_FOUR_CC('F', 'u', 'n', 'A');
static DescType const kMinimumProperty = STYLET_FOUR_CC('C', 'M', 'i', 'n');
static DescType const kMaximumProperty = STYLET_FOUR_CC('C', 'M', 'a', 'x');
static DescType const kOverrideProperty = STYLET_FOUR_CC('O', 'v', 'r', 'F');
static DescType const kNameProperty = STYLET_FOUR_CC('p', 'n', 'a', 'm');
static AEKeyword const kContextKindKeyword =
    STYLET_FOUR_CC('f', 'o', 'r', ' ');

static NSString *const kNotificationObject =
    @"com.wacom.tabletdriver.hardware";
static NSNotificationName const kControlNotification =
    @"com.wacom.tabletdriver.hardware.controldata";
static NSString *const kTabletNumberKey = @"Tablet Number";
static NSString *const kControlTypeKey = @"Control Type";
static NSString *const kControlNumberKey = @"Control Number";
static NSString *const kFunctionNumberKey = @"Function Number";
static NSString *const kControlValueKey = @"Control Value";

typedef NS_ENUM(NSInteger, StyletWacomControlType) {
  StyletWacomControlTypeRing = 0,
  StyletWacomControlTypeStrip = 1,
  StyletWacomControlTypeExpressKey = 2,
};

static NSAppleEventDescriptor *UInt32Descriptor(UInt32 value) {
  return [NSAppleEventDescriptor descriptorWithDescriptorType:typeUInt32
                                                        bytes:&value
                                                       length:sizeof(value)];
}

static NSAppleEventDescriptor *_Nullable ObjectSpecifier(
    DescType objectClass, NSAppleEventDescriptor *key, DescType form,
    NSAppleEventDescriptor *_Nullable container) {
  AEDesc result = {typeNull, NULL};
  NSAppleEventDescriptor *nullDescriptor =
      [NSAppleEventDescriptor nullDescriptor];
  const AEDesc *containerDescription =
      container == nil ? nullDescriptor.aeDesc : container.aeDesc;
  const OSErr error = CreateObjSpecifier(
      objectClass, containerDescription, form, key.aeDesc, false, &result);
  if (error != noErr) {
    return nil;
  }
  return [[NSAppleEventDescriptor alloc] initWithAEDescNoCopy:&result];
}

static NSAppleEventDescriptor *DriverTarget(void) {
  OSType signature = kWacomDriverSignature;
  return [NSAppleEventDescriptor descriptorWithDescriptorType:
                                                        bytes:&signature
                                                       length:sizeof(signature)];
}

static NSAppleEventDescriptor *MakeAppleEvent(AEEventClass eventClass,
                                              AEEventID eventID) {
  return [NSAppleEventDescriptor
      appleEventWithEventClass:eventClass
                       eventID:eventID
              targetDescriptor:DriverTarget()
                      returnID:kAutoGenerateReturnID
                 transactionID:kAnyTransactionID];
}

static NSAppleEventDescriptor *_Nullable SendForReply(
    NSAppleEventDescriptor *event) {
  AppleEvent reply = {typeNull, NULL};
  const OSStatus error = AESendMessage(
      event.aeDesc, &reply, kAEWaitReply | kAECanInteract,
      kDriverTimeoutTicks);
  if (error != noErr) {
    return nil;
  }
  return [[NSAppleEventDescriptor alloc] initWithAEDescNoCopy:&reply];
}

static BOOL SendWithoutReply(NSAppleEventDescriptor *event) {
  return AESendMessage(event.aeDesc, NULL, kAENoReply | kAECanInteract,
                       kDriverTimeoutTicks) == noErr;
}

static NSAppleEventDescriptor *_Nullable DriverRoute(void) {
  return ObjectSpecifier(kDriverClass, UInt32Descriptor(1),
                         formAbsolutePosition, nil);
}

static NSAppleEventDescriptor *_Nullable TabletRoute(UInt32 tablet) {
  return ObjectSpecifier(kTabletClass, UInt32Descriptor(tablet),
                         formAbsolutePosition, nil);
}

static NSAppleEventDescriptor *_Nullable ContextRoute(UInt32 context) {
  return ObjectSpecifier(kContextClass, UInt32Descriptor(context),
                         formUniqueID, nil);
}

static DescType ControlClass(StyletWacomControlType type) {
  switch (type) {
    case StyletWacomControlTypeRing:
      return kTouchRingClass;
    case StyletWacomControlTypeStrip:
      return kTouchStripClass;
    case StyletWacomControlTypeExpressKey:
      return kExpressKeyClass;
  }
  return kTouchRingClass;
}

static NSAppleEventDescriptor *_Nullable ControlRoute(
    UInt32 context, StyletWacomControlType type, UInt32 control) {
  NSAppleEventDescriptor *contextRoute = ContextRoute(context);
  if (contextRoute == nil) {
    return nil;
  }
  return ObjectSpecifier(ControlClass(type), UInt32Descriptor(control),
                         formAbsolutePosition, contextRoute);
}

static NSAppleEventDescriptor *_Nullable FunctionRoute(
    UInt32 context, StyletWacomControlType type, UInt32 control,
    UInt32 function) {
  NSAppleEventDescriptor *controlRoute = ControlRoute(context, type, control);
  if (controlRoute == nil) {
    return nil;
  }
  return ObjectSpecifier(kControlFunctionClass, UInt32Descriptor(function),
                         formAbsolutePosition, controlRoute);
}

static UInt32 CountObjects(DescType objectClass,
                           NSAppleEventDescriptor *_Nullable container) {
  if (container == nil) {
    return 0;
  }
  NSAppleEventDescriptor *event =
      MakeAppleEvent(kAECoreSuite, kAECountElements);
  [event setDescriptor:[NSAppleEventDescriptor descriptorWithTypeCode:objectClass]
            forKeyword:keyAEObjectClass];
  [event setDescriptor:container forKeyword:keyDirectObject];
  NSAppleEventDescriptor *reply = SendForReply(event);
  const SInt32 count =
      [[reply descriptorForKeyword:keyDirectObject] int32Value];
  return count > 0 ? (UInt32)count : 0;
}

static NSAppleEventDescriptor *_Nullable ReadProperty(
    DescType property, DescType requestedType,
    NSAppleEventDescriptor *_Nullable container) {
  if (container == nil) {
    return nil;
  }
  NSAppleEventDescriptor *propertyRoute = ObjectSpecifier(
      formPropertyID,
      [NSAppleEventDescriptor descriptorWithTypeCode:property], formPropertyID,
      container);
  if (propertyRoute == nil) {
    return nil;
  }
  NSAppleEventDescriptor *event = MakeAppleEvent(kAECoreSuite, kAEGetData);
  [event setDescriptor:propertyRoute forKeyword:keyDirectObject];
  [event setDescriptor:
             [NSAppleEventDescriptor descriptorWithTypeCode:requestedType]
            forKeyword:keyAERequestedType];
  return [SendForReply(event) descriptorForKeyword:keyDirectObject];
}

static BOOL WriteBooleanProperty(DescType property, BOOL value,
                                 NSAppleEventDescriptor *_Nullable container) {
  if (container == nil) {
    return NO;
  }
  NSAppleEventDescriptor *propertyRoute = ObjectSpecifier(
      formPropertyID,
      [NSAppleEventDescriptor descriptorWithTypeCode:property], formPropertyID,
      container);
  if (propertyRoute == nil) {
    return NO;
  }
  Boolean booleanValue = value ? 1 : 0;
  NSAppleEventDescriptor *event = MakeAppleEvent(kAECoreSuite, kAESetData);
  [event setDescriptor:propertyRoute forKeyword:keyDirectObject];
  [event setDescriptor:[NSAppleEventDescriptor descriptorWithTypeCode:typeBoolean]
            forKeyword:keyAERequestedType];
  [event setDescriptor:
             [NSAppleEventDescriptor descriptorWithDescriptorType:typeBoolean
                                                            bytes:&booleanValue
                                                           length:sizeof(booleanValue)]
            forKeyword:keyAEData];
  return SendWithoutReply(event);
}

static UInt32 CreateContext(UInt32 tablet) {
  NSAppleEventDescriptor *tabletRoute = TabletRoute(tablet);
  if (tabletRoute == nil) {
    return 0;
  }
  NSAppleEventDescriptor *event =
      MakeAppleEvent(kAECoreSuite, kAECreateElement);
  [event setDescriptor:
             [NSAppleEventDescriptor descriptorWithTypeCode:kContextClass]
            forKeyword:keyAEObjectClass];
  [event setDescriptor:tabletRoute forKeyword:keyAEInsertHere];
  [event setDescriptor:
             [NSAppleEventDescriptor descriptorWithTypeCode:kBlankContext]
            forKeyword:kContextKindKeyword];
  NSAppleEventDescriptor *reply = SendForReply(event);
  const SInt32 context =
      [[reply descriptorForKeyword:keyDirectObject] int32Value];
  return context > 0 ? (UInt32)context : 0;
}

static void DestroyContext(UInt32 context) {
  NSAppleEventDescriptor *contextRoute = ContextRoute(context);
  if (contextRoute == nil) {
    return;
  }
  NSAppleEventDescriptor *event = MakeAppleEvent(kAECoreSuite, kAEDelete);
  [event setDescriptor:contextRoute forKeyword:keyDirectObject];
  SendWithoutReply(event);
}

static NSString *ControlKey(UInt32 tablet, StyletWacomControlType type,
                            UInt32 control) {
  return [NSString stringWithFormat:@"%u:%ld:%u", tablet, (long)type,
                                    control];
}

}

@interface StyletWacomControls ()

@property(nonatomic, copy) StyletWacomPacketHandler packetHandler;
@property(nonatomic) BOOL listening;
@property(nonatomic) BOOL enabled;
@property(nonatomic, strong)
    NSMutableDictionary<NSNumber *, NSNumber *> *contexts;
@property(nonatomic, strong)
    NSMutableArray<NSDictionary<NSString *, NSNumber *> *> *overrides;
@property(nonatomic, strong)
    NSMutableDictionary<NSNumber *, NSDictionary<NSString *, id> *> *pads;
@property(nonatomic, strong)
    NSMutableDictionary<NSString *, NSArray<NSNumber *> *> *ranges;
@property(nonatomic, strong)
    NSMutableDictionary<NSString *, NSNumber *> *buttonStates;

- (void)claimTablet:(UInt32)tablet
        tabletRoute:(NSAppleEventDescriptor *)tabletRoute;
- (void)releaseOverrides;
- (void)handleControlNotification:(NSNotification *)notification;
- (void)emitPadDevice:(NSDictionary<NSString *, id> *)pad
                 phase:(NSString *)phase;

@end

@implementation StyletWacomControls

- (instancetype)initWithPacketHandler:(StyletWacomPacketHandler)packetHandler {
  self = [super init];
  if (self != nil) {
    _packetHandler = [packetHandler copy];
    _contexts = [NSMutableDictionary dictionary];
    _overrides = [NSMutableArray array];
    _pads = [NSMutableDictionary dictionary];
    _ranges = [NSMutableDictionary dictionary];
    _buttonStates = [NSMutableDictionary dictionary];
  }
  return self;
}

- (void)dealloc {
  [self releaseOverrides];
}

- (void)setListening:(BOOL)listening {
  if (_listening == listening) {
    return;
  }
  _listening = listening;
  if (listening && self.enabled) {
    for (NSNumber *tablet in self.pads) {
      [self emitPadDevice:self.pads[tablet] phase:@"added"];
    }
  }
}

- (BOOL)setTabletPadOverrideEnabled:(BOOL)enabled {
  if (!enabled) {
    const BOOL wasEnabled = self.enabled;
    [self releaseOverrides];
    return wasEnabled;
  }
  if (self.enabled) {
    return YES;
  }
  id usageDescription =
      [NSBundle.mainBundle objectForInfoDictionaryKey:
                               @"NSAppleEventsUsageDescription"];
  if (![usageDescription isKindOfClass:NSString.class] ||
      [(NSString *)usageDescription length] == 0) {
    return NO;
  }

  const UInt32 tabletCount = CountObjects(kTabletClass, DriverRoute());
  for (UInt32 tablet = 1; tablet <= tabletCount; ++tablet) {
    NSAppleEventDescriptor *tabletRoute = TabletRoute(tablet);
    NSAppleEventDescriptor *connected =
        ReadProperty(kConnectedProperty, typeBoolean, tabletRoute);
    if (connected != nil && !connected.booleanValue) {
      continue;
    }
    [self claimTablet:tablet tabletRoute:tabletRoute];
  }
  if (self.overrides.count == 0) {
    [self releaseOverrides];
    return NO;
  }

  [[NSDistributedNotificationCenter defaultCenter]
                addObserver:self
                   selector:@selector(handleControlNotification:)
                       name:kControlNotification
                     object:kNotificationObject
         suspensionBehavior:NSNotificationSuspensionBehaviorDrop];
  self.enabled = YES;
  if (self.listening) {
    for (NSNumber *tablet in self.pads) {
      [self emitPadDevice:self.pads[tablet] phase:@"added"];
    }
  }
  return YES;
}

- (void)claimTablet:(UInt32)tablet
        tabletRoute:(NSAppleEventDescriptor *)tabletRoute {
  const UInt32 context = CreateContext(tablet);
  if (context == 0) {
    return;
  }

  NSMutableArray<NSString *> *features =
      [NSMutableArray arrayWithObject:@"deviceInfo"];
  UInt32 buttonCount = 0;
  BOOL claimed = NO;
  NSArray<NSNumber *> *types = @[
    @(StyletWacomControlTypeExpressKey), @(StyletWacomControlTypeRing),
    @(StyletWacomControlTypeStrip)
  ];
  for (NSNumber *typeValue in types) {
    const StyletWacomControlType type =
        (StyletWacomControlType)typeValue.integerValue;
    const UInt32 controlCount = CountObjects(ControlClass(type),
                                              ContextRoute(context));
    if (controlCount == 0) {
      continue;
    }
    BOOL claimedType = NO;
    for (UInt32 control = 1; control <= controlCount; ++control) {
      NSAppleEventDescriptor *controlRoute =
          ControlRoute(context, type, control);
      const UInt32 minimum = (UInt32)[ReadProperty(
          kMinimumProperty, typeUInt32, controlRoute) int32Value];
      const UInt32 maximum = (UInt32)[ReadProperty(
          kMaximumProperty, typeUInt32, controlRoute) int32Value];
      self.ranges[ControlKey(tablet, type, control)] =
          @[ @(minimum), @(maximum) ];

      const UInt32 functionCount = CountObjects(
          kControlFunctionClass, controlRoute);
      for (UInt32 function = 1; function <= functionCount; ++function) {
        NSAppleEventDescriptor *functionRoute =
            FunctionRoute(context, type, control, function);
        NSAppleEventDescriptor *available = ReadProperty(
            kFunctionAvailableProperty, typeBoolean, functionRoute);
        if (available == nil || !available.booleanValue ||
            !WriteBooleanProperty(kOverrideProperty, YES, functionRoute)) {
          continue;
        }
        [self.overrides addObject:@{
          @"context" : @(context),
          @"type" : @(type),
          @"control" : @(control),
          @"function" : @(function),
        }];
        claimed = YES;
        claimedType = YES;
      }
    }
    if (!claimedType) {
      continue;
    }
    if (type == StyletWacomControlTypeExpressKey) {
      buttonCount = controlCount;
      [features addObject:@"tabletPadButtons"];
    } else if (type == StyletWacomControlTypeRing) {
      [features addObject:@"tabletPadRing"];
    } else {
      [features addObject:@"tabletPadStrip"];
    }
  }

  if (!claimed) {
    DestroyContext(context);
    return;
  }
  self.contexts[@(tablet)] = @(context);
  NSString *name =
      [ReadProperty(kNameProperty, typeUTF8Text, tabletRoute) stringValue];
  self.pads[@(tablet)] = @{
    @"tablet" : @(tablet),
    @"name" : name.length == 0 ? @"Wacom tablet controls" : name,
    @"buttonCount" : @(buttonCount),
    @"features" : features,
  };
}

- (void)releaseOverrides {
  [[NSDistributedNotificationCenter defaultCenter]
      removeObserver:self
                name:kControlNotification
              object:kNotificationObject];
  if (self.listening) {
    for (NSNumber *tablet in self.pads) {
      [self emitPadDevice:self.pads[tablet] phase:@"removed"];
    }
  }
  for (NSDictionary<NSString *, NSNumber *> *entry in
       [self.overrides reverseObjectEnumerator]) {
    WriteBooleanProperty(
        kOverrideProperty, NO,
        FunctionRoute(entry[@"context"].unsignedIntValue,
                      (StyletWacomControlType)entry[@"type"].integerValue,
                      entry[@"control"].unsignedIntValue,
                      entry[@"function"].unsignedIntValue));
  }
  for (NSNumber *context in self.contexts.allValues) {
    DestroyContext(context.unsignedIntValue);
  }
  [self.contexts removeAllObjects];
  [self.overrides removeAllObjects];
  [self.pads removeAllObjects];
  [self.ranges removeAllObjects];
  [self.buttonStates removeAllObjects];
  self.enabled = NO;
}

- (void)handleControlNotification:(NSNotification *)notification {
  NSDictionary *info = notification.userInfo;
  const UInt32 tablet = [info[kTabletNumberKey] unsignedIntValue];
  const StyletWacomControlType type =
      (StyletWacomControlType)[info[kControlTypeKey] integerValue];
  const UInt32 control = [info[kControlNumberKey] unsignedIntValue];
  const UInt32 function = [info[kFunctionNumberKey] unsignedIntValue];
  NSNumber *rawValue = info[kControlValueKey];
  if (!self.listening || self.pads[@(tablet)] == nil || control == 0 ||
      type < StyletWacomControlTypeRing ||
      type > StyletWacomControlTypeExpressKey || rawValue == nil) {
    return;
  }

  NSString *controlName;
  NSString *phase = @"changed";
  NSNumber *value = nil;
  if (type == StyletWacomControlTypeExpressKey) {
    controlName = @"button";
    const BOOL pressed = rawValue.boolValue;
    NSString *key = ControlKey(tablet, type, control);
    NSNumber *previous = self.buttonStates[key];
    self.buttonStates[key] = @(pressed);
    if (previous != nil && previous.boolValue == pressed) {
      return;
    }
    phase = pressed ? @"began" : @"ended";
  } else {
    controlName =
        type == StyletWacomControlTypeRing ? @"ring" : @"strip";
    NSArray<NSNumber *> *range = self.ranges[ControlKey(tablet, type, control)];
    const double minimum = range.count == 2 ? range[0].doubleValue : 0.0;
    const double maximum = range.count == 2 ? range[1].doubleValue : 1.0;
    const double extent = maximum - minimum;
    const double normalized = extent > 0.0
                                  ? (rawValue.doubleValue - minimum) / extent
                                  : 0.0;
    value = @(fmin(1.0, fmax(0.0, normalized)));
  }

  NSMutableDictionary<NSString *, id> *packet = [@{
    @"type" : @"pad",
    @"timestampMicros" : @(TimestampMicros()),
    @"nativeDeviceIdentifier" :
        [NSString stringWithFormat:@"wacom-dri-pad:%u", tablet],
    @"control" : controlName,
    @"controlIndex" : @(control - 1),
    @"phase" : phase,
  } mutableCopy];
  if (value != nil) {
    packet[@"value"] = value;
  }
  if (function > 0) {
    packet[@"mode"] = @(function - 1);
  }
  self.packetHandler(packet);
}

- (void)emitPadDevice:(NSDictionary<NSString *, id> *)pad
                 phase:(NSString *)phase {
  if (!self.listening) {
    return;
  }
  const UInt32 tablet = [pad[@"tablet"] unsignedIntValue];
  NSMutableDictionary<NSString *, id> *packet = [@{
    @"type" : @"device",
    @"timestampMicros" : @(TimestampMicros()),
    @"phase" : phase,
    @"kind" : @"pad",
    @"nativeDeviceIdentifier" :
        [NSString stringWithFormat:@"wacom-dri-pad:%u", tablet],
    @"name" : pad[@"name"],
    @"features" : pad[@"features"],
  } mutableCopy];
  NSNumber *buttonCount = pad[@"buttonCount"];
  if (buttonCount.unsignedIntValue > 0) {
    packet[@"buttonCount"] = buttonCount;
  }
  self.packetHandler(packet);
}

@end
