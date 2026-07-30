#pragma once

#include "sublift/models/model_bundle.hpp"

namespace sublift::paddle_detail {

using ModelType = sublift::models::ModelType;
using ModelPaths = sublift::models::ModelPaths;
using ModelBundle = sublift::models::ModelBundle;

using sublift::models::parse_model_type;
using sublift::models::model_type_to_string;
using sublift::models::expand_user_path;
using sublift::models::resolve_model_dir;
using sublift::models::get_expected_model_paths;
using sublift::models::validate_model_paths;

}  // namespace sublift::paddle_detail
