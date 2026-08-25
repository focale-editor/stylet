import Cocoa
import FlutterMacOS

/// Method-channel name shared with Stylet's Dart backend.
private let methodChannelName = "dev.focale.stylet/methods"

/// Event-channel name shared with Stylet's Dart backend.
private let eventChannelName = "dev.focale.stylet/events"

/// macOS backend for native tablet pressure, tilt, rotation, and controls.
public final class StyletPlugin: NSObject, FlutterPlugin, FlutterStreamHandler {
  /// Request-response channel retained for the plugin lifetime.
  private let methodChannel: FlutterMethodChannel

  /// Continuous event channel retained for the plugin lifetime.
  private let eventChannel: FlutterEventChannel

  /// Flutter view whose coordinate system defines emitted positions.
  private weak var view: NSView?

  /// Current Dart event consumer, or `nil` while the stream is idle.
  private var eventSink: FlutterEventSink?

  /// Token returned by AppKit for the active local event monitor.
  private var eventMonitor: Any?

  /// Most recently emitted position, used to calculate deltas.
  private var lastPosition: NSPoint?

  /// Tablet tool identifier associated with `lastPosition`.
  private var lastPointingDeviceIdentifier: Int?

  /// Creates a channel backend associated with Flutter's rendering view.
  private init(binaryMessenger: FlutterBinaryMessenger, view: NSView?) {
    methodChannel = FlutterMethodChannel(
      name: methodChannelName,
      binaryMessenger: binaryMessenger
    )
    eventChannel = FlutterEventChannel(
      name: eventChannelName,
      binaryMessenger: binaryMessenger
    )
    self.view = view
    super.init()
  }

  /// Removes AppKit observation before the backend is released.
  deinit {
    stopMonitoring()
  }

  /// Registers Stylet's channels and passive tablet event monitor.
  public static func register(with registrar: FlutterPluginRegistrar) {
    let instance = StyletPlugin(
      binaryMessenger: registrar.messenger,
      view: registrar.view
    )
    registrar.addMethodCallDelegate(instance, channel: instance.methodChannel)
    instance.eventChannel.setStreamHandler(instance)
  }

  /// Handles capability requests from the Dart backend.
  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    guard call.method == "getCapabilities" else {
      result(FlutterMethodNotImplemented)
      return
    }
    result([
      "pressure",
      "tilt",
      "orientation",
      "barrelRotation",
      "tangentialPressure",
      "primaryButton",
      "secondaryButton",
      "eraser",
      "hover",
    ])
  }

  /// Starts native observation for a Dart event-channel subscription.
  public func onListen(
    withArguments arguments: Any?,
    eventSink events: @escaping FlutterEventSink
  ) -> FlutterError? {
    eventSink = events
    startMonitoring()
    return nil
  }

  /// Stops native observation when the Dart subscription is cancelled.
  public func onCancel(withArguments arguments: Any?) -> FlutterError? {
    eventSink = nil
    stopMonitoring()
    return nil
  }

  /// Installs an application-local monitor that always returns the source event.
  private func startMonitoring() {
    guard eventMonitor == nil else {
      return
    }
    let mask: NSEvent.EventTypeMask = [
      .tabletPoint,
      .tabletProximity,
      .mouseMoved,
      .leftMouseDown,
      .leftMouseDragged,
      .leftMouseUp,
      .rightMouseDown,
      .rightMouseDragged,
      .rightMouseUp,
      .otherMouseDown,
      .otherMouseDragged,
      .otherMouseUp,
    ]
    eventMonitor = NSEvent.addLocalMonitorForEvents(matching: mask) { [weak self] event in
      self?.observe(event: event)
      return event
    }
  }

  /// Removes the AppKit event monitor and clears correlation state.
  private func stopMonitoring() {
    if let eventMonitor {
      NSEvent.removeMonitor(eventMonitor)
    }
    eventMonitor = nil
    lastPosition = nil
    lastPointingDeviceIdentifier = nil
  }

  /// Filters one AppKit event and emits it when it belongs to a tablet tool.
  private func observe(event: NSEvent) {
    guard let view, isTabletEvent(event: event) else {
      return
    }
    if let eventWindow = event.window, eventWindow !== view.window {
      return
    }
    sendMotion(event: event, in: view)
  }

  /// Emits one normalized motion packet for an AppKit tablet event.
  private func sendMotion(event: NSEvent, in view: NSView) {
    guard let eventSink else {
      return
    }
    let isProximity = event.type == .tabletProximity || event.subtype == .tabletProximity
    let pointingDeviceIdentifier = event.pointingDeviceID
    let position = position(for: event, in: view, isProximity: isProximity)
    let sameTool = lastPointingDeviceIdentifier == pointingDeviceIdentifier && lastPosition != nil
    let previousPosition = sameTool ? lastPosition ?? position : position
    let isDown = tipIsDown(for: event)
    let phase = phaseName(for: event, isProximity: isProximity, isDown: isDown)
    let tool = event.pointingDeviceType == .eraser ? "eraser" : "pen"
    var packet: [String: Any] = [
      "type": "motion",
      "timestampMicros": Int64((event.timestamp * 1_000_000).rounded()),
      "phase": phase,
      "tool": tool,
      "pointerIdentifier": pointingDeviceIdentifier,
      "deviceIdentifier": event.deviceID,
      "nativeDeviceIdentifier": nativeIdentifier(for: event),
      "x": Double(position.x),
      "y": Double(position.y),
      "deltaX": Double(position.x - previousPosition.x),
      "deltaY": Double(position.y - previousPosition.y),
      "buttons": flutterButtons(for: event, isDown: isDown),
      "isDown": isDown,
      "features": [
        "pressure",
        "tilt",
        "orientation",
        "barrelRotation",
        "tangentialPressure",
        "primaryButton",
        "secondaryButton",
        "eraser",
        "hover",
      ],
    ]

    if !isProximity {
      let tiltComponents = tiltComponents(for: event)
      packet["pressure"] = Double(event.pressure)
      packet["pressureMinimum"] = 0.0
      packet["pressureMaximum"] = 1.0
      packet["tiltX"] = tiltComponents.x
      packet["tiltY"] = tiltComponents.y
      packet["tilt"] = tiltComponents.magnitude
      packet["orientation"] = tiltComponents.orientation
      packet["barrelRotation"] = Double(event.rotation) * .pi / 180
      packet["tangentialPressure"] = Double(event.tangentialPressure)
    }
    eventSink(packet)

    if phase == "removed" {
      lastPosition = nil
      lastPointingDeviceIdentifier = nil
    } else {
      lastPosition = position
      lastPointingDeviceIdentifier = pointingDeviceIdentifier
    }
  }

  /// Whether an AppKit event carries tablet point or proximity data.
  private func isTabletEvent(event: NSEvent) -> Bool {
    event.type == .tabletPoint || event.type == .tabletProximity
      || event.subtype == .tabletPoint || event.subtype == .tabletProximity
  }

  /// Converts an AppKit window position to Flutter's top-left coordinate space.
  private func position(for event: NSEvent, in view: NSView, isProximity: Bool) -> NSPoint {
    if isProximity, let lastPosition {
      return lastPosition
    }
    let localPosition = view.convert(event.locationInWindow, from: nil)
    return NSPoint(x: localPosition.x, y: view.bounds.height - localPosition.y)
  }

  /// Whether the tablet tip is currently in contact with the surface.
  private func tipIsDown(for event: NSEvent) -> Bool {
    switch event.type {
    case .leftMouseDown, .leftMouseDragged:
      return true
    case .leftMouseUp, .tabletProximity:
      return false
    default:
      return event.pressure > 0
    }
  }

  /// Converts an AppKit event into Stylet's cross-platform lifecycle phase.
  private func phaseName(for event: NSEvent, isProximity: Bool, isDown: Bool) -> String {
    if isProximity {
      return event.isEnteringProximity ? "added" : "removed"
    }
    switch event.type {
    case .leftMouseDown:
      return "down"
    case .leftMouseUp:
      return "up"
    default:
      return isDown ? "move" : "hover"
    }
  }

  /// Converts AppKit's mouse button mask into Flutter's stylus button bits.
  private func flutterButtons(for event: NSEvent, isDown: Bool) -> Int {
    var buttons = isDown ? 1 : 0
    if event.buttonMask.contains(.penLowerSide) {
      buttons |= 2
    }
    if event.buttonMask.contains(.penUpperSide) {
      buttons |= 4
    }
    return buttons
  }

  /// Produces a descriptive stable identifier for one physical tablet tool.
  private func nativeIdentifier(for event: NSEvent) -> String {
    "\(event.systemTabletID):\(event.pointingDeviceSerialNumber):\(event.uniqueID)"
  }

  /// Converts normalized AppKit tilt axes to radians and derived polar angles.
  private func tiltComponents(for event: NSEvent) -> (
    x: Double,
    y: Double,
    magnitude: Double,
    orientation: Double
  ) {
    let x = Double(event.tilt.x) * .pi / 2
    let y = Double(event.tilt.y) * .pi / 2
    let tangentX = tan(x)
    let tangentY = tan(y)
    return (
      x: x,
      y: y,
      magnitude: atan(hypot(tangentX, tangentY)),
      orientation: atan2(tangentX, -tangentY)
    )
  }
}
