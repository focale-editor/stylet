#include "include/stylet/stylet_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "stylet_plugin.h"

/** Registers the C++ implementation through Flutter's generated C entry point. */
void StyletPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  stylet::StyletPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
