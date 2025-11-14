#include "RestActions.hpp"

#include "nlohmann/json.hpp"
// Enable std::optional<T> support for JSON (local adapter) – include before using JSON in headers
#include "slic3r/Utils/json_optional.hpp"
#include "slic3r/Utils/ActionRegister.hpp"

#include <wx/app.h>
#include <boost/log/trivial.hpp>
#include <filesystem>
#include <future>
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

// GUI and printing includes
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
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
// task updates
#include "slic3r/Utils/ActionRegister.hpp"

using Slic3r::Utils::ActionRegister;

namespace {
static const char* net_code_to_str(int code)
{
    switch (code) {
    case 0: return "ok";
    case BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_3MF_CONFIG_TO_OSS_FAILED: return "WR: upload config 3mf failed (-2030)";
    case BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_3MF_TO_OSS_FAILED: return "WR: upload 3mf failed (-2110)";
    case BAMBU_NETWORK_ERR_PRINT_WR_POST_TASK_FAILED: return "WR: post task failed (-2120)";
    case BAMBU_NETWORK_ERR_PRINT_SP_UPLOAD_3MF_CONFIG_TO_OSS_FAILED: return "SP: upload config 3mf failed (-3030)";
    case BAMBU_NETWORK_ERR_PRINT_SP_FILE_NOT_EXIST: return "SP: 3mf file not exist (-3070)";
    case BAMBU_NETWORK_ERR_PRINT_SP_POST_TASK_FAILED: return "SP: post task failed (-3120)";
    case BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED: return "LP: ftp upload failed (-4020)";
    case BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED: return "LP: mqtt publish failed (-4030)";
    case BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED: return "connect: connection to printer failed (-6010)";
    default: return "unknown";
    }
}
struct ImportParams {
    std::string path;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ImportParams, path)
};
struct ImportResult {
    std::string status;
    std::string message;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ImportResult, status, message)
};
struct SliceParams {
    std::optional<int> plate_index; // optional: defaults to current plate
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SliceParams, plate_index)
};
struct SliceResult {
    std::string status;
    std::string message;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SliceResult, status, message)
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
struct PrintResult {
    std::string status;
    std::string message;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(PrintResult, status, message)
};

// Devices listing
struct DevicesParams {
    bool include_local = true; // optional in JSON; defaults to true
    bool include_user  = true; // optional in JSON; defaults to true
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
struct DevicesResult {
    std::string status;
    std::string message;
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

struct GetLogsResult {
    std::string status;
    std::string message;
    std::string file;      // resolved filename returned
    std::string content;   // UTF-8 text content (tail)
    bool         truncated = false;
    std::uint64_t size = 0;      // total file size
    std::uint64_t returned = 0;  // bytes returned in content
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(GetLogsResult, status, message, file, content, truncated, size, returned)
};

// Pretty-print all fields of BBL::PrintParams for diagnostics (masking sensitive values)
static std::string dump_print_params(const BBL::PrintParams& p)
{
    auto tf = [](bool v){ return v ? "true" : "false"; };
    auto mask = [](const std::string& s){ return s.empty() ? std::string("") : std::string(s.size(), '*'); };
    std::ostringstream oss;
    oss << "PrintParams{"
        << "dev_id='" << p.dev_id << "', "
        << "task_name='" << p.task_name << "', "
        << "project_name='" << p.project_name << "', "
        << "preset_name='" << p.preset_name << "', "
        << "filename='" << p.filename << "', "
        << "config_filename='" << p.config_filename << "', "
        << "plate_index=" << p.plate_index << ", "
        << "ftp_folder='" << p.ftp_folder << "', "
        << "ftp_file='" << p.ftp_file << "', "
        << "ftp_file_md5='" << p.ftp_file_md5 << "', "
        << "ams_mapping='" << p.ams_mapping << "', "
        << "ams_mapping2='" << p.ams_mapping2 << "', "
        << "ams_mapping_info='" << p.ams_mapping_info << "', "
        << "nozzles_info='" << p.nozzles_info << "', "
        << "connection_type='" << p.connection_type << "', "
        << "comments='" << p.comments << "', "
        << "origin_profile_id=" << p.origin_profile_id << ", "
        << "stl_design_id=" << p.stl_design_id << ", "
        << "origin_model_id='" << p.origin_model_id << "', "
        << "print_type='" << p.print_type << "', "
        << "dst_file='" << p.dst_file << "', "
        << "dev_name='" << p.dev_name << "', "
        << "dev_ip='" << p.dev_ip << "', "
        << "use_ssl_for_ftp=" << tf(p.use_ssl_for_ftp) << ", "
        << "use_ssl_for_mqtt=" << tf(p.use_ssl_for_mqtt) << ", "
        << "username='" << p.username << "', "
        << "password='" << mask(p.password) << "', "
        << "task_bed_leveling=" << tf(p.task_bed_leveling) << ", "
        << "task_flow_cali=" << tf(p.task_flow_cali) << ", "
        << "task_vibration_cali=" << tf(p.task_vibration_cali) << ", "
        << "task_layer_inspect=" << tf(p.task_layer_inspect) << ", "
        << "task_record_timelapse=" << tf(p.task_record_timelapse) << ", "
        << "task_use_ams=" << tf(p.task_use_ams) << ", "
        << "task_bed_type='" << p.task_bed_type << "', "
        << "extra_options='" << p.extra_options << "', "
        << "auto_bed_leveling=" << p.auto_bed_leveling << ", "
        << "auto_flow_cali=" << p.auto_flow_cali << ", "
        << "auto_offset_cali=" << p.auto_offset_cali << ", "
        << "task_ext_change_assist=" << tf(p.task_ext_change_assist)
        << "}";
    return oss.str();
}

// Task status query
struct TaskStatusParams { std::string id; NLOHMANN_DEFINE_TYPE_INTRUSIVE(TaskStatusParams, id) };
struct TaskStatusResult { std::string status; std::string message; nlohmann::json task; NLOHMANN_DEFINE_TYPE_INTRUSIVE(TaskStatusResult, status, message, task) };

// Cancel print params/results (must be at namespace scope; MSVC rejects friend defs inside local classes)
struct CancelParams {
    std::string dev_id;
    std::string job_id;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(CancelParams, dev_id, job_id)
};
struct CancelResult {
    std::string status;
    std::string message;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(CancelResult, status, message)
};

// Clear plate params/results
struct ClearPlateParams {
    std::optional<int> plate_index; // optional, clear current plate when not provided
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ClearPlateParams, plate_index)
};
struct ClearPlateResult {
    std::string status;
    std::string message;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ClearPlateResult, status, message)
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

struct AmsFilamentsResult {
    std::string status;
    std::string message;
    std::vector<AmsTrayInfo> trays;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AmsFilamentsResult, status, message, trays)
};
} // namespace

// Note: optional parameters (e.g., plate_index) use the intrusive macro
// and should be passed as null when omitted, e.g., {"plate_index": null}.

void register_rest_actions(ActionRegister& reg)
{
    // cancel_print: {"action":"cancel_print","dev_id":"...","job_id":"..."}
    reg.register_action<CancelParams, CancelResult>("cancel_print", [](const CancelParams& in) -> CancelResult {
        CancelResult out; out.status = "error";
        wxAppConsole* raw_app = wxAppConsole::GetInstance();
        if (!raw_app) { out.message = "no wxApp instance"; return out; }
        auto* gapp = dynamic_cast<Slic3r::GUI::GUI_App*>(raw_app);
        if (!gapp) { out.message = "wxApp is not GUI_App"; return out; }

        std::string tid = Slic3r::Utils::current_action_task_id();
        Slic3r::Utils::update_action_task(tid, "scheduled", "cancel scheduled");
        Slic3r::Utils::append_action_task_log(tid, std::string("cancel scheduled dev_id='") + in.dev_id + "' job_id='" + in.job_id + "'");

        auto prom = std::make_shared<std::promise<CancelResult>>();
        auto fut  = prom->get_future();
        gapp->CallAfter([prom, gapp, in, tid]() mutable {
            CancelResult result; result.status = "error"; result.message = "unknown";
            try {
                Slic3r::Utils::update_action_task(tid, "running", "cancel running");
                Slic3r::Utils::append_action_task_log(tid, "cancel running on UI thread");
                auto* dm = gapp->getDeviceManager();
                if (!dm) { result.message = "no DeviceManager"; prom->set_value(result); return; }

                Slic3r::MachineObject* mo = nullptr;
                if (!in.dev_id.empty()) {
                    mo = dm->get_local_machine(in.dev_id);
                    if (!mo) mo = dm->get_user_machine(in.dev_id);
                } else {
                    mo = dm->get_selected_machine();
                }
                if (!mo) { result.message = "device not found"; Slic3r::Utils::append_action_task_log(tid, "device not found"); prom->set_value(result); return; }

                int ret = 0;
                if (!in.job_id.empty()) {
                    Slic3r::Utils::append_action_task_log(tid, std::string("sending command_task_cancel job_id=") + in.job_id);
                    ret = mo->command_task_cancel(in.job_id);
                } else {
                    Slic3r::Utils::append_action_task_log(tid, "sending command_task_abort (no job_id provided)");
                    ret = mo->command_task_abort();
                }

                if (ret == 0) {
                    result.status = "ok"; result.message = "cancel sent";
                    Slic3r::Utils::update_action_task(tid, "success", "cancel sent");
                } else {
                    result.status = "error"; result.message = std::string("cancel failed: ") + std::to_string(ret);
                    Slic3r::Utils::update_action_task(tid, "failed", result.message);
                }
                prom->set_value(result);
            } catch (const std::exception& ex) {
                CancelResult err; err.status = "error"; err.message = std::string("exception: ") + ex.what();
                Slic3r::Utils::update_action_task(tid, "failed", err.message);
                prom->set_value(std::move(err));
            } catch (...) {
                CancelResult err; err.status = "error"; err.message = "unknown exception";
                Slic3r::Utils::update_action_task(tid, "failed", err.message);
                prom->set_value(std::move(err));
            }
        });

        // Short wait for immediate result; otherwise return scheduled
        if (fut.wait_for(std::chrono::seconds(1)) == std::future_status::ready) return fut.get();
        out.status = "ok"; out.message = "scheduled cancel"; return out;
    });
    
    // clear_plate: {"action":"clear_plate","plate_index":0} (plate_index optional; clears current when omitted)
    reg.register_action<ClearPlateParams, ClearPlateResult>("clear_plate", [](const ClearPlateParams& in) -> ClearPlateResult {
        ClearPlateResult out; out.status = "error";
        wxAppConsole* raw_app = wxAppConsole::GetInstance();
        if (!raw_app) { out.message = "no wxApp instance"; return out; }
        if (auto* gapp = dynamic_cast<Slic3r::GUI::GUI_App*>(raw_app)) {
            std::string tid = Slic3r::Utils::current_action_task_id();
            Slic3r::Utils::update_action_task(tid, "scheduled", "clear plate scheduled");
            if (in.plate_index && *in.plate_index >= 0)
                Slic3r::Utils::append_action_task_log(tid, std::string("clear scheduled plate_index=") + std::to_string(*in.plate_index));
            else
                Slic3r::Utils::append_action_task_log(tid, "clear scheduled (current plate)");

            gapp->CallAfter([in, gapp, tid]() {
                try {
                    Slic3r::Utils::update_action_task(tid, "running", "clear plate running");
                    Slic3r::Utils::append_action_task_log(tid, "clear running on UI thread");
                    Slic3r::GUI::Plater* plater = gapp->plater_;
                    if (!plater) {
                        BOOST_LOG_TRIVIAL(error) << "[RestServer] Plater not available; cannot clear plate";
                        Slic3r::Utils::append_action_task_log(tid, "Plater not available; cannot clear");
                        Slic3r::Utils::update_action_task(tid, "failed", "no Plater");
                        return;
                    }
                    if (in.plate_index && *in.plate_index >= 0)
                        plater->select_plate(*in.plate_index, true);
                    plater->remove_curr_plate_all();
                    Slic3r::Utils::append_action_task_log(tid, "plate cleared");
                    Slic3r::Utils::update_action_task(tid, "success", "plate cleared");
                } catch (const std::exception &ex) {
                    BOOST_LOG_TRIVIAL(error) << "[RestServer] Exception while clearing plate on UI thread: " << ex.what();
                    Slic3r::Utils::update_action_task(tid, "failed", ex.what());
                    Slic3r::Utils::append_action_task_log(tid, std::string("exception: ") + ex.what());
                }
            });
            out.status = "ok"; out.message = "scheduled clear";
        } else {
            out.status = "error"; out.message = "wxApp is not GUI_App";
        }
        return out;
    });

    // ams_filaments: {"action":"ams_filaments","dev_id":"<optional>"}
    reg.register_action<AmsFilamentsParams, AmsFilamentsResult>("ams_filaments", [](const AmsFilamentsParams& in) -> AmsFilamentsResult {
        AmsFilamentsResult out; out.status = "error";
        wxAppConsole* raw_app = wxAppConsole::GetInstance();
        if (!raw_app) { out.message = "no wxApp instance"; return out; }
        auto* gapp = dynamic_cast<Slic3r::GUI::GUI_App*>(raw_app);
        if (!gapp) { out.message = "wxApp is not GUI_App"; return out; }

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
                        bool use_ssl_mqtt = mo->local_use_ssl_for_mqtt;
                        int conn_ret = agent->connect_printer(dev_id, dev_ip, username, password, use_ssl_mqtt);
                        (void)conn_ret; // best-effort; AMS listing still attempted below
                    }
                } catch (...) {}
                // Proactively request a state push so AMS/tray data is populated even if the UI Device tab wasn't visited
                try { mo->command_request_push_all(true); } catch (...) {}

                auto* fs = mo->GetFilaSystem();
                if (!fs) { result.status = "ok"; result.message = "no fila system"; prom->set_value(std::move(result)); return; }
                auto& ams_list = fs->GetAmsList();
                for (auto &ams_pair : ams_list) {
                    const std::string ams_id = ams_pair.first;
                    int ams_index = -1;
                    try { ams_index = static_cast<int>(std::stoi(ams_id)); } catch (...) { ams_index = -1; }
                    Slic3r::DevAms* ams = ams_pair.second;
                    if (!ams) continue;
                    const auto& trays = ams->GetTrays();
                    for (const auto &tray_pair : trays) {
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
                        try { info.filament_id = mo->get_filament_id(ams_id, tray_id); } catch (...) {}
                        try { info.filament_type = mo->get_filament_type(ams_id, tray_id); } catch (...) {}
                        try { info.color = t->color; } catch (...) {}
                        try { info.is_bbl = t->is_bbl; } catch (...) {}
                        try { info.exists = t->is_exists; } catch (...) {}
                        try { info.remain = t->remain; } catch (...) {}
                        try { info.nozzle_temp_min = t->nozzle_temp_min; } catch (...) {}
                        try { info.nozzle_temp_max = t->nozzle_temp_max; } catch (...) {}
                        try { info.setting_id = t->setting_id; } catch (...) {}
                        try { info.filament_setting_id = t->filament_setting_id; } catch (...) {}
                        result.trays.emplace_back(std::move(info));
                    }
                }
                // Fallback: if AMS list is empty but the machine reports AMS support, probe known id ranges via MachineObject APIs
                if (result.trays.empty() && mo->HasAms()) {
                    try {
                        for (int ams_idx = 0; ams_idx < 4; ++ams_idx) { // typical AMS count
                            std::string ams_id = std::to_string(ams_idx);
                            for (int slot_idx = 0; slot_idx < 6; ++slot_idx) { // some models have up to 6; most 4
                                std::string tray_id = std::to_string(slot_idx);
                                Slic3r::DevAmsTray* t = mo->get_ams_tray(ams_id, tray_id);
                                if (!t) continue;
                                // Heuristic: consider a tray valid if exists, is_bbl, or has any identifying info
                                bool any_info = t->is_exists || t->is_bbl || !t->filament_setting_id.empty() || !t->setting_id.empty() || !t->type.empty() || !t->color.empty();
                                if (!any_info) continue;
                                AmsTrayInfo info;
                                info.ams_id      = ams_id;
                                info.ams_index   = ams_idx;
                                info.tray_id     = tray_id;
                                info.slot_index  = slot_idx;
                                try { info.filament_id = mo->get_filament_id(ams_id, tray_id); } catch (...) {}
                                try { info.filament_type = mo->get_filament_type(ams_id, tray_id); } catch (...) {}
                                try { info.color = t->color; } catch (...) {}
                                try { info.is_bbl = t->is_bbl; } catch (...) {}
                                try { info.exists = t->is_exists; } catch (...) {}
                                try { info.remain = t->remain; } catch (...) {}
                                try { info.nozzle_temp_min = t->nozzle_temp_min; } catch (...) {}
                                try { info.nozzle_temp_max = t->nozzle_temp_max; } catch (...) {}
                                try { info.setting_id = t->setting_id; } catch (...) {}
                                try { info.filament_setting_id = t->filament_setting_id; } catch (...) {}
                                result.trays.emplace_back(std::move(info));
                            }
                        }
                    } catch (...) { /* fallback best-effort */ }
                }

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
                                auto* fs2 = mo->GetFilaSystem();
                                if (fs2) {
                                    auto& ams_list2 = fs2->GetAmsList();
                                    for (auto &ap : ams_list2) {
                                        const std::string ams_id = ap.first;
                                        int ams_index = -1; try { ams_index = static_cast<int>(std::stoi(ams_id)); } catch (...) { ams_index = -1; }
                                        Slic3r::DevAms* ams = ap.second; if (!ams) continue;
                                        const auto& trays = ams->GetTrays();
                                        for (const auto &tp : trays) {
                                            const std::string tray_id = tp.first;
                                            Slic3r::DevAmsTray* t = tp.second; if (!t) continue;
                                            int slot_index = -1; try { slot_index = static_cast<int>(std::stoi(tray_id)); } catch (...) { slot_index = -1; }
                                            AmsTrayInfo info;
                                            info.ams_id = ams_id; info.ams_index = ams_index; info.tray_id = tray_id; info.slot_index = slot_index;
                                            try { info.filament_id = mo->get_filament_id(ams_id, tray_id); } catch (...) {}
                                            try { info.filament_type = mo->get_filament_type(ams_id, tray_id); } catch (...) {}
                                            try { info.color = t->color; } catch (...) {}
                                            try { info.is_bbl = t->is_bbl; } catch (...) {}
                                            try { info.exists = t->is_exists; } catch (...) {}
                                            try { info.remain = t->remain; } catch (...) {}
                                            try { info.nozzle_temp_min = t->nozzle_temp_min; } catch (...) {}
                                            try { info.nozzle_temp_max = t->nozzle_temp_max; } catch (...) {}
                                            try { info.setting_id = t->setting_id; } catch (...) {}
                                            try { info.filament_setting_id = t->filament_setting_id; } catch (...) {}
                                            retry.trays.emplace_back(std::move(info));
                                        }
                                    }
                                }
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
    reg.register_action<ImportParams, ImportResult>("import", [](const ImportParams& in) -> ImportResult {
        ImportResult out;
        if (in.path.empty()) { out.status = "error"; out.message = "missing path"; return out; }
        wxAppConsole* raw_app = wxAppConsole::GetInstance();
        if (!raw_app) { out.status = "error"; out.message = "no wxApp instance"; return out; }
        if (auto* gapp = dynamic_cast<Slic3r::GUI::GUI_App*>(raw_app)) {
            std::string tid = Slic3r::Utils::current_action_task_id();
            Slic3r::Utils::update_action_task(tid, "scheduled", "import scheduled");
            Slic3r::Utils::append_action_task_log(tid, std::string("import scheduled path='") + in.path + "'");
            gapp->CallAfter([p = in.path, gapp, tid]() {
                try {
                    Slic3r::Utils::update_action_task(tid, "running", "import running");
                    Slic3r::Utils::append_action_task_log(tid, "import running on UI thread");
                    Slic3r::GUI::Plater* plater = gapp->plater_;
                    if (!plater) { BOOST_LOG_TRIVIAL(error) << "[RestServer] Plater not available; cannot import: " << p; Slic3r::Utils::append_action_task_log(tid, "Plater not available; cannot import"); return; }
                    std::vector<std::string> files { p };
                    plater->load_files(files);
                    // Import runs synchronously on UI thread; mark as success when load_files returns
                    Slic3r::Utils::append_action_task_log(tid, "import submitted to Plater");
                    Slic3r::Utils::update_action_task(tid, "success", "import finished");
                    Slic3r::Utils::append_action_task_log(tid, "import finished");
                } catch (const std::exception &ex) {
                    BOOST_LOG_TRIVIAL(error) << "[RestServer] Exception while importing on UI thread: " << ex.what();
                    Slic3r::Utils::update_action_task(tid, "failed", ex.what());
                    Slic3r::Utils::append_action_task_log(tid, std::string("exception: ") + ex.what());
                }
            });
            out.status = "ok"; out.message = "scheduled import";
        } else {
            out.status = "error"; out.message = "wxApp is not GUI_App";
        }
        return out;
    });

    // slice_plate: {"action":"slice_plate","plate_index":0}
    reg.register_action<SliceParams, SliceResult>("slice_plate", [](const SliceParams& in) -> SliceResult {
        SliceResult out;
        if (in.plate_index && *in.plate_index < 0) { out.status = "error"; out.message = "invalid plate_index"; return out; }
        wxAppConsole* raw_app = wxAppConsole::GetInstance();
        if (!raw_app) { out.status = "error"; out.message = "no wxApp instance"; return out; }
        if (auto* gapp = dynamic_cast<Slic3r::GUI::GUI_App*>(raw_app)) {
            std::string tid = Slic3r::Utils::current_action_task_id();
            Slic3r::Utils::update_action_task(tid, "scheduled", "slice scheduled");
            if (in.plate_index && *in.plate_index >= 0)
                Slic3r::Utils::append_action_task_log(tid, std::string("slice scheduled plate_index=") + std::to_string(*in.plate_index));
            else
                Slic3r::Utils::append_action_task_log(tid, "slice scheduled (current plate)");
            gapp->CallAfter([opt_idx = in.plate_index, gapp, tid]() {
                try {
                    Slic3r::Utils::update_action_task(tid, "running", "slice running");
                    Slic3r::Utils::append_action_task_log(tid, "slice running on UI thread");
                    Slic3r::GUI::Plater* plater = gapp->plater_;
                    if (!plater) { BOOST_LOG_TRIVIAL(error) << "[RestServer] Plater not available; cannot slice plate"; Slic3r::Utils::append_action_task_log(tid, "Plater not available; cannot slice"); return; }
                    int idx = (opt_idx && *opt_idx >= 0) ? *opt_idx : plater->get_partplate_list().get_curr_plate_index();
                    plater->select_plate(idx, true);
                    plater->reslice();
                    Slic3r::Utils::update_action_task(tid, "submitted", "slice submitted");
                    Slic3r::Utils::append_action_task_log(tid, "slice submitted");
                    // Watch for slicing to finish and flip state to success
                    try {
                        auto done = std::make_shared<std::atomic<bool>>(false);
                        std::thread([gapp, tid, idx, done]() {
                            using namespace std::chrono;
                            auto deadline = steady_clock::now() + minutes(30);
                            while (!done->load()) {
                                std::this_thread::sleep_for(milliseconds(500));
                                if (steady_clock::now() > deadline) {
                                    // Timeout – mark as failed
                                    gapp->CallAfter([tid]() {
                                        Slic3r::Utils::append_action_task_log(tid, "slice timeout after 30 minutes");
                                        Slic3r::Utils::update_action_task(tid, "failed", "slice timeout");
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
                                        Slic3r::Utils::append_action_task_log(tid, "slice finished");
                                        Slic3r::Utils::update_action_task(tid, "success", "slice finished");
                                        done->store(true);
                                    }
                                });
                            }
                        }).detach();
                    } catch (...) { /* watcher best-effort */ }
                } catch (const std::exception &ex) {
                    BOOST_LOG_TRIVIAL(error) << "[RestServer] Exception while slicing on UI thread: " << ex.what();
                    Slic3r::Utils::update_action_task(tid, "failed", ex.what());
                    Slic3r::Utils::append_action_task_log(tid, std::string("exception: ") + ex.what());
                }
            });
            out.status = "ok"; out.message = "scheduled slice";
        } else { out.status = "error"; out.message = "wxApp is not GUI_App"; }
        return out;
    });

    // print_plate: {"action":"print_plate","plate_index":0,"dev_id":"...","dev_ip":"...","all":false}
    reg.register_action<PrintParamsReq, PrintResult>("print_plate", [](const PrintParamsReq& in) -> PrintResult {
        PrintResult out;
        if (in.plate_index && *in.plate_index < 0 && !in.all) { out.status = "error"; out.message = "invalid plate_index"; return out; }
        wxAppConsole* raw_app = wxAppConsole::GetInstance();
        if (!raw_app) { out.status = "error"; out.message = "no wxApp instance"; return out; }
        if (auto* gapp = dynamic_cast<Slic3r::GUI::GUI_App*>(raw_app)) {
            std::string pin = in.plate_index ? std::to_string(*in.plate_index) : std::string("current");
            BOOST_LOG_TRIVIAL(info) << "[RestServer] print_plate: scheduling print (plate_index=" << pin << ", all=" << in.all << ", dev_id='" << in.dev_id << "', dev_ip='" << in.dev_ip << "')";
            std::string tid = Slic3r::Utils::current_action_task_id();
            Slic3r::Utils::update_action_task(tid, "scheduled", "print scheduled");
            if (in.plate_index)
                Slic3r::Utils::append_action_task_log(tid, std::string("print scheduled plate_index=") + std::to_string(*in.plate_index) + (in.all?" (all)":""));
            else
                Slic3r::Utils::append_action_task_log(tid, std::string("print scheduled ") + (in.all?"(all current context)":"(current plate)"));
            gapp->CallAfter([in, gapp, tid]() {
                try {
                    Slic3r::Utils::update_action_task(tid, "running", "print running");
                    Slic3r::Utils::append_action_task_log(tid, "print running on UI thread");
                    Slic3r::GUI::Plater* plater = gapp->plater_;
                    if (!plater) { BOOST_LOG_TRIVIAL(error) << "[RestServer] UI thread: Plater not available; cannot print"; Slic3r::Utils::append_action_task_log(tid, "Plater not available; cannot print"); return; }
                    BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: Plater found, preparing PrintPrepareData";
                    Slic3r::Utils::append_action_task_log(tid, "Plater found, preparing job data");

                    int target_idx = (in.plate_index && *in.plate_index >= 0) ? *in.plate_index : plater->get_partplate_list().get_curr_plate_index();
                    BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: exporting 3mf for plate_index=" << target_idx;
                    Slic3r::Utils::append_action_task_log(tid, std::string("exporting 3mf for plate_index=") + std::to_string(target_idx));
                    int export_res = plater->send_gcode(target_idx, nullptr);
                    if (export_res < 0) {
                        BOOST_LOG_TRIVIAL(error) << "[RestServer] UI thread: export 3mf failed (code=" << export_res << ")";
                        Slic3r::Utils::update_action_task(tid, "failed", "export 3mf failed");
                        Slic3r::Utils::append_action_task_log(tid, std::string("export 3mf failed code=") + std::to_string(export_res));
                        return;
                    }

                    // Also export the config 3mf so params.config_filename is valid (fixes -2030 upload config failure)
                    Slic3r::Utils::append_action_task_log(tid, "exporting config 3mf");
                    int cfg_res = plater->export_config_3mf(target_idx, nullptr);
                    if (cfg_res < 0) {
                        BOOST_LOG_TRIVIAL(error) << "[RestServer] UI thread: export config 3mf failed (code=" << cfg_res << ")";
                        Slic3r::Utils::update_action_task(tid, "failed", "export config 3mf failed");
                        Slic3r::Utils::append_action_task_log(tid, std::string("export config 3mf failed code=") + std::to_string(cfg_res));
                        return;
                    }

                    Slic3r::GUI::PrintPrepareData job_data; plater->get_print_job_data(&job_data);
                    BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: got PrintPrepareData, 3mf_path='" << job_data._3mf_path.string() << "' config_3mf='" << job_data._3mf_config_path.string() << "'";
                    Slic3r::Utils::append_action_task_log(tid, std::string("prepared: 3mf='") + job_data._3mf_path.string() + "' cfg='" + job_data._3mf_config_path.string() + "'");

                    BBL::PrintParams params;
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

                    if (auto* tm = gapp->getTaskManager()) {
                        BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: TaskManager available; starting print via TaskManager";
                        Slic3r::Utils::append_action_task_log(tid, "starting print via TaskManager");
                        std::vector<BBL::PrintParams> jobs { params };
                        tm->start_print(jobs, nullptr);
                        BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: TaskManager::start_print invoked";
                        Slic3r::Utils::append_action_task_log(tid, "TaskManager::start_print invoked");
                    } else if (auto* agent = gapp->getAgent()) {
                        BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: NetworkAgent available; starting print via NetworkAgent";
                        Slic3r::Utils::append_action_task_log(tid, "starting print via NetworkAgent");
                        if (!params.dev_id.empty()) {
                            int set_ret = agent->set_user_selected_machine(params.dev_id);
                            BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: set_user_selected_machine returned " << set_ret << " for dev_id='" << params.dev_id << "'";
                            Slic3r::Utils::append_action_task_log(tid, std::string("set_user_selected_machine ret=") + std::to_string(set_ret));
                        }
                        BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: PrintParams filename='" << params.filename << "' config='" << params.config_filename << "' dev_id='" << params.dev_id << "' project='" << params.project_name << "' preset='" << params.preset_name << "' conn='" << params.connection_type << "'";
                        Slic3r::Utils::append_action_task_log(tid, std::string("params file='") + params.filename + "' cfg='" + params.config_filename + "' dev_id='" + params.dev_id + "' conn='" + params.connection_type + "'");
                        // Full parameter dump for diagnostics
                        try {
                            Slic3r::Utils::append_action_task_log(tid, dump_print_params(params));
                        } catch (...) { /* ignore logging errors */ }
                        try { std::error_code fec; bool exists = std::filesystem::exists(params.filename, fec); BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: filename exists=" << (exists?"true":"false") << ", ec='" << (fec ? fec.message() : std::string()) << "'"; } catch (...) {}
                        try { std::error_code cec; bool cexists = std::filesystem::exists(params.config_filename, cec); BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: config_filename exists=" << (cexists?"true":"false") << ", ec='" << (cec ? cec.message() : std::string()) << "'"; } catch (...) {}
                        try { std::error_code fec; bool exists = std::filesystem::exists(params.filename, fec); Slic3r::Utils::append_action_task_log(tid, std::string("file exists=") + (exists?"true":"false")); } catch (...) {}
                        try { std::error_code cec; bool cexists = std::filesystem::exists(params.config_filename, cec); Slic3r::Utils::append_action_task_log(tid, std::string("cfg exists=") + (cexists?"true":"false")); } catch (...) {}
                        if (!params.dev_id.empty()) {
                            if (auto* dm = gapp->getDeviceManager()) {
                                Slic3r::MachineObject* mo = dm->get_local_machine(params.dev_id);
                                if (!mo) mo = dm->get_user_machine(params.dev_id);
                                if (mo) {
                                    if (params.dev_ip.empty()) params.dev_ip = mo->get_dev_ip();
                                    params.username = "bblp";
                                    params.password = mo->get_access_code();
                                    params.use_ssl_for_ftp = mo->local_use_ssl_for_ftp;
                                    params.use_ssl_for_mqtt = mo->local_use_ssl_for_mqtt;
                                    params.ftp_folder = mo->get_ftp_folder();
                                    try { params.dev_name = mo->get_dev_name(); } catch (...) {}
                                    BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: populated from MachineObject: dev_ip='" << params.dev_ip << "' ftp_folder='" << params.ftp_folder << "'";
                                    Slic3r::Utils::append_action_task_log(tid, std::string("device: ip='") + params.dev_ip + "' ftp='" + params.ftp_folder + "'");
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
                            if (applied) Slic3r::Utils::append_action_task_log(tid, "applied client-provided AMS mapping");
                            else Slic3r::Utils::append_action_task_log(tid, "no AMS mapping provided; using current device settings");
                        } catch (...) {}
                        // Log complete PrintParams after device enrichment as well
                        try {
                            Slic3r::Utils::append_action_task_log(tid, dump_print_params(params));
                        } catch (...) { /* ignore logging errors */ }
                        if (!params.dev_id.empty()) {
                            // Use same credentials as GUI path: username "bblp" and access code password
                            int conn_ret = agent->connect_printer(params.dev_id, params.dev_ip, params.username, params.password, params.use_ssl_for_mqtt);
                            BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: connect_printer returned " << conn_ret;
                            Slic3r::Utils::append_action_task_log(tid, std::string("connect_printer ret=") + std::to_string(conn_ret));
                        }
                        // Wire callbacks to capture progress and key milestones
                        BBL::OnUpdateStatusFn upd = [tid](int status, int code, std::string msg){
                            try { Slic3r::Utils::append_action_task_log(tid, std::string("update status=") + std::to_string(status) + " code=" + std::to_string(code) + " msg=" + msg); } catch (...) {}
                            // Heuristics: mark submitted when we reach sending or waiting-for-printer stages
                            if (status == BBL::SendingPrintJobStage::PrintingStageSending || status == BBL::SendingPrintJobStage::PrintingStageWaitPrinter) {
                                Slic3r::Utils::update_action_task(tid, "submitted", "print job sent");
                            }
                            if (status == BBL::SendingPrintJobStage::PrintingStageFinished) {
                                Slic3r::Utils::update_action_task(tid, "success", "print job finished");
                            }
                            if (status == BBL::SendingPrintJobStage::PrintingStageERROR && code != 0) {
                                Slic3r::Utils::update_action_task(tid, "failed", std::string("print error: ") + net_code_to_str(code));
                            }
                        };
                        BBL::WasCancelledFn cancelled = [](){ return false; };
                        BBL::OnWaitFn waitfn = [tid](int status, std::string job_info){
                            try { Slic3r::Utils::append_action_task_log(tid, std::string("wait status=") + std::to_string(status)); } catch (...) {}
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
                            Slic3r::Utils::append_action_task_log(tid, std::string("preflight send_gcode ret=") + std::to_string(pre_res));
                            if (pre_res != 0) {
                                Slic3r::Utils::update_action_task(tid, "failed", "preflight failed: invalid access code or IP");
                                return;
                            }
                        }

                        // Log SSL flags for diagnostics
                        Slic3r::Utils::append_action_task_log(tid, std::string("ssl flags mqtt=") + (params.use_ssl_for_mqtt?"true":"false") + ", ftp=" + (params.use_ssl_for_ftp?"true":"false"));

                        // Prefer LAN local print WITH RECORD first (matches UI flow); then plain local, then cloud
                        int netres = agent->start_local_print_with_record(params, upd, cancelled, waitfn);
                        BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: start_local_print_with_record returned " << netres;
                        Slic3r::Utils::append_action_task_log(tid, std::string("start_local_print_with_record ret=") + std::to_string(netres) + " (" + net_code_to_str(netres) + ")");
                        if (netres < 0) {
                            netres = agent->start_local_print(params, upd, cancelled);
                            BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: start_local_print returned " << netres;
                            Slic3r::Utils::append_action_task_log(tid, std::string("start_local_print ret=") + std::to_string(netres) + " (" + net_code_to_str(netres) + ")");
                            // Retry once on MQTT publish failure by toggling SSL for MQTT and reconnecting (now unconditional and safe)
                            if (netres == BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED) {
                                Slic3r::Utils::append_action_task_log(tid, "retry: toggling mqtt ssl and reconnecting");
                                // Work on a copy to avoid side-effects if other branches re-use params
                                BBL::PrintParams retry_params = params;
                                retry_params.use_ssl_for_mqtt = !params.use_ssl_for_mqtt;
                                if (!retry_params.dev_id.empty()) {
                                    int rc = agent->connect_printer(retry_params.dev_id, retry_params.dev_ip, retry_params.username, retry_params.password, retry_params.use_ssl_for_mqtt);
                                    Slic3r::Utils::append_action_task_log(tid, std::string("reconnect ret=") + std::to_string(rc));
                                }
                                int retry = agent->start_local_print(retry_params, upd, cancelled);
                                BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: start_local_print (retry ssl toggle) returned " << retry;
                                Slic3r::Utils::append_action_task_log(tid, std::string("start_local_print retry ret=") + std::to_string(retry) + " (" + net_code_to_str(retry) + ")");
                                if (retry >= 0) netres = retry;
                            }
                        }
                        if (netres < 0) { netres = agent->start_print(params, upd, cancelled, waitfn); BOOST_LOG_TRIVIAL(info) << "[RestServer] UI thread: start_print returned " << netres; }
                        if (netres < 0) { Slic3r::Utils::append_action_task_log(tid, std::string("start_print ret=") + std::to_string(netres) + " (" + net_code_to_str(netres) + ")"); }
                        if (netres < 0) {
                            Slic3r::Utils::update_action_task(tid, "failed", std::string("network start failed: ") + std::to_string(netres));
                            Slic3r::Utils::append_action_task_log(tid, std::string("network start failed code=") + std::to_string(netres));
                        } else {
                            Slic3r::Utils::update_action_task(tid, "submitted", "print job sent");
                            Slic3r::Utils::append_action_task_log(tid, "print job sent");
                        }
                    } else {
                        BOOST_LOG_TRIVIAL(error) << "[RestServer] UI thread: No TaskManager or NetworkAgent available; cannot start print";
                        Slic3r::Utils::update_action_task(tid, "failed", "no TaskManager or NetworkAgent");
                        Slic3r::Utils::append_action_task_log(tid, "no TaskManager or NetworkAgent available");
                    }
                } catch (const std::exception &ex) {
                    BOOST_LOG_TRIVIAL(error) << "[RestServer] Exception while printing on UI thread: " << ex.what();
                    Slic3r::Utils::update_action_task(tid, "failed", ex.what());
                    Slic3r::Utils::append_action_task_log(tid, std::string("exception: ") + ex.what());
                }
            });
            out.status = "ok"; out.message = "scheduled print";
        } else { out.status = "error"; out.message = "wxApp is not GUI_App"; }
        return out;
    });

    // list_devices: {"action":"list_devices"}
    auto list_devices_fn = [](const DevicesParams& in) -> DevicesResult {
        DevicesResult out;
        wxAppConsole* raw_app = wxAppConsole::GetInstance();
        if (!raw_app) { out.status = "error"; out.message = "no wxApp instance"; return out; }
        auto* gapp = dynamic_cast<Slic3r::GUI::GUI_App*>(raw_app);
        if (!gapp) { out.status = "error"; out.message = "wxApp is not GUI_App"; return out; }

        auto prom = std::make_shared<std::promise<DevicesResult>>();
        auto fut  = prom->get_future();
        gapp->CallAfter([prom, gapp, in]() mutable {
            DevicesResult result; result.status = "ok"; result.message = "ok";
            try {
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
                prom->set_value(std::move(result));
            } catch (const std::exception& ex) {
                DevicesResult err; err.status = "error"; err.message = std::string("exception: ") + ex.what();
                prom->set_value(std::move(err));
            } catch (...) {
                DevicesResult err; err.status = "error"; err.message = "unknown exception";
                prom->set_value(std::move(err));
            }
        });

        if (fut.wait_for(std::chrono::seconds(1)) == std::future_status::ready) {
            return fut.get();
        } else {
            out.status = "error"; out.message = "timeout waiting for GUI thread"; return out;
        }
    };

    reg.register_action<DevicesParams, DevicesResult>("devices", list_devices_fn);

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
        GetLogsResult out; out.status = "error";
        try {
            const std::string base_dir = Slic3r::data_dir();
            if (base_dir.empty()) { out.message = "data_dir is empty"; return out; }
            boost::filesystem::path log_folder = boost::filesystem::path(base_dir) / "log";
            if (!boost::filesystem::exists(log_folder)) { out.message = "log folder does not exist"; return out; }

            // Security: only allow basename in 'file'
            auto contains_sep = [](const std::string& s){ return s.find('/') != std::string::npos || s.find('\\') != std::string::npos; };
            if (!in.file.empty() && contains_sep(in.file)) { out.message = "invalid file parameter"; return out; }

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

            if (target.empty()) { out.message = in.file.empty() ? "no log files found" : "specified log file not found"; return out; }

            // Tail read
            boost::nowide::ifstream ifs(target.string(), std::ios::in | std::ios::binary);
            if (!ifs) { out.message = "failed to open log file"; return out; }
            ifs.seekg(0, std::ios::end);
            std::uint64_t fsize = static_cast<std::uint64_t>(ifs.tellg());
            std::uint64_t maxb  = in.max_bytes == 0 ? 65536ULL : in.max_bytes;
            std::uint64_t start = (fsize > maxb) ? (fsize - maxb) : 0ULL;
            std::uint64_t readn = fsize - start;
            ifs.seekg(static_cast<std::streamoff>(start), std::ios::beg);
            std::string buf;
            buf.resize(static_cast<size_t>(readn));
            if (readn > 0) ifs.read(&buf[0], static_cast<std::streamsize>(readn));

            out.status    = "ok";
            out.message   = "ok";
            out.file      = target.filename().string();
            out.content   = std::move(buf);
            out.truncated = (start != 0ULL);
            out.size      = fsize;
            out.returned  = readn;
            return out;
        } catch (const std::exception& ex) {
            out.status = "error"; out.message = std::string("exception: ") + ex.what(); return out;
        } catch (...) {
            out.status = "error"; out.message = "unknown exception"; return out;
        }
    });
}
