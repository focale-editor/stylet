package dev.focale.stylet

import android.app.Activity
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import io.flutter.embedding.android.FlutterView
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.embedding.engine.plugins.activity.ActivityAware
import io.flutter.embedding.engine.plugins.activity.ActivityPluginBinding
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import kotlin.math.PI
import kotlin.math.abs

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

    override fun onAttachedToEngine(flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
        methodChannel = MethodChannel(flutterPluginBinding.binaryMessenger, METHOD_CHANNEL_NAME).also { channel -> channel.setMethodCallHandler(this) }
        eventChannel = EventChannel(flutterPluginBinding.binaryMessenger, EVENT_CHANNEL_NAME).also { channel -> channel.setStreamHandler(this) }
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        detachFromFlutterView()
        methodChannel?.setMethodCallHandler(null)
        eventChannel?.setStreamHandler(null)
        methodChannel = null
        eventChannel = null
        eventSink = null
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
    }

    override fun onCancel(arguments: Any?) {
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

    /** Emits every stylus pointer contained in [event]. */
    private fun handleMotionEvent(event: MotionEvent) {
        if (eventSink == null) {
            return
        }
        val phase = phaseFor(event) ?: return
        val indices = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN,
            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_DOWN,
            MotionEvent.ACTION_POINTER_UP,
            -> listOf(event.actionIndex)

            else -> (0 until event.pointerCount).toList()
        }
        for (pointerIndex in indices) {
            if (isStylus(event, pointerIndex)) {
                emitPointer(event, pointerIndex, phase)
            }
        }
    }

    /** Builds and sends one normalized platform-channel motion packet. */
    private fun emitPointer(
        event: MotionEvent,
        pointerIndex: Int,
        phase: String,
    ) {
        val density = flutterView?.resources?.displayMetrics?.density?.toDouble() ?: 1.0
        val pointerIdentifier = event.getPointerId(pointerIndex)
        val x = event.getX(pointerIndex).toDouble() / density
        val y = event.getY(pointerIndex).toDouble() / density
        val previous = lastPositions[pointerIdentifier]
        val isDown = phase == "down" || phase == "move"
        val device = event.device
        val pressureRange = motionRange(device, MotionEvent.AXIS_PRESSURE)
        val distanceRange = motionRange(device, MotionEvent.AXIS_DISTANCE)
        val rotationRange = motionRange(device, MotionEvent.AXIS_RZ)
        val features = mutableListOf("pressure", "tilt", "orientation", "primaryButton", "secondaryButton", "eraser", "hover")
        val packet = mutableMapOf<String, Any>(
            "type" to "motion",
            "timestampMicros" to event.eventTime * 1000,
            "phase" to phase,
            "tool" to if (event.getToolType(pointerIndex) == MotionEvent.TOOL_TYPE_ERASER) "eraser" else "pen",
            "pointerIdentifier" to pointerIdentifier,
            "deviceIdentifier" to event.deviceId,
            "nativeDeviceIdentifier" to (device?.descriptor ?: event.deviceId.toString()),
            "x" to x,
            "y" to y,
            "deltaX" to (x - (previous?.first ?: x)),
            "deltaY" to (y - (previous?.second ?: y)),
            "buttons" to flutterButtons(event, isDown),
            "isDown" to isDown,
            "pressure" to event.getPressure(pointerIndex).toDouble(),
            "pressureMinimum" to (pressureRange?.min?.toDouble() ?: 0.0),
            "pressureMaximum" to (pressureRange?.max?.toDouble() ?: 1.0),
            "tilt" to event.getAxisValue(MotionEvent.AXIS_TILT, pointerIndex).toDouble(),
            "orientation" to event.getOrientation(pointerIndex).toDouble(),
        )
        if (distanceRange != null) {
            features.add("distance")
            packet["distance"] = event.getAxisValue(MotionEvent.AXIS_DISTANCE, pointerIndex).toDouble()
            packet["distanceMaximum"] = distanceRange.max.toDouble()
        }
        if (rotationRange != null) {
            features.add("barrelRotation")
            packet["barrelRotation"] = rotationInRadians(event.getAxisValue(MotionEvent.AXIS_RZ, pointerIndex).toDouble(), rotationRange)
        }
        packet["features"] = features
        eventSink?.success(packet)

        if (phase == "up" || phase == "cancel" || phase == "removed") {
            lastPositions.remove(pointerIdentifier)
        } else {
            lastPositions[pointerIdentifier] = Pair(x, y)
        }
    }

    /** Maps an Android action to Stylet's pointer lifecycle vocabulary. */
    private fun phaseFor(event: MotionEvent): String? = when (event.actionMasked) {
        MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> "down"
        MotionEvent.ACTION_MOVE -> "move"
        MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> "up"
        MotionEvent.ACTION_CANCEL -> "cancel"
        MotionEvent.ACTION_HOVER_ENTER -> "added"
        MotionEvent.ACTION_HOVER_MOVE -> "hover"
        MotionEvent.ACTION_HOVER_EXIT -> "removed"
        MotionEvent.ACTION_BUTTON_PRESS, MotionEvent.ACTION_BUTTON_RELEASE -> if (event.pressure > 0) "move" else "hover"
        else -> null
    }

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

    /** Returns the device range for [axis] when the driver advertises one. */
    private fun motionRange(
        device: InputDevice?,
        axis: Int,
    ): InputDevice.MotionRange? = device?.getMotionRange(axis)

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
