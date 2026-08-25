#ifndef FLUTTER_PLUGIN_STYLET_WINTAB_H_
#define FLUTTER_PLUGIN_STYLET_WINTAB_H_

#include <windows.h>

#include <flutter/encodable_value.h>

#include <memory>

namespace stylet {

/** Dynamically loads Wintab to enrich Windows Ink packets when available. */
class WintabBackend {
 private:
  /** Platform implementation kept out of the plugin's public native header. */
  class Implementation;

  /** Optional loaded Wintab context and its short-lived sample cache. */
  std::unique_ptr<Implementation> implementation_;

 public:
  /** Tries to load the installed tablet driver and open a private context. */
  explicit WintabBackend(HWND window);

  /** Closes the private context and unloads the driver module. */
  ~WintabBackend();

  /** Prevents copying a backend that owns a native context and DLL handle. */
  WintabBackend(const WintabBackend&) = delete;

  /** Prevents assigning a backend that owns a native context and DLL handle. */
  WintabBackend& operator=(const WintabBackend&) = delete;

  /** Whether the installed driver exposes usable tangential-pressure packets. */
  bool supports_tangential_pressure() const;

  /** Caches driver data from one Wintab window message. */
  bool HandleWindowMessage(UINT message, WPARAM wparam, LPARAM lparam);

  /** Adds matching Wintab-only values to one Windows Ink packet. */
  void EnrichPacket(DWORD timestamp_millis, flutter::EncodableMap* packet,
                    flutter::EncodableList* features);

  /** Clears samples that must not survive a Dart stream subscription. */
  void ClearSamples();
};

}  // namespace stylet

#endif  // FLUTTER_PLUGIN_STYLET_WINTAB_H_
