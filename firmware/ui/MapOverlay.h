#pragma once
/*
 * Shared Map / NewMaps overlay:
 *   - the map menu: a fullscreen list (GPS info, Center on me, Set my position
 *     here), opened with m / Enter or the MENU chip on the touchscreen;
 *   - "pick" mode: the user pans the map so the crosshair sits on a spot, then
 *     confirms. Used to set the manual (fallback) position visually.
 *
 * Each map screen owns one MapOverlay and forwards key/nav/touch to it first.
 * The overlay works on the map's centre (clat/clon) passed by reference, so it
 * never needs to know which map it is on.
 */
#include "UITask.h"

class MapOverlay {
public:
  enum Mode : uint8_t { OFF = 0, MENU, PICK };
  Mode mode() const { return _mode; }
  void openMenu() { _mode = MENU; _sel = 0; }
  void openPick() { _mode = PICK; }
  void close()    { _mode = OFF; }

  // MENU: paints the whole screen - call INSTEAD of drawing the map, then return.
  void drawMenu(UITask& ui);
  // OFF: the MENU chip. PICK: big crosshair + confirm banner. Call after the map.
  void drawOver(UITask& ui, double clat, double clon);

  // Return true when consumed. In PICK mode, pan/zoom input is NOT consumed so
  // the map keeps handling it.
  bool key(UITask& ui, uint8_t k, double& clat, double& clon);
  bool nav(UITask& ui, NavEvent e, double& clat, double& clon);
  bool touch(UITask& ui, const TouchEvent& e, double& clat, double& clon);

  // "47.12345 N  116.12345 W"
  static void fmtLatLon(char* out, size_t n, double lat, double lon);

private:
  void run(UITask& ui, int item, double& clat, double& clon);
  void confirmPick(UITask& ui, double clat, double clon);
  Mode _mode = OFF;
  int  _sel = 0;
};
