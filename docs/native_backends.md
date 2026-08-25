# Native backends and optional dependencies

Stylet keeps its Dart dependency graph portable. Native libraries are either
part of the operating system, discovered at build time, or loaded from an
installed tablet driver at runtime. An unavailable optional backend never
prevents the Flutter application from starting.

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

## Upstream references

- [Wayland tablet-v2 protocol](https://cgit.freedesktop.org/wayland/wayland-protocols/tree/stable/tablet/tablet-v2.xml)
- [Windows pointer history](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getpointerpeninfohistory)
- [Wacom Wintab reference](https://developer-docs.wacom.com/docs/icbt/windows/wintab/wintab-reference/)
- [AppKit tablet events](https://developer.apple.com/documentation/appkit/nsevent)
- [AppKit mouse and tablet coalescing](https://developer.apple.com/documentation/appkit/nsevent/ismousecoalescingenabled)
