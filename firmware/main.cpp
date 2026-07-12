/*
 * MeshDeck OS - standalone MeshCore firmware for the LilyGo T-Deck
 *
 * Built on the open-source MeshCore stack (MIT licence).
 * Turns the T-Deck into a self-contained mesh communicator:
 * chat UI, offline map, repeater tools, noise monitor, terminal and more.
 * BLE companion mode stays available, so the phone apps still work too.
 */

#include <Arduino.h>
#include <Mesh.h>
#include <SPIFFS.h>
#include "MyMesh.h"

DataStore store(SPIFFS, rtc_clock);

#include <helpers/esp32/SerialBLEInterface.h>
SerialBLEInterface serial_interface;

#include "ui/UITask.h"
UITask ui_task(&board, &serial_interface);

StdRNG fast_rng;
SimpleMeshTables tables;
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store, &ui_task);

void halt() {
  while (1);
}

void setup() {
  Serial.begin(115200);

  board.begin();

  // Bring the display + input hardware up first so we can show a boot splash
  ui_task.earlyInit();

  if (!radio_init()) {
    ui_task.fatalError("Radio init failed");
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  ui_task.bootStatus("loading storage...");
  SPIFFS.begin(true);
  store.begin();

  ui_task.bootStatus("starting mesh...");
  the_mesh.begin(true);

  ui_task.bootStatus("starting bluetooth...");
  serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
  the_mesh.startInterface(serial_interface);

  // Recover + cleanly (re)start I2C, then probe keyboard/touch, RIGHT BEFORE the
  // sensor scan - so the bus is healthy and the scan (and every later read) is
  // fast, and nothing can undo the timeout in between.
  ui_task.hw.initI2CDevices();

  ui_task.bootStatus("starting sensors...");
  sensors.begin();

#if ENV_INCLUDE_GPS == 1
  the_mesh.applyGpsPrefs();
#endif

  ui_task.bootStatus("starting ui...");
  ui_task.begin(&the_mesh, &sensors, the_mesh.getNodePrefs());

  board.onBootComplete();
}

void loop() {
  the_mesh.loop();
  sensors.loop();
  ui_task.loop();
  rtc_clock.tick();
}
