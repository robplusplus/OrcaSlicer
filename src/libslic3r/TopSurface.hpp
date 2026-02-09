#ifndef slic3r_TopSurface_hpp_
#define slic3r_TopSurface_hpp_

#include "libslic3r.h"
#include "ExPolygon.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

// Orca: Filter out small/thin upper-layer features (e.g. embossed text) that would
// otherwise fragment the top surface below. Used by both perimeter generation
// and surface-type detection.
Polygons top_surface_filter_features(const PrintRegionConfig &config,
    const ExPolygons &current_contour, const Polygons &upper_slices_clipped);

} // namespace Slic3r

#endif // slic3r_TopSurface_hpp_
