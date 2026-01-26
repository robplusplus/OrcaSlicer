#include "RestUtils.hpp"
#include <sstream>

namespace Slic3r {
namespace Utils {

const char* net_code_to_str(int code)
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

std::string dump_print_params(const BBL::PrintParams& p)
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

} // namespace Utils
} // namespace Slic3r
