#pragma once

// Modern multi-texture ribbon combine. A modern (Cata+) ribbon trail layers 3 textures meant to
// combine as tex0*tex1*tex2*color*4. The host engine draws an N-texture ribbon as N sequential
// single-texture passes, which cannot reproduce that product, so it reads as a flat strip. This
// reproduces the intended look in ONE pass via the fixed-function texture stages (MODULATE,
// MODULATE, MODULATE4X) by pre-binding the extra layers to s1/s2 and applying the stage combine at
// the draw call. No vertex/pixel shader swap; the engine's own transforms and additive blend stand.
namespace wraith::features::ribbon
{
    void Install();
}
