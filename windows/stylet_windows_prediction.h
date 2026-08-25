#ifndef FLUTTER_PLUGIN_STYLET_WINDOWS_PREDICTION_H_
#define FLUTTER_PLUGIN_STYLET_WINDOWS_PREDICTION_H_

#include <windows.h>

#include <flutter/encodable_value.h>

#include <functional>
#include <memory>

namespace stylet {

/** Bridges the experimental Windows App SDK pointer prediction API. */
class WindowsPredictionBackend {
 public:
  /** Callback receiving one prediction packet encoded for Flutter. */
  using PacketCallback = std::function<void(flutter::EncodableMap)>;

 private:
  /** Platform implementation hidden from builds without Windows App SDK. */
  class Implementation;

  /** Optional prediction source and its Windows App Runtime lifetime. */
  std::unique_ptr<Implementation> implementation_;

 public:
  /** Tries to attach a pointer predictor to the supplied Flutter view. */
  WindowsPredictionBackend(HWND window, PacketCallback callback);

  /** Detaches prediction events and releases Windows App Runtime resources. */
  ~WindowsPredictionBackend();

  /** Prevents copying a backend that owns WinRT event registrations. */
  WindowsPredictionBackend(const WindowsPredictionBackend&) = delete;

  /** Prevents assigning a backend that owns WinRT event registrations. */
  WindowsPredictionBackend& operator=(const WindowsPredictionBackend&) =
      delete;

  /** Whether the experimental predictor was initialized successfully. */
  bool is_available() const;

  /** Enables or disables work for the current Dart stream subscription. */
  void SetListening(bool is_listening);
};

}  // namespace stylet

#endif  // FLUTTER_PLUGIN_STYLET_WINDOWS_PREDICTION_H_
