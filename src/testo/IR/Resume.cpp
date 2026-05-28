
#include "Resume.hpp"

namespace IR {

nlohmann::json ResumePos::to_json() const {
	return nlohmann::json{
		{"file", file},
		{"offset", offset},
		{"line", line},
		{"column", column},
	};
}

ResumePos ResumePos::from_json(const nlohmann::json& j) {
	ResumePos result;
	result.file = j.at("file").get<std::string>();
	result.offset = j.at("offset").get<size_t>();
	result.line = j.at("line").get<uint32_t>();
	result.column = j.at("column").get<uint32_t>();
	return result;
}

ResumePos ResumePos::from_pos(const Pos& pos) {
	ResumePos result;
	result.file = pos.file.generic_string();
	result.offset = pos.offset;
	result.line = pos.line;
	result.column = pos.column;
	return result;
}

bool ResumePos::matches(const Pos& pos) const {
	return pos.offset == offset && pos.file.generic_string() == file;
}

nlohmann::json ResumeInfo::to_json() const {
	return nlohmann::json{
		{"test_cksum", test_cksum},
		{"stack_frames", stack_frames},
		{"pos", pos.to_json()},
		{"vm_running", vm_running},
	};
}

ResumeInfo ResumeInfo::from_json(const nlohmann::json& j) {
	ResumeInfo result;
	result.test_cksum = j.at("test_cksum").get<std::string>();
	result.stack_frames = j.at("stack_frames");
	result.pos = ResumePos::from_json(j.at("pos"));
	// vm_running was introduced after the initial version of resume metadata;
	// older _tmp snapshots may not contain it - default to empty.
	if (j.count("vm_running")) {
		result.vm_running = j.at("vm_running").get<std::map<std::string, bool>>();
	}
	return result;
}

}
