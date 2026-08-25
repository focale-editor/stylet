# Native backends and optional dependencies

Stylet keeps its Dart dependency graph portable. Native integrations use
operating-system APIs, a linked portable support library, build-time discovery,
or a tablet driver loaded at runtime. An unavailable optional backend never
prevents the Flutter application from starting.

## Android

The Android SDK supplies motion history and device metadata directly. Stylet
expands every `MotionEvent` history entry, preserving its axes and timestamp,
before the current sample. API level 34 and newer use nanosecond timestamps;
older systems retain Android's millisecond precision. `InputManager` reports
stylus-capable devices already connected and every later add, change, or remove
notification. Android 13's cancelled-pointer flag is translated into a cancel
phase whenever it applies to an emitted stylus pointer.

Stylet links `androidx.input:input-motionprediction:1.0.0`. One predictor is
associated with the active Flutter view and emits replaceable future
trajectories through `Stylet.predictions`. AndroidX uses the platform predictor
where supported and falls back to its own implementation on older or
unsupported devices. Prediction failures never interrupt authoritative motion.

AndroidX Ink and Graphics Core are intentionally not dependencies: they own a
native ink renderer rather than expose additional stylus measurements. A
consumer such as Focale can adopt them independently if it delegates canvas
rendering to Android. Samsung S Pen Remote is likewise outside the core plugin
because its Air Actions are vendor-specific remote commands, not portable
pointer samples.

## iOS and iPadOS

UIKit supplies coalesced Apple Pencil samples and predicted touches without an
external dependency. Stylet sends coalesced samples as authoritative motion and
publishes each predicted trajectory separately so the next callback can replace
it. Values marked as estimated retain UIKit's `estimationUpdateIndex`; later
`touchesEstimatedPropertiesUpdated` callbacks become correction packets for
pressure, location, altitude, azimuth, and Apple Pencil Pro roll.

PencilKit remains outside Stylet because it provides its own canvas, stroke
model, tools, and renderer. Applications can use PencilKit independently when a
native drawing surface is desirable without changing the raw-input contract.

## Linux

GTK 3 remains the universal fallback. It supplies pressure, tilt, distance,
rotation, slider axes, device USB identifiers, and tablet-pad events when the
active GDK backend exposes them. On X11, GTK obtains this information through
XInput 2, so Stylet does not need to open a second X connection or link
directly to `libXi`.

When `pkg-config` finds Wayland client 1.20 or newer and GDK's Wayland
integration, the Linux build also compiles a direct implementation of
`zwp_tablet_manager_v2`. The protocol source generated from the official
tablet-v2 XML is included in the package, so `wayland-protocols` is not a
consumer build dependency. At runtime Stylet binds the protocol only when the
compositor advertises it for the Flutter view's seat. This path adds:

- per-tablet, per-tool, and per-pad connection metadata;
- complete frame boundaries and per-tool capabilities;
- pressure, distance, tilt, barrel rotation, airbrush slider, and stylus wheel;
- pad buttons, touch rings, touch strips, relative dials, and mapping modes.

Tablet-v2 version 2 is negotiated when available for dial events; version 1
compositors retain every earlier feature.

Wayland tablet-v2, XInput 2, and GTK expose measured samples but no prediction
primitive. A future software predictor can use Stylet's prediction packet
without changing the Linux protocol backend.

If the compositor omits tablet-v2, or if the Flutter view is running under
X11, Stylet transparently continues with GTK. `libinput` is deliberately not
used directly: compositors own libinput devices and deliver the supported
application-facing protocol.

`libwacom` is useful to settings applications that need a model database, but
it is not required for event capture. Stylet prefers identifiers and control
descriptions reported live by Wayland or GTK, avoiding a model database that
could disagree with the active driver.

Typical Fedora build dependency:

```console
sudo dnf install wayland-devel gtk3-devel
```

Typical Debian or Ubuntu build dependency:

```console
sudo apt install libwayland-dev libgtk-3-dev
```

The plugin still builds without the Wayland development package; only the
direct tablet-v2 path is omitted. Distribution builds can force the GTK/GDK
fallback with the CMake option `-DSTYLET_ENABLE_WAYLAND=OFF`.

## Web

The federated Web backend reads the browser's Pointer Events directly and
constructs the same immutable Dart events as the native channel decoder. It
adds values that Flutter Web does not currently preserve completely, including
both tilt components, precise altitude/azimuth orientation, twist, tangential
pressure, eraser state, and the barrel button. Fractional CSS-pixel coordinates
are retained.

When available, `getCoalescedEvents()` supplies the complete measured history.
Current Flutter Web versions already expand these events into the normal
pointer pipeline; Stylet also exposes them through `nativeMotions` so the extra
axes can be correlated sample by sample. In a secure context supporting
`pointerrawupdate`, Stylet uses that event for the highest-rate measured stream
and retains `pointermove` only for predictions, avoiding duplicate motion.

`getPredictedEvents()` becomes replaceable `Stylet.predictions` trajectories.
The Pointer Events specification guarantees predicted coordinates, timestamps,
ordering, and pointer identity, but not future pressure or orientation. Stylet
therefore carries the last real pen state across the future positions rather
than interpreting browser defaults as predicted sensor values.

Every optional member is detected on the live browser prototype. Missing,
insecure, or partially implemented APIs fall back to ordinary `pointermove`
and Flutter pointer data. The emerging Pointer Events Level 4
`persistentDeviceId` is preserved when non-zero. It identifies a pointing
device only within the current browsing session and can appear after the first
sample; it does not expose a product name or connection lifecycle. Pointer
Events expose no tablet-pad controls or stylus body gestures.

The experimental Ink API is supported separately through `StyletInkTrail`. It
delegates a short visual trail to the browser compositor and does not produce
input samples. It remains opt-in because applications must choose the brush
color, diameter, and presentation area. Usage and limitations are documented
in [`web_support.md`](web_support.md).

## Windows

Windows Ink (`WM_POINTER`) is always the primary backend and requires no
redistributable library. Stylet calls `GetPointerPenInfoHistory` so coalesced
hardware samples are sent to Dart in chronological batches instead of keeping
only the newest point. `GetPointerDevice` supplies the driver product name.

Some Wacom airbrush tools expose tangential pressure only through Wintab. Stylet
therefore attempts to load `Wintab32.dll` from the Windows system directory,
opens a private message context, and correlates its timestamps with Windows Ink
packets. The matched value enriches the existing packet, so clients do not
receive duplicate pointer movements. There is no link-time Wintab dependency
and Stylet does not redistribute the DLL. Without an installed compatible
driver, Windows Ink continues unchanged.

Windows App SDK exposes `Microsoft.UI.Input.PointerPredictor`. Stylet can attach
it to Flutter's ordinary Win32 `HWND` with the experimental
`InputPointerSource.GetForWindowId` bridge. Because that bridge and its runtime
are not stable, the integration is disabled by default and never affects the
normal `WM_POINTER`/Wintab build.

To opt in, use CMake 3.31 or newer and set the option before Flutter includes
its generated plugins in the application's `windows/CMakeLists.txt`:

```cmake
set(STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION ON CACHE BOOL "" FORCE)
include(flutter/generated_plugins.cmake)
```

Stylet then restores pinned `Microsoft.WindowsAppSDK.Foundation`
2.3.7-experimental, `Microsoft.WindowsAppSDK.Runtime`
2.3.2-experimentalA, and C++/WinRT packages from NuGet. Their complete resolved
graph and content hashes are checked by `stylet-packages.lock.json`. The
matching experimental Windows App Runtime must also be installed on the target
machine. Stylet bundles the small bootstrap DLL, initializes the framework at
runtime, and advertises `predictedSamples` only if initialization and the HWND
bridge both succeed. Otherwise authoritative Windows Ink input continues
unchanged without showing an installer dialog or terminating the application.

## macOS

AppKit's tablet `NSEvent` API already exposes pressure, tilt, rotation,
tangential pressure, tool identity, and USB vendor/model identifiers. Stylet
temporarily disables AppKit mouse-event coalescing while Dart listens, then
restores the application's previous setting. This favors complete drawing
samples without permanently changing global application behavior.

The Wacom Driver Request Interface can override ExpressKeys, Touch Rings, and
Touch Strips, but doing so replaces the user's per-application driver mappings
and can trigger Automation permissions. Stylet therefore does not claim those
controls implicitly. Standard pen data still benefits from an installed Wacom
driver through AppKit.

AppKit publishes measured tablet events but has no predicted-event API
corresponding to UIKit's `predictedTouches(for:)`.

## Upstream references

- [Wayland tablet-v2 protocol](https://cgit.freedesktop.org/wayland/wayland-protocols/tree/stable/tablet/tablet-v2.xml)
- [Windows pointer history](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getpointerpeninfohistory)
- [Windows App SDK pointer predictor](https://learn.microsoft.com/windows/windows-app-sdk/api/winrt/microsoft.ui.input.pointerpredictor)
- [Experimental HWND input-source bridge](https://learn.microsoft.com/windows/windows-app-sdk/api/winrt/microsoft.ui.input.inputpointersource.getforwindowid)
- [Windows App SDK downloads](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads)
- [Wacom Wintab reference](https://developer-docs.wacom.com/docs/icbt/windows/wintab/wintab-reference/)
- [AppKit tablet events](https://developer.apple.com/documentation/appkit/nsevent)
- [AppKit mouse and tablet coalescing](https://developer.apple.com/documentation/appkit/nsevent/ismousecoalescingenabled)
- [Android motion history](https://developer.android.com/reference/android/view/MotionEvent)
- [AndroidX motion prediction](https://developer.android.com/jetpack/androidx/releases/input)
- [Android input devices](https://developer.android.com/reference/android/hardware/input/InputManager)
- [Android stylus palm rejection](https://developer.android.com/develop/adaptive-apps/cookbook/stylus-palm-rejection)
- [Apple Pencil input](https://developer.apple.com/documentation/uikit/handling-input-from-apple-pencil)
- [UIKit predicted touches](https://developer.apple.com/documentation/uikit/uievent/predictedtouches%28for%3A%29)
- [Samsung S Pen Remote SDK](https://developer.samsung.com/galaxy-spen-remote/overview.html)
- [Pointer Events](https://www.w3.org/TR/pointerevents3/)
- [Pointer Events Level 4](https://www.w3.org/TR/pointerevents4/)
- [Web Ink API](https://developer.mozilla.org/docs/Web/API/Ink_API)
