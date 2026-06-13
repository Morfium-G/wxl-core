#pragma once

// The M2 feature. Teaches the host 3.3.5 M2 loader to also read modern (Cata+/Legion) models.
namespace wraith::features::m2
{
    void Install();

    // Scope the lowered alpha-key reference (0.5) to modern models at draw time; stock 264 keeps 0.878.
    void AlphaScope_Install();
}
