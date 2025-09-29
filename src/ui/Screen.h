#pragma once
#include <stdint.h>
#include <Arduino.h>
#include "gfx/Display.h"

namespace ui {

// Minimal screen interface for refactor migration
class IScreen {
public:
  virtual ~IScreen() = default;
  virtual const char* name() const = 0;

  // Lifecycle hooks
  virtual void enter() {}           // called when screen becomes active
  virtual void exit() {}            // called when screen is deactivated

  // Called once per loop; keep non-blocking
  virtual void tick() {}

  // Draw incremental portion; keep fast, may be called often
  virtual void draw(ui_gfx::Display& d) = 0;
};

} // namespace ui

