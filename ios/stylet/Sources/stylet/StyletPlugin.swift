import Flutter
import UIKit

/// Method-channel name shared with Stylet's Dart backend.
private let methodChannelName = "dev.focale.stylet/methods"

/// Event-channel name shared with Stylet's Dart backend.
private let eventChannelName = "dev.focale.stylet/events"

/// Observes Apple Pencil touches without claiming or cancelling Flutter input.
private final class PencilObservationGestureRecognizer: UIGestureRecognizer {
  /// Callback invoked with the stable touch and one chronological real batch.
  private let sampleObserver: (UITouch, [UITouch]) -> Void

  /// Callback invoked with each atomic replacement prediction.
  private let predictionObserver: (UITouch, [UITouch]) -> Void

  /// Callback invoked when UIKit refines previously estimated properties.
  private let correctionObserver: (Set<UITouch>) -> Void

  /// Creates a passive recognizer with callbacks for every high-fidelity path.
  init(
    sampleObserver: @escaping (UITouch, [UITouch]) -> Void,
    predictionObserver: @escaping (UITouch, [UITouch]) -> Void,
    correctionObserver: @escaping (Set<UITouch>) -> Void
  ) {
    self.sampleObserver = sampleObserver
    self.predictionObserver = predictionObserver
    self.correctionObserver = correctionObserver
    super.init(target: nil, action: nil)
    allowedTouchTypes = [NSNumber(value: UITouch.TouchType.pencil.rawValue)]
    cancelsTouchesInView = false
    delaysTouchesBegan = false
    delaysTouchesEnded = false
  }

  override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
    observe(touches, with: event)
  }

  override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent) {
    observe(touches, with: event)
  }

  override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent) {
    observe(touches, with: event)
    state = .failed
  }

  override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent) {
    observe(touches, with: event)
    state = .failed
  }

  override func touchesEstimatedPropertiesUpdated(_ touches: Set<UITouch>) {
    correctionObserver(Set(touches.filter { touch in touch.type == .pencil }))
  }

  /// Forwards coalesced samples in chronological order when UIKit provides them.
  private func observe(_ touches: Set<UITouch>, with event: UIEvent) {
    for touch in touches where touch.type == .pencil {
      let samples = event.coalescedTouches(for: touch) ?? [touch]
      sampleObserver(touch, samples)
      predictionObserver(touch, event.predictedTouches(for: touch) ?? [])
    }
  }
}

/// iOS backend for Apple Pencil motion, double-tap, squeeze, and barrel roll.
public final class StyletPlugin: NSObject, FlutterPlugin, FlutterStreamHandler,
  UIPencilInteractionDelegate
{
  /// Request-response channel retained for the plugin lifetime.
  private let methodChannel: FlutterMethodChannel

  /// Continuous event channel retained for the plugin lifetime.
  private let eventChannel: FlutterEventChannel

  /// Flutter view whose coordinate system defines emitted positions.
  private weak var view: UIView?

  /// Current Dart event consumer, or `nil` while the stream is idle.
  private var eventSink: FlutterEventSink?

  /// Passive touch observer installed while Dart listens.
  private var touchRecognizer: PencilObservationGestureRecognizer?

  /// Apple Pencil hover observer installed on supported iPad hardware.
  private var hoverRecognizer: UIHoverGestureRecognizer?

  /// System interaction that reports Apple Pencil body gestures.
  private var pencilInteraction: UIPencilInteraction?

  /// Latest position for each active touch, used to calculate deltas.
  private var lastPositions: [ObjectIdentifier: CGPoint] = [:]

  /// Latest Apple Pencil hover position, used to calculate hover deltas.
  private var lastHoverPosition: CGPoint?

  /// Original packets retained until every promised property update arrives.
  private var pendingEstimatePackets: [String: [String: Any]] = [:]

  /// Property updates still expected for each retained sample identifier.
  private var pendingEstimateProperties: [String: UITouch.Properties] = [:]

  /// Creates a channel backend associated with Flutter's root view.
  private init(binaryMessenger: FlutterBinaryMessenger, view: UIView?) {
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

  /// Registers Stylet's channels and passive Apple Pencil observers.
  public static func register(with registrar: FlutterPluginRegistrar) {
    let instance = StyletPlugin(
      binaryMessenger: registrar.messenger(),
      view: registrar.viewController?.view
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
    var features = [
      "pressure",
      "tilt",
      "orientation",
      "primaryButton",
      "hover",
      "doubleTap",
      "historicalSamples",
      "predictedSamples",
      "estimatedPropertyUpdates",
    ]
    if #available(iOS 17.5, *) {
      features.append("barrelRotation")
      features.append("squeeze")
    }
    result(features)
  }

  /// Starts native observation for a Dart event-channel subscription.
  public func onListen(
    withArguments arguments: Any?,
    eventSink events: @escaping FlutterEventSink
  ) -> FlutterError? {
    eventSink = events
    startObserving()
    return nil
  }

  /// Stops native observation when the Dart subscription is cancelled.
  public func onCancel(withArguments arguments: Any?) -> FlutterError? {
    eventSink = nil
    stopObserving()
    return nil
  }

  /// Reports the legacy Apple Pencil double-tap callback used before iOS 17.5.
  public func pencilInteractionDidTap(_ interaction: UIPencilInteraction) {
    sendAction(
      action: "doubleTap",
      phase: "discrete",
      timestamp: ProcessInfo.processInfo.systemUptime,
      pose: nil
    )
  }

  /// Reports a double tap together with its hover pose on modern iPadOS.
  @available(iOS 17.5, *)
  public func pencilInteraction(
    _ interaction: UIPencilInteraction,
    didReceiveTap tap: UIPencilInteraction.Tap
  ) {
    sendAction(
      action: "doubleTap",
      phase: "discrete",
      timestamp: tap.timestamp,
      pose: poseMap(from: tap.hoverPose)
    )
  }

  /// Reports every phase of an Apple Pencil Pro squeeze interaction.
  @available(iOS 17.5, *)
  public func pencilInteraction(
    _ interaction: UIPencilInteraction,
    didReceiveSqueeze squeeze: UIPencilInteraction.Squeeze
  ) {
    sendAction(
      action: "squeeze",
      phase: actionPhaseName(for: squeeze.phase),
      timestamp: squeeze.timestamp,
      pose: poseMap(from: squeeze.hoverPose)
    )
  }

  /// Reports Apple Pencil pose, distance, and barrel roll while hovering.
  @available(iOS 16.1, *)
  @objc
  private func handleHover(_ recognizer: UIHoverGestureRecognizer) {
    guard let eventSink, let view else {
      return
    }
    let position = recognizer.location(in: view)
    let previousPosition = lastHoverPosition ?? position
    let isRemoved = recognizer.state == .ended || recognizer.state == .cancelled
    var features = ["distance", "tilt", "orientation", "hover"]
    var packet: [String: Any] = [
      "type": "motion",
      "timestampMicros": Int64((ProcessInfo.processInfo.systemUptime * 1_000_000).rounded()),
      "phase": isRemoved ? "removed" : "hover",
      "tool": "pen",
      "pointerIdentifier": -1,
      "deviceIdentifier": 0,
      "nativeDeviceIdentifier": "apple-pencil",
      "x": Double(position.x),
      "y": Double(position.y),
      "deltaX": Double(position.x - previousPosition.x),
      "deltaY": Double(position.y - previousPosition.y),
      "buttons": 0,
      "isDown": false,
      "distance": Double(recognizer.zOffset),
      "distanceMaximum": 1.0,
      "tilt": Double((.pi / 2) - recognizer.altitudeAngle),
      "orientation": Double(recognizer.azimuthAngle(in: view)),
    ]
    if #available(iOS 17.5, *) {
      packet["barrelRotation"] = Double(recognizer.rollAngle)
      features.append("barrelRotation")
    }
    packet["features"] = features
    eventSink(packet)
    lastHoverPosition = isRemoved ? nil : position
  }

  /// Installs native observers without altering Flutter's gesture decisions.
  private func startObserving() {
    guard touchRecognizer == nil, let view else {
      return
    }
    let recognizer = PencilObservationGestureRecognizer(
      sampleObserver: { [weak self] touch, samples in
        self?.sendMotions(for: samples, touchIdentifier: ObjectIdentifier(touch))
      },
      predictionObserver: { [weak self] touch, samples in
        self?.sendPrediction(for: touch, samples: samples)
      },
      correctionObserver: { [weak self] touches in
        self?.sendCorrections(for: touches)
      }
    )
    view.addGestureRecognizer(recognizer)
    touchRecognizer = recognizer

    if #available(iOS 16.1, *) {
      let hover = UIHoverGestureRecognizer(target: self, action: #selector(handleHover(_:)))
      hover.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.pencil.rawValue)]
      hover.cancelsTouchesInView = false
      view.addGestureRecognizer(hover)
      hoverRecognizer = hover
    }

    let interaction = UIPencilInteraction()
    interaction.delegate = self
    view.addInteraction(interaction)
    pencilInteraction = interaction
  }

  /// Removes every observer and clears interaction-scoped correlation state.
  private func stopObserving() {
    if let touchRecognizer {
      touchRecognizer.view?.removeGestureRecognizer(touchRecognizer)
    }
    if let hoverRecognizer {
      hoverRecognizer.view?.removeGestureRecognizer(hoverRecognizer)
    }
    if let pencilInteraction {
      pencilInteraction.view?.removeInteraction(pencilInteraction)
    }
    touchRecognizer = nil
    hoverRecognizer = nil
    pencilInteraction = nil
    lastPositions.removeAll()
    lastHoverPosition = nil
    pendingEstimatePackets.removeAll()
    pendingEstimateProperties.removeAll()
  }

  /// Emits one chronological Apple Pencil batch and retains pending estimates.
  private func sendMotions(
    for touches: [UITouch],
    touchIdentifier: ObjectIdentifier
  ) {
    guard let eventSink, let view else {
      return
    }
    var previousPosition = lastPositions[touchIdentifier]
    var packets: [[String: Any]] = []
    for touch in touches {
      guard let packet = motionPacket(
        for: touch,
        touchIdentifier: touchIdentifier,
        previousPosition: previousPosition,
        includesEstimationMetadata: true
      ) else {
        continue
      }
      registerPendingEstimate(for: touch, packet: packet)
      packets.append(packet)
      previousPosition = touch.location(in: view)
    }
    guard let lastTouch = touches.last, !packets.isEmpty else {
      return
    }
    if packets.count == 1 {
      eventSink(packets[0])
    } else {
      eventSink(["type": "batch", "events": packets])
    }

    if lastTouch.phase == .ended || lastTouch.phase == .cancelled {
      lastPositions.removeValue(forKey: touchIdentifier)
    } else if let previousPosition {
      lastPositions[touchIdentifier] = previousPosition
    }
  }

  /// Emits one prediction packet that atomically replaces the preceding preview.
  private func sendPrediction(for touch: UITouch, samples: [UITouch]) {
    guard let eventSink, let view, touch.type == .pencil else {
      return
    }
    let touchIdentifier = ObjectIdentifier(touch)
    var previousPosition = lastPositions[touchIdentifier] ?? touch.location(in: view)
    var packets: [[String: Any]] = []
    for sample in samples {
      guard let packet = motionPacket(
        for: sample,
        touchIdentifier: touchIdentifier,
        previousPosition: previousPosition,
        includesEstimationMetadata: false
      ) else {
        continue
      }
      packets.append(packet)
      previousPosition = sample.location(in: view)
    }
    eventSink([
      "type": "prediction",
      "timestampMicros": Int64((touch.timestamp * 1_000_000).rounded()),
      "pointerIdentifier": touchIdentifier.hashValue,
      "deviceIdentifier": 0,
      "nativeDeviceIdentifier": "apple-pencil",
      "samples": packets,
    ])
  }

  /// Emits complete corrected samples for every estimated-property notification.
  private func sendCorrections(for touches: Set<UITouch>) {
    guard let eventSink else {
      return
    }
    for touch in touches {
      guard let identifier = sampleIdentifier(for: touch),
        let originalPacket = pendingEstimatePackets[identifier],
        let previouslyExpected = pendingEstimateProperties[identifier]
      else {
        continue
      }
      let originalX = originalPacket["x"] as? Double ?? 0
      let originalY = originalPacket["y"] as? Double ?? 0
      let originalDeltaX = originalPacket["deltaX"] as? Double ?? 0
      let originalDeltaY = originalPacket["deltaY"] as? Double ?? 0
      let previousPosition = CGPoint(
        x: originalX - originalDeltaX,
        y: originalY - originalDeltaY
      )
      guard var correctedPacket = motionPacket(
        for: touch,
        touchIdentifier: ObjectIdentifier(touch),
        previousPosition: previousPosition,
        includesEstimationMetadata: true
      ) else {
        continue
      }
      for key in ["timestampMicros", "phase", "buttons", "isDown"] {
        correctedPacket[key] = originalPacket[key]
      }
      let stillExpected = touch.estimatedPropertiesExpectingUpdates
      let correctedProperties = previouslyExpected.subtracting(stillExpected)
      eventSink([
        "type": "correction",
        "timestampMicros": Int64(
          (ProcessInfo.processInfo.systemUptime * 1_000_000).rounded()
        ),
        "sampleIdentifier": identifier,
        "correctedProperties": propertyNames(for: correctedProperties),
        "sample": correctedPacket,
      ])

      if stillExpected.isEmpty {
        pendingEstimatePackets.removeValue(forKey: identifier)
        pendingEstimateProperties.removeValue(forKey: identifier)
      } else {
        pendingEstimatePackets[identifier] = correctedPacket
        pendingEstimateProperties[identifier] = stillExpected
      }
    }
  }

  /// Builds one motion map without mutating pointer correlation state.
  private func motionPacket(
    for touch: UITouch,
    touchIdentifier: ObjectIdentifier,
    previousPosition: CGPoint?,
    includesEstimationMetadata: Bool
  ) -> [String: Any]? {
    guard let view, touch.type == .pencil else {
      return nil
    }
    let position = touch.location(in: view)
    let isDown = touch.phase == .began || touch.phase == .moved || touch.phase == .stationary
    var features = ["pressure", "tilt", "orientation", "primaryButton"]
    var packet: [String: Any] = [
      "type": "motion",
      "timestampMicros": Int64((touch.timestamp * 1_000_000).rounded()),
      "phase": phaseName(for: touch.phase),
      "tool": "pen",
      "pointerIdentifier": touchIdentifier.hashValue,
      "deviceIdentifier": 0,
      "nativeDeviceIdentifier": "apple-pencil",
      "x": Double(position.x),
      "y": Double(position.y),
      "deltaX": Double(position.x - (previousPosition?.x ?? position.x)),
      "deltaY": Double(position.y - (previousPosition?.y ?? position.y)),
      "buttons": isDown ? 1 : 0,
      "isDown": isDown,
      "pressure": Double(touch.force),
      "pressureMinimum": 0.0,
      "pressureMaximum": Double(touch.maximumPossibleForce),
      "tilt": Double((.pi / 2) - touch.altitudeAngle),
      "orientation": Double(touch.azimuthAngle(in: view)),
    ]
    if #available(iOS 17.5, *) {
      packet["barrelRotation"] = Double(touch.rollAngle)
      features.append("barrelRotation")
    }
    packet["features"] = features
    if includesEstimationMetadata {
      if let identifier = sampleIdentifier(for: touch) {
        packet["sampleIdentifier"] = identifier
      }
      let estimatedProperties = propertyNames(for: touch.estimatedProperties)
      if !estimatedProperties.isEmpty {
        packet["estimatedProperties"] = estimatedProperties
      }
      let expectedProperties = propertyNames(
        for: touch.estimatedPropertiesExpectingUpdates
      )
      if !expectedProperties.isEmpty {
        packet["propertiesExpectingUpdates"] = expectedProperties
      }
    }
    return packet
  }

  /// Retains [packet] when UIKit promises later values for [touch].
  private func registerPendingEstimate(for touch: UITouch, packet: [String: Any]) {
    let expectedProperties = touch.estimatedPropertiesExpectingUpdates
    guard !expectedProperties.isEmpty, let identifier = sampleIdentifier(for: touch) else {
      return
    }
    pendingEstimatePackets[identifier] = packet
    pendingEstimateProperties[identifier] = expectedProperties
  }

  /// Returns the platform-channel identifier for one estimable touch sample.
  private func sampleIdentifier(for touch: UITouch) -> String? {
    guard let index = touch.estimationUpdateIndex else {
      return nil
    }
    return "apple-pencil:\(index.stringValue)"
  }

  /// Converts a UIKit estimated-property mask into Stylet field names.
  private func propertyNames(for properties: UITouch.Properties) -> [String] {
    var names: [String] = []
    if properties.contains(.location) {
      names.append("position")
    }
    if properties.contains(.force) {
      names.append("pressure")
    }
    if properties.contains(.altitude) {
      names.append("tilt")
    }
    if properties.contains(.azimuth) {
      names.append("orientation")
    }
    if #available(iOS 17.5, *), properties.contains(.roll) {
      names.append("barrelRotation")
    }
    return names
  }

  /// Emits one normalized Apple Pencil body-action packet.
  private func sendAction(
    action: String,
    phase: String,
    timestamp: TimeInterval,
    pose: [String: Any]?
  ) {
    guard let eventSink else {
      return
    }
    var packet: [String: Any] = [
      "type": "action",
      "timestampMicros": Int64((timestamp * 1_000_000).rounded()),
      "action": action,
      "phase": phase,
    ]
    if let pose {
      packet["pose"] = pose
    }
    eventSink(packet)
  }

  /// Converts a UIKit touch phase into Stylet's cross-platform vocabulary.
  private func phaseName(for phase: UITouch.Phase) -> String {
    switch phase {
    case .began:
      return "down"
    case .moved, .stationary:
      return "move"
    case .ended:
      return "up"
    case .cancelled:
      return "cancel"
    default:
      return "hover"
    }
  }

  /// Converts a UIKit body-action phase into Stylet's vocabulary.
  @available(iOS 17.5, *)
  private func actionPhaseName(for phase: UIPencilInteraction.Phase) -> String {
    switch phase {
    case .began:
      return "began"
    case .changed:
      return "changed"
    case .ended:
      return "ended"
    case .cancelled:
      return "cancelled"
    @unknown default:
      return "cancelled"
    }
  }

  /// Converts an optional system hover pose into a standard-codec map.
  @available(iOS 17.5, *)
  private func poseMap(from pose: UIPencilHoverPose?) -> [String: Any]? {
    guard let pose else {
      return nil
    }
    return [
      "x": Double(pose.location.x),
      "y": Double(pose.location.y),
      "distance": Double(pose.zOffset),
      "tilt": Double((.pi / 2) - pose.altitudeAngle),
      "orientation": Double(pose.azimuthAngle),
      "barrelRotation": Double(pose.rollAngle),
    ]
  }
}
