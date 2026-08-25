#include <flutter_linux/flutter_linux.h>
#include <gtest/gtest.h>

#include <cstring>

#include "stylet_plugin_private.h"

namespace stylet::test {

/** Verifies that Linux advertises its native rotation and slider axes. */
TEST(StyletPlugin, ReportsAdvancedCapabilities) {
  g_autoptr(FlMethodResponse) response = stylet_get_capabilities();
  ASSERT_NE(response, nullptr);
  ASSERT_TRUE(FL_IS_METHOD_SUCCESS_RESPONSE(response));
  FlValue* result = fl_method_success_response_get_result(
      FL_METHOD_SUCCESS_RESPONSE(response));
  ASSERT_EQ(fl_value_get_type(result), FL_VALUE_TYPE_LIST);

  gboolean found_rotation = FALSE;
  gboolean found_tangential_pressure = FALSE;
  gboolean found_device_info = FALSE;
  gboolean found_pad_buttons = FALSE;
  gboolean found_pad_ring = FALSE;
  gboolean found_pad_strip = FALSE;
#ifdef STYLET_HAS_WAYLAND
  gboolean found_wheel = FALSE;
  gboolean found_pad_dial = FALSE;
#endif
  for (size_t index = 0; index < fl_value_get_length(result); ++index) {
    const gchar* feature = fl_value_get_string(
        fl_value_get_list_value(result, index));
    found_rotation |= strcmp(feature, "barrelRotation") == 0;
    found_tangential_pressure |= strcmp(feature, "tangentialPressure") == 0;
    found_device_info |= strcmp(feature, "deviceInfo") == 0;
    found_pad_buttons |= strcmp(feature, "tabletPadButtons") == 0;
    found_pad_ring |= strcmp(feature, "tabletPadRing") == 0;
    found_pad_strip |= strcmp(feature, "tabletPadStrip") == 0;
#ifdef STYLET_HAS_WAYLAND
    found_wheel |= strcmp(feature, "wheel") == 0;
    found_pad_dial |= strcmp(feature, "tabletPadDial") == 0;
#endif
  }
  EXPECT_TRUE(found_rotation);
  EXPECT_TRUE(found_tangential_pressure);
  EXPECT_TRUE(found_device_info);
  EXPECT_TRUE(found_pad_buttons);
  EXPECT_TRUE(found_pad_ring);
  EXPECT_TRUE(found_pad_strip);
#ifdef STYLET_HAS_WAYLAND
  EXPECT_TRUE(found_wheel);
  EXPECT_TRUE(found_pad_dial);
#endif
}

}  // namespace stylet::test
