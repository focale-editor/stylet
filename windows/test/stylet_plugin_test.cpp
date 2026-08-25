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
  for (const flutter::EncodableValue& value : capabilities) {
    const std::string* feature = std::get_if<std::string>(&value);
    if (feature != nullptr && *feature == "barrelRotation") {
      found_rotation = true;
    }
  }
  EXPECT_TRUE(found_rotation);
}

}  // namespace stylet::test
