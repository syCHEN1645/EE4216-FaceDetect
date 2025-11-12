#ifndef FIREBASE_LOGGER_H
#define FIREBASE_LOGGER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Firebase logger
 * Call this after WiFi is connected
 * @return ESP_OK on success
 */
esp_err_t firebase_logger_init(void);

/**
 * @brief Log face detection event to Firebase
 * @param person_id Name/ID of detected person (NULL for unknown)
 * @param confidence Detection confidence score (0.0 to 1.0)
 * @param x Bounding box X coordinate
 * @param y Bounding box Y coordinate
 * @param w Bounding box width
 * @param h Bounding box height
 * @return ESP_OK on success
 */
esp_err_t firebase_log_detection(const char *person_id, float confidence,
                                 int x, int y, int w, int h);

/**
 * @brief Enable or disable Firebase logging
 * @param enable true to enable, false to disable
 */
void firebase_logger_enable(bool enable);

/**
 * @brief Check if Firebase logger is enabled
 * @return true if enabled, false otherwise
 */
bool firebase_logger_is_enabled(void);

#ifdef __cplusplus
}
#endif

#endif // FIREBASE_LOGGER_H
