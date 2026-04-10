#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>

#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

TFT_eSPI tft = TFT_eSPI();
XPT2046_Bitbang ts(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);

static lv_color_t buf[240 * 20];

// ====================== FLUSH ======================
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t*)px_map, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

// ====================== TOUCH CON FEEDBACK VISUAL ======================
void my_touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
  TouchPoint p = ts.getTouch();

  if (p.zRaw > 250) {
    int16_t x = constrain(map(p.y, 220, 20, 0, 239), 0, 239);
    int16_t y = constrain(map(p.x, 20, 299, 0, 319), 0, 319);

    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;

    // Dibuja un punto verde donde tocas (feedback visual)
    lv_obj_t *dot = lv_obj_create(lv_screen_active());
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_pos(dot, x - 5, y - 5);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
  } 
  else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ====================== SETUP ======================
void setup() {
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  ts.begin();

  lv_init();

  lv_display_t *disp = lv_display_create(240, 320);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touch_read);

  // Pantalla inicial
  lv_obj_t *label = lv_label_create(lv_screen_active());
  lv_label_set_text(label, "Touch Test\nToca la pantalla\nDeberías ver puntos verdes");
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, -40);

  Serial.begin(115200);
  Serial.println("Touch test iniciado - toca la pantalla");
}

void loop() {
  lv_timer_handler();
  delay(5);
}