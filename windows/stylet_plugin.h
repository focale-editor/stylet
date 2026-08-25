#ifndef FLUTTER_PLUGIN_STYLET_PLUGIN_H_
#define FLUTTER_PLUGIN_STYLET_PLUGIN_H_

#include <windows.h>

#include <flutter/encodable_value.h>
#include <flutter/event_channel.h>
#include <flutter/event_sink.h>
#include <flutter/method_call.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace stylet {

/** Forward declaration of the optional dynamically loaded Wintab bridge. */
class WintabBackend;

/** Collects high-fidelity WM_POINTER pen data for the Dart Stylet API. */
class StyletPlugin : public flutter::Plugin {
 private:
  /** Registrar used to remove the window procedure delegate during teardown. */
  flutter::PluginRegistrarWindows* registrar_;

  /** Native Flutter view window used for client-coordinate conversion. */
  HWND view_window_ = nullptr;

  /** Identifier returned for the registered top-level window procedure. */
  int window_proc_delegate_id_ = -1;

  /** Request-response channel retained for the plugin lifetime. */
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      method_channel_;

  /** Continuous input channel retained for the plugin lifetime. */
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>> event_channel_;

  /** Current Dart event sink, or null while nobody listens. */
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;

  /** Latest logical position for each Windows pointer identifier. */
  std::unordered_map<UINT32, std::pair<double, double>> last_positions_;

  /** Native pointer devices already announced to the current Dart listener. */
  std::unordered_set<std::string> announced_devices_;

  /** Optional Wintab bridge used for driver-only pen axes. */
  std::unique_ptr<WintabBackend> wintab_backend_;

 public:
  /** Creates a plugin associated with a live Windows registrar. */
  explicit StyletPlugin(flutter::PluginRegistrarWindows* registrar);

  /** Unregisters native input observation and releases platform channels. */
  ~StyletPlugin() override;

  /** Prevents copying a plugin that owns channel and registrar handles. */
  StyletPlugin(const StyletPlugin&) = delete;

  /** Prevents assigning a plugin that owns channel and registrar handles. */
  StyletPlugin& operator=(const StyletPlugin&) = delete;

  /** Registers Stylet's channels and passive WM_POINTER observer. */
  static void RegisterWithRegistrar(
      flutter::PluginRegistrarWindows* registrar);

  /** Returns every advanced feature implemented by the Windows backend. */
  static flutter::EncodableList GetCapabilities();

 private:
  /** Handles one request arriving on the Dart method channel. */
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  /** Observes one top-level window message without consuming it. */
  std::optional<LRESULT> HandleWindowMessage(HWND window, UINT message,
                                             WPARAM wparam, LPARAM lparam);

  /** Retrieves every hardware sample coalesced into the current pointer message. */
  std::vector<POINTER_PEN_INFO> ReadPenHistory(UINT32 pointer_id) const;

  /** Announces a Windows pointer device once per Dart stream subscription. */
  void AnnounceDevice(const POINTER_PEN_INFO& pen_info);

  /** Converts and emits every sample represented by one Windows pointer message. */
  void EmitPenEvents(HWND window, UINT message, UINT32 pointer_id);

  /** Converts one Windows pen sample into a standard-codec packet. */
  flutter::EncodableMap BuildPenPacket(HWND window, UINT message,
                                       UINT32 pointer_id,
                                       const POINTER_PEN_INFO& pen_info);

  /** Returns capabilities adjusted to the optional drivers available now. */
  flutter::EncodableList GetCurrentCapabilities() const;
};

}  // namespace stylet

#endif  // FLUTTER_PLUGIN_STYLET_PLUGIN_H_
