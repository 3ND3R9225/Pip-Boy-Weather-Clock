#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include "Arduino_DriveBus_Library.h"
#include "pin_config.h"
#include <ui.h>

#include "WiFiHelper.h"
#include "WeatherHelper.h"

namespace {
constexpr unsigned long kBatteryCheckIntervalMs = 10000;
constexpr unsigned long kNtpCheckIntervalMs = 10000;
constexpr unsigned long kWeatherCheckIntervalMs = 300000;

#ifdef BOARD_CYD
constexpr int kDisplayWidth = 320;
constexpr int kDisplayHeight = 240;
constexpr int kBacklightPin = 21;
#else
constexpr int kDisplayWidth = LCD_WIDTH;
constexpr int kDisplayHeight = LCD_HEIGHT;
constexpr int kBacklightPin = LCD_EN;
#endif

unsigned long lastBatteryCheck = 0;
unsigned long lastNTPTimeCheck = 0;
unsigned long lastWeatherCheck = 0;

int32_t battery_voltage = 0;

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;

Arduino_DataBus *bus = nullptr;
Arduino_GFX *gfx = nullptr;

#ifndef BOARD_CYD
std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
    std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);

void Arduino_IIC_Touch_Interrupt(void);

std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(IIC_Bus, FT3168_DEVICE_ADDRESS,
                                                       TP_RST, TP_INT, Arduino_IIC_Touch_Interrupt));

std::unique_ptr<Arduino_IIC> SY6970(new Arduino_SY6970(IIC_Bus, SY6970_DEVICE_ADDRESS,
                                                       DRIVEBUS_DEFAULT_VALUE, DRIVEBUS_DEFAULT_VALUE));

void Arduino_IIC_Touch_Interrupt(void) { FT3168->IIC_Interrupt_Flag = true; }
#endif

void createDisplay() {
#ifdef BOARD_CYD
    bus = new Arduino_ESP32SPI(15 /* DC */, 5 /* CS */, 14 /* SCK */, 13 /* MOSI */, 12 /* MISO */);
    gfx = new Arduino_ILI9341(bus, 4 /* RST */, 1 /* rotation */, false /* IPS */, kDisplayWidth, kDisplayHeight);
#else
    bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1,
        LCD_SDIO2, LCD_SDIO3);
    gfx = new Arduino_CO5300(bus, LCD_RST,
                             0 /* rotation */, false /* IPS */, kDisplayWidth, kDisplayHeight,
                             20 /* col offset 1 */, 0 /* row offset 1 */, 0 /* col_offset2 */, 0 /* row_offset2 */);
#endif
}

void removeSplash(lv_timer_t *timer) {
    lv_obj_t *splash = static_cast<lv_obj_t *>(timer->user_data);
    if (splash && lv_obj_is_valid(splash)) {
        lv_obj_del(splash);
    }
    lv_timer_del(timer);
}

void addPlaceholderPanels() {
    lv_obj_t *gpsTab = lv_tabview_add_tab(ui_Tab_main1, "GPS");
    lv_obj_t *gpsLabel = lv_label_create(gpsTab);
    lv_label_set_text(gpsLabel, "GPS placeholder\nComing soon");
    lv_obj_set_style_text_align(gpsLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(gpsLabel);

    lv_obj_t *settingsTab = lv_tabview_add_tab(ui_Tab_main1, "SET");
    lv_obj_t *settingsLabel = lv_label_create(settingsTab);
    lv_label_set_text(settingsLabel, "Settings placeholder\nComing soon");
    lv_obj_set_style_text_align(settingsLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(settingsLabel);

    lv_obj_t *splash = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(splash);
    lv_obj_set_size(splash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(splash, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *splashLabel = lv_label_create(splash);
    lv_label_set_text(splashLabel, "Pip-Boy\nSplash placeholder");
    lv_obj_set_style_text_color(splashLabel, lv_color_hex(0x00C200), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(splashLabel, &ui_font_J35, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(splashLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(splashLabel);

    lv_timer_create(removeSplash, 2000, splash);
}

int voltageToPercentage(float voltage) {
    constexpr float minVoltage = 3.0F;
    constexpr float maxVoltage = 4.2F;
    constexpr int hysteresisThreshold = 3;

    voltage = max(minVoltage, min(maxVoltage, voltage));

    int percentage = 0;
    if (voltage <= minVoltage) return 0;
    if (voltage >= maxVoltage) return 100;

    if (voltage < 3.5F) {
        percentage = static_cast<int>((voltage - 3.0F) / 0.5F * 10);
    } else if (voltage < 3.7F) {
        percentage = 10 + static_cast<int>((voltage - 3.5F) / 0.2F * 30);
    } else if (voltage < 3.9F) {
        percentage = 40 + static_cast<int>((voltage - 3.7F) / 0.2F * 20);
    } else if (voltage < 4.0F) {
        percentage = 60 + static_cast<int>((voltage - 3.9F) / 0.1F * 20);
    } else if (voltage < 4.1F) {
        percentage = 80 + static_cast<int>((voltage - 4.0F) / 0.1F * 10);
    } else {
        percentage = 90 + static_cast<int>((voltage - 4.1F) / 0.1F * 10);
    }

    static int lastPercentage = -1;
    static float lastVoltage = -1;

    if (lastVoltage == -1 || abs(voltage - lastVoltage) > 0.02 || abs(percentage - lastPercentage) > hysteresisThreshold) {
        lastPercentage = percentage;
        lastVoltage = voltage;
    }

    return lastPercentage;
}

void readBatteryStatus() {
#ifdef BOARD_CYD
    // Placeholder until a CYD-specific battery readout is wired in.
    lv_bar_set_value(ui_Bar_battery, 100, LV_ANIM_ON);
#else
    battery_voltage = SY6970->IIC_Read_Device_Value(SY6970->Arduino_IIC_Power::Value_Information::POWER_BATTERY_VOLTAGE);
    const float voltage = battery_voltage / 1000.0F;
    const int battery_percentage = voltageToPercentage(voltage);

    Serial.printf("Battery Voltage: %.2f V (%d mV), Battery Percentage: %d%%\n", voltage, battery_voltage, battery_percentage);
    lv_bar_set_value(ui_Bar_battery, battery_percentage, LV_ANIM_ON);
#endif
}

void setupSY6970() {
#ifndef BOARD_CYD
    Serial.println("Starting SY6970 initialization");

    while (!SY6970->begin()) {
        Serial.println("SY6970 initialization failed, retrying...");
        delay(2000);
    }

    while (!SY6970->IIC_Write_Device_State(SY6970->Arduino_IIC_Power::Device::POWER_DEVICE_ADC_MEASURE,
                                           SY6970->Arduino_IIC_Power::Device_State::POWER_DEVICE_ON)) {
        Serial.println("Failed to activate ADC Measurement, retrying...");
        delay(2000);
    }
    Serial.println("SY6970 initialization successful");
#endif
}

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    const uint32_t w = (area->x2 - area->x1 + 1);
    const uint32_t h = (area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif

    lv_disp_flush_ready(disp);
}

#ifndef BOARD_CYD
void my_touchpad_read(lv_indev_drv_t *, lv_indev_data_t *data) {
    if (FT3168->IIC_Interrupt_Flag) {
        FT3168->IIC_Interrupt_Flag = false;

        const int32_t touch_x = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
        const int32_t touch_y = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
        const uint8_t fingers_number = FT3168->IIC_Read_Device_Value(FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);

        if (fingers_number > 0) {
            data->state = LV_INDEV_STATE_PR;
            data->point.x = touch_x;
            data->point.y = touch_y;
        } else {
            data->state = LV_INDEV_STATE_REL;
        }
    }
}
#endif

void my_rounder_cb(lv_disp_drv_t *, lv_area_t *area) {
    if (area->x1 % 2 != 0) area->x1 += 1;
    if (area->y1 % 2 != 0) area->y1 += 1;

    const uint32_t w = (area->x2 - area->x1 + 1);
    const uint32_t h = (area->y2 - area->y1 + 1);
    if (w % 2 != 0) area->x2 -= 1;
    if (h % 2 != 0) area->y2 -= 1;
}

void lvgl_initialization() {
    lv_init();

    lv_color_t *buf_1 = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * kDisplayWidth * 40, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lv_color_t *buf_2 = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * kDisplayWidth * 40, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    while (!buf_1 || !buf_2) {
        Serial.println("LVGL disp_draw_buf allocate failed!");
        delay(1000);
    }

    lv_disp_draw_buf_init(&draw_buf, buf_1, buf_2, kDisplayWidth * 40);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = kDisplayWidth;
    disp_drv.ver_res = kDisplayHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.rounder_cb = my_rounder_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.full_refresh = 1;
    lv_disp_drv_register(&disp_drv);

#ifndef BOARD_CYD
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);
#endif
}

}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.println("Starting Setup...");

    pinMode(kBacklightPin, OUTPUT);
    digitalWrite(kBacklightPin, HIGH);

    createDisplay();

#ifndef BOARD_CYD
    if (FT3168->begin() == false) {
        Serial.println("FT3168 initialization failed");
    } else {
        Serial.println("FT3168 initialization successful");
    }
#else
    Serial.println("CYD build: touch controller not enabled yet (placeholder)");
#endif

    gfx->begin();
    gfx->fillScreen(BLACK);
    gfx->Display_Brightness(255);

    lvgl_initialization();
    ui_init();
    addPlaceholderPanels();

    setupSY6970();
    readBatteryStatus();

    walking_Animation(ui_Image_time, 0);
    walking_Animation(ui_Image_weather, 0);
    thumbsup_Animation(ui_Image_data, 0);

    setupWiFi();
    fetchWeatherData();
}

void loop() {
    lv_timer_handler();

    const unsigned long now = millis();

    if (now - lastNTPTimeCheck >= kNtpCheckIntervalMs) {
        fetchNTPTime();
        lastNTPTimeCheck = now;
    }

    if (now - lastBatteryCheck >= kBatteryCheckIntervalMs) {
        readBatteryStatus();
        lastBatteryCheck = now;
    }

    if (now - lastWeatherCheck >= kWeatherCheckIntervalMs) {
        fetchWeatherData();
        lastWeatherCheck = now;
    }
}
