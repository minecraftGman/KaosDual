#pragma once
#ifndef PICO_BRIDGE_H
#define PICO_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include "kaos_protocol.h"

/* Initialise UART2 and start the RX task.
 * Call once from app_main after g_sky_mutex is created. */
void pico_bridge_init(void);

/* Send a loaded Skylander dump to the Pico for a given slot.
 * data must be a raw (still-encrypted) 1024-byte dump — the Pico
 * will decrypt it itself so keys stay on the Pico side. */
void pico_bridge_load(uint8_t slot, const uint8_t *raw_dump_1024);

/* Tell the Pico to unload a slot */
void pico_bridge_unload(uint8_t slot);

/* Tell the Pico to switch portal type (0=SSA/Giants, 1=SwapForce, 2=TrapTeam, 3=Imaginators) */
void pico_bridge_set_portal_type(uint8_t type);

/* Returns true if the Pico has sent MSG_PICO_READY since boot */
bool pico_bridge_is_ready(void);

#endif
