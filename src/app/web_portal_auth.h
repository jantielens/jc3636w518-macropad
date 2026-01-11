#pragma once

struct AsyncWebServerRequest;

bool portal_auth_gate(AsyncWebServerRequest* request);
