#pragma once
#include <memory>
#include "ui/Screen.h"

namespace ui {

class UiController {
public:
  UiController() = default;

  void setScreen(std::shared_ptr<IScreen> scr) {
    if (scr == current_) return;
    if (current_) current_->exit();
    current_ = std::move(scr);
    if (current_) current_->enter();
  }

  std::shared_ptr<IScreen> current() const { return current_; }

  void tick() { if (current_) current_->tick(); }
  void draw(ui_gfx::Display& d) { if (current_) current_->draw(d); }

private:
  std::shared_ptr<IScreen> current_;
};

} // namespace ui

