#pragma once

#include <cstddef>

namespace wraith
{
    // Base for typed views over raw engine memory. A view is never instantiated: an engine pointer is cast to
    // the view type once, then its fields are read through named accessors (at<T>) instead of scattered
    // reinterpret_casts. Empty base (no data) so the cast and layout are unchanged.
    struct EngineView
    {
    protected:
        template <class T> T* at(size_t off) const
        {
            return reinterpret_cast<T*>(const_cast<char*>(reinterpret_cast<const char*>(this)) + off);
        }
    };
}
