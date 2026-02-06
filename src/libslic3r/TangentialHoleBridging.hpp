#ifndef slic3r_TangentialHoleBridging_hpp_
#define slic3r_TangentialHoleBridging_hpp_

namespace Slic3r {

class PrintObject;

class TangentialHoleBridging {
public:
    // Main entry point to apply the tangential sacrificial bridging
    // Modifies the layers of the print_object in-place.
    static void apply(PrintObject* print_object);
};

} // namespace Slic3r

#endif // slic3r_TangentialHoleBridging_hpp_
