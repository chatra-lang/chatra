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
#include "chatra_debugger.h"
#include <deque>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <csignal>

#include <stdio.h>
#include <unistd.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "chatra_core/CharacterClass.h"
#include "chatra_core/LexicalAnalyzer.h"

#ifdef CHATRA_ENABLE_LANGUAGE_TESTS
int runLanguageTests(int argc, char* argv[]);
#endif

struct OptionError : std::exception {};

enum ScriptSourceType {
	Stdin, File, Arg, Interactive
};

static std::string argv0;
static std::vector<std::tuple<ScriptSourceType, size_t, std::string>> optFiles;
static std::vector<std::string> optPaths;
static unsigned optThreads = std::numeric_limits<unsigned>::max();

class Host;
class DebuggerHost;
static std::shared_ptr<Host> host;
static std::shared_ptr<cha::Runtime> runtime;
static std::shared_ptr<DebuggerHost> debuggerHost;
static std::shared_ptr<cha::d::IDebugger> debugger;

static cha::InstanceId interactiveInstanceId;
static std::atomic<bool> interactiveInstanceReady = {false};

enum class InterruptionState {
	Masked,
	Acceptable,
	Interrupted,
	Accepted,
};

static std::atomic<InterruptionState> interruptionState = {InterruptionState::Masked};

enum class Target {
	Package, Instance, Thread, Frame, Scope, Object, BreakPoint
};
CHATRA_ENUM_HASH(Target)
static constexpr auto invalidTarget = static_cast<Target>(std::numeric_limits<std::underlying_type<Target>::type>::max());

struct BreakPointState {
	cha::d::BreakPointId breakPointId = static_cast<cha::d::BreakPointId>(std::numeric_limits<size_t>::max());
	cha::d::CodePoint codePoint;
};

static cha::SpinLock lockBreak;
static std::vector<BreakPointState> breakPoints;

static std::mutex mtBreak;
static std::condition_variable cvBreak;


static void help(FILE* stream) {
	std::fprintf(stream, "usage: %s [options...] [script files... (or stdin if omitted)]\n"
			"options:\n"
			" -h, --help\n"
			"    print this help message and exit\n"
			" -V, --version\n"
			"    print the chatra version number\n"
			" -t <NUM>, --thread <NUM>\n"
			"    multi-thread mode with number of threads as NUM\n"
			" -I <PATH>\n"
			"    add directory of searching scripts\n"
			"    (This is applied only for \"import\", not for command line)\n"
			" -c <script>\n"
			"    script passed by string; can be specified with multiple times\n"
			" !\n"
			"    enter interactive mode after loading all scripts specified in parameters;\n"
			"    must be specified at the end of parameters",
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
	auto it0 = std::find_if(line.cbegin(), line.cend(), cha::isNotSpace);
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

static bool isBlockStatement(const std::string& statement) {
	auto t0Length = findToken(statement);
	if (t0Length == 0)
		return false;

	const char* const tokens[] = {
			"def", "class", "sync", "if", "else", "for", "while", "switch", "case", "default", "do", "catch", "finally"
	};
	for (auto* token : tokens) {
		auto length = std::strlen(token);
		if (t0Length == length && !std::strncmp(statement.data(), token, length))
			return true;
	}

	auto t1Length = skipColon(statement, t0Length);
	if (t1Length == 0)
		return false;

	auto offset = t0Length + t1Length;
	auto t2Length = findToken(statement, offset);
	if (t2Length != 0) {
		if (t2Length == 3 && !std::strncmp(statement.data() + offset, "for", 3))
			return true;
		if (t2Length == 5 && !std::strncmp(statement.data() + offset, "while", 5))
			return true;
	}

	return false;
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

			if (isBlockStatement(processedLine))
				indent++;

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

	// std::printf("[%s]\n", out.data());
	return out;
}

template <class Type, CHATRA_WHEN(std::is_integral<Type>::value)>
static Type consume(const std::string& opt, std::deque<std::string>& args) {
	if (args.empty()) {
		std::fprintf(stderr, "\"%s\" requires an integer parameter\n", opt.data());
		throw OptionError();
	}
	try {
		auto ret = std::stoi(args.front());
		args.pop_front();
		return static_cast<Type>(ret);
	}
	catch (const std::exception&) {
		std::fprintf(stderr, "parameter for \"%s\" is not a number or overflow\n", opt.data());
		throw OptionError();
	}
}

template <class Type, CHATRA_WHEN(std::is_same<Type, std::string>::value)>
static Type consume(const std::string& opt, std::deque<std::string>& args) {
	if (args.empty()) {
		std::fprintf(stderr, "\"%s\" requires an string parameter\n", opt.data());
		throw OptionError();
	}
	auto ret = args.front();
	args.pop_front();
	return ret;
}

class Host final : public cha::IHost {
private:
	cha::SpinLock lockScriptCache;
	std::unordered_map<std::string, std::string> scriptCache;

public:
	void push(const std::string& packageName, const std::vector<cha::Script>& scripts) {
		std::lock_guard<cha::SpinLock> lock0(lockScriptCache);
		for (auto& script : scripts)
			scriptCache.emplace(packageName + ":" + script.name, script.script);
	}

	std::string refCache(const cha::d::CodePoint& point) {
		if (point.lineNo == 0)
			return "";

		std::lock_guard<cha::SpinLock> lock0(lockScriptCache);
		auto key = point.packageName + ":" + point.fileName;
		auto it0 = scriptCache.find(key);
		if (it0 == scriptCache.cend())
			return "";

		auto& script = it0->second;
		unsigned lineNo = 1;
		for (auto it1 = script.cbegin(); it1 != script.cend(); ) {
			auto itLf = std::find(it1, script.cend(), '\n');
			if (lineNo++ == point.lineNo)
				return std::string(it1, itLf);
			it1 = (itLf == script.cend() ? itLf : itLf + 1);
		}
		return "[EOF]";
	}

	void console(const std::string& message) override {
		std::printf("%s", message.data());
		std::fflush(stdout);
	}

	cha::PackageInfo queryPackage(const std::string& packageName) override {
		for (auto& path : optPaths) {
			try {
				auto prefix = (path.empty() ? "" : path.back() == '/' ? path : path + "/");
				auto r0 = cha::PackageInfo{{{packageName, loadScript((prefix + packageName + ".cha").data())}}, {}, nullptr};
				push(packageName, r0.scripts);
				return r0;
			}
			catch (const cha::PackageNotFoundException&) {
				// do nothing
			}
		}
		auto r1 = cha::queryEmbeddedPackage(packageName);
		push(packageName, r1.scripts);
		return r1;
	}

	cha::IDriver* queryDriver(cha::DriverType driverType) override {
		switch (driverType) {
		case cha::DriverType::FileSystem:  return cha::getStandardFileSystem();
		default:  return nullptr;
		}
	}

	void onInteractiveInstanceReady(cha::InstanceId) override {
		interactiveInstanceReady = true;
		cvBreak.notify_one();
	}
};

static void signalHandler(int signal) {
	if (signal == SIGINT) {
		InterruptionState expected = InterruptionState::Acceptable;
		if (interruptionState.compare_exchange_strong(expected, InterruptionState::Interrupted)) {
			std::fprintf(stderr, "\n[pause requested - press CTRL-C again to interrupt process]\n");
			std::fflush(stderr);
			cvBreak.notify_one();
		}
		else {
			std::fprintf(stderr, "\n");
			std::fflush(stderr);
			std::exit(-1);
		}
	}
}

template <typename Pred>
static void interruptible(Pred pred) {
	interruptionState = InterruptionState::Acceptable;
	try {
		pred();
	}
	catch (...) {
		interruptionState = InterruptionState::Masked;
		throw;
	}
	interruptionState = InterruptionState::Masked;
}

static char* prompt(bool blockContinuation, bool lineContinuation,
		unsigned sectionNo, unsigned lineNo) {

	static std::string ret;

	if (lineContinuation)
		ret = "\1\033[0m\2:" + std::to_string(lineNo) + ">>\1\033[0m\2 ";
	else if (blockContinuation)
		ret = "\1\033[0m\2chatra[" + std::to_string(sectionNo) + "]:" + std::to_string(lineNo) + ">\1\033[0m\2 ";
	else
		ret = "\1\033[0m\033[7m\2chatra[" + std::to_string(sectionNo) + "]:" + std::to_string(lineNo) + ">\1\033[0m\2 ";

	return const_cast<char*>(ret.data());
}

static void dLog(unsigned indent, const char* format, ...) {
	va_list args;
	va_start(args, format);
	std::printf("%s%s\n", std::string(indent * 2, ' ').data(), cha::formatTextV(format, args).data());
	std::fflush(stdout);
	va_end(args);
}

static void dLogC(unsigned indent, const char* format, ...) {
	va_list args;
	va_start(args, format);

	auto v = cha::formatTextV(format, args);

	constexpr const char* replaces[][2] = {
			{"|", "\033[0m\033[2m|\033[0m"},
			{"<", "\033[0m\033[32m"},
			{">", "\033[0m"},
			{"[", "\033[0m\033[2m[\033[0m"},
			{"]", "\033[0m\033[2m]\033[0m"},
			{"^", "\033[0m\033[33m"},
			{"~", "\033[0m\033[2m"},
			{"$", "\033[0m"},
	};

	for (size_t i = v.size(); i-- > 0; ) {
		if (i != 0 && v[i - 1] == '\\') {
			v.replace(--i, 1, "");
			continue;
		}
		for (auto& r : replaces) {
			if (v[i] == r[0][0]) {
				v.replace(i, 1, r[1]);
				break;
			}
		}
	}

	std::printf("%s\033[0m%s\033[0m\n", std::string(indent * 2, ' ').data(), v.data());
	std::fflush(stdout);
	va_end(args);
}

static std::string escape(const std::string& v) {
	auto ret = v;
	for (size_t i = v.size(); i-- > 0; ) {
		auto c = ret[i];
		if (c == '|' || c == '<' || c == '>' || c == '[' || c == ']' || c == '^' || c == '~' || c == '$' || c == '\\')
			ret.insert(i, "\\");
	}
	return ret;
}

static void dError(const char* format, ...) {
	va_list args;
	va_start(args, format);
	std::fprintf(stderr, "%s\n", cha::formatTextV(format, args).data());
	std::fflush(stderr);
	va_end(args);
}

// simplfied version of Parser.cpp::parseStringLiteral()
static std::string parseSimpleStringLiteral(const std::string& str) {
	size_t index = 1;
	std::string ret;
	while (index < str.size()) {
		if (str[index] != '\\') {
			size_t pitch = cha::byteCount(str, index);
			ret.append(str.cbegin() + index, str.cbegin() + index + pitch);
			index += pitch;
			continue;
		}

		switch (str[++index]) {
		case '\'':
		case '"':
		case '\\':
			ret.append(1, str[index++]);
			break;

		default:
			throw cha::IllegalArgumentException("unknown escape sequence");
		}
	}
	return ret;
}

static std::tuple<Target, size_t> parseTargetId(const std::string& str) {
	if (str.size() < 2)
		throw cha::IllegalArgumentException("invalid target ID");

	size_t index;
	try {
		index = static_cast<size_t>(std::stoull(str.substr(1)));
	}
	catch (const std::invalid_argument&) {
		throw cha::IllegalArgumentException("specified ID was not a number");
	}
	catch (const std::out_of_range&) {
		throw cha::IllegalArgumentException("specified ID was out of range");
	}

	switch (str[0]) {
	case 'P':  return std::make_tuple(Target::Package, index);
	case 'I':  return std::make_tuple(Target::Instance, index);
	case 'T':  return std::make_tuple(Target::Thread, index);
	case 'F':  return std::make_tuple(Target::Frame, index);
	case 'S':  return std::make_tuple(Target::Scope, index);
	case 'O':  return std::make_tuple(Target::Object, index);
	case 'B':  return std::make_tuple(Target::BreakPoint, index);
	default:
		throw cha::IllegalArgumentException("unknown ID prefix");
	}
}

template <class Type, Target targetType>
static Type consumeTargetId(const std::string& opt, const char* targetName, std::deque<std::string>& args) {
	auto e = parseTargetId(consume<std::string>(opt, args));
	if (std::get<0>(e) != targetType)
		throw cha::IllegalArgumentException("required %s-ID", targetName);
	return static_cast<Type>(std::get<1>(e));
}

static std::string formatTargetId(const std::tuple<Target, size_t>& e) {
	constexpr const char* prefix[] = {"P", "I", "T", "F", "S", "O", "B"};
	return prefix[static_cast<size_t>(std::get<0>(e))] + std::to_string(std::get<1>(e));
}

template <class InputIterator, typename Convert>
static std::string formatList(InputIterator first, InputIterator last, Convert convert) {
	std::string ret;
	for (; first != last; first++) {
		if (!ret.empty())
			ret += ", ";
		ret += convert(*first);
	}
	return ret;
}

template <class Container, typename Convert>
static std::string formatList(const Container& container, Convert convert) {
	return formatList(container.cbegin(), container.cend(), convert);
}

static void sortScripts(std::vector<cha::Script>& scripts) {
	std::sort(scripts.begin(), scripts.end(), [](const cha::Script& a, const cha::Script& b) {
		return a.name < b.name;
	});
}

static cha::d::CodePoint parseCodePoint(std::deque<std::string>& args) {
	if (args.empty() || args.front() != "@")
		throw cha::IllegalArgumentException();
	args.pop_front();

	bool hasPackageName;
	std::string packageName;

	if (!args.empty() && args[0] == ":")
		hasPackageName = true;
	else if (args.size() >= 2 && args[1] == ":") {
		hasPackageName = true;
		packageName = args[0];
		args.pop_front();
		args.pop_front();
	}

	if (args.size() < 4 || args[1] != "(" || args[3] != ")")
		throw cha::IllegalArgumentException();

	std::string fileName = args[0];
	args.pop_front();
	args.pop_front();
	auto lineNo = consume<unsigned>("line number in code point", args);
	args.pop_front();

	auto packages = debugger->getPackagesState();
	if (packages.empty())
		throw cha::IllegalArgumentException("no packages", packageName.data());

	for (auto& p : packages)
		sortScripts(p.scripts);

	// verify file name
	decltype(packages[0].scripts.cbegin()) it1;
	auto it0 = std::find_if(packages.cbegin(), packages.cend(), [&](const cha::d::PackageState& p) {
		if (hasPackageName && p.packageName != packageName)
			return false;
		it1 = std::find_if(p.scripts.cbegin(), p.scripts.cend(), [&](const cha::Script& s) {
			return s.name == fileName;
		});
		return it1 != p.scripts.cend();
	});

	if (it0 == packages.cend()) {
		it0 = std::find_if(packages.cbegin(), packages.cend(), [&](const cha::d::PackageState& p) {
			if (hasPackageName && p.packageName != packageName)
				return false;
			it1 = std::find_if(p.scripts.cbegin(), p.scripts.cend(), [&](const cha::Script& s) {
				return s.name.find(fileName) != std::string::npos;
			});
			return it1 != p.scripts.cend();
		});
	}

	if (it0 == packages.cend() && hasPackageName) {
		unsigned fileNo;
		try {
			fileNo = static_cast<unsigned>(std::stoi(fileName));
		}
		catch (const std::exception&) {
			fileNo = std::numeric_limits<unsigned>::max();
		}
		if (fileNo != std::numeric_limits<unsigned>::max()) {
			it0 = std::find_if(packages.cbegin(), packages.cend(), [&](const cha::d::PackageState& p) {
				if (p.packageName != packageName)
					return false;
				if (fileNo >= p.scripts.size())
					return false;
				it1 = p.scripts.cbegin() + fileNo;
				return true;
			});
		}
	}

	if (it0 == packages.cend())
		throw cha::IllegalArgumentException("file \"%s:%s\" is not found", packageName.data(), fileName.data());

	cha::d::CodePoint ret;
	ret.packageName = it0->packageName;
	ret.fileName = it1->name;
	ret.lineNo = lineNo;
	return ret;
}

static std::string formatCodePoint(const cha::d::CodePoint& p) {
	if (p.packageName.empty() && p.fileName.empty() && p.lineNo == 0)
		return "@unknown";

	std::string pos = "@";
	if (!p.packageName.empty())
		pos += p.packageName + "/";
	pos += (p.fileName.empty() ? "?" : p.fileName);

	auto line = host->refCache(p);
	auto skip = static_cast<size_t>(std::distance(line.cbegin(), std::find_if(line.cbegin(), line.cend(), cha::isNotSpace)));
	return cha::formatText("%s(%u): %s",
			pos.data(), p.lineNo, line.substr(skip).data());
}

static std::string formatCodePoint(cha::d::ThreadId threadId) {
	auto vt = debugger->getThreadState(threadId);
	if (vt.frameIds.empty())
		return "@unknown";

	auto vf = debugger->getFrameState(threadId, vt.frameIds.front());
	return formatCodePoint(vf.current);
}

static void showStepRunResult(cha::d::ThreadId threadId, cha::d::StepRunResult r) {
	switch (r) {
	case cha::d::StepRunResult::NotTraceable:  dError("[target thread points unknown code point]");  return;
	case cha::d::StepRunResult::NotInRunning:  dError("[target thread is not in running state]");  return;
	case cha::d::StepRunResult::Finished:  dLog(0, "[finished]");  break;
	default:
		break;
	}

	std::string codePoint = formatCodePoint(threadId);
	switch(r) {
	case cha::d::StepRunResult::Blocked:  dLog(0, "[target thread entered in blocked state] %s", codePoint.data());  break;
	case cha::d::StepRunResult::WaitingResources:  dLog(0, "[target thread entered in waiting resource state] %s", codePoint.data());  break;
	case cha::d::StepRunResult::Stopped:  dLog(0, "[paused] %s", codePoint.data());  break;
	case cha::d::StepRunResult::BreakPoint:  dLog(1, "T%zu %s", static_cast<size_t>(threadId), codePoint.data());  break;
	case cha::d::StepRunResult::AbortedByHost:  dLog(0, "[paused] %s", codePoint.data());  break;
	default:
		break;
	}
}

static void showValue(unsigned indent, const std::string& name, const cha::d::Value& v) {
	std::string out = escape(name) + ": ";
	switch(v.valueType) {
	case cha::d::ValueType::Null:  out += "null";  break;
	case cha::d::ValueType::Bool:  out += cha::formatText("Bool(%s)", v.vBool ? "true" : "false");  break;
	case cha::d::ValueType::Int:  out += cha::formatText("Int(%lld)", static_cast<long long>(v.vInt));  break;
	case cha::d::ValueType::Float:  out += cha::formatText("Float(%g)", v.vFloat);  break;
	case cha::d::ValueType::String:
		out += cha::formatText("String(\"%s\")", escape(v.vString).data());
		break;
	case cha::d::ValueType::Object:
		out += cha::formatText("%s(<O%zu>)", v.className.data(), static_cast<size_t>(v.objectId));
		break;
	case cha::d::ValueType::Method:
		out += cha::formatText("~method:$ %s%s", escape(v.methodName).data(), escape(v.methodArgs).data());
		break;
	}
	dLogC(indent, "%s", out.data());
}

static void showValues(unsigned indent, const std::unordered_map<std::string, cha::d::Value>& values) {
	std::vector<std::tuple<std::string, cha::d::Value>> valuesList;
	valuesList.reserve(values.size());
	for (auto& e : values)
		valuesList.emplace_back(std::make_tuple(e.first, e.second));

	std::sort(valuesList.begin(), valuesList.end(), [](decltype(valuesList)::const_reference a, decltype(valuesList)::const_reference b) {
		return std::get<0>(a) < std::get<0>(b);
	});

	for (auto& e : valuesList)
		showValue(indent, std::get<0>(e), std::get<1>(e));
}

class DebuggerHost : public cha::d::IDebuggerHost {
public:
	void onBreakPoint(cha::d::BreakPointId breakPointId) override {
		BreakPointState b;
		{
			std::lock_guard<cha::SpinLock> lock0(lockBreak);
			auto it = std::find_if(breakPoints.cbegin(), breakPoints.cend(), [&](const BreakPointState& bs) {
				return bs.breakPointId == breakPointId;
			});
			if (it != breakPoints.cend())
				b = *it;
		}
		if (b.breakPointId == static_cast<cha::d::BreakPointId>(std::numeric_limits<size_t>::max()))
			dError("[reached to unknown breakpoint B%zu]", static_cast<size_t>(breakPointId));
		else {
			dLog(0, "[reached to breakpoint B%zu]", static_cast<size_t>(breakPointId));
			dLog(1, "B%zu %s", static_cast<size_t>(breakPointId), formatCodePoint(b.codePoint).data());
		}

		cvBreak.notify_one();
	}

	bool onStepRunLoop() override {
		InterruptionState expected = InterruptionState::Interrupted;
		if (interruptionState.compare_exchange_strong(expected, InterruptionState::Accepted))
			return false;
		return true;
	}
};

static void safePause() {
	try {
		debugger->pause();
	}
	catch (const cha::d::IllegalRuntimeStateException&) {
		// do nothing
	}
}

static bool checkInterruption() {
	InterruptionState expected = InterruptionState::Interrupted;
	if (interruptionState.compare_exchange_strong(expected, InterruptionState::Accepted)) {
		safePause();
		dLog(0, "[paused]");
		return true;
	}
	return false;
}

static void runUntilBreak() {
	interactiveInstanceReady = false;
	debugger->resume();

	if (optThreads == std::numeric_limits<unsigned>::max()) {
		while (runtime->handleQueue()) {
			if (debugger->isPaused())
				return;
			if (checkInterruption())
				return;
			if (interactiveInstanceReady)
				break;
		}
		if (interactiveInstanceReady) {
			interactiveInstanceReady = false;
			safePause();
			return;
		}
		dLog(0, "[finished]");
		safePause();
	}
	else {
		std::unique_lock<std::mutex> lock0(mtBreak);
		cvBreak.wait(lock0, [&]() {
			return debugger->isPaused() || checkInterruption() || interactiveInstanceReady;
		});
		interactiveInstanceReady = false;
	}
}

#define FILTER_PACKAGE(v)  if (filters.count(Target::Package) != 0 && \
				filters.at(Target::Package) != static_cast<size_t>(v.packageId))  \
			continue
#define FILTER_PRIMARY_PACKAGE(v)  if (filters.count(Target::Package) != 0 && \
				filters.at(Target::Package) != static_cast<size_t>(v.primaryPackageId))  \
			continue
#define FILTER_INSTANCE(v)  if (filters.count(Target::Instance) != 0 && \
				filters.at(Target::Instance) != static_cast<size_t>(v.instanceId))  \
			continue
#define FILTER_THREAD(v)  if (filters.count(Target::Thread) != 0 && \
				filters.at(Target::Thread) != static_cast<size_t>(v.threadId))  \
			continue
#define FILTER_FRAME(v)  if (filters.count(Target::Thread) != 0 && filters.count(Target::Frame) != 0 && \
				filters.at(Target::Frame) != static_cast<size_t>(v.frameId))  \
			continue
#define FILTER_BREAKPOINT(v)  if (filters.count(Target::BreakPoint) != 0 && \
				filters.at(Target::BreakPoint) != static_cast<size_t>(v.breakPointId))  \
			continue

static constexpr const char* scopeFrameTypeName[] = {
		"Package", "ScriptRoot", "Class", "Method", "Block",
};

static Target findTarget(const std::deque<std::string>& args) {
	auto target = invalidTarget;
	for (auto& arg : args) {
		try {
			auto e = parseTargetId(arg);
			if (target == invalidTarget || static_cast<size_t>(target) < static_cast<size_t>(std::get<0>(e)))
				target = std::get<0>(e);
		}
		catch (const cha::IllegalArgumentException&) {
			// do nothing
		}
	}
	return target;
}

static Target parseTarget(const std::string& arg) {
	if (arg == "package" || arg == "packages")
		return Target::Package;
	else if (arg == "instance" || arg == "instances")
		return Target::Instance;
	else if (arg == "thread" || arg == "threads")
		return Target::Thread;
	else if (arg == "frame" || arg == "frames")
		return Target::Frame;
	else if (arg == "scope" || arg == "scopes")
		return Target::Scope;
	else if (arg == "object" || arg == "objects")
		return Target::Object;
	else if (arg == "breakpoint" || arg == "breakpoints")
		return Target::BreakPoint;
	else
		return invalidTarget;
}

static std::unordered_map<Target, size_t> parseFilters(std::deque<std::string>& args) {
	std::unordered_map<Target, size_t> filters;
	while (!args.empty()) {
		if (args.front() == "," || args.front() == ":" || args.front() == "/") {
			args.pop_front();
			continue;
		}
		auto e = parseTargetId(args.front());
		filters.emplace(std::get<0>(e), std::get<1>(e));
		args.pop_front();
	}

	if (!filters.empty()) {
		dLogC(0, "filtered with %s:", formatList(filters, [](decltype(filters)::const_reference e) {
			return "<" + formatTargetId(e) + ">";
		}).data());
	}

	return filters;
}

static void showPackages(const std::unordered_map<Target, size_t>& filters) {
	for (auto& v : debugger->getPackagesState()) {
		FILTER_PACKAGE(v);
		sortScripts(v.scripts);
		size_t scriptIndex = 0;
		dLogC(0, "[<P%zu>] \"%s\", %ufiles:{%s}",
				static_cast<size_t>(v.packageId), escape(v.packageName).data(), v.scripts.size(),
				formatList(v.scripts, [&](const cha::Script& s) {
					return "<(" + std::to_string(scriptIndex++) + ")>" + escape(s.name);
				}).data());
	}
}

static void showInstances(const std::unordered_map<Target, size_t>& filters) {
	for (auto& v : debugger->getInstancesState()) {
		FILTER_PRIMARY_PACKAGE(v);
		FILTER_INSTANCE(v);
		if (filters.count(Target::Thread) != 0 && v.threadIds.cend() == std::find(v.threadIds.cbegin(), v.threadIds.cend(),
				static_cast<cha::d::ThreadId>(filters.at(Target::Thread))))
			continue;

		dLogC(0, "[<I%zu>] primary=<P%zu>, %uthreads:{%s}",
				static_cast<size_t>(v.instanceId), static_cast<size_t>(v.primaryPackageId),
				v.threadIds.size(),
				formatList(v.threadIds, [](cha::d::ThreadId id) {
					return "<T" + std::to_string(static_cast<size_t>(id)) + ">";
				}).data());
	}
}

static void showThreads(const std::unordered_map<Target, size_t>& filters) {
	for (auto& vi : debugger->getInstancesState()) {
		FILTER_PRIMARY_PACKAGE(vi);
		FILTER_INSTANCE(vi);
		for (auto threadId : vi.threadIds) {
			auto vt = debugger->getThreadState(threadId);
			FILTER_THREAD(vt);
			dLogC(0, "[<T%zu>] <I%zu>, primary=<P%zu>, %uframes:{%s} %s",
					static_cast<size_t>(threadId), static_cast<size_t>(vi.instanceId),
					static_cast<size_t>(vi.primaryPackageId), vt.frameIds.size(),
					formatList(vt.frameIds, [&](cha::d::FrameId id) {
						return "<T" + std::to_string(static_cast<size_t>(threadId)) + ":F" + std::to_string(static_cast<size_t>(id)) + ">" +
								" type=" + scopeFrameTypeName[static_cast<size_t>(debugger->getFrameState(threadId, id).frameType)];
					}).data(),
					escape(formatCodePoint(threadId)).data());
		}
	}
}

static void showFrames(const std::unordered_map<Target, size_t>& filters) {
	for (auto& vi : debugger->getInstancesState()) {
		FILTER_PRIMARY_PACKAGE(vi);
		FILTER_INSTANCE(vi);
		for (auto threadId : vi.threadIds) {
			auto vt = debugger->getThreadState(threadId);
			FILTER_THREAD(vt);
			for (auto frameId : vt.frameIds) {
				auto vf = debugger->getFrameState(threadId, frameId);
				FILTER_FRAME(vf);
				dLogC(0, "[<T%zu:F%zu>] <I%zu>, primary=<P%zu>, type=%s, %uscopes:{%s} %s",
						static_cast<size_t>(threadId), static_cast<size_t>(frameId),
						static_cast<size_t>(vi.instanceId), static_cast<size_t>(vi.primaryPackageId),
						scopeFrameTypeName[static_cast<size_t>(vf.frameType)],
						vf.scopeIds.size(),
						formatList(vf.scopeIds, [&](cha::d::ScopeId id) {
							return "<T" + std::to_string(static_cast<size_t>(threadId)) + ":S" + std::to_string(static_cast<size_t>(id)) + ">" +
									" type=" + scopeFrameTypeName[static_cast<size_t>(debugger->getScopeState(threadId, id).scopeType)];
						}).data(),
						escape(formatCodePoint(vf.current)).data());
			}
		}
	}
}

static void showScopes(const std::unordered_map<Target, size_t>& filters) {
	if (filters.count(Target::Thread) == 0 || filters.count(Target::Scope) == 0)
		throw cha::IllegalArgumentException("thread-ID and scope-ID required to show a scope");

	auto v = debugger->getScopeState(static_cast<cha::d::ThreadId>(filters.at(Target::Thread)),
			static_cast<cha::d::ScopeId>(filters.at(Target::Scope)));
	cha::d::InstanceState vi;
	for (auto& vt : debugger->getInstancesState()) {
		if (vt.threadIds.cend() != std::find(vt.threadIds.cbegin(), vt.threadIds.cend(), v.threadId)) {
			vi = vt;
			break;
		}
	}

	dLogC(0, "[<T%zu:S%zu>] <I%zu>, primary=<P%zu>, type=%s, %uvalues:",
			static_cast<size_t>(v.threadId), static_cast<size_t>(v.scopeId),
			static_cast<size_t>(vi.instanceId), static_cast<size_t>(vi.primaryPackageId),
			scopeFrameTypeName[static_cast<size_t>(v.scopeType)],
			v.values.size());
	showValues(1, v.values);
}

static void showObjects(const std::unordered_map<Target, size_t>& filters) {
	if (filters.count(Target::Object) == 0)
		throw cha::IllegalArgumentException("object-ID required");

	auto objectId = static_cast<cha::d::ObjectId>(filters.at(Target::Object));
	auto v = debugger->getObjectState(objectId);

	dLogC(0, "[<O%zu>] class=%s", static_cast<size_t>(objectId), escape(v.className).data());
	showValues(1, v.values);
}

static void showBreakPoints(const std::unordered_map<Target, size_t>& filters) {
	std::lock_guard<cha::SpinLock> lock0(lockBreak);
	for (auto& b : breakPoints) {
		FILTER_BREAKPOINT(b);
		dLogC(0, "[<B%zu>] %s", static_cast<size_t>(b.breakPointId), escape(formatCodePoint(b.codePoint)).data());
	}
}

static void show(std::deque<std::string> args) {
	auto target = parseTarget(args.front());
	if (target != invalidTarget)
		args.pop_front();
	else
		target = findTarget(args);

	if (target == invalidTarget)
		throw cha::IllegalArgumentException("entity \"%s\" is not found", args.front().data());

	auto filters = parseFilters(args);

	switch (target) {
	case Target::Package:  showPackages(filters);  break;
	case Target::Instance:  showInstances(filters);  break;
	case Target::Thread:  showThreads(filters);  break;
	case Target::Frame:  showFrames(filters);  break;
	case Target::Scope:  showScopes(filters);  break;
	case Target::Object:  showObjects(filters);  break;
	case Target::BreakPoint:  showBreakPoints(filters);  break;
	}
}

static void processDebuggerCommand(const std::string& input) {

	auto sTable = cha::StringTable::newInstance();
	auto nullErrorReceiver = cha::INullErrorReceiver();
	auto errorReceiver = cha::IErrorReceiverBridge(nullErrorReceiver);
	auto lines = cha::parseLines(errorReceiver, sTable, "(debugger-command)", 1, input);

	if (errorReceiver.hasError() || lines.empty() ||
			lines[0]->tokens.size() < 2 || lines[0]->tokens[1].type != cha::TokenType::Name) {
		dError("syntax error (type \"!h\" to show help)");
		return;
	}

	std::deque<std::string> args;
	for (auto& line : lines) {
		for (auto& token : line->tokens) {
			try {
				switch (token.type) {
				case cha::TokenType::Number:
					args.emplace_back(token.literal);
					break;
				case cha::TokenType::String:
					if (token.literal[0] != '\'' && token.literal[0] != '"')
						throw cha::IllegalArgumentException("string quotation pattern used with specified parameter is not supported");
					args.emplace_back(parseSimpleStringLiteral(token.literal));
					break;
				default:
					args.emplace_back(sTable->ref(token.sid));
					break;
				}
			}
			catch (const cha::IllegalArgumentException& ex) {
				dError("syntax error: %s", ex.what() == nullptr ? "unknown reason" : ex.what());
			}
		}
	}
	args.pop_front();
	auto cmd = args.front();
	args.pop_front();

	try {
		if (cmd == "h" || cmd == "help") {
			constexpr const char* commands[][3] = {
					{"^run$", "[<name>:] <script>", "load script from file <script> and create an instance"},
					{"^resume$", "", "switch to running state until any break-point hits or CTRL-C break"},
					{"[^show$|^s$|^list$|^ls$|^l$]", "[package[s]] [<Pxx>]", "show packages information"},
					{"[^show$|^s$|^list$|^ls$|^l$]", "[instance[s]] [<Pxx>] [<Ixx>]", "show instances information"},
					{"[^show$|^s$|^list$|^ls$|^l$]", "[thread[s]] [<Pxx>] [<Ixx>]", "show threads information"},
					{"[^show$|^s$|^list$|^ls$|^l$]", "[frame[s]] [<Pxx>] [<Ixx>] [<Txx>] [<Txx>:<Fxx>]", "show frames in threads information"},
					{"[^show$|^s$|^list$|^ls$|^l$]", "[scope[s]] <Txx>:<Sxx>", "show scope information"},
					{"[^show$|^s$|^list$|^ls$|^l$]", "[object[s]] <Oxx>", "show object information"},
					{"[^show$|^s$|^list$|^ls$|^l$]", "[breakpoint[s]] [<Bxx>]", "show breakpoints"},
					{"^breakpoint$|^break$|^b$", "@ [<package>:] <fileName> (<line#>)", "add breakpoint"},
					{"^del$", "<Bxx>", "delete breakpoint"},
					{"^step$", "in|into|over|out <Txx>", "step run"},
					{"^i$", "<Txx>", "step into"},
					{"^o$", "<Txx>", "step over"},
					{"^r$", "<Txx>", "step out"},
			};

			dLog(0, "debugger command:");
			for (auto& line : commands) {
				dLogC(1, "^!$%s %s", line[0], line[1]);
				dLog(2, "%s", line[2]);
			}
		}
		else if (cmd == "run") {  // [name:] script
			std::string vName;
			if (args.size() >= 2 && args[1] == ":") {
				vName = args.front();
				args.pop_front();
				args.pop_front();
			}
			auto vScript = consume<std::string>(cmd, args);

			try {
				std::vector<cha::Script> scripts = {{vName.empty() ? vScript : vName, loadScript(vScript.data())}};
				host->push("", scripts);
				auto packageId = runtime->loadPackage(scripts);
				auto instanceId = runtime->run(packageId);
				dLogC(0, "\\[<I%zu>\\] <P%zu> %s %s",
						static_cast<size_t>(instanceId), static_cast<size_t>(packageId),
						vName.empty() ? "~file:$" : escape(vName + ":").data(), escape(vScript).data());
			}
			catch (const cha::PackageNotFoundException&) {
				dError("\"%s\" is not found", vScript.data());
			}
		}
		else if (cmd == "resume") {
			interruptible([&]() {
				runUntilBreak();
			});
		}
		else if (cmd == "l" || cmd == "ls" || cmd == "list" || cmd == "s" || cmd == "show") {
			if (args.empty()) {
				dLog(0, "package  instance  thread  frame  scope  object  breakpoint");
				return;
			}
			show(std::move(args));
		}
		else if (cmd == "b" || cmd == "break" || cmd == "breakpoint") {
			auto codePoint = parseCodePoint(args);
			auto breakPointId = debugger->addBreakPoint(codePoint);

			std::lock_guard<cha::SpinLock> lock0(lockBreak);
			breakPoints.emplace_back();
			auto& b = breakPoints.back();
			b.breakPointId = breakPointId;
			b.codePoint = std::move(codePoint);
		}
		else if (cmd == "del") {
			auto breakPointId = consumeTargetId<cha::d::BreakPointId, Target::BreakPoint>("del", "breakpoint", args);

			std::lock_guard<cha::SpinLock> lock0(lockBreak);
			auto it = std::find_if(breakPoints.cbegin(), breakPoints.cend(), [&](const BreakPointState& b) {
				return b.breakPointId == breakPointId;
			});
			if (it == breakPoints.cend())
				throw cha::IllegalArgumentException("breakpoint B%zu is not found", static_cast<size_t>(breakPointId));

			debugger->removeBreakPoint(breakPointId);
			breakPoints.erase(it);
		}
		else if (cmd == "step") {
			auto verb = consume<std::string>("step", args);
			auto threadId = consumeTargetId<cha::d::ThreadId, Target::Thread>("step", "thread", args);

			interruptible([&]() {
				if (verb == "in" || verb == "into")
					showStepRunResult(threadId, debugger->stepInto(threadId));
				else if (verb == "over")
					showStepRunResult(threadId, debugger->stepOver(threadId));
				else if (verb == "out")
					showStepRunResult(threadId, debugger->stepOut(threadId));
				else
					throw cha::IllegalArgumentException("unknown command: \"step %s\"", verb.data());
			});
		}
		else if (cmd == "i") {
			auto threadId = consumeTargetId<cha::d::ThreadId, Target::Thread>("step into", "thread", args);
			interruptible([&]() {
				showStepRunResult(threadId, debugger->stepInto(threadId));
			});
		}
		else if (cmd == "o") {
			auto threadId = consumeTargetId<cha::d::ThreadId, Target::Thread>("step over", "thread", args);
			interruptible([&]() {
				showStepRunResult(threadId, debugger->stepOver(threadId));
			});
		}
		else if (cmd == "r") {
			auto threadId = consumeTargetId<cha::d::ThreadId, Target::Thread>("step out", "thread", args);
			interruptible([&]() {
				showStepRunResult(threadId, debugger->stepOut(threadId));
			});
		}
		else {
			args.push_front(cmd);
			show(std::move(args));
		}
	}
	catch (const cha::IllegalArgumentException& ex) {
		dError("debugger: %s", ex.what() == nullptr ? "illegal argument" : ex.what());
	}
	catch (const cha::NativeException& ex) {
		dError("debugger: %s", ex.what() == nullptr ? "error" : ex.what());
	}
	catch (const OptionError&) {
		// do nothing
	}
}

[[noreturn]] static void interactiveMode() {
	std::signal(SIGINT, signalHandler);

	rl_bind_key('\t', rl_insert);
	stifle_history(1000);

	interactiveInstanceId = runtime->createInteractiveInstance();

	dLog(0, "Chatra interactive frontend");
	dLog(0, "type \"!h\" to show debugger command help");

	unsigned sectionNo = 0;
	unsigned lineNo = 1;
	std::string lines;
	std::string line;
	bool lineContinuation = false;
	bool blockContinuation = false;

	for (;;) {
		auto buffer = readline(prompt(blockContinuation, lineContinuation, sectionNo, lineNo));
		auto length = std::strlen(buffer);
		lineNo++;

		unsigned lineIndent = 0;
		for (int i = 0; i < length; i++) {
			if (buffer[i] == '\t')
				lineIndent++;
			else if (i + 3 < length && buffer[i] == ' ' && buffer[i + 1] == ' ' && buffer[i + 2] == ' ' && buffer[i + 3] == ' ') {
				lineIndent++;
				i += 3;
			}
		}

		auto subLine = std::string(buffer, length);

		if (lineContinuation) {
			for (unsigned i = 0; i < lineIndent + 2; i++)
				subLine.insert(subLine.cbegin(), '\t');
		}

		add_history(subLine.data());

		if (subLine.empty() || subLine.back() != '\n')  // may be always true
			subLine += '\n';

		if (subLine.size() >= 2 && subLine[subLine.size() - 2] == '\\') {
			lineContinuation = true;
			line += subLine.substr(0, subLine.size() - 2) + '\n';
			continue;
		}
		else {
			lineContinuation = false;
			line += subLine;
		}

		// Remove line continuation marker when it comes from history and
		// contains wrapped line
		for (;;) {
			auto pos = line.find("\\\n");
			if (pos == std::string::npos)
				break;
			line.erase(pos, 1);
		}

		lines += line;
		if (blockContinuation) {
			if (line.size() <= 1)
				blockContinuation = false;
			else {
				line.clear();
				continue;
			}
		}
		else {
			auto it = std::find_if(line.cbegin(), line.cend(), cha::isNotSpace);
			if (isBlockStatement(std::string(it, line.cend()))) {
				blockContinuation = true;
				line.clear();
				continue;
			}
		}
		line.clear();
		auto input = lines.substr(std::distance(lines.cbegin(), std::find_if(lines.cbegin(), lines.cend(), cha::isNotSpace)));
		lines.clear();
		lineNo = 1;

		if (input.cend() == std::find_if(input.cbegin(), input.cend(), [](char c) {
				return cha::isNotSpace(c) && c != '\n';  })) {
			continue;
		}

		if (input[0] == '!') {
			processDebuggerCommand(input);
			continue;
		}

		try {
			auto scriptName = "chatra[" + std::to_string(sectionNo++) + "]";
			host->push("", {{scriptName, input}});
			runtime->push(interactiveInstanceId, scriptName, input);
		}
		catch (const cha::NativeException& ex) {
			dError("interactive mode: %s", ex.what() == nullptr ? "error" : ex.what());
			continue;
		}

		interruptible([&]() {
			runUntilBreak();
		});
	}
}

static bool isTTY() {
	return isatty(fileno(stdin));
}

int main(int argc, char* argv[]) {
#if 0
	//const char* args_test[] = {"chatra", "--language-test", "--baseline"};
	//const char* args_test[] = {"chatra", "--language-test", "--serialize", "1000"};
	//const char* args_test[] = {"chatra", "--language-test", "--serialize-reproduce", "emb_format: 226 1956 197 787 479 54 709"};
	const char* args_test[] = {"chatra", "debug_test.cha", "!"};
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
			else if (arg == "-V" || arg == "--version")
				optFiles.emplace_back(ScriptSourceType::Arg, argSourceIndex++, "import sys; log(chatraVersion())");
			else if (arg == "-t" || arg == "--thread")
				optThreads = consume<unsigned>(arg, args);
			else if (arg == "-I")
				optPaths.emplace_back(consume<std::string>(arg, args));
			else if (arg == "-c")
				optFiles.emplace_back(ScriptSourceType::Arg, argSourceIndex++, consume<std::string>(arg, args));
			else if (arg == "-")
				optFiles.emplace_back(ScriptSourceType::Stdin, 0, "");
			else if (arg == "!" && args.empty())
				optFiles.emplace_back(ScriptSourceType::Interactive, 0, "");
			else
				optFiles.emplace_back(ScriptSourceType::File, 0, std::move(arg));
		}
	}
	catch (const OptionError&) {
		help(stderr);
		return 1;
	}

	if (optFiles.empty())
		optFiles.emplace_back(isTTY() ? ScriptSourceType::Interactive
				: ScriptSourceType::Stdin, 0, "");

	bool isInteractive = (std::get<0>(optFiles.back()) == ScriptSourceType::Interactive);
	if (isInteractive && !isTTY()) {
		std::fprintf(stderr, "interactive cannot use with non-tty mode");
		return 1;
	}

	host = std::make_shared<Host>();
	runtime = cha::Runtime::newInstance(host, {}, optThreads);
	debuggerHost = std::make_shared<DebuggerHost>();
	debugger = runtime->getDebugger(debuggerHost);

	if (isInteractive)
		debugger->pause();

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

		case ScriptSourceType::Interactive:
			continue;
		}
		if (ret != 0)
			break;

		std::vector<cha::Script> scripts = {{name, script}};
		host->push("", scripts);
		auto packageId = runtime->loadPackage(scripts);
		auto instanceId = runtime->run(packageId);

		if (isInteractive)
			continue;

		if (optThreads == std::numeric_limits<unsigned>::max())
			runtime->loop();
		else
			instanceIds.emplace_back(instanceId);
	}

	if (isInteractive && ret == 0)
		interactiveMode();

	while (!instanceIds.empty()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (!runtime->isRunning(instanceIds.back()))
			instanceIds.pop_back();
	}

	runtime->shutdown();

	return ret;
}
