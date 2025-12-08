#ifndef SPLASH_SCREEN_H
#define SPLASH_SCREEN_H

#include "../base_screen.h"

class SplashScreen : public BaseScreen {
 public:
  lv_obj_t* root() override;
  void handle(const UiEvent &evt) override;
  void cleanup() override;

 private:
  void build();
  lv_obj_t* root_ = nullptr;
  lv_obj_t* status_label_ = nullptr;
};

#endif // SPLASH_SCREEN_H
