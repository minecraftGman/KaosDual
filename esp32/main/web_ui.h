#pragma once
#ifndef WEB_UI_H
#define WEB_UI_H

#include "esp_http_server.h"

/* Start the HTTP server and register all URI handlers */
httpd_handle_t web_ui_start(void);

/* Stop the HTTP server */
void web_ui_stop(httpd_handle_t server);

#endif
