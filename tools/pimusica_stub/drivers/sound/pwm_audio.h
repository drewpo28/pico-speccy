#pragma once
#include <stdint.h>
extern volatile int16_t* volatile pcm_card_ring;
extern volatile uint32_t pcm_card_mask, pcm_card_r, pcm_card_w;
void pcm_card_attach(int16_t* ring, uint32_t frames);
void pcm_card_detach();
