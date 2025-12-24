#ifndef LV_CONF_H
#define LV_CONF_H

/* Minimal LVGL configuration for Waveshare 3.49" (640x172) using RGB565 */

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1
#define LV_MEM_SIZE (48U * 1024U)
#define LV_USE_LOG 0
#define LV_DPI_DEF 160
#define LV_TICK_CUSTOM 0
#define LV_ENABLE_GC 0
#define LV_USE_USER_DATA 1

/* Fonts: keep a small set to reduce flash */
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_48 0
#define LV_FONT_DEFAULT &lv_font_montserrat_20

// Match LVGL canvas to the Waveshare panel
#define LV_HOR_RES_MAX 640
#define LV_VER_RES_MAX 172

/* Disable features we don't need */
#define LV_USE_GPU_STM32_DMA2D 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_FREERTOS 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MEM 0
#define LV_USE_ASSERT_STR 0
#define LV_USE_ASSERT_OBJ 0
#define LV_USE_ASSERT_STYLE 0

#endif // LV_CONF_H
