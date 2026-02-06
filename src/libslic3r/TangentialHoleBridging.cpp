#include "TangentialHoleBridging.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "Layer.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"

namespace Slic3r {

void TangentialHoleBridging::apply(PrintObject* print_object)
{
    const std::vector<Layer*>& m_layers = print_object->layers();

    // Iterate layers (bottom to top) to find counterbore shelf overhangs.
    for (size_t i = 2; i < m_layers.size(); ++i) {
        Layer* layer_shelf = m_layers[i];
        Layer* layer_below = m_layers[i - 1];

        // Iterate over all regions to ensure correct material assignment
        for (size_t region_id = 0; region_id < print_object->num_printing_regions(); ++region_id) {
            const LayerRegion* shelf_region = layer_shelf->get_region(region_id);
            if (!shelf_region || shelf_region->slices.empty()) continue;

            // Check if this specific region has the feature enabled
            if (print_object->printing_region(region_id).config().counterbore_hole_bridging != chbTangential)
                continue;

            // Find Overhangs: Material in THIS Region's Shelf that is NOT in the layer Below (any material).
            // We compare against the FULL layer_below->lslices because support is provided by *anything* below,
            // regardless of material.
            ExPolygons overhangs = diff_ex(shelf_region->slices.surfaces, layer_below->lslices);
            if (overhangs.empty()) continue;

            for (const ExPolygon& ov : overhangs) {
                // Calculate Anchor Zone (Shelf Contour + 3mm)
                double anchor_dist = scale_(3.0);
                Polygons shelf_contour;
                shelf_contour.push_back(ov.contour);
                ExPolygons anchor_zone = offset_ex(shelf_contour, anchor_dist);
                
                // Iterate over all holes in the overhang.
                for (const Polygon& hole : ov.holes) {
                    BoundingBox hole_bbox = hole.bounding_box();
                    Point center = hole_bbox.center();
                    coord_t rx = hole_bbox.size().x() / 2;
                    coord_t ry = hole_bbox.size().y() / 2;

                    if (rx < scale_(1.0) || ry < scale_(1.0)) continue;

                    double overlap = std::max(scale_(0.5), (double)rx * 0.15);
                    
                    auto make_rect = [](coord_t x_min, coord_t y_min, coord_t x_max, coord_t y_max) {
                        Polygon p;
                        p.points.resize(4);
                        p.points[0] = Point(x_min, y_min);
                        p.points[1] = Point(x_max, y_min);
                        p.points[2] = Point(x_max, y_max);
                        p.points[3] = Point(x_min, y_max);
                        return p;
                    };

                    coord_t c_x = center.x();
                    coord_t c_y = center.y();
                    coord_t ovl = static_cast<coord_t>(overlap);
                    coord_t huge_len = scale_(1000.0);

                    // 1. Vertical Bars (Left / Right) -> Layer N-1
                    ExPolygons bridges_vertical;
                    {
                        bridges_vertical.push_back(ExPolygon(make_rect(
                            c_x - rx - huge_len, c_y - huge_len,
                            c_x - rx + ovl, c_y + huge_len
                        )));
                        bridges_vertical.push_back(ExPolygon(make_rect(
                            c_x + rx - ovl, c_y - huge_len,
                            c_x + rx + huge_len, c_y + huge_len
                        )));
                    }

                    // 2. Horizontal Bars (Top / Bottom) -> Layer N-2
                    ExPolygons bridges_horizontal;
                    {
                        bridges_horizontal.push_back(ExPolygon(make_rect(
                            c_x - huge_len, c_y - ry - huge_len,
                            c_x + huge_len, c_y - ry + ovl
                        )));
                        bridges_horizontal.push_back(ExPolygon(make_rect(
                            c_x - huge_len, c_y + ry - ovl,
                            c_x + huge_len, c_y + ry + huge_len
                        )));
                    }

                    bridges_vertical = intersection_ex(bridges_vertical, anchor_zone);
                    bridges_horizontal = intersection_ex(bridges_horizontal, anchor_zone);
                    
                    auto integrate_polys = [&](Layer* layer, const ExPolygons& polys_to_add, size_t target_region_id) {
                        // 1. Clip to Layer Footprint
                        Polygons layer_outlines;
                        for (const ExPolygon& exp : layer->lslices) {
                             layer_outlines.push_back(exp.contour);
                        }
                        ExPolygons footprint = union_ex(layer_outlines);
                        ExPolygons clipped_polys = intersection_ex(polys_to_add, footprint);
                        
                        if (clipped_polys.empty()) return;

                        // Strategy: Negative Mask (avoid neighbor holes)
                        Polygons neighbor_holes;
                        for (const ExPolygon& exp : layer->lslices) {
                            for (const Polygon& h : exp.holes) {
                                if (!h.contains(center)) {
                                    neighbor_holes.push_back(h);
                                }
                            }
                        }
                        
                        ExPolygons forbidden_area;
                        if (!neighbor_holes.empty()) {
                            forbidden_area = union_ex(neighbor_holes);
                        }
                        
                        ExPolygons valid_polys;
                        if (forbidden_area.empty()) {
                            valid_polys = clipped_polys;
                        } else {
                            valid_polys = diff_ex(clipped_polys, forbidden_area);
                        }
                        
                    
                        
                    if (valid_polys.empty()) return;
                        

                        
                    // Subtract the bridge area from ALL other regions to prevent overlap/collisions.
                        
                    // This creates space for the "Anchor" part of the bridge in the existing walls.
                        
                    for (size_t r_idx = 0; r_idx < layer->region_count(); ++r_idx) {
                        
                        if (r_idx == target_region_id) continue;
                        
                        
                        
                        LayerRegion* other_region = layer->get_region(r_idx);
                        
                        if (!other_region || other_region->slices.empty()) continue;
                        

                        
                        ExPolygons cut_polys = diff_ex(other_region->slices.surfaces, valid_polys);
                        
                        other_region->slices.surfaces.clear();
                        
                        other_region->slices.append(cut_polys, stInternal);
                        
                    }
                        

                        
                    // Merge into the SPECIFIC Region
                        
                    LayerRegion* region = layer->get_region(target_region_id);                        if (!region) return; // Should not happen given print_object structure

                        ExPolygons existing_polys = to_expolygons(region->slices.surfaces);
                        ExPolygons merged_polys = union_ex(existing_polys, valid_polys);
                        
                        region->slices.surfaces.clear();
                        region->slices.append(merged_polys, stInternal);
                        
                        // Update global lslices
                        layer->lslices = union_ex(layer->lslices, valid_polys);
                    };

                    // Apply to layers N-1 and N-2 using the CURRENT region_id
                    int layer_idx_N1 = (int)i - 1;
                    if (layer_idx_N1 >= 0) integrate_polys(m_layers[layer_idx_N1], bridges_vertical, region_id);

                    int layer_idx_N2 = (int)i - 2;
                    if (layer_idx_N2 >= 0) integrate_polys(m_layers[layer_idx_N2], bridges_horizontal, region_id);
                }
            }
        }
    }
}

} // namespace Slic3r