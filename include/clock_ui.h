/*****************************************************************************
* | File     :   clock_ui.h
* | Function :   pico-clock 界面 / 音频 / 电源键 / 显示旋转
******************************************************************************/
#ifndef _CLOCK_UI_H_
#define _CLOCK_UI_H_

#include <stdio.h>
#include "pico/stdlib.h"
#include "DEV_Config.h"
#include "LCD_3in49.h"
#include "Touch.h"
#include "QMI8658.h"
#include "PCF85063A.h"
#include "qspi_pio.h"
#include "lvgl.h"

/* 物理面板分辨率（竖屏 172x640） */
#define DISP_HOR_RES 172
#define DISP_VER_RES 640

void ClockUI_Init(void);    /* 初始化 LVGL（显示 + 触摸） */
void ClockUI_Build(void);   /* 构建界面（时钟/信息/设置三页） */
void App_Init(void);        /* 初始化音频 + core1 + RTC 校正 */
void App_Poll(void);        /* 主循环高频：电源键 / 空闲静音 / 亮度提示 */
void App_Update(void);      /* 主循环低频：刷新时钟/信息 */

#endif
