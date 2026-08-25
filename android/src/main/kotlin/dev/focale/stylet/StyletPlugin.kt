package dev.focale.stylet

import android.app.Activity
import android.content.Context
import android.hardware.input.InputManager
import android.os.Build
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import androidx.input.motionprediction.MotionEventPredictor
import io.flutter.embedding.android.FlutterView
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.embedding.engine.plugins.activity.ActivityAware
import io.flutter.embedding.engine.plugins.activity.ActivityPluginBinding
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import kotlin.math.PI
import kotlin.math.abs

/** Couples a serialized motion sample with the position used for the next delta. */
private class BuiltMotionPacket {
    /** Standard-codec map sent through Flutter's event channel. */
    val value: MutableMap<String, Any>

    /** Logical-pixel position represented by [value]. */
    val position: Pair<Double, Double>

    /** Creates a built packet from its serialized [value] and [position]. */
    constructor(
        value: MutableMap<String, Any>,
        position: Pair<Double, Double>,
    ) {
        this.value = value
        this.position = position
    }
}

/** Collects Android stylus axes without consuming Flutter's pointer events. */
class StyletPlugin :
    FlutterPlugin,
    ActivityAware,
    MethodChannel.MethodCallHandler,
    EventChannel.StreamHandler {
    /** Names and constants shared with the Dart method-channel implementation. */
    private companion object {
        /** Request-response channel used by Stylet. */
        const val METHOD_CHANNEL_NAME = "dev.focale.stylet/methods"

        /** Continuous input channel used by Stylet. */
        const val EVENT_CHANNEL_NAME = "dev.focale.stylet/events"

        /** Flutter button bit representing tip contact. */
        const val FLUTTER_PRIMARY_BUTTON = 1

        /** Flutter button bit representing the first stylus side button. */
        const val FLUTTER_PRIMARY_STYLUS_BUTTON = 2

        /** Flutter button bit representing the second stylus side button. */
        const val FLUTTER_SECONDARY_STYLUS_BUTTON = 4
    }

    /** Method channel registered with the current Flutter engine. */
    private var methodChannel: MethodChannel? = null

    /** Event channel registered with the current Flutter engine. */
    private var eventChannel: EventChannel? = null

    /** Active Dart event sink, or null while nobody listens. */
    private var eventSink: EventChannel.EventSink? = null

    /** Activity whose Flutter view currently owns the input listeners. */
    private var activity: Activity? = null

    /** Flutter view observed passively for touch and generic motion. */
    private var flutterView: FlutterView? = null

    /** Android service that reports input-device connection and metadata changes. */
    private var inputManager: InputManager? = null

    /** Predictor associated with the currently observed Flutter view. */
    private var motionEventPredictor: MotionEventPredictor? = null

    /** Latest complete metadata packet for each connected stylus device. */
    private val deviceDescriptions = mutableMapOf<Int, Map<String, Any>>()

    /** Whether [inputDeviceListener] is currently registered. */
    private var isInputDeviceListenerRegistered = false

    /** Last logical position for each Android pointer identifier. */
    private val lastPositions = mutableMapOf<Int, Pair<Double, Double>>()

    /** Passive listener for contact motion such as down, move, and up. */
    private val touchListener = View.OnTouchListener { _, event ->
        handleMotionEvent(event)
        false
    }

    /** Passive listener for hover and stylus button motion. */
    private val genericMotionListener = View.OnGenericMotionListener { _, event ->
        handleMotionEvent(event)
        false
    }

    /** Listener that translates Android input-device lifetime notifications. */
    private val inputDeviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) {
            updateInputDevice(deviceId, "added")
        }

        override fun onInputDeviceChanged(deviceId: Int) {
            updateInputDevice(deviceId, "changed")
        }

        override fun onInputDeviceRemoved(deviceId: Int) {
            removeInputDevice(deviceId)
        }
    }

    override fun onAttachedToEngine(flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
        methodChannel = MethodChannel(flutterPluginBinding.binaryMessenger, METHOD_CHANNEL_NAME).also { channel -> channel.setMethodCallHandler(this) }
        eventChannel = EventChannel(flutterPluginBinding.binaryMessenger, EVENT_CHANNEL_NAME).also { channel -> channel.setStreamHandler(this) }
        inputManager = flutterPluginBinding.applicationContext.getSystemService(Context.INPUT_SERVICE) as? InputManager
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        detachFromFlutterView()
        stopInputDeviceObservation()
        methodChannel?.setMethodCallHandler(null)
        eventChannel?.setStreamHandler(null)
        methodChannel = null
        eventChannel = null
        eventSink = null
        inputManager = null
    }

    override fun onAttachedToActivity(binding: ActivityPluginBinding) {
        activity = binding.activity
        attachToFlutterView(binding.activity)
    }

    override fun onDetachedFromActivityForConfigChanges() {
        detachFromActivity()
    }

    override fun onReattachedToActivityForConfigChanges(binding: ActivityPluginBinding) {
        onAttachedToActivity(binding)
    }

    override fun onDetachedFromActivity() {
        detachFromActivity()
    }

    override fun onMethodCall(
        call: MethodCall,
        result: MethodChannel.Result,
    ) {
        when (call.method) {
            "getCapabilities" -> result.success(
                listOf(
                    "pressure",
                    "tilt",
                    "orientation",
                    "distance",
                    "barrelRotation",
                    "primaryButton",
                    "secondaryButton",
                    "eraser",
                    "hover",
                    "historicalSamples",
                    "predictedSamples",
                    "deviceInfo",
                ),
            )

            else -> result.notImplemented()
        }
    }

    override fun onListen(
        arguments: Any?,
        events: EventChannel.EventSink,
    ) {
        eventSink = events
        startInputDeviceObservation()
    }

    override fun onCancel(arguments: Any?) {
        stopInputDeviceObservation()
        eventSink = null
    }

    /** Finds and observes the Flutter view after the activity has built its hierarchy. */
    private fun attachToFlutterView(activity: Activity) {
        val content = activity.window.decorView
        content.post {
            if (this.activity !== activity) {
                return@post
            }
            val view = findFlutterView(content) ?: return@post
            if (flutterView === view) {
                return@post
            }
            detachFromFlutterView()
            flutterView = view
            motionEventPredictor = MotionEventPredictor.newInstance(view)
            view.setOnTouchListener(touchListener)
            view.setOnGenericMotionListener(genericMotionListener)
        }
    }

    /** Removes listeners and activity state while preserving engine channels. */
    private fun detachFromActivity() {
        detachFromFlutterView()
        activity = null
    }

    /** Removes the passive listeners from the previously observed Flutter view. */
    private fun detachFromFlutterView() {
        flutterView?.setOnTouchListener(null)
        flutterView?.setOnGenericMotionListener(null)
        flutterView = null
        motionEventPredictor = null
        lastPositions.clear()
    }

    /** Recursively returns the first Flutter view below [view]. */
    private fun findFlutterView(view: View): FlutterView? {
        if (view is FlutterView) {
            return view
        }
        if (view !is ViewGroup) {
            return null
        }
        for (index in 0 until view.childCount) {
            val found = findFlutterView(view.getChildAt(index))
            if (found != null) {
                return found
            }
        }
        return null
    }

    /** Registers device callbacks and publishes every stylus device already present. */
    private fun startInputDeviceObservation() {
        val manager = inputManager ?: return
        if (isInputDeviceListenerRegistered) {
            return
        }
        manager.registerInputDeviceListener(inputDeviceListener, null)
        isInputDeviceListenerRegistered = true
        for (deviceId in InputDevice.getDeviceIds()) {
            if (!deviceDescriptions.containsKey(deviceId)) {
                updateInputDevice(deviceId, "added")
            }
        }
    }

    /** Unregisters device callbacks and releases cached native descriptions. */
    private fun stopInputDeviceObservation() {
        if (isInputDeviceListenerRegistered) {
            inputManager?.unregisterInputDeviceListener(inputDeviceListener)
        }
        isInputDeviceListenerRegistered = false
        deviceDescriptions.clear()
    }

    /** Publishes complete metadata for one added or changed Android input device. */
    private fun updateInputDevice(
        deviceId: Int,
        requestedPhase: String,
    ) {
        val device = inputManager?.getInputDevice(deviceId) ?: return
        if (!isStylusDevice(device)) {
            removeInputDevice(deviceId)
            return
        }
        val hadDescription = deviceDescriptions.containsKey(deviceId)
        if (hadDescription && requestedPhase == "added") {
            return
        }
        val description = describeInputDevice(device)
        deviceDescriptions[deviceId] = description
        val phase = if (hadDescription) requestedPhase else "added"
        emitDevice(description, phase)
    }

    /** Publishes removal using the latest metadata cached for [deviceId]. */
    private fun removeInputDevice(deviceId: Int) {
        val description = deviceDescriptions.remove(deviceId) ?: return
        emitDevice(description, "removed")
    }

    /** Returns standard-codec metadata for one stylus-capable Android device. */
    private fun describeInputDevice(device: InputDevice): Map<String, Any> {
        val features = mutableListOf("deviceInfo", "primaryButton", "secondaryButton", "eraser")
        if (motionRange(device, MotionEvent.AXIS_PRESSURE, InputDevice.SOURCE_STYLUS) != null) {
            features.add("pressure")
        }
        if (motionRange(device, MotionEvent.AXIS_TILT, InputDevice.SOURCE_STYLUS) != null) {
            features.add("tilt")
        }
        if (motionRange(device, MotionEvent.AXIS_ORIENTATION, InputDevice.SOURCE_STYLUS) != null) {
            features.add("orientation")
        }
        if (motionRange(device, MotionEvent.AXIS_DISTANCE, InputDevice.SOURCE_STYLUS) != null) {
            features.add("distance")
            features.add("hover")
        }
        if (motionRange(device, MotionEvent.AXIS_RZ, InputDevice.SOURCE_STYLUS) != null) {
            features.add("barrelRotation")
        }
        return buildMap {
            put("kind", "tablet")
            put("nativeDeviceIdentifier", nativeDeviceIdentifier(device, device.id))
            put("name", device.name)
            if (device.vendorId != 0) {
                put("vendorIdentifier", device.vendorId)
            }
            if (device.productId != 0) {
                put("productIdentifier", device.productId)
            }
            put("features", features)
        }
    }

    /** Emits a device event by combining cached [description] with lifecycle data. */
    private fun emitDevice(
        description: Map<String, Any>,
        phase: String,
    ) {
        val packet = description.toMutableMap()
        packet["type"] = "device"
        packet["timestampMicros"] = SystemClock.uptimeMillis() * 1000
        packet["phase"] = phase
        eventSink?.success(packet)
    }

    /** Whether [device] advertises Android's standard stylus input source. */
    private fun isStylusDevice(device: InputDevice): Boolean =
        (device.sources and InputDevice.SOURCE_STYLUS) == InputDevice.SOURCE_STYLUS

    /** Emits every chronological stylus sample contained in [event]. */
    private fun handleMotionEvent(event: MotionEvent) {
        if (eventSink == null) {
            return
        }
        val phase = phaseFor(event) ?: return
        val allStylusPointerIndices = (0 until event.pointerCount).filter { pointerIndex -> isStylus(event, pointerIndex) }
        if (allStylusPointerIndices.isEmpty()) {
            return
        }
        val prediction = recordAndPredict(event)
        val pointerIndices = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN,
            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_DOWN,
            MotionEvent.ACTION_POINTER_UP,
            -> listOf(event.actionIndex)

            else -> (0 until event.pointerCount).toList()
        }.filter { pointerIndex -> isStylus(event, pointerIndex) }
        if (pointerIndices.isEmpty()) {
            try {
                emitPredictions(
                    referenceEvent = event,
                    pointerIndices = allStylusPointerIndices,
                    phase = phase,
                    prediction = prediction,
                )
            } finally {
                prediction?.recycle()
            }
            return
        }

        val previousPositions = pointerIndices.associate { pointerIndex ->
            val pointerIdentifier = event.getPointerId(pointerIndex)
            pointerIdentifier to lastPositions[pointerIdentifier]
        }.toMutableMap()
        val packets = mutableListOf<Map<String, Any>>()
        if ((phase == "move" || phase == "hover") && event.historySize > 0) {
            for (historyPosition in 0 until event.historySize) {
                for (pointerIndex in pointerIndices) {
                    val pointerIdentifier = event.getPointerId(pointerIndex)
                    val built = buildMotionPacket(
                        event = event,
                        pointerIndex = pointerIndex,
                        phase = phase,
                        previousPosition = previousPositions[pointerIdentifier],
                        historyPosition = historyPosition,
                    )
                    packets.add(built.value)
                    previousPositions[pointerIdentifier] = built.position
                }
            }
        }
        for (pointerIndex in pointerIndices) {
            val pointerIdentifier = event.getPointerId(pointerIndex)
            val built = buildMotionPacket(
                event = event,
                pointerIndex = pointerIndex,
                phase = phase,
                previousPosition = previousPositions[pointerIdentifier],
            )
            packets.add(built.value)
            previousPositions[pointerIdentifier] = built.position
        }
        emitPackets(packets)

        for (pointerIndex in pointerIndices) {
            val pointerIdentifier = event.getPointerId(pointerIndex)
            if (phase == "up" || phase == "cancel" || phase == "removed") {
                lastPositions.remove(pointerIdentifier)
            } else {
                previousPositions[pointerIdentifier]?.let { position -> lastPositions[pointerIdentifier] = position }
            }
        }

        try {
            emitPredictions(
                referenceEvent = event,
                pointerIndices = allStylusPointerIndices,
                phase = phase,
                prediction = prediction,
            )
        } finally {
            prediction?.recycle()
        }
    }

    /** Builds one normalized motion packet at a current or historical position. */
    private fun buildMotionPacket(
        event: MotionEvent,
        pointerIndex: Int,
        phase: String,
        previousPosition: Pair<Double, Double>?,
        historyPosition: Int? = null,
    ): BuiltMotionPacket {
        val density = flutterView?.resources?.displayMetrics?.density?.toDouble() ?: 1.0
        val pointerIdentifier = event.getPointerId(pointerIndex)
        val x = sampleAxisValue(event, MotionEvent.AXIS_X, pointerIndex, historyPosition) / density
        val y = sampleAxisValue(event, MotionEvent.AXIS_Y, pointerIndex, historyPosition) / density
        val isDown = phase == "down" || phase == "move"
        val device = event.device
        val pressureRange = motionRange(device, MotionEvent.AXIS_PRESSURE, event.source)
        val tiltRange = motionRange(device, MotionEvent.AXIS_TILT, event.source)
        val orientationRange = motionRange(device, MotionEvent.AXIS_ORIENTATION, event.source)
        val distanceRange = motionRange(device, MotionEvent.AXIS_DISTANCE, event.source)
        val rotationRange = motionRange(device, MotionEvent.AXIS_RZ, event.source)
        val features = mutableListOf("primaryButton", "secondaryButton", "eraser", "hover")
        val packet = mutableMapOf<String, Any>(
            "type" to "motion",
            "timestampMicros" to sampleTimeMicros(event, historyPosition),
            "phase" to phase,
            "tool" to if (event.getToolType(pointerIndex) == MotionEvent.TOOL_TYPE_ERASER) "eraser" else "pen",
            "pointerIdentifier" to pointerIdentifier,
            "deviceIdentifier" to event.deviceId,
            "nativeDeviceIdentifier" to nativeDeviceIdentifier(device, event.deviceId),
            "x" to x,
            "y" to y,
            "deltaX" to (x - (previousPosition?.first ?: x)),
            "deltaY" to (y - (previousPosition?.second ?: y)),
            "buttons" to flutterButtons(event, isDown),
            "isDown" to isDown,
        )
        if (pressureRange != null) {
            features.add("pressure")
            packet["pressure"] = sampleAxisValue(event, MotionEvent.AXIS_PRESSURE, pointerIndex, historyPosition)
            packet["pressureMinimum"] = pressureRange.min.toDouble()
            packet["pressureMaximum"] = pressureRange.max.toDouble()
        }
        if (tiltRange != null) {
            features.add("tilt")
            packet["tilt"] = sampleAxisValue(event, MotionEvent.AXIS_TILT, pointerIndex, historyPosition)
        }
        if (orientationRange != null) {
            features.add("orientation")
            packet["orientation"] = sampleAxisValue(event, MotionEvent.AXIS_ORIENTATION, pointerIndex, historyPosition)
        }
        if (distanceRange != null) {
            features.add("distance")
            packet["distance"] = sampleAxisValue(event, MotionEvent.AXIS_DISTANCE, pointerIndex, historyPosition)
            packet["distanceMaximum"] = distanceRange.max.toDouble()
        }
        if (rotationRange != null) {
            features.add("barrelRotation")
            packet["barrelRotation"] = rotationInRadians(
                sampleAxisValue(event, MotionEvent.AXIS_RZ, pointerIndex, historyPosition),
                rotationRange,
            )
        }
        packet["features"] = features
        return BuiltMotionPacket(packet, Pair(x, y))
    }

    /** Sends [packets] atomically while preserving chronological ordering. */
    private fun emitPackets(packets: List<Map<String, Any>>) {
        when (packets.size) {
            0 -> return
            1 -> eventSink?.success(packets.single())
            else -> eventSink?.success(mapOf("type" to "batch", "events" to packets))
        }
    }

    /** Records [event] and asks AndroidX for a trajectory targeting the next frame. */
    private fun recordAndPredict(event: MotionEvent): MotionEvent? {
        val predictor = motionEventPredictor ?: return null
        return try {
            predictor.record(event)
            predictor.predict()
        } catch (_: IllegalArgumentException) {
            val view = flutterView ?: return null
            motionEventPredictor = MotionEventPredictor.newInstance(view)
            null
        }
    }

    /** Emits a replaceable prediction, including an empty packet when it expires. */
    private fun emitPredictions(
        referenceEvent: MotionEvent,
        pointerIndices: List<Int>,
        phase: String,
        prediction: MotionEvent?,
    ) {
        for (referencePointerIndex in pointerIndices) {
            val pointerIdentifier = referenceEvent.getPointerId(referencePointerIndex)
            val predictionPointerIndex = prediction?.findPointerIndex(pointerIdentifier) ?: -1
            val samples = mutableListOf<Map<String, Any>>()
            val pointerPhase = predictionPhaseFor(referenceEvent, referencePointerIndex, phase)
            if (prediction != null && predictionPointerIndex >= 0 && pointerPhase != "up" && pointerPhase != "cancel" && pointerPhase != "removed") {
                val predictedPhase = if (pointerPhase == "added" || pointerPhase == "hover") "hover" else "move"
                var previousPosition = lastPositions[pointerIdentifier]
                for (historyPosition in 0 until prediction.historySize) {
                    val built = buildMotionPacket(
                        event = prediction,
                        pointerIndex = predictionPointerIndex,
                        phase = predictedPhase,
                        previousPosition = previousPosition,
                        historyPosition = historyPosition,
                    )
                    samples.add(built.value)
                    previousPosition = built.position
                }
                val built = buildMotionPacket(
                    event = prediction,
                    pointerIndex = predictionPointerIndex,
                    phase = predictedPhase,
                    previousPosition = previousPosition,
                )
                samples.add(built.value)
            }
            eventSink?.success(
                mapOf(
                    "type" to "prediction",
                    "timestampMicros" to sampleTimeMicros(referenceEvent),
                    "pointerIdentifier" to pointerIdentifier,
                    "deviceIdentifier" to referenceEvent.deviceId,
                    "nativeDeviceIdentifier" to nativeDeviceIdentifier(referenceEvent.device, referenceEvent.deviceId),
                    "samples" to samples,
                ),
            )
        }
    }

    /** Resolves the phase of one stylus when another pointer caused the action. */
    private fun predictionPhaseFor(
        event: MotionEvent,
        pointerIndex: Int,
        eventPhase: String,
    ): String {
        val actionTargetsOnePointer = event.actionMasked == MotionEvent.ACTION_POINTER_DOWN || event.actionMasked == MotionEvent.ACTION_POINTER_UP
        if (!actionTargetsOnePointer || pointerIndex == event.actionIndex) {
            return eventPhase
        }
        return if (event.getPressure(pointerIndex) > 0) "move" else "hover"
    }

    /** Maps an Android action to Stylet's pointer lifecycle vocabulary. */
    private fun phaseFor(event: MotionEvent): String? = when (event.actionMasked) {
        MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> "down"
        MotionEvent.ACTION_MOVE -> "move"
        MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> if (isPalmCancellation(event)) "cancel" else "up"
        MotionEvent.ACTION_CANCEL -> "cancel"
        MotionEvent.ACTION_HOVER_ENTER -> "added"
        MotionEvent.ACTION_HOVER_MOVE -> "hover"
        MotionEvent.ACTION_HOVER_EXIT -> "removed"
        MotionEvent.ACTION_BUTTON_PRESS, MotionEvent.ACTION_BUTTON_RELEASE -> if (event.pressure > 0) "move" else "hover"
        else -> null
    }

    /** Whether Android marked this pointer transition as rejected palm input. */
    private fun isPalmCancellation(event: MotionEvent): Boolean =
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && event.flags and MotionEvent.FLAG_CANCELED != 0

    /** Whether one pointer in [event] is a regular or inverted stylus. */
    private fun isStylus(
        event: MotionEvent,
        pointerIndex: Int,
    ): Boolean = event.getToolType(pointerIndex) == MotionEvent.TOOL_TYPE_STYLUS || event.getToolType(pointerIndex) == MotionEvent.TOOL_TYPE_ERASER

    /** Converts Android button state into Flutter's public button bit field. */
    private fun flutterButtons(
        event: MotionEvent,
        isDown: Boolean,
    ): Int {
        var buttons = if (isDown) FLUTTER_PRIMARY_BUTTON else 0
        if (event.buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY != 0) {
            buttons = buttons or FLUTTER_PRIMARY_STYLUS_BUTTON
        }
        if (event.buttonState and MotionEvent.BUTTON_STYLUS_SECONDARY != 0) {
            buttons = buttons or FLUTTER_SECONDARY_STYLUS_BUTTON
        }
        return buttons
    }

    /** Reads one current or historical axis value from [event]. */
    private fun sampleAxisValue(
        event: MotionEvent,
        axis: Int,
        pointerIndex: Int,
        historyPosition: Int?,
    ): Double = if (historyPosition == null) {
        event.getAxisValue(axis, pointerIndex).toDouble()
    } else {
        event.getHistoricalAxisValue(axis, pointerIndex, historyPosition).toDouble()
    }

    /** Returns a current or historical event timestamp with the best available precision. */
    private fun sampleTimeMicros(
        event: MotionEvent,
        historyPosition: Int? = null,
    ): Long {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            val nanoseconds = if (historyPosition == null) event.eventTimeNanos else event.getHistoricalEventTimeNanos(historyPosition)
            return nanoseconds / 1000
        }
        val milliseconds = if (historyPosition == null) event.eventTime else event.getHistoricalEventTime(historyPosition)
        return milliseconds * 1000
    }

    /** Returns the stable descriptor used by device and motion packets. */
    private fun nativeDeviceIdentifier(
        device: InputDevice?,
        deviceId: Int,
    ): String = device?.descriptor?.takeIf { descriptor -> descriptor.isNotBlank() } ?: "android-device:$deviceId"

    /** Returns the device range for [axis] and [source] when advertised. */
    private fun motionRange(
        device: InputDevice?,
        axis: Int,
        source: Int,
    ): InputDevice.MotionRange? = device?.getMotionRange(axis, source) ?: device?.getMotionRange(axis)

    /** Converts a device-specific Z-rotation axis into clockwise radians. */
    private fun rotationInRadians(
        value: Double,
        range: InputDevice.MotionRange,
    ): Double {
        val minimum = range.min.toDouble()
        val maximum = range.max.toDouble()
        if (maxOf(abs(minimum), abs(maximum)) > 2 * PI) {
            return value * PI / 180.0
        }
        if (minimum >= 0.0 && maximum <= 1.0) {
            return value * 2 * PI
        }
        if (minimum >= -1.0 && maximum <= 1.0) {
            return value * PI
        }
        return value
    }
}
