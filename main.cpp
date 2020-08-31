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
#include <deque>
#include <thread>
#include <cstdio>

#ifdef CHATRA_ENABLE_LANGUAGE_TESTS
int runLanguageTests(int argc, char* argv[]);
#endif

struct OptionError : std::exception {};

static std::string argv0;
static std::vector<std::string> optFiles;
static std::vector<std::string> optPaths;
static unsigned optThreads = std::numeric_limits<unsigned>::max();

static void help(FILE* stream) {
	std::fprintf(stream, "usage: %s [options...] [scriptfiles... (or stdin if omitted)]\n"
			"options:\n"
			" -h, --help\n"
			"    print this help message and exit\n"
			" -t [NUM], --thread [NUM]\n"
			"    multi-thread mode with number of threads as NUM\n"
			" -I [PATH]\n"
			"    add directory of searching scripts\n"
			"    (This is applied only for \"import\", not for command line)\n",
			argv0.data());
}

static std::string loadScript(const char* fileName) {
	FILE* fp = std::fopen(fileName, "rt");
	if (fp == nullptr)
		throw cha::PackageNotFoundException();
	std::fseek(fp, 0, SEEK_END);
	std::vector<char> buffer(static_cast<size_t>(ftell(fp)));
	std::fseek(fp, 0, SEEK_SET);
	auto size = std::fread(buffer.data(), 1, buffer.size(), fp);
	std::fclose(fp);
	return std::string(buffer.cbegin(), buffer.cbegin() + size);
}

static std::string loadStdin() {
	std::vector<char> buffer;
	std::vector<char> tmp(1024);
	while (!feof(stdin)) {
		for (;;) {
			auto size = std::fread(tmp.data(), 1, tmp.size(), stdin);
			buffer.insert(buffer.cend(), tmp.cbegin(), tmp.cbegin() + size);
			if (size < tmp.size())
				break;
		}
	}
	return std::string(buffer.cbegin(), buffer.cend());
}

template <class Type, CHATRA_WHEN(std::is_integral<Type>::value)>
static Type consume(const std::string& opt, std::deque<std::string>& args) {
	if (args.empty()) {
		std::fprintf(stderr, "option \"%s\" requires additional an integer parameter\n", opt.data());
		throw OptionError();
	}
	try {
		auto ret = std::stoi(args.front());
		args.pop_front();
		return static_cast<Type>(ret);
	}
	catch (const std::exception&) {
		std::fprintf(stderr, "parameter for option \"%s\" is not a number or overflow\n", opt.data());
		throw OptionError();
	}
}

template <class Type, CHATRA_WHEN(std::is_same<Type, std::string>::value)>
static Type consume(const std::string& opt, std::deque<std::string>& args) {
	if (args.empty()) {
		std::fprintf(stderr, "option \"%s\" requires additional an string parameter\n", opt.data());
		throw OptionError();
	}
	auto ret = args.front();
	args.pop_front();
	return ret;
}

class Host : public cha::IHost {
public:
	void console(const std::string& message) override {
		std::printf("%s", message.data());
		std::fflush(stdout);
	}

	cha::PackageInfo queryPackage(const std::string& packageName) override {
		for (auto& path : optPaths) {
			try {
				auto prefix = (path.empty() ? "" : path.back() == '/' ? path : path + "/");
				return {{{packageName, loadScript((prefix + packageName + ".cha").data())}}, {}, nullptr};
			}
			catch (const cha::PackageNotFoundException&) {
				// do nothing
			}
		}
		return cha::queryEmbeddedPackage(packageName);
	}

	cha::IDriver* queryDriver(cha::DriverType driverType) override {
		switch (driverType) {
		case cha::DriverType::FileSystem:  return cha::getStandardFileSystem();
		default:  return nullptr;
		}
	}
};

int main(int argc, char* argv[]) {
#if 0
	const char* args_test[] = {"chatra", "--language-test", "--baseline"};
	//const char* args_test[] = {"chatra", "--language-test", "--serialize", "1000"};
	//const char* args_test[] = {"chatra", "--language-test", "--serialize-reproduce", "emb_format: 226 1956 197 787 479 54 709"};
	//const char* args_test[] = {"chatra", "samples_in_docs/readme_first_sample.cha"};
	//const char* args_test[] = {"chatra", "--help"};
	//const char* args_test[] = {"chatra", "--language-test", "--parse", "chatra_emb/containers.cha"};
	argc = sizeof(args_test) / sizeof(args_test[0]);
	argv = const_cast<char**>(args_test);
#endif

#ifdef CHATRA_ENABLE_LANGUAGE_TESTS
	if (argc >= 2 && std::string(argv[1]) == "--language-test")
		return runLanguageTests(argc - 1, argv + 1);
#endif

	argv0 = argv[0];
	std::deque<std::string> args;
	for (int i = 1; i < argc; i++)
		args.emplace_back(argv[i]);

	optPaths.emplace_back("");

	try {
		while (!args.empty()) {
			auto arg = args.front();
			args.pop_front();

			if (arg == "-h" || arg == "--help") {
				help(stdout);
				return 0;
			}
			else if (arg == "-t" || arg == "--thread")
				optThreads = consume<unsigned>(arg, args);
			else if (arg == "-I")
				optPaths.emplace_back(consume<std::string>(arg, args));
			else
				optFiles.emplace_back(std::move(arg));
		}
	}
	catch (const OptionError&) {
		help(stderr);
		return 1;
	}

	if (optFiles.empty())
		optFiles.emplace_back("");

	auto host = std::make_shared<Host>();
	auto runtime = cha::Runtime::newInstance(host, {}, optThreads);

	int ret = 0;
	std::vector<cha::InstanceId> instanceIds;
	for (auto& file : optFiles) {
		auto name = (file.empty() ? "stdin" : file);
		std::string script;
		try {
			script = (file.empty() ? loadStdin() : loadScript(file.data()));
		}
		catch (const cha::PackageNotFoundException&) {
			std::fprintf(stderr, "%s: file not found\n", name.data());
			ret = 1;
			break;
		}

		auto packageId = runtime->loadPackage(cha::Script{name, script});
		auto instanceId = runtime->run(packageId);

		if (optThreads == std::numeric_limits<unsigned>::max())
			runtime->loop();
		else
			instanceIds.emplace_back(instanceId);
	}

	while (!instanceIds.empty()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (!runtime->isRunning(instanceIds.back()))
			instanceIds.pop_back();
	}

	runtime->shutdown();

	return ret;
}
