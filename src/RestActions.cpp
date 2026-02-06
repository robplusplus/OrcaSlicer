#include "RestActions.hpp"
#include "RestUtils.hpp"

#include "nlohmann/json.hpp"
// Enable std::optional<T> support for JSON (local adapter) – include before using JSON in headers
#include "slic3r/Utils/json_optional.hpp"
#include "slic3r/Utils/ActionRegister.hpp"

#include <wx/app.h>
#include <boost/log/trivial.hpp>
#include <filesystem>
#include <future>
#include <type_traits>
#include <algorithm>
#include <regex>
#include <vector>
#include <string>
#include <cstdint>
#include <sstream>
#include <thread>
#include <atomic>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <cmath>

// GUI and printing includes
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include "slic3r/GUI/TaskManager.hpp"
#include "slic3r/GUI/Jobs/PrintJob.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
// AMS/filament structures
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
// presets
#include "libslic3r/PresetBundle.hpp"
// utils for md5
#include "libslic3r/Utils.hpp"
// bed type helper
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/libslic3r.h"

namespace BBL = Slic3r;

// task updates
#include "slic3r/Utils/ActionRegister.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/MainFrame.hpp"
// ...existing code...

using Slic3r::Utils::ActionRegister;
using Slic3r::Utils::TaskState;
using Slic3r::Utils::net_code_to_str;
using Slic3r::Utils::dump_print_params;

namespace {

struct BaseResult {
    std::string status;
    std::string message;
};

struct DefaultResult : BaseResult {
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(DefaultResult, status, message)
};

struct ImportParams {
    std::string path;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ImportParams, path)
};

struct SliceParams {
    std::optional<int> plate_index; // optional: defaults to current plate
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SliceParams, plate_index)
};

struct PrintParamsReq {
    std::optional<int> plate_index; // optional: defaults to current plate when all=false
    bool all = false;
    std::string dev_id;
    std::string dev_ip;
    bool retry_mqtt_toggle = false; // optional safety toggle for a second attempt
    // Optional AMS mapping overrides; if absent/null, current device settings will be used
    std::optional<std::string> ams_mapping;      // e.g. "[2,-1,-1,-1]"
    std::optional<std::string> ams_mapping2;     // e.g. JSON array of {ams_id,slot_id}
    std::optional<std::string> ams_mapping_info; // e.g. detailed mapping info
    std::optional<std::string> nozzles_info;     // optional nozzle info json
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(PrintParamsReq, plate_index, all, dev_id, dev_ip, retry_mqtt_toggle, ams_mapping, ams_mapping2, ams_mapping_info, nozzles_info)
};

// Devices listing
struct DevicesParams {
    bool include_local = true; // optional in JSON; defaults to true
    bool include_user  = true; // optional in JSON; defaults to true
};

struct SelectTabParams {
    std::string tab;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SelectTabParams, tab)
};

inline void to_json(nlohmann::json& j, const DevicesParams& p)
{
    // Only include keys when they differ from defaults, to keep payload compact
    j = nlohmann::json::object();
    if (p.include_local != true) j["include_local"] = p.include_local;
    if (p.include_user  != true) j["include_user"]  = p.include_user;
}

inline void from_json(const nlohmann::json& j, DevicesParams& p)
{
    // Gracefully handle missing fields by defaulting to true
    p.include_local = j.value("include_local", true);
    p.include_user  = j.value("include_user",  true);
}

struct DeviceInfo {
    std::string dev_id;
    std::string dev_name;
    std::string dev_ip;
    std::string connection_type;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(DeviceInfo, dev_id, dev_name, dev_ip, connection_type)
};
struct DevicesResult : BaseResult {
    std::vector<DeviceInfo> devices;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(DevicesResult, status, message, devices)
};

// Get logs
struct GetLogsParams {
    std::string file;      // optional: exact filename to read (basename only)
    std::uint64_t max_bytes = 65536; // tail bytes from end; defaults to 64 KiB
};

inline void to_json(nlohmann::json& j, const GetLogsParams& p)
{
    j = nlohmann::json::object();
    if (!p.file.empty()) j["file"] = p.file;
    if (p.max_bytes != 65536ULL) j["max_bytes"] = p.max_bytes;
}

inline void from_json(const nlohmann::json& j, GetLogsParams& p)
{
    p.file = j.value("file", std::string());
    p.max_bytes = j.value("max_bytes", 65536ULL);
}

struct GetLogsResult : BaseResult {
    std::string file;      // resolved filename returned
    std::string content;   // UTF-8 text content (tail)
    bool         truncated = false;
    std::uint64_t size = 0;      // total file size
    std::uint64_t returned = 0;  // bytes returned in content
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(GetLogsResult, status, message, file, content, truncated, size, returned)
};

// Task status query
struct TaskStatusParams { std::string id; NLOHMANN_DEFINE_TYPE_INTRUSIVE(TaskStatusParams, id) };
struct TaskStatusResult : BaseResult { nlohmann::json task; NLOHMANN_DEFINE_TYPE_INTRUSIVE(TaskStatusResult, status, message, task) };

// Cancel print params/results (must be at namespace scope; MSVC rejects friend defs inside local classes)
struct CancelParams {
    std::string dev_id;
    std::string job_id;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(CancelParams, dev_id, job_id)
};

// Clear plate params/results
struct ClearPlateParams {
    std::optional<int> plate_index; // optional, clear current plate when not provided
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ClearPlateParams, plate_index)
};

// AMS filament listing params/results
struct AmsFilamentsParams {
    // Optional target device; if omitted/null, uses the currently selected device
    std::optional<std::string> dev_id;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AmsFilamentsParams, dev_id)
};

struct AmsTrayInfo {
    std::string ams_id;          // AMS identifier (string)
    int         ams_index = -1;  // AMS index when numeric, else -1
    std::string tray_id;         // tray/slot identifier (string)
    int         slot_index = -1; // slot index when numeric, else -1
    std::string filament_id;     // resolved filament id
    std::string filament_type;   // resolved filament type
    std::string color;           // hex color string if available
    bool        is_bbl = false;  // whether it’s a Bambu spool
    bool        exists = false;  // tray reported present
    int         remain = -1;     // % remaining if known
    std::string nozzle_temp_min; // from tray meta if available
    std::string nozzle_temp_max; // from tray meta if available
    std::string setting_id;      // tray_info_idx
    std::string filament_setting_id; // setting_id
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AmsTrayInfo, ams_id, ams_index, tray_id, slot_index, filament_id, filament_type, color, is_bbl, exists, remain, nozzle_temp_min, nozzle_temp_max, setting_id, filament_setting_id)
};

struct AmsFilamentsResult : BaseResult {
    std::vector<AmsTrayInfo> trays;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AmsFilamentsResult, status, message, trays)
};

struct SyncAMSParams {
    bool overwrite;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SyncAMSParams, overwrite)
};

// --- Helper Functions ---

Slic3r::GUI::GUI_App* get_gui_app(std::string& error_msg) {
    wxAppConsole* raw_app = wxAppConsole::GetInstance();
    if (!raw_app) { error_msg = "no wxApp instance"; return nullptr; }
    auto* gapp = dynamic_cast<Slic3r::GUI::GUI_App*>(raw_app);
    if (!gapp) { error_msg = "wxApp is not GUI_App"; return nullptr; }
    return gapp;
}

void task_log(const std::string& tid, const std::string& msg, bool is_error = false) {
    if (is_error) BOOST_LOG_TRIVIAL(error) << "[RestServer] " << msg;
    else BOOST_LOG_TRIVIAL(info) << "[RestServer] " << msg;
    Slic3r::Utils::append_action_task_log(tid, msg);
}

void task_update(const std::string& tid, TaskState status, const std::string& msg) {
    BOOST_LOG_TRIVIAL(info) << "[RestServer] task_update: tid=" << tid << " status=" << Slic3r::Utils::to_string(status) << " msg=" << msg;
    Slic3r::Utils::update_action_task(tid, status, msg);
}

void task_update(const std::string& tid, const std::string& status, const std::string& msg) {
    BOOST_LOG_TRIVIAL(info) << "[RestServer] task_update: tid=" << tid << " status=" << status << " msg=" << msg;
    Slic3r::Utils::update_action_task(tid, status, msg);
}

void fill_tray_details(AmsTrayInfo& info, Slic3r::MachineObject* mo, Slic3r::DevAmsTray* t, const std::string& ams_id, const std::string& tray_id) {
    try { info.filament_id = mo->get_filament_id(ams_id, tray_id); } catch (...) {}
    try { info.filament_type = mo->get_filament_type(ams_id, tray_id); } catch (...) {}
    
    info.color = t->color;
    info.is_bbl = t->is_bbl;
    info.exists = t->is_exists;
    info.remain = t->remain;
    info.nozzle_temp_min = t->nozzle_temp_min;
    info.nozzle_temp_max = t->nozzle_temp_max;
    info.setting_id = t->setting_id;
    info.filament_setting_id = t->filament_setting_id;
}

void populate_ams_trays(Slic3r::MachineObject* mo, std::vector<AmsTrayInfo>& trays) {
    auto* fs = mo->GetFilaSystem();
    if (!fs) return;
    auto& ams_list = fs->GetAmsList();
    for (auto &ams_pair : ams_list) {
        const std::string ams_id = ams_pair.first;
        int ams_index = -1;
        try { ams_index = static_cast<int>(std::stoi(ams_id)); } catch (...) { ams_index = -1; }
        Slic3r::DevAms* ams = ams_pair.second;
        if (!ams) continue;
        const auto& tray_map = ams->GetTrays();
        for (const auto &tray_pair : tray_map) {
            const std::string tray_id = tray_pair.first;
            Slic3r::DevAmsTray* t = tray_pair.second;
            if (!t) continue;
            int slot_index = -1;
            try { slot_index = static_cast<int>(std::stoi(tray_id)); } catch (...) { slot_index = -1; }

            AmsTrayInfo info;
            info.ams_id      = ams_id;
            info.ams_index   = ams_index;
            info.tray_id     = tray_id;
            info.slot_index  = slot_index;
            
            fill_tray_details(info, mo, t, ams_id, tray_id);
            trays.emplace_back(std::move(info));
        }
    }
}

void populate_ams_trays_fallback(Slic3r::MachineObject* mo, std::vector<AmsTrayInfo>& trays) {
    if (!trays.empty() || !mo->HasAms()) return;
    try {
        for (int ams_idx = 0; ams_idx < 4; ++ams_idx) { // typical AMS count
            std::string ams_id = std::to_string(ams_idx);
            for (int slot_idx = 0; slot_idx < 6; ++slot_idx) { // some models have up to 6; most 4
                std::string tray_id = std::to_string(slot_idx);
                Slic3r::DevAmsTray* t = mo->get_ams_tray(ams_id, tray_id);
                if (!t) continue;
                // Heuristic: consider a tray valid if exists, is_bbl, or has any identifying info
                bool any_info = t->is_exists || t->is_bbl || !t->filament_setting_id.empty() || !t->setting_id.empty() || !t->color.empty();
                if (!any_info) continue;
                AmsTrayInfo info;
                info.ams_id      = ams_id;
                info.ams_index   = ams_idx;
                info.tray_id     = tray_id;
                info.slot_index  = slot_idx;
                
                fill_tray_details(info, mo, t, ams_id, tray_id);
                trays.emplace_back(std::move(info));
            }
        }
    } catch (...) { /* fallback best-effort */ }
}

// Helper to prepare PrintParams from request and current state
bool prepare_print_params(Slic3r::GUI::GUI_App* gapp, const PrintParamsReq& in, const std::string& tid, BBL::PrintParams& params) {
    Slic3r::GUI::Plater* plater = gapp->plater_;
    if (!plater) { 
        task_log(tid, "UI thread: Plater not available; cannot print", true); 
        return false; 
    }
    task_log(tid, "UI thread: Plater found, preparing PrintPrepareData");

    int target_idx = (in.plate_index && *in.plate_index >= 0) ? *in.plate_index : plater->get_partplate_list().get_curr_plate_index();
    task_log(tid, std::string("UI thread: exporting 3mf for plate_index=") + std::to_string(target_idx));
    int export_res = plater->send_gcode(target_idx, nullptr);
    if (export_res < 0) {
        task_update(tid, TaskState::Failed, "export 3mf failed");
        task_log(tid, std::string("UI thread: export 3mf failed (code=") + std::to_string(export_res) + ")", true);
        return false;
    }

    // Also export the config 3mf so params.config_filename is valid (fixes -2030 upload config failure)
    task_log(tid, "exporting config 3mf");
    int cfg_res = plater->export_config_3mf(target_idx, nullptr);
    if (cfg_res < 0) {
        task_update(tid, TaskState::Failed, "export config 3mf failed");
        task_log(tid, std::string("UI thread: export config 3mf failed (code=") + std::to_string(cfg_res) + ")", true);
        return false;
    }

    Slic3r::GUI::PrintPrepareData job_data; plater->get_print_job_data(&job_data);
    task_log(tid, std::string("UI thread: got PrintPrepareData, 3mf_path='") + job_data._3mf_path.string() + "' config_3mf='" + job_data._3mf_config_path.string() + "'");

    if (plater->using_exported_file()) params.filename = plater->get_3mf_filename();
    else params.filename = job_data._3mf_path.string();
    params.config_filename = job_data._3mf_config_path.string();
    int curr_plate_idx = (job_data.plate_idx >= 0) ? (job_data.plate_idx + 1) : (plater->get_partplate_list().get_curr_plate_index() + 1);
    params.plate_index = curr_plate_idx;
    params.print_type = "from_normal";
    params.connection_type = "lan"; // ensure LAN flow
    // Align defaults with UI typical values for A1/A1M
    params.task_bed_leveling   = true;
    params.task_flow_cali      = true;
    params.task_vibration_cali = false;
    params.task_layer_inspect  = true;
    params.task_record_timelapse = false;
    params.task_use_ams        = true;
    params.task_bed_type.clear();
    params.extra_options.clear();
    // Common auto_* defaults seen in UI flows
    params.auto_bed_leveling = 1;
    params.auto_flow_cali    = 1;
    params.auto_offset_cali  = 2;
    // Derive ftp_file from filename for LAN FTP paths and ensure it doesn't start with a '.'
    try {
        params.ftp_file = boost::filesystem::path(params.filename).filename().string();
        if (!params.ftp_file.empty() && params.ftp_file.front() == '.') {
            params.ftp_file.erase(0, 1);
        }
        // For LAN local print, let the networking lib derive final name; empty is acceptable
        params.ftp_file.clear();
    } catch (...) {}
    // Compute MD5 for the upload file if available
    try {
        if (!params.filename.empty()) {
            // For LAN path, let the library or device handle MD5; avoid mismatches by not setting it here
            params.ftp_file_md5.clear();
        }
    } catch (...) {}

    // Populate bed type to match UI (e.g., "textured_plate")
    try {
        if (auto* plate = plater->get_partplate_list().get_curr_plate()) {
            auto bed_type = plate->get_bed_type(true);
            params.task_bed_type = Slic3r::bed_type_to_gcode_string(bed_type);
        }
    } catch (...) {}
    // Provide reasonable names like UI does
    try {
        std::string proj = gapp->plater_->get_project_name().ToUTF8().data();
        if (proj.empty()) proj = "project";
        // match UI: project_name used for job naming, keep under ~100 chars implicitly
        params.project_name = proj;
        // Derive a simple task name
        params.task_name = proj;
    } catch (...) {}
    if (gapp->preset_bundle) {
        try { params.preset_name = gapp->preset_bundle->prints.get_selected_preset_name(); } catch (...) {}
    }
    if (!in.dev_id.empty()) params.dev_id = in.dev_id;
    if (!in.dev_ip.empty()) params.dev_ip = in.dev_ip;

    return true;
}

struct SelectPrinterParams {
    std::string dev_id;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SelectPrinterParams, dev_id)
};

struct SelectPresetParams {
    std::string name;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SelectPresetParams, name)
};

struct ListPresetsParams {
    bool include_invisible = false;
};

inline void to_json(nlohmann::json& j, const ListPresetsParams& p) {
    j = nlohmann::json::object();
    if (p.include_invisible) j["include_invisible"] = p.include_invisible;
}

inline void from_json(const nlohmann::json& j, ListPresetsParams& p) {
    p.include_invisible = j.value("include_invisible", false);
}

struct PresetInfo {
    std::string name;
    bool is_system;
    bool is_user;
    bool is_visible;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(PresetInfo, name, is_system, is_user, is_visible)
};

struct ListPrintersResult : BaseResult {
    std::vector<PresetInfo> printers;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ListPrintersResult, status, message, printers)
};

struct ListPrintProfilesResult : BaseResult {
    std::vector<PresetInfo> profiles;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ListPrintProfilesResult, status, message, profiles)
};



struct PaintHeightRangeParams {
    int object_index = 0;
    float start_z = 0.0f;
    float end_z = 0.0f;
    int filament_id = 1;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(PaintHeightRangeParams, object_index, start_z, end_z, filament_id)
};

struct ArrangePrimeTowerParams {};
inline void to_json(nlohmann::json& j, const ArrangePrimeTowerParams& p) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json& j, ArrangePrimeTowerParams& p) {}

std::string arrange_prime_tower_impl(Slic3r::GUI::GUI_App* gapp) {
    BOOST_LOG_TRIVIAL(info) << "arrange_prime_tower_impl: enter";
    if (!gapp) return "gapp is null";
    try {
        if (!gapp->plater() || !gapp->preset_bundle) return "plater or preset_bundle null";
        
        auto* plater = gapp->plater();
        // Check if prints collection is empty (using size() or iterators if empty() is missing)
        if (gapp->preset_bundle->prints.begin() == gapp->preset_bundle->prints.end()) return "no prints";

        // Access the edited preset config directly
        auto& print_config = gapp->preset_bundle->prints.get_edited_preset().config;
        
        if (!print_config.has("enable_prime_tower") || !print_config.opt_bool("enable_prime_tower")) {
             BOOST_LOG_TRIVIAL(info) << "arrange_prime_tower: enable_prime_tower is false or missing";
             return "enable_prime_tower is false or missing";
        }
        
        auto* plate = plater->get_partplate_list().get_curr_plate();
        if (!plate) return "no current plate";
        
        // Get bed bounding box (unscaled)
        Slic3r::BoundingBoxf3 bed_bbox_f = plate->get_build_volume(true);
        if (bed_bbox_f.min.x() >= bed_bbox_f.max.x() || bed_bbox_f.min.y() >= bed_bbox_f.max.y()) return "invalid bed bbox";
        
        // Scale to coord_t for WipeTower::move_box_inside_box
        Slic3r::BoundingBox bed_bbox_scaled(
            Slic3r::Point::new_scale(bed_bbox_f.min.x(), bed_bbox_f.min.y()),
            Slic3r::Point::new_scale(bed_bbox_f.max.x(), bed_bbox_f.max.y())
        );
        
        auto& project_config = gapp->preset_bundle->project_config;
        if (!project_config.has("wipe_tower_x") || !project_config.has("wipe_tower_y")) {
             BOOST_LOG_TRIVIAL(error) << "arrange_prime_tower: wipe_tower_x/y missing in project config";
             return "wipe_tower_x/y missing in project config";
        }
        if (!print_config.has("prime_tower_width")) {
             BOOST_LOG_TRIVIAL(error) << "arrange_prime_tower: prime_tower_width missing in print config";
             return "prime_tower_width missing in print config";
        }

        int plate_idx = plater->get_partplate_list().get_curr_plate_index();
        if (plate_idx < 0) return "plate_idx < 0";

        auto* opt_x = dynamic_cast<Slic3r::ConfigOptionFloats*>(project_config.option("wipe_tower_x"));
        auto* opt_y = dynamic_cast<Slic3r::ConfigOptionFloats*>(project_config.option("wipe_tower_y"));

        if (!opt_x || !opt_y) return "opt_x/y null";
        if (plate_idx >= opt_x->values.size() || plate_idx >= opt_y->values.size()) {
             BOOST_LOG_TRIVIAL(error) << "arrange_prime_tower: plate_idx out of bounds";
             return "plate_idx out of bounds";
        }

        // Try to find wipe tower volume
        auto* canvas = gapp->plater()->canvas3D();
        if (canvas) {
             for (auto* vol : canvas->get_volumes().volumes) {
                 if (vol && vol->is_wipe_tower) {
                     Slic3r::Vec3d tower_origin = vol->get_volume_offset();
                     Slic3r::BoundingBoxf3 tower_bbox = vol->bounding_box();
                     tower_bbox.translate(tower_origin);
                     
                     Slic3r::BoundingBox tower_bbox2d(
                         Slic3r::Point::new_scale(tower_bbox.min.x(), tower_bbox.min.y()),
                         Slic3r::Point::new_scale(tower_bbox.max.x(), tower_bbox.max.y())
                     );
                     
                     auto* print = plate->fff_print();
                     bool show_read_wipe_tower = print && print->is_step_done(Slic3r::psWipeTower);
                     double brim_width = print_config.opt_float("prime_tower_brim_width");
                     double margin = show_read_wipe_tower ? ::WIPE_TOWER_MARGIN : brim_width + 0.5;
                     
                     Slic3r::Vec2f offset = Slic3r::WipeTower::move_box_inside_box(tower_bbox2d, bed_bbox_scaled, scale_(margin));
                     
                     if (offset.x() != 0 || offset.y() != 0) {
                         double new_x = tower_origin.x() + offset.x();
                         double new_y = tower_origin.y() + offset.y();
                         
                         opt_x->values[plate_idx] = new_x;
                         opt_y->values[plate_idx] = new_y;

                         if (plate_idx < plater->model().wipe_tower.positions.size()) {
                             plater->model().wipe_tower.positions[plate_idx] = Slic3r::Vec2d(new_x, new_y);
                         }
                         plater->update();

                         BOOST_LOG_TRIVIAL(info) << "arrange_prime_tower: moved existing tower to " << new_x << ", " << new_y;
                     } else {
                         BOOST_LOG_TRIVIAL(info) << "arrange_prime_tower: existing tower already in position";
                     }
                     return "";
                 }
             }
        }

        double x = opt_x->values[plate_idx];
        double y = opt_y->values[plate_idx];
        double width = print_config.opt_float("prime_tower_width");
        double angle = 0.0;
        if (project_config.has("wipe_tower_rotation_angle")) {
            angle = project_config.opt_float("wipe_tower_rotation_angle");
        } else if (print_config.has("wipe_tower_rotation_angle")) {
            angle = print_config.opt_float("wipe_tower_rotation_angle");
        }
        double brim_width = print_config.opt_float("prime_tower_brim_width");
        
        BOOST_LOG_TRIVIAL(info) << "arrange_prime_tower: calculating new pos. x=" << x << " y=" << y << " w=" << width << " a=" << angle;

        if (width <= 0.001) return "width too small";

        // Assume square if depth unknown (it depends on tool changes), or use width as approximation
        double depth = width; 
        
        // Calculate rotated bounding box
        double angle_rad = angle * M_PI / 180.0;
        double c = cos(angle_rad);
        double s = sin(angle_rad);
        
        Slic3r::BoundingBox tower_bbox_scaled;
        
        // Rotate around center (width/2, depth/2)
        double cx = width / 2.0;
        double cy = depth / 2.0;
        
        double pts[4][2] = { {0,0}, {width,0}, {width,depth}, {0,depth} };
        
        for(int i=0; i<4; ++i) {
            double px = pts[i][0];
            double py = pts[i][1];
            
            // Rotate around center
            double dx = px - cx;
            double dy = py - cy;
            
            double rx = dx * c - dy * s;
            double ry = dx * s + dy * c;
            
            // Translate back to center, then to (x,y)
            tower_bbox_scaled.merge(Slic3r::Point::new_scale(x + cx + rx, y + cy + ry));
        }
        
        // Use margin logic similar to Selection.cpp
        // If brim is auto (-1), use a safe default or 2.0 margin
        float margin = (brim_width > 0) ? (float)brim_width + 0.5f : 2.0f;
        
        Slic3r::Vec2f offset = Slic3r::WipeTower::move_box_inside_box(tower_bbox_scaled, bed_bbox_scaled, scale_(margin));
        
        if (offset.x() != 0 || offset.y() != 0) {
            double new_x = x + offset.x();
            double new_y = y + offset.y();

            opt_x->values[plate_idx] = new_x;
            opt_y->values[plate_idx] = new_y;

            if (plate_idx < plater->model().wipe_tower.positions.size()) {
                plater->model().wipe_tower.positions[plate_idx] = Slic3r::Vec2d(new_x, new_y);
            }
            plater->update();

            BOOST_LOG_TRIVIAL(info) << "arrange_prime_tower: moved to " << new_x << ", " << new_y;
        } else {
            BOOST_LOG_TRIVIAL(info) << "arrange_prime_tower: already in position";
        }
        
        return "";
    } catch (const std::exception& ex) {
        return std::string("Exception: ") + ex.what();
    } catch (...) {
        return "Unknown exception";
    }
}

struct SetObjectFilamentParams {
    int object_index = 0;
    int filament_id = 1; // 1-based extruder index
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SetObjectFilamentParams, object_index, filament_id)
};

struct ArrangeObjectsParams {};
inline void to_json(nlohmann::json& j, const ArrangeObjectsParams& p) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json& j, ArrangeObjectsParams& p) {}

// Note: optional parameters (e.g., plate_index) use the intrusive macro
// and should be passed as null when omitted, e.g., {"plate_index": null}.

class TaskContext {
public:
    TaskContext(const std::string& name)
    {
        tid_ = Slic3r::Utils::current_action_task_id();

        std::string err;
        gapp_ = get_gui_app(err);
        if (!gapp_) {
            Slic3r::Utils::update_action_task(tid_, TaskState::Failed, "no GUI_App instance: " + err);
            Slic3r::Utils::append_action_task_log(tid_, "no GUI_App instance: " + err);
            return;
        }
     
        Slic3r::Utils::update_action_task(tid_, TaskState::Scheduled, name + " scheduled");
        Slic3r::Utils::append_action_task_log(tid_, name + " scheduled");
    }

    ~TaskContext()
    {
        // Destructor
    }

    void log(const std::string& msg, bool is_error = false)
    {
        if (is_error) BOOST_LOG_TRIVIAL(error) << "[RestServer] " << msg;
        else BOOST_LOG_TRIVIAL(info) << "[RestServer] " << msg;
        Slic3r::Utils::append_action_task_log(tid_, msg);
    }

    void update(TaskState status, const std::string& msg)
    {
        state_ = status;
        BOOST_LOG_TRIVIAL(info) << "[RestServer] task_update: tid=" << tid_ << " status=" << Slic3r::Utils::to_string(status) << " msg=" << msg;
        Slic3r::Utils::update_action_task(tid_, status, msg);
    }

    TaskState state() const { return state_; }

    Slic3r::GUI::GUI_App* gapp() const { return gapp_; }
    const std::string& task_id() const { return tid_; }
    bool is_valid() const { return gapp_ != nullptr; }

private:
    std::string tid_;
    Slic3r::GUI::GUI_App* gapp_ = nullptr;  
    TaskState state_ = TaskState::Scheduled;
};

template <class Args, class Return, typename Func>
void registerAsyncAction(ActionRegister& reg, const std::string& name, Func func)
{
   reg.register_action<Args, Return>(name, [func, name](const Args& in) -> Return {
        auto context = std::make_shared<TaskContext>(name);
        if (!context->is_valid()) {
            Return ret; ret.status = "error"; ret.message = "no GUI_App instance"; return ret;
        }

        auto prom = std::make_shared<std::promise<Return>>();
        auto fut  = prom->get_future();
        context->gapp()->CallAfter([prom, func, in, context]() mutable {
            try {
                context->update(TaskState::Running, "running");
                context->log("running on UI thread");
                
                using ResultType = std::invoke_result_t<Func, const Args&, std::shared_ptr<TaskContext>>;
                if constexpr (std::is_void_v<ResultType>) {
                    func(in, context);
                    if (context->state() == TaskState::Running) {
                        context->update(TaskState::Completed, "ok");
                    }
                    Return result; result.status = "ok"; result.message = "ok";
                    prom->set_value(result);
                } else if constexpr (std::is_same_v<ResultType, bool>) {
                    bool ok = func(in, context);
                    if (ok) {
                        if (context->state() == TaskState::Running) {
                            context->update(TaskState::Completed, "ok");
                        }
                        Return result; result.status = "ok"; result.message = "ok";
                        prom->set_value(result);
                    } else {
                        Return result; result.status = "error"; result.message = "failed";
                        context->update(TaskState::Failed, "failed");
                        prom->set_value(result);
                    }
                } else {
                    Return result = func(in, context);
                    prom->set_value(result);
                }
            } catch (const std::exception& ex) {
                Return err; err.status = "error"; err.message = std::string("exception: ") + ex.what();
                context->update(TaskState::Failed, err.message);
                prom->set_value(std::move(err));
            } catch (...) {
                Return err; err.status = "error"; err.message = "unknown exception";
                context->update(TaskState::Failed, err.message);
                prom->set_value(std::move(err));
            }
        });

        // Short wait for immediate result; otherwise return scheduled
        if (fut.wait_for(std::chrono::seconds(1)) == std::future_status::ready) return fut.get();
        Return out; out.status = "ok"; out.message = "scheduled"; return out;
    });
}


// Helper for listing presets
std::vector<PresetInfo> list_presets_impl(Slic3r::GUI::GUI_App* gapp, const ListPresetsParams& in, const Slic3r::PresetCollection& collection) {
    std::vector<PresetInfo> out;
    for (const auto& preset : collection) {
        if (preset.is_visible || in.include_invisible) {
            PresetInfo info;
            info.name = preset.name;
            info.is_system = preset.is_system;
            info.is_user = preset.is_user();
            info.is_visible = preset.is_visible;
            out.push_back(std::move(info));
        }
    }
    return out;
}

// Helper for selecting presets
void select_preset_impl(std::shared_ptr<TaskContext> context, const SelectPresetParams& in, Slic3r::Preset::Type type, Slic3r::PresetCollection& collection) {
    if (in.name.empty()) throw std::runtime_error("missing name");
    
    auto* gapp = context->gapp();
    if (!gapp->preset_bundle) throw std::runtime_error("PresetBundle not available");

    std::string type_str = (type == Slic3r::Preset::TYPE_PRINTER) ? "printer" : "print profile";
    context->log("select " + type_str + " scheduled name='" + in.name + "'");
    context->update(TaskState::Running, "selecting " + type_str);
    
    bool success = false;
    if (Slic3r::GUI::Tab* tab = gapp->get_tab(type)) {
        success = tab->select_preset(in.name);
    } else {
        success = collection.select_preset_by_name(in.name, true);
    }

    if (!success) throw std::runtime_error("preset not found or selection failed");

    context->log(type_str + " selected");
    arrange_prime_tower_impl(gapp);
}

} // namespace

void register_rest_actions(ActionRegister& reg)
{
    // cancel_print: {"action":"cancel_print","dev_id":"...","job_id":"..."}
    registerAsyncAction<CancelParams, DefaultResult>(reg, "cancel_print", [](const CancelParams& in, std::shared_ptr<TaskContext> context) {
        context->log(std::string("cancel scheduled dev_id='") + in.dev_id + "' job_id='" + in.job_id + "'");

        auto* gapp = context->gapp();
        auto* dm = gapp->getDeviceManager();
        if (!dm) throw std::runtime_error("no DeviceManager");

        Slic3r::MachineObject* mo = nullptr;
        if (!in.dev_id.empty()) {
            mo = dm->get_local_machine(in.dev_id);
            if (!mo) mo = dm->get_user_machine(in.dev_id);
        } else {
            mo = dm->get_selected_machine();
        }
        if (!mo) { context->log("device not found"); throw std::runtime_error("device not found"); }

        int ret = 0;
        if (!in.job_id.empty()) {
            context->log(std::string("sending command_task_cancel job_id=") + in.job_id);
            ret = mo->command_task_cancel(in.job_id);
        } else {
            context->log("sending command_task_abort (no job_id provided)");
            ret = mo->command_task_abort();
        }

        if (ret != 0) {
            std::string msg = std::string("cancel failed: ") + std::to_string(ret);
            throw std::runtime_error(msg);
        }

        context->log("cancel sent");
    });


    
    // clear_plate: {"action":"clear_plate","plate_index":0} (plate_index optional; clears current when omitted)
    registerAsyncAction<ClearPlateParams, DefaultResult>(reg, "clear_plate", [](const ClearPlateParams& in, std::shared_ptr<TaskContext> context) {
        if (in.plate_index && *in.plate_index >= 0)
            context->log(std::string("clear scheduled plate_index=") + std::to_string(*in.plate_index));
        else
            context->log("clear scheduled (current plate)");

        Slic3r::GUI::Plater* plater = context->gapp()->plater_;
        if (!plater) throw std::runtime_error("Plater not available");

        if (in.plate_index && *in.plate_index >= 0)
            plater->select_plate(*in.plate_index, true);
        plater->remove_curr_plate_all();
        context->log("plate cleared");
    });

    // ams_filaments: {"action":"ams_filaments","dev_id":"<optional>"}
    reg.register_action<AmsFilamentsParams, AmsFilamentsResult>("ams_filaments", [](const AmsFilamentsParams& in) -> AmsFilamentsResult {
        AmsFilamentsResult out; out.status = "error";
        std::string err;
        auto* gapp = get_gui_app(err);
        if (!gapp) { out.message = err; return out; }

        auto prom = std::make_shared<std::promise<AmsFilamentsResult>>();
        auto fut  = prom->get_future();
        gapp->CallAfter([prom, gapp, in]() mutable {
            AmsFilamentsResult result; result.status = "ok"; result.message = "ok";
            try {
                auto* dm = gapp->getDeviceManager();
                if (!dm) { result.status = "error"; result.message = "no DeviceManager"; prom->set_value(std::move(result)); return; }
                Slic3r::MachineObject* mo = nullptr;
                if (in.dev_id && !in.dev_id->empty()) {
                    mo = dm->get_local_machine(*in.dev_id);
                    if (!mo) mo = dm->get_user_machine(*in.dev_id);
                } else {
                    mo = dm->get_selected_machine();
                }
                if (!mo) { result.status = "error"; result.message = "device not found"; prom->set_value(std::move(result)); return; }
                // Ensure a live connection so AMS data is populated
                try {
                    if (auto* agent = gapp->getAgent()) {
                        try { agent->set_user_selected_machine(mo->get_dev_id()); } catch (...) {}
                        std::string dev_id = mo->get_dev_id();
                        std::string dev_ip = mo->get_dev_ip();
                        std::string username = "bblp";
                        std::string password = mo->get_access_code();
                        bool use_ssl_mqtt = mo->local_use_ssl;
                        int conn_ret = agent->connect_printer(dev_id, dev_ip, username, password, use_ssl_mqtt);
                        (void)conn_ret; // best-effort; AMS listing still attempted below
                    }
                } catch (...) {}
                // Proactively request a state push so AMS/tray data is populated even if the UI Device tab wasn't visited
                try { mo->command_request_push_all(true); } catch (...) {}

                populate_ams_trays(mo, result.trays);
                populate_ams_trays_fallback(mo, result.trays);

                if (!result.trays.empty()) { prom->set_value(std::move(result)); return; }
                // If still empty, do a single delayed retry to give the push time to arrive without blocking UI thread
                try {
                    auto done = std::make_shared<std::atomic<bool>>(false);
                    std::thread([prom, gapp, mo, done]() {
                        using namespace std::chrono_literals;
                        std::this_thread::sleep_for(900ms);
                        gapp->CallAfter([prom, gapp, mo, done]() mutable {
                            if (done->exchange(true)) return; // ensure single set
                            AmsFilamentsResult retry; retry.status = "ok"; retry.message = "ok";
                            try {
                                populate_ams_trays(mo, retry.trays);
                            } catch (...) {}
                            // If still empty, just return empty ok
                            prom->set_value(std::move(retry));
                        });
                    }).detach();
                } catch (...) {
                    // On failure to spawn retry thread, return immediately
                    prom->set_value(std::move(result));
                }
            } catch (const std::exception& ex) {
                AmsFilamentsResult err; err.status = "error"; err.message = std::string("exception: ") + ex.what();
                prom->set_value(std::move(err));
            } catch (...) {
                AmsFilamentsResult err; err.status = "error"; err.message = "unknown exception";
                prom->set_value(std::move(err));
            }
        });

        // Allow up to ~2s to cover connect + single delayed retry
        if (fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready) return fut.get();
        out.status = "error"; out.message = "timeout waiting for GUI thread"; return out;
    });
    // import: {"action":"import","path":"C:\\file.stl"}
    registerAsyncAction<ImportParams, DefaultResult>(reg, "import", [](const ImportParams& in, std::shared_ptr<TaskContext> context) {
        if (in.path.empty()) throw std::runtime_error("missing path");
        
        context->log(std::string("import scheduled path='") + in.path + "'");
        
        Slic3r::GUI::Plater* plater = context->gapp()->plater_;
        if (!plater) throw std::runtime_error("Plater not available");
        
        std::vector<std::string> files { in.path };
        plater->load_files(files);
        
        context->log("import submitted to Plater");
        context->log("import finished");
    });



    // slice_plate: {"action":"slice_plate","plate_index":0}
    registerAsyncAction<SliceParams, DefaultResult>(reg, "slice_plate", [](const SliceParams& in, std::shared_ptr<TaskContext> context) {
        if (in.plate_index && *in.plate_index < 0) throw std::runtime_error("invalid plate_index");
        
        if (in.plate_index && *in.plate_index >= 0)
            context->log(std::string("slice scheduled plate_index=") + std::to_string(*in.plate_index));
        else
            context->log("slice scheduled (current plate)");

        Slic3r::GUI::Plater* plater = context->gapp()->plater_;
        if (!plater) throw std::runtime_error("Plater not available");
        
        int idx = (in.plate_index && *in.plate_index >= 0) ? *in.plate_index : plater->get_partplate_list().get_curr_plate_index();
        plater->select_plate(idx, true);
        plater->reslice();
        
        context->update(TaskState::Submitted, "slice submitted");
        context->log("slice submitted");
        
        // Watch for slicing to finish and flip state to success
        try {
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::string tid = context->task_id();
            auto gapp = context->gapp();
            std::thread([gapp, tid, idx, done]() {
                using namespace std::chrono;
                auto deadline = steady_clock::now() + minutes(30);
                while (!done->load()) {
                    std::this_thread::sleep_for(milliseconds(500));
                    if (steady_clock::now() > deadline) {
                        // Timeout – mark as failed
                        gapp->CallAfter([tid]() {
                            task_log(tid, "slice timeout after 30 minutes");
                            task_update(tid, TaskState::Failed, "slice timeout");
                        });
                        done->store(true);
                        break;
                    }
                    gapp->CallAfter([gapp, tid, idx, done]() {
                        if (!gapp || !gapp->plater_) return;
                        auto* pl = gapp->plater_;
                        // If still slicing, keep waiting
                        if (pl->is_background_process_slicing()) return;
                        // Check target plate readiness
                        auto& ppl = pl->get_partplate_list();
                        Slic3r::GUI::PartPlate* plate = nullptr;
                        try { plate = ppl.get_plate(idx); } catch (...) { plate = nullptr; }
                        if (plate && plate->is_slice_result_valid() && plate->is_slice_result_ready_for_print()) {
                            task_log(tid, "slice finished");
                            task_update(tid, TaskState::Completed, "slice finished");
                            done->store(true);
                        }
                    });
                }
            }).detach();
        } catch (...) { /* watcher best-effort */ }
    });

    // print_plate: {"action":"print_plate","plate_index":0,"dev_id":"...","dev_ip":"...","all":false}
    registerAsyncAction<PrintParamsReq, DefaultResult>(reg, "print_plate", [](const PrintParamsReq& in, std::shared_ptr<TaskContext> context) {
        if (in.plate_index && *in.plate_index < 0 && !in.all) throw std::runtime_error("invalid plate_index");
        
        std::string pin = in.plate_index ? std::to_string(*in.plate_index) : std::string("current");
        if (in.plate_index)
            context->log(std::string("print scheduled plate_index=") + std::to_string(*in.plate_index) + (in.all?" (all)":""));
        else
            context->log(std::string("print scheduled ") + (in.all?"(all current context)":"(current plate)"));

        auto* gapp = context->gapp();
        std::string tid = context->task_id();
        
        BBL::PrintParams params;
        if (!prepare_print_params(gapp, in, tid, params)) {
             throw std::runtime_error("failed to prepare print params");
        }

        if (auto* tm = gapp->getTaskManager()) {
            context->log("UI thread: TaskManager available; starting print via TaskManager");
            std::vector<BBL::PrintParams> jobs { params };
            tm->start_print(jobs, nullptr);
            context->log("UI thread: TaskManager::start_print invoked");
        } else if (auto* agent = gapp->getAgent()) {
            context->log("UI thread: NetworkAgent available; starting print via NetworkAgent");
            if (!params.dev_id.empty()) {
                int set_ret = agent->set_user_selected_machine(params.dev_id);
                context->log(std::string("UI thread: set_user_selected_machine returned ") + std::to_string(set_ret) + " for dev_id='" + params.dev_id + "'");
            }
            context->log(std::string("UI thread: PrintParams filename='") + params.filename + "' config='" + params.config_filename + "' dev_id='" + params.dev_id + "' project='" + params.project_name + "' preset='" + params.preset_name + "' conn='" + params.connection_type + "'");
            // Full parameter dump for diagnostics
            try {
                context->log(dump_print_params(params));
            } catch (...) { /* ignore logging errors */ }
            try { std::error_code fec; bool exists = std::filesystem::exists(params.filename, fec); context->log(std::string("UI thread: filename exists=") + (exists?"true":"false") + ", ec='" + (fec ? fec.message() : std::string()) + "'"); } catch (...) {}
            try { std::error_code cec; bool cexists = std::filesystem::exists(params.config_filename, cec); context->log(std::string("UI thread: config_filename exists=") + (cexists?"true":"false") + ", ec='" + (cec ? cec.message() : std::string()) + "'"); } catch (...) {}
            if (!params.dev_id.empty()) {
                if (auto* dm = gapp->getDeviceManager()) {
                    Slic3r::MachineObject* mo = dm->get_local_machine(params.dev_id);
                    if (!mo) mo = dm->get_user_machine(params.dev_id);
                    if (mo) {
                        if (params.dev_ip.empty()) params.dev_ip = mo->get_dev_ip();
                        params.username = "bblp";
                        params.password = mo->get_access_code();
                        params.use_ssl_for_ftp = mo->local_use_ssl_for_ftp;
                        params.use_ssl_for_mqtt = mo->local_use_ssl;
                        params.ftp_folder = mo->get_ftp_folder();
                        try { params.dev_name = mo->get_dev_name(); } catch (...) {}
                        context->log(std::string("UI thread: populated from MachineObject: dev_ip='") + params.dev_ip + "' ftp_folder='" + params.ftp_folder + "'");
                    }
                }
            }
            // Apply optional AMS mapping if provided by the client; otherwise rely on current device settings
            try {
                bool applied = false;
                if (in.ams_mapping && !in.ams_mapping->empty()) { params.ams_mapping = *in.ams_mapping; applied = true; }
                if (in.ams_mapping2 && !in.ams_mapping2->empty()) { params.ams_mapping2 = *in.ams_mapping2; applied = true; }
                if (in.ams_mapping_info && !in.ams_mapping_info->empty()) { params.ams_mapping_info = *in.ams_mapping_info; applied = true; }
                if (in.nozzles_info && !in.nozzles_info->empty()) { params.nozzles_info = *in.nozzles_info; applied = true; }
                if (applied) context->log("applied client-provided AMS mapping");
                else context->log("no AMS mapping provided; using current device settings");
            } catch (...) {}
            // Log complete PrintParams after device enrichment as well
            try {
                context->log(dump_print_params(params));
            } catch (...) { /* ignore logging errors */ }
            if (!params.dev_id.empty()) {
                // Use same credentials as GUI path: username "bblp" and access code password
                int conn_ret = agent->connect_printer(params.dev_id, params.dev_ip, params.username, params.password, params.use_ssl_for_mqtt);
                context->log(std::string("UI thread: connect_printer returned ") + std::to_string(conn_ret));
            }
            // Wire callbacks to capture progress and key milestones
            BBL::OnUpdateStatusFn upd = [tid](int status, int code, std::string msg){
                try { task_log(tid, std::string("update status=") + std::to_string(status) + " code=" + std::to_string(code) + " msg=" + msg); } catch (...) {}
                // Heuristics: mark submitted when we reach sending or waiting-for-printer stages
                if (status == BBL::SendingPrintJobStage::PrintingStageSending || status == BBL::SendingPrintJobStage::PrintingStageWaitPrinter) {
                    task_update(tid, TaskState::Submitted, "print job sent");
                }
                if (status == BBL::SendingPrintJobStage::PrintingStageFinished) {
                    task_update(tid, TaskState::Completed, "print job finished");
                }
                if (status == BBL::SendingPrintJobStage::PrintingStageERROR && code != 0) {
                    task_update(tid, TaskState::Failed, std::string("print error: ") + net_code_to_str(code));
                }
            };
            BBL::WasCancelledFn cancelled = [](){ return false; };
            BBL::OnWaitFn waitfn = [tid](int status, std::string job_info){
                try { task_log(tid, std::string("wait status=") + std::to_string(status)); } catch (...) {}
                // Align with GUI behavior: generally return true to proceed without long blocking loops
                return true;
            };

            // Optional preflight: verify access code/IP by sending a tiny file to sdcard, LAN only
            if (params.connection_type == std::string("lan") && !params.password.empty() && !params.dev_ip.empty()) {
                BBL::PrintParams pre;
                pre.dev_id = params.dev_id;
                pre.dev_ip = params.dev_ip;
                pre.use_ssl_for_ftp = params.use_ssl_for_ftp;
                pre.use_ssl_for_mqtt = params.use_ssl_for_mqtt;
                pre.username = params.username;
                pre.password = params.password;
                pre.connection_type = "lan";
                pre.project_name = "verify_job";
                try {
                    std::string temp_file = Slic3r::resources_dir() + std::string("/check_access_code.txt");
                    pre.filename = temp_file;
                } catch (...) {}
                int pre_res = agent->start_send_gcode_to_sdcard(pre, nullptr, nullptr, nullptr);
                context->log(std::string("preflight send_gcode ret=") + std::to_string(pre_res));
                if (pre_res != 0) {
                    throw std::runtime_error("preflight failed: invalid access code or IP");
                }
            }

            // Log SSL flags for diagnostics
            context->log(std::string("ssl flags mqtt=") + (params.use_ssl_for_mqtt?"true":"false") + ", ftp=" + (params.use_ssl_for_ftp?"true":"false"));

            // Prefer LAN local print WITH RECORD first (matches UI flow); then plain local, then cloud
            int netres = agent->start_local_print_with_record(params, upd, cancelled, waitfn);
            context->log(std::string("UI thread: start_local_print_with_record returned ") + std::to_string(netres) + " (" + net_code_to_str(netres) + ")");
            if (netres < 0) {
                netres = agent->start_local_print(params, upd, cancelled);
                context->log(std::string("UI thread: start_local_print returned ") + std::to_string(netres) + " (" + net_code_to_str(netres) + ")");
                // Retry once on MQTT publish failure by toggling SSL for MQTT and reconnecting (now unconditional and safe)
                if (netres == BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED) {
                    context->log("retry: toggling mqtt ssl and reconnecting");
                    // Work on a copy to avoid side-effects if other branches re-use params
                    BBL::PrintParams retry_params = params;
                    retry_params.use_ssl_for_mqtt = !params.use_ssl_for_mqtt;
                    if (!retry_params.dev_id.empty()) {
                        int rc = agent->connect_printer(retry_params.dev_id, retry_params.dev_ip, retry_params.username, retry_params.password, retry_params.use_ssl_for_mqtt);
                        context->log(std::string("reconnect ret=") + std::to_string(rc));
                    }
                    int retry = agent->start_local_print(retry_params, upd, cancelled);
                    context->log(std::string("UI thread: start_local_print (retry ssl toggle) returned ") + std::to_string(retry) + " (" + net_code_to_str(retry) + ")");
                    if (retry >= 0) netres = retry;
                }
            }
            if (netres < 0) {
                netres = agent->start_print(params, upd, cancelled, waitfn);
                context->log(std::string("UI thread: start_print returned ") + std::to_string(netres) + " (" + net_code_to_str(netres) + ")");
            }
            if (netres < 0) {
                context->update(TaskState::Failed, std::string("network start failed: ") + std::to_string(netres));
                context->log(std::string("network start failed code=") + std::to_string(netres));
            } else {
                context->update(TaskState::Submitted, "print job sent");
                context->log("print job sent");
            }
        } else {
            context->update(TaskState::Failed, "no TaskManager or NetworkAgent");
            context->log("UI thread: No TaskManager or NetworkAgent available; cannot start print", true);
        }
    });

    // list_devices: {"action":"list_devices"}
    registerAsyncAction<DevicesParams, DevicesResult>(reg, "devices", [](const DevicesParams& in, std::shared_ptr<TaskContext> context) -> DevicesResult {
        DevicesResult result; result.status = "ok"; result.message = "ok";
        auto* gapp = context->gapp();
        
        const bool inc_local = in.include_local;
        const bool inc_user  = in.include_user;
        if (auto* dm = gapp->getDeviceManager()) {
            if (inc_local) {
                auto local = dm->get_local_machinelist();
                for (auto &it : local) {
                    if (auto* mo = it.second) {
                        DeviceInfo di; di.dev_id = mo->get_dev_id(); di.dev_name = mo->get_dev_name(); di.dev_ip = mo->get_dev_ip(); di.connection_type = mo->connection_type();
                        result.devices.emplace_back(std::move(di));
                    }
                }
            }
            if (inc_user) {
                auto users = dm->get_user_machinelist();
                for (auto &it : users) {
                    if (auto* mo = it.second) {
                        DeviceInfo di; di.dev_id = mo->get_dev_id(); di.dev_name = mo->get_dev_name(); di.dev_ip = mo->get_dev_ip(); di.connection_type = mo->connection_type();
                        result.devices.emplace_back(std::move(di));
                    }
                }
            }
        }
        return result;
    });

    // task_status: {"action":"task_status","id":"<task_id>"}
    reg.register_action<TaskStatusParams, TaskStatusResult>("task_status", [](const TaskStatusParams& in) -> TaskStatusResult {
        TaskStatusResult out; out.status = "error";
        if (in.id.empty()) { out.message = "missing id"; return out; }
        nlohmann::json snap;
        if (!Slic3r::Utils::get_action_task_snapshot(in.id, snap)) { out.message = "unknown task"; return out; }
        out.status = "ok"; out.message = "ok"; out.task = std::move(snap); return out;
    });

    // get_logs: {"action":"get_logs","params":{"max_bytes":65536}} or with specific {"file":"debug_...log.0"}
    reg.register_action<GetLogsParams, GetLogsResult>("get_logs", [](const GetLogsParams& in) -> GetLogsResult {
        const std::string base_dir = Slic3r::data_dir();
        if (base_dir.empty()) throw std::runtime_error("data_dir is empty");
        boost::filesystem::path log_folder = boost::filesystem::path(base_dir) / "log";
        if (!boost::filesystem::exists(log_folder)) throw std::runtime_error("log folder does not exist");

        // Security: only allow basename in 'file'
        auto contains_sep = [](const std::string& s){ return s.find('/') != std::string::npos || s.find('\\') != std::string::npos; };
        if (!in.file.empty() && contains_sep(in.file)) throw std::runtime_error("invalid file parameter");

        // Pick target file: explicit match or the most recently modified file
        boost::filesystem::path target;
        std::time_t best_time = 0;
        for (auto& it : boost::filesystem::directory_iterator(log_folder)) {
            const auto p = it.path();
            boost::system::error_code ec;
            if (!boost::filesystem::is_regular_file(p, ec)) continue;
            const auto fname = p.filename().string();
            if (!in.file.empty()) {
                if (fname == in.file) { target = p; break; }
                else continue;
            }
            std::time_t lw = 0;
            try { lw = boost::filesystem::last_write_time(p); } catch (...) { lw = 0; }
            if (!target.empty()) {
                if (lw > best_time) { target = p; best_time = lw; }
            } else {
                target = p; best_time = lw;
            }
        }

        if (target.empty()) throw std::runtime_error(in.file.empty() ? "no log files found" : "specified log file not found");

        // Tail read
        boost::nowide::ifstream ifs(target.string(), std::ios::in | std::ios::binary);
        if (!ifs) throw std::runtime_error("failed to open log file");
        ifs.seekg(0, std::ios::end);
        std::uint64_t fsize = static_cast<std::uint64_t>(ifs.tellg());
        std::uint64_t maxb  = in.max_bytes == 0 ? 65536ULL : in.max_bytes;
        std::uint64_t start = (fsize > maxb) ? (fsize - maxb) : 0ULL;
        std::uint64_t readn = fsize - start;
        ifs.seekg(static_cast<std::streamoff>(start), std::ios::beg);
        std::string buf;
        buf.resize(static_cast<size_t>(readn));
        if (readn > 0) ifs.read(&buf[0], static_cast<std::streamsize>(readn));

        GetLogsResult out;
        out.status    = "ok";
        out.message   = "ok";
        out.file      = target.filename().string();
        out.content   = std::move(buf);
        out.truncated = (start != 0ULL);
        out.size      = fsize;
        out.returned  = readn;
        return out;
    });
    // select_printer: {"action":"select_printer","dev_id":"..."}
    registerAsyncAction<SelectPrinterParams, DefaultResult>(reg, "select_printer", [](const SelectPrinterParams& in, std::shared_ptr<TaskContext> context) {
        if (in.dev_id.empty()) throw std::runtime_error("missing dev_id");
        
        auto* gapp = context->gapp();
        
        context->log("select printer scheduled dev_id='" + in.dev_id + "'");
        context->update(TaskState::Running, "selecting printer");
        
        if (auto* agent = gapp->getAgent()) {
            int ret = agent->set_user_selected_machine(in.dev_id);
            if (ret != 0) {
                throw std::runtime_error("failed to select printer (agent returned " + std::to_string(ret) + ")");
            } else {
                context->log("printer selected");
                // Auto-arrange prime tower if needed
                // Defer execution to allow printer switch to complete (events to process)
                gapp->CallAfter([gapp](){
                    arrange_prime_tower_impl(gapp);
                });
            }
        } else {
            throw std::runtime_error("NetworkAgent not available");
        }
    });
    // select_device: {"action":"select_device","dev_id":"..."}
    registerAsyncAction<SelectPrinterParams, DefaultResult>(reg, "select_device", [](const SelectPrinterParams& in, std::shared_ptr<TaskContext> context) {
        if (in.dev_id.empty()) throw std::runtime_error("missing dev_id");
        
        auto* gapp = context->gapp();
        if (!gapp->mainframe) throw std::runtime_error("MainFrame not available");
        
        context->log("select device scheduled dev_id='" + in.dev_id + "'");
        context->update(TaskState::Running, "selecting device");
        
        std::string dev_id = in.dev_id;
        
        Slic3r::DeviceManager* dev = gapp->getDeviceManager();
        if (!dev) {
            context->update(TaskState::Failed, "DeviceManager not available");
            return;
        }

        if (!dev->set_selected_machine(dev_id)) {
            context->update(TaskState::Failed, "Failed to set selected machine");
            return;
        }

        Slic3r::MachineObject *obj_ = dev->get_selected_machine();
        if (obj_) {
            obj_->last_cali_version = -1;
            obj_->reset_pa_cali_history_result();
            obj_->reset_pa_cali_result();
            
            Slic3r::GUI::Sidebar &sidebar = gapp->sidebar();
            sidebar.update_sync_status(obj_);
            sidebar.set_need_auto_sync_after_connect_printer(sidebar.need_auto_sync_extruder_list_after_connect_priner(obj_));
        }
        
        if (gapp->mainframe) {
            gapp->mainframe->select_tab(Slic3r::GUI::MainFrame::tpMonitor);
        }
        
        context->log("device selected");
        context->update(TaskState::Completed, "device selected");
    });
    
    // select_printer_preset: {"action":"select_printer_preset","name":"..."}
    registerAsyncAction<SelectPresetParams, DefaultResult>(reg, "select_printer_preset", [](const SelectPresetParams& in, std::shared_ptr<TaskContext> context) {
        select_preset_impl(context, in, Slic3r::Preset::TYPE_PRINTER, context->gapp()->preset_bundle->printers);
    });
    // list_printers: {"action":"list_printers"}
    registerAsyncAction<ListPresetsParams, ListPrintersResult>(reg, "list_printers", [](const ListPresetsParams& in, std::shared_ptr<TaskContext> context) -> ListPrintersResult {
        ListPrintersResult result; result.status = "ok"; result.message = "ok";
        auto* gapp = context->gapp();
        if (!gapp->preset_bundle) { result.status = "error"; result.message = "PresetBundle not available"; return result; }
        result.printers = list_presets_impl(gapp, in, gapp->preset_bundle->printers);
        return result;
    });

    // select_print_profile: {"action":"select_print_profile","name":"..."}
    registerAsyncAction<SelectPresetParams, DefaultResult>(reg, "select_print_profile", [](const SelectPresetParams& in, std::shared_ptr<TaskContext> context) {
        select_preset_impl(context, in, Slic3r::Preset::TYPE_PRINT, context->gapp()->preset_bundle->prints);
    });

    // list_print_profiles: {"action":"list_print_profiles","include_invisible":true}
    registerAsyncAction<ListPresetsParams, ListPrintProfilesResult>(reg, "list_print_profiles", [](const ListPresetsParams& in, std::shared_ptr<TaskContext> context) -> ListPrintProfilesResult {
        ListPrintProfilesResult result; result.status = "ok"; result.message = "ok";
        auto* gapp = context->gapp();
        if (!gapp->preset_bundle) { result.status = "error"; result.message = "PresetBundle not available"; return result; }
        result.profiles = list_presets_impl(gapp, in, gapp->preset_bundle->prints);
        return result;
    });

    // paint_height_range: {"action":"paint_height_range", "object_index":0, "start_z":10.0, "end_z":20.0, "filament_id":1}
    registerAsyncAction<PaintHeightRangeParams, DefaultResult>(reg, "paint_height_range", [](const PaintHeightRangeParams& in, std::shared_ptr<TaskContext> context) {
        auto* gapp = context->gapp();
        
        context->log("paint height range scheduled");
        context->update(TaskState::Running, "painting height range");
        context->log("UI thread: painting height range");

        Slic3r::GUI::Plater* plater = gapp->plater();
        if (!plater) throw std::runtime_error("Plater not available");
        
        Slic3r::Model& model = plater->model();
        if (in.object_index < 0 || in.object_index >= model.objects.size()) {
            throw std::runtime_error("Invalid object index");
        }
        
        Slic3r::ModelObject* obj = model.objects[in.object_index];
        Slic3r::EnforcerBlockerType type = (Slic3r::EnforcerBlockerType)in.filament_id;
        
        bool changed = false;
        if (!obj->instances.empty()) {
            // Use the first instance to determine world coordinates
            Slic3r::ModelInstance* inst = obj->instances[0];
            Slic3r::Transform3d inst_matrix = inst->get_transformation().get_matrix();
            
            // Get the bounding box of the instance in world coordinates to find the bottom Z
            Slic3r::BoundingBoxf3 inst_bbox = obj->instance_bounding_box(0, false);
            float z_bottom = (float)inst_bbox.min.z();
            
            float z_world = z_bottom + in.start_z;
            float height = in.end_z - in.start_z;
            
            if (height > 0) {
                for (Slic3r::ModelVolume* vol : obj->volumes) {
                    if (vol->type() != Slic3r::ModelVolumeType::MODEL_PART) continue;
                    
                    Slic3r::TriangleSelector selector(vol->mesh());
                    selector.deserialize(vol->mmu_segmentation_facets.get_data());
                    
                    // Combine instance and volume transformations
                    Slic3r::Transform3d vol_matrix = vol->get_matrix();
                    Slic3r::Transform3d total_matrix = inst_matrix * vol_matrix;

                    auto cursor = Slic3r::TriangleSelector::SinglePointCursor::cursor_factory(
                        z_world, 
                        Slic3r::Vec3f(0,0,0), 
                        height, 
                        total_matrix, 
                        Slic3r::TriangleSelector::ClippingPlane()
                    );
                    
                    Slic3r::Transform3d total_matrix_no_translate = total_matrix;
                    total_matrix_no_translate.translation() = Slic3r::Vec3d::Zero();

                    selector.select_patch(0, std::move(cursor), type, total_matrix_no_translate, true);
                    vol->mmu_segmentation_facets.set(selector);
                    changed = true;
                }
            }
        }
        
        if (changed) {
            plater->update();
            context->log("height range painted");
        } else {
            context->log("no changes made");
        }
    });

    // arrange_prime_tower: {"action":"arrange_prime_tower"}
    registerAsyncAction<ArrangePrimeTowerParams, DefaultResult>(reg, "arrange_prime_tower", [](const ArrangePrimeTowerParams& in, std::shared_ptr<TaskContext> context) {
        auto* gapp = context->gapp();
        
        context->update(TaskState::Running, "arranging prime tower");
        std::string err = arrange_prime_tower_impl(gapp);
        if (err.empty()) {
            context->log("prime tower arranged");
        } else {
            throw std::runtime_error(err);
        }
    });

    // set_object_filament: {"action":"set_object_filament", "object_index":0, "filament_id":1}
    registerAsyncAction<SetObjectFilamentParams, DefaultResult>(reg, "set_object_filament", [](const SetObjectFilamentParams& in, std::shared_ptr<TaskContext> context) {
        auto* gapp = context->gapp();
        
        context->update(TaskState::Running, "setting object filament");
        
        Slic3r::GUI::Plater* plater = gapp->plater();
        if (!plater) throw std::runtime_error("Plater not available");
        
        Slic3r::Model& model = plater->model();
        if (in.object_index < 0 || in.object_index >= model.objects.size()) {
            throw std::runtime_error("Invalid object index");
        }
        
        Slic3r::ModelObject* obj = model.objects[in.object_index];
        obj->config.set_key_value("extruder", new Slic3r::ConfigOptionInt(in.filament_id));
        
        plater->changed_object(in.object_index);
        plater->update();
        
        context->log("object filament set");
    });

    // arrange_objects: {"action":"arrange_objects"}
    registerAsyncAction<ArrangeObjectsParams, DefaultResult>(reg, "arrange_objects", [](const ArrangeObjectsParams& in, std::shared_ptr<TaskContext> context) {
        auto* gapp = context->gapp();
        std::string tid = context->task_id();
        
        context->update(TaskState::Running, "arranging objects");
        
        Slic3r::GUI::Plater* plater = gapp->plater();
        if (!plater) throw std::runtime_error("Plater not available");
        
        if (plater->last_arrange_job_is_finished()) {
            plater->arrange();
            context->update(TaskState::Submitted, "arrange submitted");

            // Poll for completion
            try {
                auto done = std::make_shared<std::atomic<bool>>(false);
                std::thread([gapp, tid, done]() {
                    using namespace std::chrono;
                    auto deadline = steady_clock::now() + minutes(10);
                    while (!done->load()) {
                        std::this_thread::sleep_for(milliseconds(200));
                        if (steady_clock::now() > deadline) {
                            gapp->CallAfter([tid]() {
                                task_update(tid, TaskState::Failed, "arrange timeout");
                            });
                            done->store(true);
                            break;
                        }
                        gapp->CallAfter([gapp, tid, done]() {
                            if (!gapp || !gapp->plater_) return;
                            // m_arrange_running is false when idle/finished
                            if (!gapp->plater_->m_arrange_running.load()) {
                                task_update(tid, TaskState::Completed, "objects arranged");
                                done->store(true);
                            }
                        });
                    }
                }).detach();
            } catch (...) { /* watcher best-effort */ }
        } else {
            throw std::runtime_error("Arrange already in progress");
        }
    });
    // select_tab: {"action":"select_tab","tab":"<tab_name>"}
    registerAsyncAction<SelectTabParams, DefaultResult>(reg, "select_tab", [](const SelectTabParams& in, std::shared_ptr<TaskContext> context) {
        if (in.tab.empty()) throw std::runtime_error("missing tab name");
        
        auto* gapp = context->gapp();
        if (!gapp->mainframe) throw std::runtime_error("MainFrame not available");
        
        int tab_idx = -1;
        if (in.tab == "home") tab_idx = Slic3r::GUI::MainFrame::tpHome;
        else if (in.tab == "prepare") tab_idx = Slic3r::GUI::MainFrame::tp3DEditor;
        else if (in.tab == "preview") tab_idx = Slic3r::GUI::MainFrame::tpPreview;
        else if (in.tab == "device") tab_idx = Slic3r::GUI::MainFrame::tpMonitor;
        else if (in.tab == "multi_device") tab_idx = Slic3r::GUI::MainFrame::tpMultiDevice;
        else if (in.tab == "project") tab_idx = Slic3r::GUI::MainFrame::tpProject;
        else if (in.tab == "calibration") tab_idx = Slic3r::GUI::MainFrame::tpCalibration;
        else throw std::runtime_error("unknown tab name: " + in.tab);

        context->log("select tab scheduled name='" + in.tab + "' idx=" + std::to_string(tab_idx));
        context->update(TaskState::Running, "selecting tab");
        
        gapp->mainframe->select_tab(size_t(tab_idx));
        
        context->log("tab selected");
    });


    
    // sync_ams_filament: {"action":"sync_ams_filament"}
    registerAsyncAction<SyncAMSParams, DefaultResult>(reg, "sync_ams_filament", [](const SyncAMSParams& in, std::shared_ptr<TaskContext> context) {
        auto* gapp = context->gapp();
        if (!gapp->mainframe) throw std::runtime_error("MainFrame not available");
        
        context->update(TaskState::Running, "syncing ams filament");
        
        Slic3r::DeviceManager* dev = gapp->getDeviceManager();
        if (!dev) {
            context->update(TaskState::Failed, "DeviceManager not available");
            return;
        }
        
        Slic3r::MachineObject *obj = dev->get_selected_machine();
        if (!obj) {
            context->update(TaskState::Failed, "No device selected");
            return;
        }
        
        Slic3r::GUI::Sidebar &sidebar = gapp->sidebar();
        auto filament_ams_list = sidebar.build_filament_ams_list(obj);
        gapp->preset_bundle->filament_ams_list = filament_ams_list;
        
        std::vector<std::pair<Slic3r::DynamicPrintConfig *,std::string>> unknowns;
        std::map<int, Slic3r::AMSMapInfo> maps;
        Slic3r::MergeFilamentInfo merge_info;
        
        // use_map = !overwrite
        bool use_map = !in.overwrite;
        
        gapp->preset_bundle->sync_ams_list(unknowns, use_map, maps, false, merge_info);
        
        context->log("ams filament synced");
        context->update(TaskState::Completed, "ams filament synced");
    });
}
