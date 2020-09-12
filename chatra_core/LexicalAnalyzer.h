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

#ifndef CHATRA_LEXICAL_ANALYZER_H
#define CHATRA_LEXICAL_ANALYZER_H

#include "Internal.h"
#include "StringTable.h"

namespace chatra {

struct Line;

enum class TokenType {
	Number,
	Name,  // includes all keywords which consists of non-operator characters (e.g. "in"), except operators
	String,  // using literal, contains prefix and beginning quotation mark (", L", r", R<<<); escape sequence is not processed
	Operator,  // includes : ; . , is and or xor not
	Annotation,  // "@"
	OpenBracket,  // ( [ {
	CloseBracket,  // ) ] }
};

struct Token final {
	std::weak_ptr<Line> line;
	unsigned index;
	size_t first;  // byte position
	size_t last;  // byte position (exclusive)
	TokenType type;
	StringId sid;
	std::string literal;  // [Number, String]

public:
	Token(std::weak_ptr<Line> line, unsigned index, size_t first, size_t last,
			TokenType type, StringId sid) noexcept
			: line(std::move(line)), index(index), first(first), last(last), type(type), sid(sid) {}
};

struct Line final {
	bool containsError;  // To avoid generating too many warnings from this line
	std::string fileName;
	unsigned lineNo;
	std::string line;  // sometimes contains LF if the line has raw-string
	unsigned indents;
	std::vector<Token> tokens;
};

constexpr unsigned WholeFile = std::numeric_limits<unsigned>::max() - 1;
constexpr unsigned UnknownLine = std::numeric_limits<unsigned>::max();

std::vector<std::shared_ptr<Line>> parseLines(IErrorReceiver& errorReceiver,
		std::shared_ptr<StringTable>& sTable, const std::string& fileName, unsigned lineNo, std::string source);

void initializeLexicalAnalyzer();

#ifndef NDEBUG
void dump(const std::shared_ptr<StringTable>& sTable, const std::shared_ptr<Line>& line);
#endif // !NDEBUG

}  // namespace chatra

#endif //CHATRA_LEXICAL_ANALYZER_H
