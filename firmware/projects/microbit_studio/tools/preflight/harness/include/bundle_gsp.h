// Harness stand-in for the generated bundle header: the setters app_studio calls.
#pragma once

#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_gsp_err_t gsp_studio_state_set_text(esp_gsp_handle_t gsp, const char *text);
esp_gsp_err_t gsp_studio_line1_set_text(esp_gsp_handle_t gsp, const char *text);
esp_gsp_err_t gsp_studio_line2_set_text(esp_gsp_handle_t gsp, const char *text);
esp_gsp_err_t gsp_studio_line1_set_y(esp_gsp_handle_t gsp, int32_t y);
esp_gsp_err_t gsp_studio_line2_set_y(esp_gsp_handle_t gsp, int32_t y);
esp_gsp_err_t gsp_studio_bear_set_visible(esp_gsp_handle_t gsp, bool visible);
esp_gsp_err_t gsp_studio_bear_set_y(esp_gsp_handle_t gsp, int32_t y);
esp_gsp_err_t gsp_studio_success_set_visible(esp_gsp_handle_t gsp, bool visible);
esp_gsp_err_t gsp_studio_seg1_set_visible(esp_gsp_handle_t gsp, bool visible);
esp_gsp_err_t gsp_studio_seg2_set_visible(esp_gsp_handle_t gsp, bool visible);
esp_gsp_err_t gsp_studio_seg3_set_visible(esp_gsp_handle_t gsp, bool visible);
esp_gsp_err_t gsp_studio_seg4_set_visible(esp_gsp_handle_t gsp, bool visible);
esp_gsp_err_t gsp_studio_seg5_set_visible(esp_gsp_handle_t gsp, bool visible);
esp_gsp_err_t gsp_studio_track1_set_visible(esp_gsp_handle_t gsp, bool visible);
esp_gsp_err_t gsp_studio_track2_set_visible(esp_gsp_handle_t gsp, bool visible);
esp_gsp_err_t gsp_studio_track3_set_visible(esp_gsp_handle_t gsp, bool visible);
esp_gsp_err_t gsp_studio_track4_set_visible(esp_gsp_handle_t gsp, bool visible);
esp_gsp_err_t gsp_studio_track5_set_visible(esp_gsp_handle_t gsp, bool visible);

#ifdef __cplusplus
}
#endif
