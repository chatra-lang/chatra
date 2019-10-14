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

#include "LexicalAnalyzer.h"

namespace chatra {

struct CodePoint {
	unsigned lineNo = 0;
	std::string line;
	size_t first = SIZE_MAX;
	size_t last = SIZE_MAX;

public:
	CodePoint() = default;

	CodePoint(unsigned lineNo, std::string line, size_t first, size_t last = SIZE_MAX)
			: lineNo(lineNo), line(std::move(line)), first(first), last(last) {
	}

	void set(unsigned lineNo, std::string line, size_t first, size_t last = SIZE_MAX) {
		this->lineNo = lineNo;
		this->line = std::move(line);
		this->first = first;
		this->last = last;
	}
};

class LexContext {
private:
	IErrorReceiver& errorReceiver;
	std::shared_ptr<StringTable>& sTable;
	std::string fileName;

public:
	explicit LexContext(IErrorReceiver& errorReceiver,
			std::shared_ptr<StringTable>& sTable, std::string fileName) : errorReceiver(errorReceiver), sTable(sTable) {
		this->fileName = std::move(fileName);
	}

	std::shared_ptr<StringTable>& getSt() {
		return sTable;
	}

	void errorAtRawLine(ErrorLevel level, unsigned lineNo, const std::string& rawLine, size_t first, size_t last,
			const std::string& message, const std::vector<std::string>& args) {

		errorReceiver.error(level, fileName, lineNo, rawLine, first, last, message, args);
	}

	void errorAtRawLine(ErrorLevel level, const CodePoint& point,
			const std::string& message, const std::vector<std::string>& args) {

		errorAtRawLine(level, point.lineNo, point.line, point.first, point.last, message, args);
	}

	void errorAtLine(ErrorLevel level, const std::shared_ptr<Line>& line, const std::vector<size_t>& columnIndex,
			size_t first, size_t last,
			const std::string& message, const std::vector<std::string>& args) {

		errorReceiver.error(level, fileName, line->lineNo, line->line,
				first == SIZE_MAX ? SIZE_MAX : columnIndex[first],
				last == SIZE_MAX ? SIZE_MAX : columnIndex[last - 1] + 1,
				message, args);
	}
};

inline constexpr bool isBeginningOfNumber(char c) {
	return '0' <= c && c <= '9';
}

inline constexpr bool isPartOfNumber(char c) {
	return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F') || c == 'x' || c == '_'
			|| c == '.' || c == '+' || c == '-';  // +/- are used for exponent (e.g. 1e-5 = 0.00001)
}

inline constexpr bool isBeginningOfOperator(char c) {
	return c == '!' || c == '%' || c == '^' || c == '&' || c == '*' || c == '-' || c == '+' || c == '=' || c == '|'
			|| c == '~' || c == ':' || c == ';' || c == ',' || c == '.' || c == '<' || c == '>' || c == '?' || c == '/';
}

inline constexpr bool isPartOfOperator(char c) {
	return c == '!' || c == '%' || c == '^' || c == '&' || c == '*' || c == '-' || c == '+' || c == '=' || c == '|'
			|| c == '~' || c == '.' || c == '<' || c == '>' || c == '?' || c == '/';
}

inline constexpr bool isSpecialCharacters(char c) {
	return isBeginningOfOperator(c) || c == '@' || c == '#' || c == '$' || c == '\\' || c == '`' || c == '\'' || c == '"'
			|| c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}';
}

inline constexpr bool isBeginningOfName(char c) {
	return !isSpace(c) && !isBeginningOfNumber(c) && !isSpecialCharacters(c);
}

inline constexpr bool isPartOfName(char c) {
	return !isSpace(c) && !isSpecialCharacters(c);
}

template <typename BeginningPredicate, typename Predicate>
static size_t matches(const std::string& str, size_t index, BeginningPredicate beginningPred, Predicate pred) {
	auto it = str.cbegin() + index;
	if (it == str.cend() || !beginningPred(*it))
		return 0;
	while (++it != str.cend()) {
		if (!pred(*it))
			break;
	}
	return std::distance(str.cbegin(), it) - index;
}

static Token & addToken(LexContext& ct, std::shared_ptr<Line>& line, const std::vector<size_t>& columnIndex,
		size_t index, size_t length, TokenType type, std::string token) {
	line->tokens.emplace_back(
			line, 0, columnIndex[index], columnIndex[index + length - 1] + 1,
			type, StringId::Invalid);
	Token & ret = line->tokens.back();

	if (type == TokenType::Number || type == TokenType::String)
		ret.literal = std::move(token);
	else
		ret.sid = add(ct.getSt(), std::move(token));
	return ret;
}

static Token & addToken(LexContext& ct, std::shared_ptr<Line>& line, const std::vector<size_t>& columnIndex,
		size_t index, TokenType type, std::string token) {
	return addToken(ct, line, columnIndex, index, 1, type, std::move(token));
}

static size_t findEndOfStringOrEmitError(LexContext& ct, std::shared_ptr<Line>& line, const std::vector<size_t>& columnIndex,
		const std::string& str, size_t tokenIndex, size_t scanIndex) {

	char quot = str[scanIndex];
	for (size_t position = scanIndex + 1; position < str.size(); ) {
		char c = str[position];
		if (c == quot)
			return position + 1 - scanIndex;
		if (c == '\\') {
			if (position + 1 == str.size())
				break;
			position += 1 + byteCount(str, position + 1);
			continue;
		}
		position += byteCount(str, position);
	}

	ct.errorAtLine(ErrorLevel::Error, line, columnIndex, tokenIndex, str.size(), "unterminated string", {});
	line->containsError = true;
	return str.size() - scanIndex;
}

static size_t findEndOfRegexpStringOrEmitError(LexContext& ct, std::shared_ptr<Line>& line, const std::vector<size_t>& columnIndex,
		const std::string& str, size_t tokenIndex, size_t scanIndex) {

	char quot = str[scanIndex];
	for (size_t position = scanIndex + 1; position < str.size(); ) {
		char c = str[position];
		if (c == quot) {
			if (position + 1 == str.size() || str[position + 1] != quot)
				return position + 1 - scanIndex;
			position += 2;
			continue;
		}
		position += byteCount(str, position);
	}

	ct.errorAtLine(ErrorLevel::Error, line, columnIndex, tokenIndex, str.size(), "unterminated regexp-string", {});
	line->containsError = true;
	return str.size() - scanIndex;
}

static size_t findEndOfRawStringOrEmitError(LexContext& ct, std::shared_ptr<Line>& line, const std::vector<size_t>& columnIndex,
		const std::string& str, size_t tokenIndex, size_t scanIndex) {

	size_t position = str.find("\n>>>", scanIndex);
	if (position == std::string::npos) {
		ct.errorAtLine(ErrorLevel::Error, line, columnIndex, tokenIndex, str.size(), "unterminated raw-string", {});
		throw AbortCompilingException();
	}
	return position + 4 - scanIndex;
}

static std::unordered_set<std::string> nameStyleOperators;
static std::vector<std::string> operatorsByLength;
static void initializeOperators() {
	nameStyleOperators = {"async", "is", "and", "or", "xor", "not"};
	operatorsByLength = {
			// 4 letters
			">>>=",
			// 3 letters
			"...", ">>>", "&*=", "**=", "&+=", "&-=", "<<=", ">>=", "/-=", "%-=", "/+=", "%+=",
			// 2 letters
			"++", "--", "&*", "**", "&+", "&-", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "^^",
			"*=", "/=", "%=", "+=", "-=", "&=", "|=", "^=", "/-", "%-", "/+", "%+",
			// 1 letter
			":", ",", ".", "~", "!", "+", "-", "*", "/", "%", "<", ">", "&", "|", "^", "?", ";", "="
	};
}

static std::shared_ptr<Line> parseLine(LexContext& ct, std::string fileName, unsigned lineNo,
		std::string rawLine, const std::string& line, const std::vector<size_t>& columnIndex) {

	std::shared_ptr<Line> ret = std::make_shared<Line>();
	ret->containsError = false;
	ret->fileName = std::move(fileName);
	ret->lineNo = lineNo;
	ret->line = std::move(rawLine);

	size_t index = 0;

	for (ret->indents = 0; startsWith(line, index, "\t"); ret->indents++)
		index += 1;

	while (index < line.size()) {
		char c = line[index];

		if (isSpace(c)) {
			index++;
			continue;
		}

		switch (c) {
		case '(':  addToken(ct, ret, columnIndex, index++, TokenType::OpenBracket, "(");  continue;
		case ')':  addToken(ct, ret, columnIndex, index++, TokenType::CloseBracket, ")");  continue;
		case '[':  addToken(ct, ret, columnIndex, index++, TokenType::OpenBracket, "[");  continue;
		case ']':  addToken(ct, ret, columnIndex, index++, TokenType::CloseBracket, "]");  continue;
		case '{':  addToken(ct, ret, columnIndex, index++, TokenType::OpenBracket, "{");  continue;
		case '}':  addToken(ct, ret, columnIndex, index++, TokenType::CloseBracket, "}");  continue;
		case '@':  addToken(ct, ret, columnIndex, index++, TokenType::Annotation, "@");  continue;
		default:
			break;
		}

		if (c == '\'' || c == '"') {
			size_t mString = findEndOfStringOrEmitError(ct, ret, columnIndex, line, index, index);
			addToken(ct, ret, columnIndex, index, mString, TokenType::String, line.substr(index, mString - 1));
			index += mString;
			continue;
		}

		if (startsWith(line, index, "L'") || startsWith(line, index, "L\"")) {
			// TODO Process string with multiple language support
			size_t mString = findEndOfStringOrEmitError(ct, ret, columnIndex, line, index, index + 1);
			addToken(ct, ret, columnIndex, index, 1 + mString, TokenType::String, line.substr(index, 1 + mString - 1));
			index += 1 + mString;
			// continue;

			ct.errorAtLine(ErrorLevel::Error, ret, columnIndex, index, 1 + mString, "string with multiple language support is not implemented", {});
			throw AbortCompilingException();
		}

		if (startsWith(line, index, "r'") || startsWith(line, index, "r\"")) {
			size_t mString = findEndOfRegexpStringOrEmitError(ct, ret, columnIndex, line, index, index + 1);
			addToken(ct, ret, columnIndex, index, 1 + mString, TokenType::String, line.substr(index, 1 + mString - 1));
			index += 1 + mString;
			continue;
		}

		if (startsWith(line, index, "R<<<\n")) {
			size_t mString = findEndOfRawStringOrEmitError(ct, ret, columnIndex, line, index, index + 4);
			addToken(ct, ret, columnIndex, index, 4 + mString, TokenType::String, line.substr(index, 4 + mString - 4));
			index += 4 + mString;
			continue;
		}

		size_t mNumber = matches(line, index, [](char c) { return isBeginningOfNumber(c); }, [](char c) { return isPartOfNumber(c); });
		if (mNumber != 0) {
			addToken(ct, ret, columnIndex, index, mNumber, TokenType::Number, line.substr(index, mNumber));
			index += mNumber;
			continue;
		}

		size_t mName = matches(line, index, [](char c) { return isBeginningOfName(c); }, [](char c) { return isPartOfName(c); });
		if (mName != 0) {
			auto token = line.substr(index, mName);
			addToken(ct, ret, columnIndex, index, mName,
					nameStyleOperators.count(token) != 0 ? TokenType::Operator : TokenType::Name, token);
			index += mName;
			continue;
		}

		size_t mOperator = matches(line, index, [](char c) { return isBeginningOfOperator(c); }, [](char c) { return isPartOfOperator(c); });
		if (mOperator != 0) {
			size_t i;
			for (i = 0; i < operatorsByLength.size(); i++) {
				auto& op = operatorsByLength[i];
				if (startsWith(line.cbegin() + index, line.cbegin() + index + mOperator, op.data(), op.length()))
					break;
			}
			if (i != operatorsByLength.size()) {
				size_t length = operatorsByLength[i].length();
				addToken(ct, ret, columnIndex, index, length, TokenType::Operator, line.substr(index, length));
				index += length;
				continue;
			}
		}

		ct.errorAtLine(ErrorLevel::Error, ret, columnIndex, index, line.size(), "invalid token or character", {});
		ret->containsError = true;
		break;
	}

	for (size_t i = 0; i < ret->tokens.size(); i++)
		ret->tokens[i].index = static_cast<unsigned>(i);

	return ret;
}

static std::string consumeLine(const std::string& source, std::string::const_iterator& it) {
	auto itLf = std::find(it, source.cend(), '\n');
	auto rawLine = std::string(it, itLf);
	it = (itLf == source.cend() ? itLf : itLf + 1);
	return rawLine;
}

struct ProcessedLine {
	bool validLine = false;
	std::string line;
	std::vector<size_t> columnIndex;
};

struct CommentState {
	unsigned level = 0;
	CodePoint start0;
};

static void append(std::string& rawLine0, ProcessedLine& line0, const std::string& rawLine1, ProcessedLine line1) {
	size_t offset = rawLine0.size();
	rawLine0 = rawLine0 + '\n' + rawLine1;
	line0.line = line0.line + '\n' + line1.line;
	line0.columnIndex.reserve(line0.line.size());
	line0.columnIndex.push_back(offset++);
	for (size_t i = 0; i < line1.line.size(); i++)
		line0.columnIndex.push_back(offset + line1.columnIndex[i]);
}

static ProcessedLine wrapRawLine(const std::string& rawLine) {
	ProcessedLine ret;
	ret.validLine = true;
	ret.line = rawLine;
	ret.columnIndex.reserve(rawLine.size());
	for (size_t i = 0; i < rawLine.size(); i++)
		ret.columnIndex.push_back(i);
	return ret;
}

static ProcessedLine removeComments(LexContext& ct, CommentState& comment,
		unsigned lineNo, const std::string& rawLine, bool keepIndents = true) {

	std::vector<char> line;
	std::vector<size_t> columnIndex;

	line.reserve(rawLine.size());
	columnIndex.reserve(rawLine.size());

	size_t position = 0;

	// Keep indents even if beginning of the line is part of block comment
	size_t extraSpacesPosition = SIZE_MAX;
	if (keepIndents) {
		while (position < rawLine.size()) {
			if (startsWith(rawLine, position, "\t")) {
				line.push_back('\t');
				columnIndex.push_back(position);
				position += 1;
			}
			else if (startsWith(rawLine, position, "    ")) {  // four spaces
				line.push_back('\t');
				columnIndex.push_back(position);
				position += 4;
			}
			else if (startsWith(rawLine, position, " ")) {
				extraSpacesPosition = std::min(extraSpacesPosition, position);
				position += 1;
			}
			else
				break;
		}
	}
	else {
		for (; position < rawLine.size(); position++) {
			if (!isSpace(rawLine[position]))
				break;
		}
	}

	// Remove comments
	bool isBoundary = true;
	bool lineComment = false;

	while (position < rawLine.size()) {
		size_t pitch = byteCount(rawLine, position);
		if (pitch == 0) {
			// This error might be caused by invalid encodings (other than UTF-8) or accidentally user specified binary file.
			// In this case, if error message contains raw text line, the message may break console.
			// To avoid this, rawLine should be set to empty string.
			ct.errorAtRawLine(ErrorLevel::Error, lineNo, "", SIZE_MAX, SIZE_MAX, "invalid character", {});
			throw AbortCompilingException();
		}

		if (startsWith(rawLine, position, "/*")) {
			if (comment.level++ == 0)
				comment.start0.set(lineNo, rawLine, position, position + 2);
			pitch = 2;
		}
		else if (startsWith(rawLine, position, "*/")) {
			if (comment.level == 0) {
				ct.errorAtRawLine(ErrorLevel::Error, lineNo, rawLine, position, position + 2, "comment mismatch", {});
				throw AbortCompilingException();
			}
			if (--comment.level == 0 && !isBoundary) {
				line.push_back(' ');
				columnIndex.push_back(SIZE_MAX);
			}
			pitch = 2;
		}
		else if (startsWith(rawLine, position, "//")) {
			lineComment |= (comment.level == 0);
			pitch = 2;
		}
		else if (comment.level == 0 && !lineComment) {
			if (isSpace(rawLine[position])) {
				if (!isBoundary) {
					// Keep character type (tab or space) and column index
					// because there is possibilities that the character is part of string literal.
					line.push_back(rawLine[position]);
					columnIndex.push_back(position);
				}
			}
			else {
				isBoundary = false;

				// If this line contains valid (non-comment) characters and contains extra spaces,
				// block level cannot be determined or may different from the one which the programmer intended for.
				if (extraSpacesPosition != SIZE_MAX) {
					ct.errorAtRawLine(ErrorLevel::Warning, lineNo, rawLine, extraSpacesPosition, position,
							"extra indents found", {});
					extraSpacesPosition = SIZE_MAX;
				}

				for (size_t i = 0; i < pitch; i++) {
					line.push_back(rawLine[position + i]);
					columnIndex.push_back(position + i);
				}
			}
		}
		position += pitch;
	}

	// Remove extra spaces at the tail of line
	while (!line.empty() && isSpace(line.back())) {
		line.pop_back();
		columnIndex.pop_back();
	}

	ProcessedLine ret;
	ret.validLine = !isBoundary;  // Skip empty lines
	if (ret.validLine) {
		ret.line = std::string(line.data(), line.size());
		ret.columnIndex = std::move(columnIndex);
	}

	return ret;
}

std::vector<std::shared_ptr<Line>> parseLines(IErrorReceiver& errorReceiver,
		std::shared_ptr<StringTable>& sTable, const std::string& fileName, unsigned lineNo, std::string source) {

	LexContext ct(errorReceiver, sTable, fileName);

	std::vector<std::shared_ptr<Line>> lines;
	CommentState comment;

	// Remove magic word and shebang
	constexpr const char* magicWord = "CHATRA_IGNORE_THIS_LINE";
	if (startsWith(source.cbegin(), source.cend(), magicWord) || startsWith(source.cbegin(), source.cend(), "#!")) {
		auto itLf = std::find(source.cbegin(), source.cend(), '\n');
		source.erase(source.cbegin(), itLf == source.cend() ? itLf : itLf + 1);
	}
	for (;;) {
		auto itLf = std::find(source.crbegin(), source.crend(), '\n');
		if (itLf == source.crend())
			break;
		auto lineLength = std::distance(source.crbegin(), itLf);
		auto line = source.substr(source.length() - lineLength);
		if (lineLength == 0 || std::all_of(line.cbegin(), line.cend(), [](char c) { return isSpace(c); })) {
			source = source.substr(0, source.length() - lineLength - 1);
			continue;
		}
		else if (startsWith(line, 0, magicWord)) {
			source = source.substr(0, source.length() - lineLength - 1);
			break;
		}
		break;
	}

	for (auto it = source.cbegin(); it != source.cend(); lineNo++) {
		auto rawLine = consumeLine(source, it);
		auto line = removeComments(ct, comment, lineNo, rawLine);
		if (!line.validLine)
			continue;

		while (comment.level == 0 && endsWith(line.line.cbegin(), line.line.cend(), "R<<<")) {
			CodePoint rawStringStarts(lineNo, rawLine, *(line.columnIndex.cend() - 4), *(line.columnIndex.cend() - 1) + 1);
			for (;;) {
				if (it == source.cend()) {
					ct.errorAtRawLine(ErrorLevel::Error, rawStringStarts, "unterminated raw-string", {});
					throw AbortCompilingException();
				}

				lineNo++;
				auto nextRawLine = consumeLine(source, it);
				auto itFirstChar = std::find_if(nextRawLine.cbegin(), nextRawLine.cend(), [](char c) { return !isSpace(c); });
				if (startsWith(itFirstChar, nextRawLine.cend(), ">>>")) {
					append(rawLine, line, nextRawLine, removeComments(ct, comment, lineNo, nextRawLine, false));
					break;
				}
				append(rawLine, line, nextRawLine, wrapRawLine(nextRawLine));
			}
		}

		// printf("\"%s\" -> \"%s\"\n", rawLine.c_str(), line.line.c_str());
		auto parsedLine = parseLine(ct, fileName, lineNo, rawLine, line.line, line.columnIndex);
		if (!parsedLine->tokens.empty())
			lines.push_back(std::move(parsedLine));
	}

	if (comment.level != 0)
		ct.errorAtRawLine(ErrorLevel::Error, comment.start0, "unterminated /* comment", {});

	return lines;
}

void initializeLexicalAnalyzer() {
	initializeOperators();
}

#ifndef NDEBUG
void dump(const std::shared_ptr<StringTable>& sTable, const std::shared_ptr<Line>& line) {
 	printf("Line{containsError=%s, file=%s, lineNo=%s, indents=%u,\n  line=\"%s\"\n  tokens=\n",
 			line->containsError ? "true" : "false",
 			line->fileName.c_str(),
 			line->lineNo == WholeFile ? "WholeFile"
	            : line->lineNo == UnknownLine ? "UnknownLine"
	            : std::to_string(line->lineNo).c_str(),
 			line->indents, line->line.c_str());
 	for (auto & token : line->tokens)
 		printf("  {index=%u, column=[%u, %u), token=\"%s\"}\n",
 				token.index, static_cast<unsigned>(token.first), static_cast<unsigned>(token.last),
 				sTable->ref(token.sid).c_str());
 	printf("]}\n");
 }
#endif // !NDEBUG

 }  // namespace chatra

 // end of file
