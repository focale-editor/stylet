#include <flutter/encodable_value.h>
#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "stylet_plugin.h"

namespace stylet::test {

/** Verifies that Windows advertises native pen rotation support. */
TEST(StyletPlugin, ReportsBarrelRotationCapability) {
  const flutter::EncodableList capabilities = StyletPlugin::GetCapabilities();
  bool found_rotation = false;
  bool found_history = false;
  bool found_device_info = false;
  for (const flutter::EncodableValue& value : capabilities) {
    const std::string* feature = std::get_if<std::string>(&value);
    if (feature != nullptr && *feature == "barrelRotation") {
      found_rotation = true;
    }
    if (feature != nullptr && *feature == "historicalSamples") {
      found_history = true;
    }
    if (feature != nullptr && *feature == "deviceInfo") {
      found_device_info = true;
    }
  }
  EXPECT_TRUE(found_rotation);
  EXPECT_TRUE(found_history);
  EXPECT_TRUE(found_device_info);
}

}  // namespace stylet::test
