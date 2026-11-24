#include "EffectRenderer.h"

EffectRenderer::EffectRenderer(ILedStrip& leds, LEDMapper& mapper)
    : leds_(leds), ledMapper_(mapper) {
}

void EffectRenderer::render(const PixelMatrix& matrix) {
    // Clear first — unmapped LEDs must be black, not stale from previous frame
    leds_.clear();

    // Iterate by LED index (0..N-1) so writes to the output buffer are sequential
    // (cache-friendly). Reads from PixelMatrix are random-access via indexToX/indexToY.
    const int numLeds = leds_.numPixels();

    for (int i = 0; i < numLeds; i++) {
        int x = ledMapper_.getX(i);
        int y = ledMapper_.getY(i);
        if (x >= 0) {  // x and y are always set together in LEDMapper::setMapping
            const RGB& color = matrix.getPixel(x, y);
            leds_.setPixelColor(i, color.r, color.g, color.b);
        }
    }
}

void EffectRenderer::clear() {
    for (int i = 0; i < leds_.numPixels(); i++) {
        leds_.setPixelColor(i, 0);
    }
}

