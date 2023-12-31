//
// Created by Magnus Nordlander on 2022-11-18.
//

#ifndef SMART_LCC_PINS_H
#define SMART_LCC_PINS_H

#define ESP_TX (0u)
#define ESP_RX (1u)
#define ESP_CTS (2u)
#define ESP_RTS (3u)

#define ESP_UART (uart0)

#define CB_TX (4u)
#define CB_RX (5u)

#define CB_UART (uart1)

#define QWIIC2_SDA (6u)
#define QWIIC2_SCL (7u)

#define QWIIC1_SDA (8u)
#define QWIIC1_SCL (9u)

#ifdef HARDWARE_REVISION_OPENLCC_R1A
#define SETTINGS_FLASH_SCLK (10u)
#define SETTINGS_FLASH_MOSI (11u)
#define SETTINGS_FLASH_MISO (12u)
#define SETTINGS_FLASH_CS (13u)

#define SD_DET_A (14u)
#define SD_DET_B (15u)

#define SERIAL_BOOT (16u)
#endif

#if defined(HARDWARE_REVISION_OPENLCC_R2A) || defined(HARDWARE_REVISION_OPENLCC_R2B)
#define SETTINGS_FLASH_SCLK (10u)
#define SETTINGS_FLASH_MOSI (11u)
#define SETTINGS_FLASH_MISO (12u)
#define SETTINGS_FLASH_WP_D2 (13u)
#define SETTINGS_FLASH_RES_D3 (14u)
#define SETTINGS_FLASH_CS (17u)

#ifdef HARDWARE_REVISION_OPENLCC_R2A
#define SD_DET_A (15u)
#endif
#define SD_DET_B (16u)

#define SERIAL_BOOT (24u)

#define LED_PIN (25u)
#endif

#define SD_SCLK (18u)
#define SD_MOSI_CMD (19u)
#define SD_MISO_DAT0 (20u)
#define SD_DAT1 (21u)
#define SD_DAT2 (22u)
#define SD_CS_DAT3 (23u)

#endif //SMART_LCC_PINS_H
