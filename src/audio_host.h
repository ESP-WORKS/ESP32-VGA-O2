#pragma once
#include <stdint.h>
#include <stddef.h>

void audioInit();
void audioShutdown();
void audioEnable(bool on);
bool audioEnabled();
void audioCloseAndShrink();
/* Push 8-bit unsigned samples. Drops if queue would exceed AUDIO_QUEUE_CAP. */
void audioPush(const uint8_t *pcm, int n);
void audioIdle();
