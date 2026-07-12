#include "AllScreens.h"
extern "C" {
#include "qrcodegen.h"
}

void QRScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("QR link");

  const char* url = ui.qrUrl();
  if (!url || !url[0]) {
    c.setTextColor(C_FG_FAINT);
    c.setCursor(90, 110);
    c.print("No link to show");
    return;
  }

  static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
  static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
  bool ok = qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_LOW,
                                 1, 10, qrcodegen_Mask_AUTO, true);
  if (!ok) {
    c.setTextColor(C_RED);
    c.setCursor(80, 110);
    c.print("Link too long for QR");
    return;
  }

  int size = qrcodegen_getSize(qr);
  int avail = SCREEN_H - STATUS_H - 40;
  int scale = avail / (size + 4);
  if (scale < 1) scale = 1;
  int px = size * scale;
  int x0 = (SCREEN_W - px) / 2;
  int y0 = STATUS_H + 8 + (avail - px) / 2;

  // white quiet zone
  c.fillRect(x0 - 8, y0 - 8, px + 16, px + 16, 0xFFFF);
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (qrcodegen_getModule(qr, x, y)) {
        c.fillRect(x0 + x * scale, y0 + y * scale, scale, scale, 0x0000);
      }
    }
  }

  // url caption
  char cap[46];
  ellipsize(cap, sizeof(cap), url);
  c.setTextSize(1);
  c.setTextColor(C_FG_DIM);
  c.setCursor((SCREEN_W - strlen(cap) * 6) / 2, SCREEN_H - 16);
  c.print(cap);
}
