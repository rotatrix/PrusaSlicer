#pragma once
#include "Slic3r/App/Scene/BedNodeTag.hpp"

namespace Slic3r::App::Plater {
// Native bed selection is separate from object selection. Only physical bed
// surfaces are navigation targets, never labels, axes or placement previews.
inline bool openaxis_bed_pickable(const Scene::BedNodeTag &tag, bool selected_only,
                                  bool bed_selected) {
    using Scene::BedElementType;
    const bool surface = tag.type == BedElementType::PlateDefault ||
                         tag.type == BedElementType::PlateTextured ||
                         tag.type == BedElementType::Model;
    return surface && !tag.is_virtual && (!selected_only || bed_selected);
}
} // namespace Slic3r::App::Plater
