
#include <catch.hpp>
#include "../parser/Parser.hpp"

void TestParseStringifyActions(const std::string& str) {
	auto block = Parser(".", str).action_block();
	auto str2 = block->to_string();
	REQUIRE(str == str2);
}

TEST_CASE("parse action wait") {
	TestParseStringifyActions("{ wait \"hello world\" interval 32s timeout 65ms; }");
	TestParseStringifyActions("{ wait \"hello world\" timeout 65ms interval 32s; }");
	TestParseStringifyActions("{ wait \"hello world\"; }");
	TestParseStringifyActions("{ wait \"hello world\" timeout 65ms; }");
	TestParseStringifyActions("{ wait \"hello world\" interval 32s; }");
	TestParseStringifyActions("{ wait \"hello world\" timeout \"${SOME_PARAM}\" interval \"some_prefix_${SOME_OTHER_PARAM}\"; }");
}

TEST_CASE("parse action type") {
	TestParseStringifyActions("{ type \"hello world\" interval 32s; }");
	TestParseStringifyActions("{ type \"hello world\" interval 32s autoswitch LEFTALT+LEFTSHIFT; }");
	TestParseStringifyActions("{ type \"hello world\" interval \"1ms\" autoswitch \"LEFTALT+SPACE\"; }");
}

TEST_CASE("parse action macro call") {
	TestParseStringifyActions("{ some_macro(); }");
	TestParseStringifyActions("{ some_macro(\"10\", \"hello world\"); }");
}

TEST_CASE("parse action mouse click") {
	TestParseStringifyActions("{ mouse click \"Next\".from_right(0).center_bottom(); }");
}

TEST_CASE("parse parallel block") {
	auto block = Parser(".", R"({
		parallel {
			vm_server1 {
				start
				type "configure.sh"; press Enter
				wait "DONE"
			}
			vm_server2 { start; wait "DONE"; }
			configure("vm_client1")
		}
		vm_server1 { stop; }
	})").command_block();

	REQUIRE(block->items.size() == 2);

	auto parallel = std::dynamic_pointer_cast<AST::ParallelBlock>(block->items.at(0));
	REQUIRE(parallel != nullptr);
	REQUIRE(parallel->block->items.size() == 3);

	//a regular command after the parallel block is still parsed
	REQUIRE(std::dynamic_pointer_cast<AST::RegularCmd>(block->items.at(1)) != nullptr);
}

TEST_CASE("parse nested parallel block") {
	auto block = Parser(".", R"({
		parallel {
			vm_a { start; }
			parallel {
				vm_b { start; }
				vm_c { start; }
			}
		}
	})").command_block();

	auto parallel = std::dynamic_pointer_cast<AST::ParallelBlock>(block->items.at(0));
	REQUIRE(parallel != nullptr);
	REQUIRE(std::dynamic_pointer_cast<AST::ParallelBlock>(parallel->block->items.at(1)) != nullptr);
}
