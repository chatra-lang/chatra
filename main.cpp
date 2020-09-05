/*
 * Programming language 'Chatra' reference implementation
 *
 * Copyright(C) 2019-2020 Chatra Project Team
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
#include <cstring>
#include <algorithm>
#include "chatra_core/CharacterClass.h"

#ifdef CHATRA_ENABLE_LANGUAGE_TESTS
int runLanguageTests(int argc, char* argv[]);
#endif

struct OptionError : std::exception {};

enum ScriptSourceType {
	Stdin, File, Arg
};

static std::string argv0;
static std::vector<std::tuple<ScriptSourceType, size_t, std::string>> optFiles;
static std::vector<std::string> optPaths;
static unsigned optThreads = std::numeric_limits<unsigned>::max();

static void help(FILE* stream) {
	std::fprintf(stream, "usage: %s [options...] [script files... (or stdin if omitted)]\n"
			"options:\n"
			" -h, --help\n"
			"    print this help message and exit\n"
			" -t <NUM>, --thread <NUM>\n"
			"    multi-thread mode with number of threads as NUM\n"
			" -I <PATH>\n"
			"    add directory of searching scripts\n"
			"    (This is applied only for \"import\", not for command line)\n"
			" -c <script>\n"
			"    script passed by string; can be specified with multiple times\n",
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

static std::string outputLine(std::string& out, unsigned indent, const std::string& line) {
	auto it0 = std::find_if(line.cbegin(), line.cend(), [](char c) { return c != ' ' && c != '\t'; });
	if (it0 == line.cend())
		return "";
	auto processedLine = std::string(it0, line.cend());
	out += std::string(indent, '\t') + processedLine + "\n";
	return processedLine;
}

static size_t findToken(const std::string& str, size_t offset = 0) {
	auto it = str.cbegin() + offset;
	if (it == str.cend() || !cha::isBeginningOfName(*it))
		return 0;
	while (++it != str.cend()) {
		if (!cha::isPartOfName(*it))
			break;
	}
	return std::distance(str.cbegin(), it) - offset;
}

static size_t skipColon(const std::string& str, size_t offset) {
	auto it = str.cbegin() + offset;
	while (it != str.cend() && cha::isSpace(*it))
		it++;
	if (it == str.cend() || *it++ != ':')
		return 0;
	while (it != str.cend() && cha::isSpace(*it))
		it++;
	return std::distance(str.cbegin(), it) - offset;
}

static std::string parseArg(size_t index, const std::string& arg) {
	(void)index;

	unsigned lvComments = 0;
	char quote = '\0';
	unsigned lvParentheses = 0;
	unsigned indent = 0;

	std::string out;
	std::string line;
	for (auto it = arg.cbegin(); it != arg.cend(); ) {
		auto left = std::distance(it, arg.cend());
		if (left >= 2 && std::equal(it, it + 2, "/*")) {
			lvComments++;
			it += 2;
			continue;
		}
		if (lvComments > 0 && left >= 2 && std::equal(it, it + 2, "*/")) {
			lvComments--;
			it += 2;
			continue;
		}
		if (lvComments > 0) {
			it++;
			continue;
		}

		if (*it == '\n') {
			it++;
			continue;
		}

		if (quote == '\0' && lvParentheses == 0 && *it == ';') {
			auto processedLine = outputLine(out, indent, line);
			line.clear();
			it++;

			auto t0Length = findToken(processedLine);
			if (t0Length != 0) {
				const char* const tokens[] = {
						"def", "class", "sync", "if", "else", "for", "while", "switch", "case", "default", "do", "catch", "finally"
				};
				bool matched = false;
				for (auto* token : tokens) {
					auto length = std::strlen(token);
					if (t0Length == length && !std::strncmp(processedLine.data(), token, length)) {
						matched = true;
						break;
					}
				}

				if (!matched) {
					auto t1Length = skipColon(processedLine, t0Length);
					if (t1Length != 0) {
						auto offset = t0Length + t1Length;
						auto t2Length = findToken(processedLine, offset);
						if (t2Length != 0) {
							if (t2Length == 3 && !std::strncmp(processedLine.data() + offset, "for", 3))
								matched = true;
							if (t2Length == 5 && !std::strncmp(processedLine.data() + offset, "while", 5))
								matched = true;
						}
					}
				}

				if (matched)
					indent++;
			}

			while (--left > 0 && *it == ';') {
				if (indent != 0)
					indent--;
				it++;
			}
			continue;
		}

		if (quote != '\0' && *it == '\\' && left >= 2) {
			line += *it++;
			line += *it++;
			continue;
		}

		if (quote != '\0' && *it == quote)
			quote = '\0';
		else if (quote == '\0' && (*it == '\'' || *it == '"'))
			quote = *it;
		else if (*it == '(')
			lvParentheses++;
		else if (*it == ')' && lvParentheses > 0)
			lvParentheses--;

		line += *it++;
	}
	outputLine(out, indent, line);

	std::printf("[%s]\n", out.data());
	return out;
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

	size_t argSourceIndex = 0;
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
			else if (arg == "-c")
				optFiles.emplace_back(ScriptSourceType::Arg, argSourceIndex++, consume<std::string>(arg, args));
			else
				optFiles.emplace_back(ScriptSourceType::File, 0, std::move(arg));
		}
	}
	catch (const OptionError&) {
		help(stderr);
		return 1;
	}

	if (optFiles.empty())
		optFiles.emplace_back(ScriptSourceType::Stdin, 0, "");

	auto host = std::make_shared<Host>();
	auto runtime = cha::Runtime::newInstance(host, {}, optThreads);

	int ret = 0;
	std::vector<cha::InstanceId> instanceIds;
	for (auto& source : optFiles) {
		std::string name;
		std::string script;

		switch (std::get<0>(source)) {
		case ScriptSourceType::Stdin:
			name = "stdin";
			script = loadStdin();
			break;

		case ScriptSourceType::File:
			name = std::get<2>(source);
			try {
				script = loadScript(name.data());
			}
			catch (const cha::PackageNotFoundException&) {
				std::fprintf(stderr, "%s: file not found\n", name.data());
				ret = 1;
			}
			break;

		case ScriptSourceType::Arg:
			name = "argument #" + std::to_string(std::get<1>(source));
			try {
				script = parseArg(std::get<1>(source), std::get<2>(source));
			}
			catch (const std::exception&) {
				std::fprintf(stderr, "%s: parse error\n", name.data());
				ret = 1;
			}
			break;
		}
		if (ret != 0)
			break;

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
