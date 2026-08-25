#include "stylet_windows_prediction.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

#if defined(STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION)
#include <MddBootstrap.h>
#include <WindowsAppSDK-VersionInfo.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Interop.h>
#endif

namespace stylet {
namespace {

#if defined(STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION)

/** Flutter button bit representing tip contact. */
constexpr int64_t kFlutterPrimaryButton = 1;

/** Flutter button bit representing the first stylus side button. */
constexpr int64_t kFlutterPrimaryStylusButton = 2;

/** Number of radians in one degree. */
constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

/** Inserts one value into an encodable map with a string key. */
void SetValue(flutter::EncodableMap* map, const char* key,
              flutter::EncodableValue value) {
  (*map)[flutter::EncodableValue(key)] = std::move(value);
}

/** Returns a stable textual identifier for a Windows pointer source device. */
std::string NativeDeviceIdentifier(HANDLE source_device) {
  std::ostringstream output;
  output << "windows:" << std::hex
         << reinterpret_cast<std::uintptr_t>(source_device);
  return output.str();
}

/** Namespace alias for the experimental Windows App SDK input API. */
namespace win_input = winrt::Microsoft::UI::Input;

/** Converts a predicted point's state into Flutter's public button bit field. */
int64_t FlutterButtons(const win_input::PointerPoint& point) {
  const win_input::PointerPointProperties properties = point.Properties();
  int64_t buttons = point.IsInContact() ? kFlutterPrimaryButton : 0;
  if (properties.IsBarrelButtonPressed()) {
    buttons |= kFlutterPrimaryStylusButton;
  }
  return buttons;
}

/** Looks up the Win32 source device associated with one pointer point. */
HANDLE SourceDevice(uint32_t pointer_id) {
  POINTER_PEN_INFO pen_info = {};
  return GetPointerPenInfo(pointer_id, &pen_info)
             ? pen_info.pointerInfo.sourceDevice
             : nullptr;
}

/** Builds one standard-codec motion map from a predicted pointer point. */
flutter::EncodableMap BuildPredictedMotion(
    const win_input::PointerPoint& point,
    const winrt::Windows::Foundation::Point& previous_position,
    HANDLE source_device) {
  const win_input::PointerPointProperties properties = point.Properties();
  const winrt::Windows::Foundation::Point position = point.Position();
  const bool is_eraser = properties.IsEraser() || properties.IsInverted();
  const double tilt_x =
      static_cast<double>(properties.XTilt()) * kDegreesToRadians;
  const double tilt_y =
      static_cast<double>(properties.YTilt()) * kDegreesToRadians;

  flutter::EncodableMap packet;
  SetValue(&packet, "type", flutter::EncodableValue("motion"));
  SetValue(&packet, "timestampMicros",
           flutter::EncodableValue(static_cast<int64_t>(point.Timestamp())));
  SetValue(&packet, "phase",
           flutter::EncodableValue(point.IsInContact() ? "move" : "hover"));
  SetValue(&packet, "tool",
           flutter::EncodableValue(is_eraser ? "eraser" : "pen"));
  SetValue(&packet, "pointerIdentifier",
           flutter::EncodableValue(static_cast<int64_t>(point.PointerId())));
  if (source_device != nullptr) {
    SetValue(&packet, "deviceIdentifier",
             flutter::EncodableValue(static_cast<int64_t>(
                 reinterpret_cast<std::intptr_t>(source_device))));
    SetValue(&packet, "nativeDeviceIdentifier",
             flutter::EncodableValue(NativeDeviceIdentifier(source_device)));
  }
  SetValue(&packet, "x",
           flutter::EncodableValue(static_cast<double>(position.X)));
  SetValue(&packet, "y",
           flutter::EncodableValue(static_cast<double>(position.Y)));
  SetValue(&packet, "deltaX",
           flutter::EncodableValue(static_cast<double>(position.X -
                                                        previous_position.X)));
  SetValue(&packet, "deltaY",
           flutter::EncodableValue(static_cast<double>(position.Y -
                                                        previous_position.Y)));
  SetValue(&packet, "buttons",
           flutter::EncodableValue(FlutterButtons(point)));
  SetValue(&packet, "isDown", flutter::EncodableValue(point.IsInContact()));
  SetValue(&packet, "pressure",
           flutter::EncodableValue(static_cast<double>(properties.Pressure())));
  SetValue(&packet, "pressureMinimum", flutter::EncodableValue(0.0));
  SetValue(&packet, "pressureMaximum", flutter::EncodableValue(1.0));
  SetValue(&packet, "tiltX", flutter::EncodableValue(tilt_x));
  SetValue(&packet, "tiltY", flutter::EncodableValue(tilt_y));
  SetValue(&packet, "tilt",
           flutter::EncodableValue(std::atan(
               std::hypot(std::tan(tilt_x), std::tan(tilt_y)))));
  SetValue(&packet, "orientation",
           flutter::EncodableValue(
               std::atan2(std::tan(tilt_x), -std::tan(tilt_y))));
  SetValue(&packet, "barrelRotation",
           flutter::EncodableValue(static_cast<double>(properties.Twist()) *
                                   kDegreesToRadians));
  SetValue(&packet, "features",
           flutter::EncodableValue(flutter::EncodableList{
               flutter::EncodableValue("pressure"),
               flutter::EncodableValue("tilt"),
               flutter::EncodableValue("orientation"),
               flutter::EncodableValue("barrelRotation"),
               flutter::EncodableValue("primaryButton"),
               flutter::EncodableValue("eraser"),
               flutter::EncodableValue("hover"),
           }));
  return packet;
}

#endif

}  // namespace

class WindowsPredictionBackend::Implementation {
 private:
  /** Callback forwarding packets to the plugin's current event sink. */
  PacketCallback callback_;

#if defined(STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION)
  /** Whether this instance successfully initialized Windows App Runtime. */
  bool bootstrap_initialized_ = false;

  /** Pointer source associated with the Flutter view window. */
  win_input::InputPointerSource pointer_source_{nullptr};

  /** Operating-system predictor associated with the pointer source. */
  win_input::PointerPredictor predictor_{nullptr};

  /** Registration token for pointer movement notifications. */
  winrt::event_token moved_token_{};

  /** Registration token for pointer release notifications. */
  winrt::event_token released_token_{};

  /** Registration token for pointer capture-loss notifications. */
  winrt::event_token capture_lost_token_{};

  /** Registration token for pointer exit notifications. */
  winrt::event_token exited_token_{};

  /** Whether the pointer-movement event handler was registered. */
  bool moved_registered_ = false;

  /** Whether the pointer-release event handler was registered. */
  bool released_registered_ = false;

  /** Whether the capture-loss event handler was registered. */
  bool capture_lost_registered_ = false;

  /** Whether the pointer-exit event handler was registered. */
  bool exited_registered_ = false;

  /** Pointer identifiers whose last emitted trajectory was non-empty. */
  std::unordered_set<uint32_t> active_predictions_;
#endif

  /** Whether the backend is ready to produce prediction packets. */
  bool available_ = false;

  /** Whether Dart currently has an active event-channel subscription. */
  bool is_listening_ = false;

 public:
  /** Initializes Windows App Runtime and attaches to the Flutter view. */
  Implementation(HWND window, PacketCallback callback)
      : callback_(std::move(callback)) {
    (void)window;
#if defined(STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION)
    if (window == nullptr) {
      return;
    }
    PACKAGE_VERSION minimum_version = {};
    minimum_version.Version = WINDOWSAPPSDK_RUNTIME_VERSION_UINT64;
    const HRESULT bootstrap_result = MddBootstrapInitialize2(
        WINDOWSAPPSDK_RELEASE_MAJORMINOR,
        WINDOWSAPPSDK_RELEASE_VERSION_TAG_W, minimum_version,
        MddBootstrapInitializeOptions_None);
    if (FAILED(bootstrap_result)) {
      return;
    }
    bootstrap_initialized_ = true;

    try {
      const winrt::Microsoft::UI::WindowId window_id =
          winrt::Microsoft::UI::GetWindowIdFromWindow(window);
      pointer_source_ = win_input::InputPointerSource::GetForWindowId(window_id);
      predictor_ =
          win_input::PointerPredictor::CreateForInputPointerSource(pointer_source_);
      moved_token_ = pointer_source_.PointerMoved(
          [this](const auto&, const win_input::PointerEventArgs& arguments) {
            HandleMoved(arguments);
          });
      moved_registered_ = true;
      released_token_ = pointer_source_.PointerReleased(
          [this](const auto&, const win_input::PointerEventArgs& arguments) {
            HandleTerminal(arguments);
          });
      released_registered_ = true;
      capture_lost_token_ = pointer_source_.PointerCaptureLost(
          [this](const auto&, const win_input::PointerEventArgs& arguments) {
            HandleTerminal(arguments);
          });
      capture_lost_registered_ = true;
      exited_token_ = pointer_source_.PointerExited(
          [this](const auto&, const win_input::PointerEventArgs& arguments) {
            HandleTerminal(arguments);
          });
      exited_registered_ = true;
      available_ = true;
    } catch (...) {
      DetachEvents();
    }
#endif
  }

  /** Releases event registrations before shutting down Windows App Runtime. */
  ~Implementation() {
#if defined(STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION)
    DetachEvents();
    if (bootstrap_initialized_) {
      MddBootstrapShutdown();
    }
#endif
  }

  /** Returns the initialization result without throwing across Flutter code. */
  bool is_available() const { return available_; }

  /** Updates stream activity and clears retained prediction bookkeeping. */
  void SetListening(bool is_listening) {
    is_listening_ = is_listening;
#if defined(STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION)
    active_predictions_.clear();
#endif
  }

 private:
#if defined(STYLET_ENABLE_EXPERIMENTAL_WINDOWS_PREDICTION)
  /** Removes registered handlers and closes the predictor if it exists. */
  void DetachEvents() noexcept {
    available_ = false;
    if (pointer_source_) {
      if (moved_registered_) {
        pointer_source_.PointerMoved(moved_token_);
        moved_registered_ = false;
      }
      if (released_registered_) {
        pointer_source_.PointerReleased(released_token_);
        released_registered_ = false;
      }
      if (capture_lost_registered_) {
        pointer_source_.PointerCaptureLost(capture_lost_token_);
        capture_lost_registered_ = false;
      }
      if (exited_registered_) {
        pointer_source_.PointerExited(exited_token_);
        exited_registered_ = false;
      }
    }
    if (predictor_) {
      try {
        predictor_.Close();
      } catch (...) {
      }
      predictor_ = nullptr;
    }
    pointer_source_ = nullptr;
    active_predictions_.clear();
  }

  /** Predicts and emits a replacement trajectory after one real pen move. */
  void HandleMoved(const win_input::PointerEventArgs& arguments) noexcept {
    if (!is_listening_) {
      return;
    }
    try {
      const win_input::PointerPoint current = arguments.CurrentPoint();
      if (current.PointerDeviceType() != win_input::PointerDeviceType::Pen) {
        return;
      }
      const uint32_t pointer_id = current.PointerId();
      const HANDLE source_device = SourceDevice(pointer_id);
      const winrt::com_array<win_input::PointerPoint> predicted_points =
          predictor_.GetPredictedPoints(current);
      if (predicted_points.empty()) {
        if (active_predictions_.erase(pointer_id) == 0) {
          return;
        }
      } else {
        active_predictions_.insert(pointer_id);
      }
      flutter::EncodableList samples;
      samples.reserve(predicted_points.size());
      winrt::Windows::Foundation::Point previous_position = current.Position();
      for (const win_input::PointerPoint& point : predicted_points) {
        samples.emplace_back(
            BuildPredictedMotion(point, previous_position, source_device));
        previous_position = point.Position();
      }
      EmitPrediction(current, source_device, std::move(samples));
    } catch (...) {
    }
  }

  /** Clears a trajectory when its pointer is released, lost, or leaves. */
  void HandleTerminal(
      const win_input::PointerEventArgs& arguments) noexcept {
    if (!is_listening_) {
      return;
    }
    try {
      const win_input::PointerPoint current = arguments.CurrentPoint();
      if (current.PointerDeviceType() != win_input::PointerDeviceType::Pen ||
          active_predictions_.erase(current.PointerId()) == 0) {
        return;
      }
      EmitPrediction(current, SourceDevice(current.PointerId()), {});
    } catch (...) {
    }
  }

  /** Encodes and forwards one complete replacement prediction packet. */
  void EmitPrediction(const win_input::PointerPoint& current,
                      HANDLE source_device,
                      flutter::EncodableList samples) {
    flutter::EncodableMap packet;
    SetValue(&packet, "type", flutter::EncodableValue("prediction"));
    SetValue(&packet, "timestampMicros",
             flutter::EncodableValue(
                 static_cast<int64_t>(current.Timestamp())));
    SetValue(&packet, "pointerIdentifier",
             flutter::EncodableValue(
                 static_cast<int64_t>(current.PointerId())));
    if (source_device != nullptr) {
      SetValue(&packet, "deviceIdentifier",
               flutter::EncodableValue(static_cast<int64_t>(
                   reinterpret_cast<std::intptr_t>(source_device))));
      SetValue(&packet, "nativeDeviceIdentifier",
               flutter::EncodableValue(NativeDeviceIdentifier(source_device)));
    }
    SetValue(&packet, "samples", flutter::EncodableValue(samples));
    callback_(std::move(packet));
  }
#endif
};

WindowsPredictionBackend::WindowsPredictionBackend(HWND window,
                                                   PacketCallback callback)
    : implementation_(
          std::make_unique<Implementation>(window, std::move(callback))) {}

WindowsPredictionBackend::~WindowsPredictionBackend() = default;

bool WindowsPredictionBackend::is_available() const {
  return implementation_->is_available();
}

void WindowsPredictionBackend::SetListening(bool is_listening) {
  implementation_->SetListening(is_listening);
}

}  // namespace stylet
