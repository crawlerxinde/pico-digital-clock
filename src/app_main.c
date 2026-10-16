/*****************************************************************************
* | File     :   app_main.c
* | Function :   pico-clock 入口：初始化外设 / LVGL / 音频，进入主循环
******************************************************************************/
#include "app.h"
#include "LCD_3in49.h"
#include "qspi_pio.h"
#include "QMI8658.h"
#include "Touch.h"
#include "PCF85063A.h"

int app_main(void)
{
    if (DEV_Module_Init() != 0) {
        return -1;
    }

    printf("pico-clock start\r\n");

    /* QSPI + LCD */
    QSPI_GPIO_Init(qspi);
    QSPI_PIO_Init(qspi);
    QSPI_4Wrie_Mode(&qspi);
    LCD_3IN49_Init();
    DEV_SET_PWM(60);
    LCD_3IN49_Clear(WHITE);

    /* 外设 */
    PCF85063A_Init();
    QMI8658_init();

    /* 界面 + 音频 */
    ClockUI_Init();
    ClockUI_Build();
    App_Init();

    while (1) {
        lv_task_handler();
        App_Poll();
        App_Update();
        DEV_Delay_ms(5);
    }

    DEV_Module_Exit();
    return 0;
}
