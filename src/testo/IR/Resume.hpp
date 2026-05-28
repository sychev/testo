
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <map>
#include <memory>
#include "../lexer/Pos.hpp"
#include "../resolver/Stack.hpp"

namespace IR {

// Identifies a location of a `snapshot create` AST node in the source.
// Equality uses (file, offset) to be robust to line/column reformatting
// of unrelated parts of the file - though see ResumeInfo::test_cksum:
// any change to the test body invalidates the resume.
struct ResumePos {
	std::string file;
	size_t offset = 0;
	uint32_t line = 0;
	uint32_t column = 0;

	nlohmann::json to_json() const;
	static ResumePos from_json(const nlohmann::json& j);
	static ResumePos from_pos(const Pos& pos);
	bool matches(const Pos& pos) const;
};

// Persisted along with each `<test>_tmp` snapshot. Captures enough state for
// the interpreter to fast-forward through the test body on rerun until it
// reaches the recorded `snapshot create` and resume normal execution from there.
struct ResumeInfo {
	std::string test_cksum;                  // matched against IR::Test::cksum on rerun
	nlohmann::json stack_frames;             // produced by stack_to_json, innermost first
	ResumePos pos;
	std::map<std::string, bool> vm_running;  // VM id -> was the VM running at snapshot time

	nlohmann::json to_json() const;
	static ResumeInfo from_json(const nlohmann::json& j);
};

}
