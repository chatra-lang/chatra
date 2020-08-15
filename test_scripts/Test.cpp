/*
 * Programming language 'Chatra' reference implementation
 *
 * Copyright(C) 2019 Chatra Project Team
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * author: Satoshi Hosokawa (chatra.hosokawa@gmail.com)
 */

#include "chatra.h"
#include "../chatra_core/Runtime.h"
#include <random>

#ifdef NDEBUG
	#error CHATRA_ENABLE_LANGUAGE_TESTS only works with debug version of chatra_core (!NDEBUG)
#endif

cha::PackageInfo getTestNativePackage();

struct OptionError : public std::exception {};
struct TestFailedException : public std::exception {};

static bool stdoutEnabled = true;

static void enableStdout(bool enabled) {
	cha::enableStdout(enabled);
	stdoutEnabled = enabled;
}

static std::string loadScript(const char* fileName) {
	FILE* fp = fopen(fileName, "rt");
	if (fp == nullptr)
		throw cha::PackageNotFoundException();
	fseek(fp, 0, SEEK_END);
	std::vector<char> buffer(static_cast<size_t>(ftell(fp)));
	fseek(fp, 0, SEEK_SET);
	auto size = fread(buffer.data(), 1, buffer.size(), fp);
	fclose(fp);
	buffer.push_back('\0');
	return std::string(buffer.cbegin(), buffer.cbegin() + size);
}

class LanguageTestHost : public cha::IHost {
public:
	void console(const std::string& message) override {
		if (!stdoutEnabled)
			return;
		std::printf("%s", message.data());
		std::fflush(stdout);
	}

	cha::PackageInfo queryPackage(const std::string& packageName) override {
		if (packageName == "test_native")
			return getTestNativePackage();

		try {
			return {{{packageName, loadScript((std::string("test_scripts/") + packageName + ".cha").data())}}, {}, nullptr};
		}
		catch (const cha::PackageNotFoundException&) {
			return cha::queryEmbeddedPackage(packageName);
		}
	}

	cha::IDriver* queryDriver(cha::DriverType driverType) override {
		switch (driverType) {
		case cha::DriverType::FileSystem:  return cha::getStandardFileSystem();
		default:  return nullptr;
		}
	}
};

static constexpr const char* standardTests[] = {
		"lex_and_operators",
		"control_flow",
		"special_operators",
		"memory_management",
		"function_call",
		"classes",
		"scope_and_visibility",
		"imports",
		"concurrent",
		"native_call",
		"exceptions",
		"embedded_containers",
		"syntax_sugar",
		"emb_sys",
		"emb_format",
		"emb_regex",
		"emb_containers",
		"emb_io",
		"emb_random",
		"emb_math",
};

static constexpr const char* multiThreadTests[] = {
		"concurrent"
};

static constexpr const char* serializeTests[] = {
		"lex_and_operators",
		"control_flow",
		"special_operators",
		"memory_management",
		"function_call",
		"classes",
		"scope_and_visibility",
		"imports",
		"concurrent",
		"native_call",
		"exceptions",
		"embedded_containers",
		"syntax_sugar",
		"emb_sys",
		"emb_format",
		"emb_regex",
		"emb_containers",
		"emb_io",
		"emb_random",
		"emb_math",
};

static void runTest(std::shared_ptr<cha::Runtime>& runtime, const char* fileName) {
	if (stdoutEnabled) {
		std::printf("runTest: %s\n", fileName);
		std::fflush(stdout);
	}
	cha::beginCheckScript();
	auto packageId = runtime->loadPackage(fileName);
	runtime->run(packageId);
	runtime->loop();
	cha::endCheckScript();
}

static void runTest() {
	//const std::vector<const char*> standardTests = {"emb_sys"};

	cha::setTestMode("baseline");

	auto host = std::make_shared<LanguageTestHost>();
	auto runtime = cha::Runtime::newInstance(host);
	runtime->addTimer("test");

	for (auto& test : standardTests)
		runTest(runtime, test);

	runtime->shutdown();
	if (!cha::showResults())
		throw TestFailedException();
}

static void runMtTest(std::shared_ptr<cha::Runtime>& runtime, const char* fileName) {
	if (stdoutEnabled) {
		std::printf("runMtTest: %s\n", fileName);
		std::fflush(stdout);
	}
	cha::beginCheckScript();
	auto packageId = runtime->loadPackage(fileName);
	runtime->run(packageId);
	cha::waitUntilFinished();
	cha::endCheckScript();
}

static void runMtTest(unsigned threadCount) {
	cha::setTestMode("baseline_mt");

	auto host = std::make_shared<LanguageTestHost>();
	auto runtime = cha::Runtime::newInstance(host, {}, threadCount);
	runtime->addTimer("test");

	for (auto& test : multiThreadTests)
		runMtTest(runtime, test);

	runtime->shutdown();
	if (!cha::showResults())
		throw TestFailedException();
}

static void runGcTest() {
	cha::setTestMode("gc");

	enableStdout(false);

	auto host = std::make_shared<LanguageTestHost>();
	auto runtime = cha::Runtime::newInstance(host);  // single thread mode
	runtime->addTimer("test");

	auto runtimeImp = std::static_pointer_cast<cha::RuntimeImp>(runtime);

	for (size_t i = 0; i < 20; i++) {
		std::printf("objectCount = %u\n", static_cast<unsigned>(runtimeImp->objectCount()));
		for (auto& test : standardTests)
			runTest(runtime, test);
	}

	enableStdout(true);

	runtime->shutdown();
	if (!cha::showResults())
		throw TestFailedException();
}

static void runSerializeTest(const std::shared_ptr<cha::IHost>& host,
		const char* fileName, const std::vector<unsigned>& step) {

	std::printf("%s:", fileName);
	for (auto s : step)
		std::printf(" %u", s);
	std::printf("\n");
	std::fflush(stdout);

	auto runtime = cha::Runtime::newInstance(host);
	runtime->addTimer("test");

	cha::beginCheckScript();
	auto packageId = runtime->loadPackage(fileName);
	runtime->run(packageId);

	for (auto s : step) {
		for (unsigned i = 0; i < s; i++) {
			if (!runtime->handleQueue())
				break;
		}
		auto stream = runtime->shutdown(true);
		std::printf("stream size = %u\n", static_cast<unsigned>(stream.size()));

		runtime.reset();
		runtime = cha::Runtime::newInstance(host, stream);
	}
	runtime->loop();

	cha::endCheckScript();
}

static void runSerializeTest(unsigned tryCount) {
	cha::setTestMode("serialize");

	std::vector<std::tuple<std::string, unsigned>> measured;
	auto host = std::make_shared<LanguageTestHost>();

	std::printf("measuring:\n");
	enableStdout(false);
	for (auto& test : serializeTests) {
		auto runtime = cha::Runtime::newInstance(host);
		runtime->addTimer("test");

		cha::beginCheckScript();
		auto packageId = runtime->loadPackage(test);
		runtime->run(packageId);
		unsigned count = 0;
		while (runtime->handleQueue())
			count++;
		cha::endCheckScript();

		measured.emplace_back(test, count);
		std::printf("%8u %s\n", count, test);
	}

	std::printf("test:\n");

	std::random_device seedGenerator;
	std::mt19937 random(seedGenerator());

	enableStdout(false);
	for (unsigned count = 0; count < tryCount; count++) {
		auto index = std::uniform_int_distribution<size_t>(0, measured.size() - 1)(random);
		auto& e = measured[index];

		auto divCount = std::uniform_int_distribution<unsigned>(1, 7)(random);
		std::vector<unsigned> step(divCount);
		for (unsigned i = 0; i < divCount; i++)
			step[i] = std::uniform_int_distribution<unsigned>(1, std::get<1>(e) - 1)(random);
		std::sort(step.begin(), step.end());
		step.emplace_back(std::get<1>(e) * 11 / 10);
		for (unsigned i = divCount; i-- > 0; )
			step[static_cast<size_t>(i) + 1] -= step[i];

		runSerializeTest(host, std::get<0>(e).data(), step);
	}
}

static void runSerializeTest(const std::string& pattern) {
	cha::setTestMode("serialize");

	auto test = pattern.substr(0, pattern.find_first_of(':'));
	auto sequence = pattern.substr(test.length() + 2);

	std::vector<unsigned> step;
	for (;;) {
		auto pos = sequence.find_first_of(' ');
		step.emplace_back(std::stoi(sequence.substr(0, pos)));
		if (pos == std::string::npos)
			break;
		sequence = sequence.substr(pos + 1);
	}

	auto host = std::make_shared<LanguageTestHost>();
	runSerializeTest(host, test.data(), step);
}

class ErrorReceiver : public cha::IErrorReceiver {
	void error(cha::ErrorLevel level,
			const std::string& fileName, unsigned lineNo, const std::string& line, size_t first, size_t last,
			const std::string& message, const std::vector<std::string>& args) override {

		auto str = cha::RuntimeImp::formatError(level, fileName, lineNo, line, first, last, message, args);
		std::fprintf(stderr, "%s", str.data());
	}
};

static void parseFile(const std::string& fileName) {
	try {
		auto script = loadScript(fileName.data());
		
		cha::initializeLexicalAnalyzer();
		cha::initializeParser();

		auto sTable = cha::StringTable::newInstance();

		auto errorReceiver = std::make_shared<ErrorReceiver>();
		auto lines = cha::parseLines(*errorReceiver, sTable, fileName, 1, script);
		for (auto& line : lines)
			cha::dump(sTable, line);

		printf("\n\n");
		cha::ParserWorkingSet ws;
		auto node = cha::groupScript(*errorReceiver, sTable, lines);
		cha::structureInnerNode(*errorReceiver, sTable, node.get(), true);
		cha::parseInnerNode(ws, *errorReceiver, sTable, node.get(), true);
		cha::dump(sTable, node);
	}
	catch (const cha::PackageNotFoundException&) {
		std::fprintf(stderr, "File not found\n");
		throw TestFailedException();
	}
	catch (const cha::AbortCompilingException&) {
		std::fprintf(stderr, "AbortCompilingException raised\n");
		throw TestFailedException();
	}
}

// TODO exception tests
//   classes_duplicated_variables -> ParserErrorException
//   classes_duplicated_class -> ParserErrorException
//   unhandled_exception_throw -> ?
//   unhandled_exception_error -> ?

int runLanguageTests(int argc, char* argv[]) {
	bool optRunTest = false;
	bool optRunGcTest = false;
	unsigned optRunSerializeTest = 0;
	std::string optSerializePattern;
	std::string optParseFile;

	try {
		for (int i = 1; i < argc; i++) {
			std::string opt(argv[i]);

			if (opt == "--baseline") {
				optRunTest = true;
			}
			else if (opt == "--gc") {
				optRunGcTest = true;
			}
			else if (opt == "--serialize") {
				if (i + 1 >= argc)
					throw OptionError();
				optRunSerializeTest = static_cast<unsigned>(std::stoi(argv[++i]));
			}
			else if (opt == "--serialize-reproduce") {
				if (i + 1 >= argc)
					throw OptionError();
				optSerializePattern = argv[++i];
			}
			else if (opt == "--parse") {
				if (i + 1 >= argc)
					throw OptionError();
				optParseFile = argv[++i];
			}
			else if (opt == "-diag")  // What's this?  This is observed only under CTest.
				continue;
			else
				throw OptionError();
		}
	}
	catch (const OptionError&) {
		std::fprintf(stderr, "Unknown option or syntax error\n");
		return 1;
	}
	catch (const std::invalid_argument&) {
		std::fprintf(stderr, "Expected integer\n");
		return 1;
	}
	catch (const std::out_of_range&) {
		std::fprintf(stderr, "An integer argument is out of range\n");
		return 1;
	}

	try {
		if (optRunTest) {
			runTest();
			runMtTest(1);
			runMtTest(3);
		}

		if (optRunGcTest)
			runGcTest();

		if (optRunSerializeTest != 0)
			runSerializeTest(optRunSerializeTest);

		if (!optSerializePattern.empty())
			runSerializeTest(optSerializePattern);

		if (!optParseFile.empty())
			parseFile(optParseFile);
	}
	catch (const TestFailedException&) {
		std::fprintf(stderr, "Language test failed.\n");
		return 1;
	}

	return 0;
}
