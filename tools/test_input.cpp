// Native simulation of the MeshDeck trackball-button state machine.
// Mirrors DeckHW::readNav() exactly so we can prove the click/back/wake/sleep
// behaviour on a PC before flashing. Build & run: see run_tests.sh
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <cassert>

enum Nav { NAV_NONE, NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_SELECT, NAV_BACK };
const char* navName(Nav n){const char*s[]={"NONE","UP","DOWN","LEFT","RIGHT","SELECT","BACK"};return s[n];}

// ---- exact copy of the interrupt-primed button logic from DeckHW::readNav ----
// clickEdge models the hardware ISR flag (set the instant the button goes low).
struct Btn {
  bool clickEdge=false, wasDown=false, longFired=false;
  uint32_t downAt=0, upSince=0;
  // `pressed` is the pin level sampled this tick; the ISR would already have set
  // clickEdge on the falling edge regardless of when we sample.
  Nav step(uint32_t now, bool low) {
    if (clickEdge && !wasDown) {
      clickEdge=false; wasDown=true; downAt=now; longFired=false; upSince=0;
    }
    if (wasDown) {
      if (low) {
        upSince=0;
        if (!longFired && now-downAt>450){ longFired=true; return NAV_BACK; }
      } else {
        if (upSince==0) upSince=now;
        if (now-upSince>25){ wasDown=false; clickEdge=false; if(!longFired) return NAV_SELECT; }
      }
    }
    return NAV_NONE;
  }
};

// ---- display wake/dim model (mirrors UITask dispatchInput + checkDim) ----
struct Display {
  bool on=true; uint32_t lastActivity=0; uint32_t timeoutMs=60000;
  // returns true if this input was swallowed to wake the screen
  bool feed(uint32_t now, Nav ev) {
    if (ev != NAV_NONE) lastActivity = now;
    if (!on) { if (ev != NAV_NONE) { on = true; return true; } return false; }
    return false;
  }
  void tick(uint32_t now) { if (on && timeoutMs && now - lastActivity > timeoutMs) on = false; }
};

int failures = 0;
void check(bool cond, const std::string& msg) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", msg.c_str());
  if (!cond) failures++;
}

// helper: press the button (fires the ISR edge) for downMs, release for upMs,
// sampling every sampleMs. Returns every event emitted across press+release.
std::vector<Nav> tap(Btn& b, uint32_t& t, uint32_t downMs, uint32_t upMs, uint32_t sampleMs) {
  std::vector<Nav> evs;
  b.clickEdge = true;   // hardware ISR fires on the falling (press) edge
  for (uint32_t end=t+downMs; t<end; t+=sampleMs){ Nav e=b.step(t,true);  if(e!=NAV_NONE)evs.push_back(e); }
  for (uint32_t end=t+upMs;   t<end; t+=sampleMs){ Nav e=b.step(t,false); if(e!=NAV_NONE)evs.push_back(e); }
  return evs;
}
void settle(Btn& b, uint32_t& t) { for(uint32_t end=t+60; t<end; t+=5) b.step(t,false); }

int main() {
  // ---------- Test 1: quick tap = SELECT (fine sampling) ----------
  printf("Test 1: quick tap selects (5ms sampling)\n");
  { Btn b; uint32_t t=0; settle(b,t);
    auto e = tap(b,t,120,60,5);
    check(e.size()==1 && e[0]==NAV_SELECT, "one SELECT on a 120ms tap");
  }

  // ---------- Test 2: quick tap = SELECT with a coarse/slow loop ----------
  printf("Test 2: quick tap selects even with a coarse 50ms loop\n");
  { Btn b; uint32_t t=0; settle(b,t);
    auto e = tap(b,t,150,150,50);
    check(e.size()==1 && e[0]==NAV_SELECT, "SELECT fires with slow sampling");
  }

  // ---------- Test 3: press-and-hold = exactly one BACK, no SELECT ----------
  printf("Test 3: press-and-hold emits one BACK and no SELECT\n");
  { Btn b; uint32_t t=0; settle(b,t);
    auto e = tap(b,t,1200,60,10);                      // hold 1.2s
    int backs=0, sels=0; for(Nav n:e){if(n==NAV_BACK)backs++;if(n==NAV_SELECT)sels++;}
    check(backs==1, "exactly one BACK");
    check(sels==0, "no stray SELECT from a hold");
  }

  // ---------- Test 4: a firm-but-short tap (<400ms) is SELECT, not BACK ----------
  printf("Test 4: a 300ms firm tap still selects (not back)\n");
  { Btn b; uint32_t t=0; settle(b,t);
    auto e = tap(b,t,300,60,10);
    check(e.size()==1 && e[0]==NAV_SELECT, "300ms tap -> SELECT");
  }

  // ---------- Test 5: two taps = two SELECTs (no accidental back) ----------
  printf("Test 5: two separate taps are two SELECTs\n");
  { Btn b; uint32_t t=0; settle(b,t);
    std::vector<Nav> all;
    for(Nav n:tap(b,t,100,150,10))all.push_back(n);
    for(Nav n:tap(b,t,100,60,10))all.push_back(n);
    check(all.size()==2 && all[0]==NAV_SELECT && all[1]==NAV_SELECT, "two SELECTs");
  }

  // ---------- Test 6: button HELD at boot never fires until released ----------
  printf("Test 6: button held at power-on emits nothing until released\n");
  { Btn b; uint32_t t=0;
    std::vector<Nav> e; for(uint32_t end=3000; t<end; t+=10){ Nav n=b.step(t,true); if(n!=NAV_NONE)e.push_back(n);}
    check(e.empty(), "no phantom event while held-from-boot (even past 400ms)");
    settle(b,t);                                       // release -> arm
    auto e2 = tap(b,t,120,60,10);
    check(e2.size()==1 && e2[0]==NAV_SELECT, "works normally after first release");
  }

  // ---------- Test 7: contact bounce on press/release -> one clean SELECT ----------
  printf("Test 7: a bouncy press+release yields exactly one SELECT\n");
  { Btn b; uint32_t t=0; settle(b,t);
    std::vector<Nav> all;
    b.clickEdge = true;                                 // ISR fires on the press edge
    for(uint32_t end=t+120; t<end; t+=3){ Nav e=b.step(t,true); if(e!=NAV_NONE)all.push_back(e);} // solid down
    // release with a brief 6ms bounce-low partway (edge fires again but is ignored)
    Nav e; e=b.step(t,false); if(e!=NAV_NONE)all.push_back(e); t+=6;      // high
    b.clickEdge=true; e=b.step(t,true); if(e!=NAV_NONE)all.push_back(e); t+=6;  // bounce low
    for(uint32_t end=t+80; t<end; t+=3){ e=b.step(t,false); if(e!=NAV_NONE)all.push_back(e);}       // stable high
    check(all.size()==1 && all[0]==NAV_SELECT, "bounce collapses to one SELECT, no false BACK");
  }

  // ---------- Test 8: a tap wakes the display and is swallowed ----------
  printf("Test 8: a tap wakes the display (swallowed, not passed through)\n");
  { Btn b; Display d; uint32_t t=0; settle(b,t);
    d.on=false; d.lastActivity=0;
    Nav e=NAV_NONE; for(Nav n:tap(b,t,120,60,10)) if(n!=NAV_NONE) e=n;
    bool swallowed = d.feed(t, e);
    check(d.on, "display turned back on from a tap");
    check(swallowed, "the waking tap was swallowed");
  }

  // ---------- Test 9: display sleeps after timeout, movement wakes it ----------
  printf("Test 9: display sleeps after idle timeout, movement wakes it\n");
  { Display d; uint32_t t=0; d.lastActivity=0;
    for(t=0;t<70000;t+=1000) d.tick(t);
    check(!d.on, "display slept after 60s idle");
    d.feed(t, NAV_DOWN);
    check(d.on, "movement woke the display");
  }

  printf("\n%s (%d failure%s)\n", failures? "TESTS FAILED":"ALL TESTS PASSED",
         failures, failures==1?"":"s");
  return failures ? 1 : 0;
}
