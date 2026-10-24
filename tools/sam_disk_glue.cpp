// tools/sam_sim disk glue: the firmware's SamFdc.cpp, exported to the C simulator.
// Built with -Itools/sam_stub ahead of -Isrc (see tools/sam_sim.c's header).
#include "speccy/machines/Sam/SamFdc.h"
#include "app/Config.h"
bool Config::samWP[2] = {true, true};   // the host test never writes the image
extern "C" {
uint8_t samfdc_in(int d, uint16_t port) { return SamFdc::in((uint8_t)d, port); }
void    samfdc_out(int d, uint16_t port, uint8_t v) { SamFdc::out((uint8_t)d, port, v); }
int     samfdc_mount(int d, const char* path) { return SamFdc::mount((uint8_t)d, path) ? 1 : 0; }
void    samfdc_frame(void) { SamFdc::frameEnd(); }
}
