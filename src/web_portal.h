#pragma once
#include <Arduino.h>

void web_portal_begin();
void web_portal_loop();
bool web_portal_take_code(String &code, String &verifier);
void web_portal_set_result(bool ok, const String &error);
// True while a firmware upload is running; percent is 0-100.
bool web_portal_update_progress(int &percent);
