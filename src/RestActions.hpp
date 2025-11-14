#pragma once

namespace Slic3r { namespace Utils { class ActionRegister; } }

// Registers all REST actions (import, slice_plate, print_plate) into the
// provided registry. Uses nlohmann::json intrusive serialization for the
// internal request/response types defined in the implementation.
void register_rest_actions(Slic3r::Utils::ActionRegister& reg);
