#include <Arduino.h>
#include <esp_display_panel.hpp>

#include <lvgl.h>
#include "lvgl_v8_port.h"
#include "ui_chat.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

static void halt_on_error(const char *message)
{
    Serial.println(message);
    Serial.println("System halted. Check board/display configuration before continuing.");
    while (true) {
        delay(1000);
    }
}

static void handle_send(const char *text)
{
    Serial.print("User: ");
    Serial.println(text);

    ui_chat_set_status("Thinking...");

    String reply = "Ban vua nhap: ";
    reply += text;
    ui_chat_add_message("assistant", reply.c_str());
    ui_chat_set_status("Ready");
}

static void handle_mic()
{
    Serial.println("Mic pressed");
    ui_chat_add_message("assistant", "Da bam nut Mic. Cho phan ghi am/I2S vao callback nay.");
    ui_chat_set_status("Ready");
}

static void handle_new_chat()
{
    Serial.println("New chat");
}

static void handle_history(uint8_t index)
{
    Serial.print("Open history index: ");
    Serial.println(index);
}

/**
 * To use the built-in examples and demos of LVGL uncomment the includes below respectively.
 */
 // #include <demos/lv_demos.h>
 // #include <examples/lv_examples.h>

void setup()
{
    String title = "LVGL porting example";

    Serial.begin(115200);
    delay(100);
    Serial.printf("PSRAM size: %u bytes\n", ESP.getPsramSize());
    if (ESP.getPsramSize() == 0) {
        halt_on_error("PSRAM is not enabled. Enable OPI PSRAM in Arduino IDE for ESP32-S3 LCD 7.");
    }

    Serial.println("Initializing board");
    Board *board = new Board();
    if (!board->init()) {
        halt_on_error("Board init failed");
    }

    #if LVGL_PORT_AVOID_TEARING_MODE
    auto lcd = board->getLCD();
    if (lcd == nullptr) {
        halt_on_error("LCD device was not created");
    }
    // When avoid tearing function is enabled, the frame buffer number should be set in the board driver
    lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
    auto lcd_bus = lcd->getBus();
    /**
     * As the anti-tearing feature typically consumes more PSRAM bandwidth, for the ESP32-S3, we need to utilize the
     * "bounce buffer" functionality to enhance the RGB data bandwidth.
     * This feature will consume `bounce_buffer_size * bytes_per_pixel * 2` of SRAM memory.
     */
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
    }
#endif
#endif
    if (!board->begin()) {
        halt_on_error("Board begin failed");
    }

    if (board->getLCD() == nullptr || board->getLCD()->getRefreshPanelHandle() == nullptr) {
        halt_on_error("LCD device is not initialized after board begin");
    }

    Serial.println("Initializing LVGL");
    if (!lvgl_port_init(board->getLCD(), board->getTouch())) {
        halt_on_error("LVGL port init failed");
    }

    Serial.println("Creating UI");
    /* Lock the mutex due to the LVGL APIs are not thread-safe */
    if (!lvgl_port_lock(-1)) {
        halt_on_error("LVGL lock failed");
    }

    ui_chat_set_send_callback(handle_send);
    ui_chat_set_mic_callback(handle_mic);
    ui_chat_set_new_chat_callback(handle_new_chat);
    ui_chat_set_history_callback(handle_history);
    ui_chat_init();

    /* Release the mutex */
    lvgl_port_unlock();
}

void loop()
{
    Serial.println("IDLE loop");
    delay(1000);
}
