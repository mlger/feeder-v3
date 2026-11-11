// Optional scale under the food hopper: a load cell on an HX711 amplifier.
//
// Wiring: HX711 DT -> D6 (GPIO12), SCK -> D7 (GPIO13), VCC -> 3V3, GND -> GND.
// If no HX711 is connected the node simply runs without a scale.
#pragma once

#include <Arduino.h>

void scaleBegin();
void scaleLoop();  // call from loop(); takes a sample whenever one is ready

bool scaleFound();                 // an HX711 is answering
bool scaleRead(int32_t *mg);       // steady weight of the food; false if not set up or still moving
bool scaleZero();                  // the hopper is empty now
bool scaleSpan(uint32_t knownMg);  // the hopper now holds this much
