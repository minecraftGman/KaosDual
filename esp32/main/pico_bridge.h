#pragma once
#ifndef PICO_BRIDGE_H
#define PICO_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include "kaos_protocol.h"

/* Initialise UART2 and start the RX task */
void pico_bridge_init(void);

/* Send a loaded Skylander dump to the Pico for a given slot */
void pico_bridge_load(uint8_t slot, const uint8_t *raw_dump_1024);

/* Tell the Pico to unload a slot */
void pico_bridge_unload(uint8_t slot);

/* Returns true if the Pico has sent MSG_PICO_READY since boot */
bool pico_bridge_is_ready(void);

#endif
