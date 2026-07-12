#include "AllScreens.h"
#include "../MyMesh.h"
#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>
#include <math.h>

// ============================================================ Last heard

#define LH_ROW_H 22
#define LH_TOP (STATUS_H + 14)
#define LH_VIS ((SCREEN_H - LH_TOP) / LH_ROW_H)

static double distKm(double lat1, double lon1, double lat2, double lon2) {
  double dlat = (lat2 - lat1) * 0.0174533;
  double dlon = (lon2 - lon1) * 0.0174533;
  double a = sin(dlat / 2) * sin(dlat / 2) +
             cos(lat1 * 0.0174533) * cos(lat2 * 0.0174533) * sin(dlon / 2) * sin(dlon / 2);
  return 6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

void LastHeardScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("Last heard");

  c.setTextSize(1);
  c.setTextColor(C_FG_FAINT);
  c.setCursor(6, STATUS_H + 4);
  c.print("station              age   snr  rssi  hops  dist");

  int n = ui.heardCount();
  if (n == 0) {
    c.setTextColor(C_FG_FAINT);
    c.setCursor(66, 110);
    c.print("Nothing heard yet - stay tuned");
    return;
  }

  double slat, slon;
  bool have_pos = ui.ownPos(slat, slon);

  for (int i = _top; i < n && i - _top < LH_VIS; i++) {
    const HeardEntry* e = ui.heardAt(i);
    if (!e) continue;
    int y = LH_TOP + (i - _top) * LH_ROW_H;
    c.fillRoundRect(2, y, SCREEN_W - 4, LH_ROW_H - 2, 4, C_BG_ALT);

    uint16_t tc = e->type == ADV_TYPE_REPEATER ? C_ORANGE :
                  e->type == ADV_TYPE_ROOM ? C_PURPLE : C_CYAN;
    c.fillRect(2, y, 3, LH_ROW_H - 2, tc);

    char nm[19];
    ellipsize(nm, sizeof(nm), e->name);
    c.setTextColor(C_FG);
    c.setCursor(10, y + 3);
    c.print(nm);

    char ago[8];
    ui.fmtAgo(ago, sizeof(ago), e->at);
    char line[64];
    char dist[12] = "-";
    if (have_pos && (e->lat || e->lon)) {
      double d = distKm(slat, slon, e->lat / 1000000.0, e->lon / 1000000.0);
      if (d < 1) snprintf(dist, sizeof(dist), "%dm", (int)(d * 1000));
      else snprintf(dist, sizeof(dist), "%.1fkm", d);
    }
    char hops[8];
    if (e->hops == 0xFF || e->hops == 0) strcpy(hops, "direct");
    else snprintf(hops, sizeof(hops), "%d", e->hops);
    snprintf(line, sizeof(line), "%-5s %-5s %-5d %-5s %s",
             ago, StrHelper::ftoa(e->snr4 / 4.0f), e->rssi, hops, dist);
    c.setTextColor(C_FG_DIM);
    c.setCursor(124, y + 3);
    c.print(line);

    // snr quality dot
    float snr = e->snr4 / 4.0f;
    uint16_t qc = snr > 2 ? C_GREEN : snr > -8 ? C_YELLOW : C_RED;
    c.fillCircle(SCREEN_W - 10, y + LH_ROW_H / 2 - 1, 3, qc);

    c.setTextColor(C_FG_FAINT);
    c.setCursor(10, y + 12);
    c.print(e->type == ADV_TYPE_REPEATER ? "repeater" : e->type == ADV_TYPE_ROOM ? "room" : "companion");
  }
}

bool LastHeardScreen::nav(NavEvent e) {
  int n = ui.heardCount();
  switch (e) {
    case NAV_UP:   if (_top > 0) _top--; return true;
    case NAV_DOWN: if (_top < n - LH_VIS) _top++; return true;
    default: return false;
  }
}

// ============================================================ Noise floor

void NoiseScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("Noise floor");

  const int GX = 34, GY = STATUS_H + 10, GW = SCREEN_W - GX - 8, GH = 150;
  const int DB_TOP = -60, DB_BOT = -130;   // y-axis range

  c.drawRect(GX, GY, GW, GH, C_FG_FAINT);
  c.setTextSize(1);
  for (int db = DB_TOP; db >= DB_BOT; db -= 10) {
    int y = GY + (DB_TOP - db) * GH / (DB_TOP - DB_BOT);
    c.drawFastHLine(GX, y, GW, C_MAP_GRID);
    char lbl[8];
    snprintf(lbl, sizeof(lbl), "%d", db);
    c.setTextColor(C_FG_FAINT);
    c.setCursor(4, y - 3);
    c.print(lbl);
  }

  int head;
  const int8_t* ring = ui.noiseRing(head);
  int px = -1, py = -1;
  int mn = 0, mx = -200;
  long sum = 0;
  int cnt = 0;
  for (int i = 0; i < NOISE_SAMPLES; i++) {
    int8_t v = ring[(head + i) % NOISE_SAMPLES];
    if (v == -128) continue;
    int x = GX + i * GW / NOISE_SAMPLES;
    int y = GY + (DB_TOP - v) * GH / (DB_TOP - DB_BOT);
    if (y < GY) y = GY;
    if (y > GY + GH) y = GY + GH;
    if (px >= 0) c.drawLine(px, py, x, y, C_ACCENT);
    px = x; py = y;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
    sum += v;
    cnt++;
  }

  // stats footer
  c.setTextColor(C_FG);
  c.setCursor(8, GY + GH + 10);
  char s[80];
  if (cnt) {
    snprintf(s, sizeof(s), "now %d dBm   min %d   max %d   avg %d", ui.lastNoise(), mn, mx, (int)(sum / cnt));
  } else {
    snprintf(s, sizeof(s), "sampling...");
  }
  c.print(s);

  c.setTextColor(C_FG_DIM);
  c.setCursor(8, GY + GH + 24);
  if (ui.lastRxMillis()) {
    snprintf(s, sizeof(s), "last rx: rssi %d dBm  snr %s dB  (%us ago)",
             (int)ui.lastRxRssi(), StrHelper::ftoa(ui.lastRxSnr()),
             (unsigned)((millis() - ui.lastRxMillis()) / 1000));
  } else {
    snprintf(s, sizeof(s), "last rx: none yet");
  }
  c.print(s);

  c.setTextColor(C_FG_FAINT);
  c.setCursor(8, GY + GH + 38);
  c.print("80 second window, 4 samples/sec");
}

// ============================================================ Trace route

void TraceScreen::enter() {
  rebuild();
  _picking = ui.trace.tag == 0;
}

void TraceScreen::rebuild() {
  _n = 0;
  int n = ui.mesh->getNumContacts();
  for (int i = 0; i < n && _n < 24; i++) {
    ContactInfo ct;
    if (!ui.mesh->getContactByIdx(i, ct)) continue;
    if (ct.out_path_len == 0xFF) continue;   // need a known path
    memcpy(_prefixes[_n], ct.id.pub_key, 6);
    StrHelper::strncpy(_names[_n], ct.name, sizeof(_names[_n]));
    _hops[_n] = ct.out_path_len;
    _n++;
  }
  if (_sel >= _n) _sel = _n ? _n - 1 : 0;
}

void TraceScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("Trace route");
  c.setTextSize(1);

  if (_picking) {
    c.setTextColor(C_FG_DIM);
    c.setCursor(6, STATUS_H + 6);
    c.print("Pick a target (only contacts with a known path):");
    if (_n == 0) {
      c.setTextColor(C_FG_FAINT);
      c.setCursor(40, 110);
      c.print("No contacts with direct paths yet.");
      c.setCursor(40, 124);
      c.print("Message someone first so a path forms.");
      return;
    }
    for (int i = 0; i < _n && i < 12; i++) {
      int y = STATUS_H + 20 + i * 17;
      bool sel = i == _sel;
      if (sel) c.fillRoundRect(4, y - 2, SCREEN_W - 8, 16, 4, C_BG_RAISED);
      c.setTextColor(sel ? C_FG : C_FG_DIM);
      c.setCursor(10, y);
      c.print(_names[i]);
      char h[16];
      snprintf(h, sizeof(h), "%d hops", _hops[i]);
      c.setCursor(SCREEN_W - 70, y);
      c.print(h);
    }
    return;
  }

  // waiting / result view
  TraceResult& tr = ui.trace;
  c.setTextColor(C_FG);
  c.setCursor(6, STATUS_H + 8);
  char hd[48];
  snprintf(hd, sizeof(hd), "Target: %s", tr.target);
  c.print(hd);

  if (!tr.valid) {
    uint32_t waited = (millis() - tr.sent_millis) / 1000;
    c.setTextColor(waited > 30 ? C_RED : C_YELLOW);
    c.setCursor(6, STATUS_H + 26);
    char w[48];
    snprintf(w, sizeof(w), waited > 30 ? "No response (%us). r = retry" : "Waiting for response... %us", (unsigned)waited);
    c.print(w);
    return;
  }

  c.setTextColor(C_GREEN);
  c.setCursor(6, STATUS_H + 26);
  char rt[48];
  snprintf(rt, sizeof(rt), "Round trip complete - %d hop%s", tr.hops, tr.hops == 1 ? "" : "s");
  c.print(rt);

  // per-hop SNR bars
  int y = STATUS_H + 48;
  for (int i = 0; i < tr.hops; i++) {
    float snr = tr.snrs[i] / 4.0f;
    c.setTextColor(C_FG_DIM);
    c.setCursor(10, y);
    char hop[40];
    snprintf(hop, sizeof(hop), "hop %d  [%02X]", i + 1, tr.hashes[i]);
    c.print(hop);

    int bw = (int)((snr + 20) * 4);   // -20..+10 dB -> 0..120px
    if (bw < 2) bw = 2;
    if (bw > 120) bw = 120;
    uint16_t bc = snr > 2 ? C_GREEN : snr > -8 ? C_YELLOW : C_RED;
    c.fillRoundRect(120, y - 1, bw, 9, 3, bc);
    char sv[16];
    snprintf(sv, sizeof(sv), "%s dB", StrHelper::ftoa(snr));
    c.setTextColor(C_FG);
    c.setCursor(126 + bw, y);
    c.print(sv);
    y += 16;
  }
  c.setTextColor(C_FG_DIM);
  c.setCursor(10, y + 4);
  char fs[40];
  snprintf(fs, sizeof(fs), "final leg to us: %s dB", StrHelper::ftoa(tr.final_snr));
  c.print(fs);

  c.setTextColor(C_FG_FAINT);
  c.setCursor(6, SCREEN_H - 12);
  c.print("r = trace again   n = new target");
}

bool TraceScreen::key(uint8_t k) {
  if (k == 'n') { _picking = true; rebuild(); return true; }
  if (k == 'r' && ui.trace.target[0]) {
    ContactInfo* c2 = ui.mesh->lookupContactByPubKey(_prefixes[_sel], 6);
    if (c2) ui.startTrace(*c2);
    return true;
  }
  if (k == 0x0D && _picking && _n) {
    ContactInfo* c2 = ui.mesh->lookupContactByPubKey(_prefixes[_sel], 6);
    if (c2 && ui.startTrace(*c2)) _picking = false;
    return true;
  }
  return false;
}

bool TraceScreen::nav(NavEvent e) {
  if (!_picking) {
    if (e == NAV_SELECT) { _picking = true; rebuild(); return true; }
    return false;
  }
  switch (e) {
    case NAV_UP:   if (_sel > 0) _sel--; return true;
    case NAV_DOWN: if (_sel < _n - 1) _sel++; return true;
    case NAV_SELECT: {
      if (_n) {
        ContactInfo* c2 = ui.mesh->lookupContactByPubKey(_prefixes[_sel], 6);
        if (c2 && ui.startTrace(*c2)) _picking = false;
      }
      return true;
    }
    default: return false;
  }
}
