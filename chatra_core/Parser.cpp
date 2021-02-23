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

#include "Parser.h"
#include "CharacterClass.h"

namespace chatra {

void Literal::save(Writer& w) const {
	w.out(type);
	switch (type) {
	case LiteralType::Null:  break;
	case LiteralType::Bool:  w.out(vBool);  break;
	case LiteralType::Int:  w.out(vInt);  break;
	case LiteralType::Float:  w.out(vFloat);  break;
	case LiteralType::String:
	case LiteralType::MultilingualString:
		w.out(vString);
		break;
	}
}

void Literal::restore(Reader & r) {
	r.in(type);
	switch (type) {
	case LiteralType::Null:  break;
	case LiteralType::Bool:  r.in(vBool);  break;
	case LiteralType::Int:  r.in(vInt);  break;
	case LiteralType::Float:  r.in(vFloat);  break;
	case LiteralType::String:
	case LiteralType::MultilingualString:
		r.in(vString);
		break;
	}
}

class ParserContext final {
private:
	IErrorReceiver& errorReceiver;
	std::shared_ptr<StringTable>& sTable;
	unsigned generatedVarCount = 0;

private:
	static std::shared_ptr<Line> getLine(const Token& token) {
		auto line = token.line.lock();
		if (!line)
			throw AbortCompilingException();
		return line;
	}

public:
	explicit ParserContext(IErrorReceiver& errorReceiver,
			std::shared_ptr<StringTable>& sTable) : errorReceiver(errorReceiver), sTable(sTable) {
	}

	std::shared_ptr<StringTable>& getSt() {
		return sTable;
	}

	std::string allocateGeneratedVar() {
		return "#" + std::to_string(generatedVarCount++);
	}

	void errorAtLine(ErrorLevel level, const std::shared_ptr<Line>& line,
			const std::string& message, const std::vector<std::string>& args) {

		errorReceiver.error(level, line->fileName, line->lineNo, line->line,
				SIZE_MAX, SIZE_MAX, message, args);
	}

	void errorAtToken(ErrorLevel level, const Token& token,
			const std::string& message, const std::vector<std::string>& args) {

		auto line = getLine(token);
		errorReceiver.error(level, line->fileName, line->lineNo, line->line,
				token.first, token.last, message, args);
	}

	void errorAtNextToken(ErrorLevel level, const Token& token,
			const std::string& message, const std::vector<std::string>& args) {

		auto line = getLine(token);
		errorReceiver.error(level, line->fileName, line->lineNo, line->line,
				token.last, token.last, message, args);
	}

	void errorAtToken(ErrorLevel level, const Token& firstToken, const Token& lastToken,
			const std::string& message, const std::vector<std::string>& args) {

		auto line = getLine(firstToken);
		errorReceiver.error(level, line->fileName, line->lineNo, line->line,
				firstToken.first, lastToken.last, message, args);
	}

	void errorAtInnerToken(ErrorLevel level, const Token& token, size_t first, size_t last,
			const std::string& message, const std::vector<std::string>& args) {

		auto line = getLine(token);
		errorReceiver.error(level, line->fileName, line->lineNo, line->line,
				token.first + first, token.first + last, message, args);
	}
};

static bool matches(const Token& token, TokenType type) {
	return token.type == type;
}

static bool matches(const Token& token, StringId sid) {
	return token.sid == sid;
}

static bool matches(const Token& token, TokenType type, StringId sid) {
	return token.type == type && token.sid == sid;
}

template <class NodePtrContainer>
static std::shared_ptr<Node>& addNode(NodePtrContainer& container, const std::shared_ptr<Line>& line, NodeType type) {
	container.emplace_back(std::make_shared<Node>());
	auto& node = container.back();

	node->type = type;
	node->line = line;
	return node;
}

template <class Iterator, typename GetLine>
static const Token* nextToken(Iterator first, Iterator last, Iterator& it, size_t& tokenIndex, GetLine getLine) {
	if (it == last)
		return nullptr;
	const Token* ret = &getLine(*it)->tokens[tokenIndex++];
	if (tokenIndex == getLine(*it)->tokens.size()) {
		it++;
		tokenIndex = 0;
		if (it != last && getLine(*it)->indents <= getLine(*first)->indents)
			it = last;
	}
	return ret;
}

template <class LinesPtrIterator>
static const Token* nextTokenFromLines(LinesPtrIterator first, LinesPtrIterator last, LinesPtrIterator& it, size_t& tokenIndex) {
	return nextToken(first, last, it, tokenIndex, [](decltype(*it) line) { return line; });
}

template <class NodePtrIterator>
static const Token* nextTokenFromNodes(NodePtrIterator first, NodePtrIterator last, NodePtrIterator &it, size_t &tokenIndex) {
	return nextToken(first, last, it, tokenIndex, [](decltype(*it) node) { return node->line; });
}

std::shared_ptr<Node> groupScript(IErrorReceiver& errorReceiver,
		std::shared_ptr<StringTable>& sTable, const std::vector<std::shared_ptr<Line>> &lines) {

	ParserContext ct(errorReceiver, sTable);

	std::vector<std::shared_ptr<Node>> stack;
	stack.reserve(10);

	stack.emplace_back(std::make_shared<Node>());
	auto& root = stack.back();
	root->type = NodeType::ScriptRoot;

	for (auto it = lines.cbegin(); it != lines.cend(); it++) {
		auto& line = *it;

		while (static_cast<size_t>(line->indents) + 1 < stack.size())
			stack.pop_back();

		NodeType type = NodeType::Unknown;
		if (matches(line->tokens[0], StringId::Def))
			type = NodeType::Def;
		else if (matches(line->tokens[0], StringId::Class))
			type = NodeType::Class;
		else if (matches(line->tokens[0], StringId::Var)) {
			auto& node = addNode(stack.back()->blockNodes, line, NodeType::Unknown);
			stack.back()->symbols.emplace_back(node);
			continue;
		}

		if (type == NodeType::Unknown) {
			addNode(stack.back()->blockNodes, line, NodeType::Unknown);
			continue;
		}

		auto itForward = it;
		size_t tokenIndex = 0;

		nextTokenFromLines(it, lines.cend(), itForward, tokenIndex);
		auto nameToken = nextTokenFromLines(it, lines.cend(), itForward, tokenIndex);
		if (matches(*nameToken, TokenType::Name, StringId::Static))
			nameToken = nextTokenFromLines(it, lines.cend(), itForward, tokenIndex);

		if (nameToken == nullptr || !(matches(*nameToken, TokenType::Name)
				|| (type == NodeType::Def && matches(*nameToken, StringId::OpenParenthesis)))) {
			ct.errorAtNextToken(ErrorLevel::Error, line->tokens[0],
					type == NodeType::Def ? "expected function name" : "expected class name", {});
			continue;
		}

		auto& node = addNode(stack, line, type);
		node->sid = (nameToken->type == TokenType::Name ? nameToken->sid : StringId::Empty);

		auto& parentNode = *(stack.end() - 2);
		parentNode->blockNodes.emplace_back(node);
		parentNode->symbols.emplace_back(node);
	}

	return stack[0];
}

struct LineAttributes final {
	NodeType type;
	bool hasBlock;
	std::vector<NodeType> parentType;
};

static std::unordered_map<StringId, LineAttributes> lineTypes;
static std::vector<NodeType> expressionParentType;
static std::unordered_set<NodeType> runnableContainerNodeTypes;
static std::unordered_set<NodeType> runnableNodeTypes;

static void initializeLineTypes() {
	const std::vector<NodeType> parentType_normalSentence = {
			NodeType::ScriptRoot, NodeType::Def, NodeType::DefOperator, NodeType::Class,
			NodeType::If, NodeType::ElseIf, NodeType::Else, NodeType::For, NodeType::While,
			NodeType::Case, NodeType::Default, NodeType::Do,
			NodeType::Catch, NodeType::Finally
	};
	const std::vector<NodeType> parentType_normalSentenceAndSwitch = {
			NodeType::ScriptRoot, NodeType::Def, NodeType::DefOperator, NodeType::Class,
			NodeType::If, NodeType::ElseIf, NodeType::Else, NodeType::For, NodeType::While,
			NodeType::Case, NodeType::Default, NodeType::Do,
			NodeType::Catch, NodeType::Finally,
			NodeType::Switch
	};

	lineTypes = {
			{StringId::Import, {NodeType::Import, false, {NodeType::ScriptRoot}}},
			{StringId::Def, {NodeType::Def, true, parentType_normalSentence}},  // includes "def operator"
			{StringId::Class, {NodeType::Class, true, {NodeType::ScriptRoot}}},
			{StringId::Sync, {NodeType::Sync, true, {NodeType::ScriptRoot, NodeType::Class}}},
			{StringId::Touch, {NodeType::Touch, false, parentType_normalSentence}},
			{StringId::Var, {NodeType::Var, false, {NodeType::ScriptRoot, NodeType::Class, NodeType::Sync}}},
			{StringId::If, {NodeType::If, true, parentType_normalSentence}},
			{StringId::Else, {NodeType::Else, true, parentType_normalSentence}},  // includes "else if"
			{StringId::For, {NodeType::For, true, parentType_normalSentence}},
			{StringId::While, {NodeType::While, true, parentType_normalSentence}},
			{StringId::Switch, {NodeType::Switch, true, parentType_normalSentence}},
			{StringId::Case, {NodeType::Case, true, {NodeType::Switch}}},
			{StringId::Default, {NodeType::Default, true, {NodeType::Switch}}},
			{StringId::Do, {NodeType::Do, true, parentType_normalSentence}},
			{StringId::Catch, {NodeType::Catch, true, parentType_normalSentenceAndSwitch}},
			{StringId::Finally, {NodeType::Finally, true, parentType_normalSentenceAndSwitch}},
			{StringId::Break, {NodeType::Break, false, parentType_normalSentence}},
			{StringId::Continue, {NodeType::Continue, false, parentType_normalSentence}},
			{StringId::Return, {NodeType::Return, false, parentType_normalSentence}},
			{StringId::Throw, {NodeType::Throw, false, parentType_normalSentence}},
	};

	expressionParentType = parentType_normalSentence;

	runnableContainerNodeTypes = {
			NodeType::If, NodeType::ElseIf, NodeType::Else, NodeType::For, NodeType::While,
			NodeType::Switch, NodeType::Default, NodeType::Do, NodeType::Catch, NodeType::Finally
	};
	runnableNodeTypes = {
			NodeType::Expression, NodeType::Touch,
			NodeType::If, NodeType::ElseIf, NodeType::Else, NodeType::For, NodeType::While,
			NodeType::Switch, NodeType::Case, NodeType::Do,
			NodeType::Break, NodeType::Continue, NodeType::Return, NodeType::Throw
	};
}

template <class NodePtrIterator>
static NodePtrIterator combineWrappedLines(NodePtrIterator first, NodePtrIterator last, bool hasBlock) {
	auto& n = *first;

	unsigned wrapIndents = n->line->indents + (hasBlock ? 2 : 1);
	std::transform(n->line->tokens.cbegin(), n->line->tokens.cend(), std::back_inserter(n->tokens),
			[](const Token& token) { return &token; });

	auto it = first;
	for (it++; it != last && wrapIndents <= (*it)->line->indents; it++) {
		std::transform((*it)->line->tokens.cbegin(), (*it)->line->tokens.cend(), std::back_inserter(n->tokens),
				[](const Token& token) { return &token; });
	}

	return it;
}

static void recursiveStructureNodes(IErrorReceiver& errorReceiver,
		std::shared_ptr<StringTable>& sTable, Node* node) {
	if (node->blockNodesState.load() < NodeState::Structured)
		structureInnerNode(errorReceiver, sTable, node, false);
	for (auto& n : node->blockNodes)
		recursiveStructureNodes(errorReceiver, sTable, n.get());
}

void structureInnerNode(IErrorReceiver& errorReceiver,
		std::shared_ptr<StringTable>& sTable, Node* node, bool recursive) {

	chatra_assert(node->blockNodes.empty() || node->blockNodesState == NodeState::Grouped);

	ParserContext ct(errorReceiver, sTable);
	unsigned expectedIndents = (node->line ? node->line->indents + 1 : 0);

	std::vector<std::shared_ptr<Node>> outNodes;
	for (auto it = node->blockNodes.cbegin(); it != node->blockNodes.cend(); ) {
		auto& n = *it;

		if (n->line->indents != expectedIndents) {
			ct.errorAtToken(ErrorLevel::Error, n->line->tokens[0],
					"indent mismatch. Expected ${0} indents but ${1} indents found",
					{std::to_string(expectedIndents), std::to_string(n->line->indents)});
			outNodes.clear();
			break;
		}

		if (n->line->tokens[0].type == TokenType::Name) {
			// Look ahead 3 tokens to find "label : loop", "def operator" or "else if" pattern
			auto itForward = it;
			size_t tokenIndex = 0;

			auto token0 = nextTokenFromNodes(it, node->blockNodes.cend(), itForward, tokenIndex);
			auto token1 = nextTokenFromNodes(it, node->blockNodes.cend(), itForward, tokenIndex);
			auto token2 = nextTokenFromNodes(it, node->blockNodes.cend(), itForward, tokenIndex);

			auto keyToken = token0;
			StringId labelName = StringId::Invalid;
			if (token2 != nullptr && !lineTypes.count(token0->sid)
					&& matches(*token1, StringId::Colon) && matches(*token2, TokenType::Name)) {
				keyToken = token2;
				labelName = token0->sid;
			}

			auto itType = lineTypes.find(keyToken->sid);
			if (itType != lineTypes.end()) {
				auto const& attr = itType->second;
				it = combineWrappedLines(it, node->blockNodes.cend(), attr.hasBlock);

				if (token2 != nullptr && labelName != StringId::Invalid) {
					if (n->tokens.size() < 3 || token2 != n->tokens[2]) {
						ct.errorAtToken(ErrorLevel::Error, *token2, "this line seems to be part of wrapping line, but indents may be incorrect", {});
						// Skip few lines which is considered as part of wrapping line
						it = (itForward == node->blockNodes.cend() || tokenIndex == 0 ? itForward : itForward + 1);
						continue;
					}
					if (attr.type != NodeType::For && attr.type != NodeType::While) {
						ct.errorAtToken(ErrorLevel::Error, *token2, R"(only "for" or "while" can take label)", {});
						continue;
					}
					n->sid = labelName;
				}

				// Check whether sentence is correctly placed
				if (std::find(attr.parentType.cbegin(), attr.parentType.cend(), node->type) == attr.parentType.cend()) {
					ct.errorAtToken(ErrorLevel::Error, *keyToken,
							std::string("statement \"") + ct.getSt()->ref(keyToken->sid) + "\" is not allowed here", {});
					continue;
				}

				n->type = attr.type;
				if (n->type == NodeType::Def && token1 != nullptr && matches(*token1, StringId::Operator))
					n->type = NodeType::DefOperator;
				if (n->type == NodeType::Else && token1 != nullptr && matches(*token1, StringId::If))
					n->type = NodeType::ElseIf;

				if (attr.hasBlock) {
					for (; it != node->blockNodes.cend() && expectedIndents < (*it)->line->indents; it++)
						n->blockNodes.push_back(*it);
				}

				outNodes.push_back(n);
				continue;
			}
		}

		it = combineWrappedLines(it, node->blockNodes.cend(), false);

		if (std::find(expressionParentType.cbegin(), expressionParentType.cend(), node->type) == expressionParentType.cend()) {
			ct.errorAtLine(ErrorLevel::Error, n->line, "expression is not allowed here", {});
			continue;
		}

		n->type = NodeType::Expression;
		outNodes.push_back(n);
	}

	// Check empty block
	if (runnableContainerNodeTypes.count(node->type) != 0) {
		bool runnableNodeFound = false;
		for (auto& n : outNodes) {
			if (runnableNodeTypes.count(n->type) != 0) {
				runnableNodeFound = true;
				break;
			}
		}
		if (!runnableNodeFound)
			ct.errorAtLine(ErrorLevel::Warning, node->line, "empty block statement", {});
	}

	// Check order of Catch/Finally and unreachable code
	bool catchFinallyFound = false;
	bool finallyFound = false;
	bool left = false;
	for (auto it = outNodes.cbegin(); it != outNodes.cend(); it++) {
		auto& n = *it;
		if (finallyFound) {
			ct.errorAtLine(ErrorLevel::Error, n->line, "nothing can be placed after \"finally\" block", {});
			outNodes.erase(it, outNodes.cend());
			break;
		}

		if (n->type == NodeType::Catch)
			catchFinallyFound = true;
		else if (n->type == NodeType::Finally) {
			catchFinallyFound = true;
			finallyFound = true;
		}
		else {
			if (left) {
				ct.errorAtLine(ErrorLevel::Warning, n->line, "unreachable code", {});
				it = outNodes.erase(it, std::find_if(it, outNodes.cend(), [](const std::shared_ptr<Node>& n) {
					return n->type == NodeType::Catch || n->type == NodeType::Finally; }));
				if (it == outNodes.cend())
					break;
				it--;
				continue;
			}

			switch (n->type) {
			case NodeType::Break:
			case NodeType::Continue:
			case NodeType::Return:
			case NodeType::Throw:
				left = true;
				break;
			default:
				break;
			}

			if (catchFinallyFound) {
				ct.errorAtLine(ErrorLevel::Error, n->line, R"(expected "catch" or "finally")", {});
				outNodes.erase(it, outNodes.cend());
				break;
			}
		}
	}

	// Check order of Case/Default
	if (node->type == NodeType::Switch) {
		bool defaultFound = false;
		for (auto it = outNodes.cbegin(); it != outNodes.cend(); it++) {
			auto& n = *it;
			if (n->type == NodeType::Default)
				defaultFound = true;
			else if (n->type == NodeType::Case && defaultFound) {
				ct.errorAtLine(ErrorLevel::Error, n->line, R"("case" cannot be placed after "default")", {});
				outNodes.erase(it, outNodes.cend());
				break;
			}
		}
	}

	// Copy outNodes to node->blockNodes with grouping IfGroup
	node->blockNodes.clear();
	for (auto& n : outNodes) {
		switch (n->type) {
		case NodeType::Sync:
			if (!recursive)
				structureInnerNode(errorReceiver, sTable, n.get(), false);
			node->blockNodes.push_back(n);
			break;

		case NodeType::If: {
			auto& nGroup = addNode(node->blockNodes, nullptr, NodeType::IfGroup);
			nGroup->blockNodes.push_back(n);
			nGroup->blockNodesState = NodeState::Structured;
			break;
		}

		case NodeType::ElseIf:
		case NodeType::Else: {
			if (node->blockNodes.empty() || node->blockNodes.back()->type != NodeType::IfGroup) {
				ct.errorAtLine(ErrorLevel::Error, n->line, R"(unexpected "else" statement)", {});
				continue;
			}
			auto& nGroup = node->blockNodes.back();
			if (nGroup->blockNodes.back()->type == NodeType::Else) {
				ct.errorAtLine(ErrorLevel::Error, n->line, R"(extra "else" statement)", {});
				continue;
			}
			nGroup->blockNodes.push_back(n);
			break;
		}

		default:
			node->blockNodes.push_back(n);
			break;
		}
	}

	if (recursive) {
		for (auto& n : node->blockNodes)
			recursiveStructureNodes(errorReceiver, sTable, n.get());
	}

	node->blockNodesState = NodeState::Structured;
}

template <class Type, typename Converter>
static Type parseNumericLiteral(ParserContext& ct, const Token& token, const std::string& str, size_t index,
		const char* literalType, Converter converter) {
	// Remove "_" since std::sto*(str, idx, ...) does not support it.
	std::string targetStr = (index == 0 ? str : str.substr(index));
	for (size_t i = 0; std::string::npos != (i = targetStr.find('_', i)); )
		targetStr.erase(i, 1);

	Type value = 0;
	try {
		size_t idx = 0;
		value = static_cast<Type>(converter(targetStr, &idx));
		if (idx < targetStr.size()) {
			ct.errorAtToken(ErrorLevel::Error, token, std::string("invalid digit ${0} in ") + literalType + " constant", {targetStr.substr(idx, 1)});
			value = 0;
		}
	}
	catch (std::invalid_argument&) {
		ct.errorAtToken(ErrorLevel::Error, token, std::string("invalid ") + literalType + " constant", {});
	}
	catch (std::out_of_range&) {
		ct.errorAtToken(ErrorLevel::Error, token, std::string(literalType) + " constant is out of range", {});
	}
	return value;
}

static uint64_t parseIntLiteral(ParserContext& ct, const Token& token, const std::string& str, size_t index,
		const char* literalType, int radix) {
	return parseNumericLiteral<uint64_t>(ct, token, str, index, literalType, [&](const std::string& str, size_t* idx) {
		return static_cast<uint64_t>(std::stoull(str, idx, radix));
	});
}

static double parseFloatLiteral(ParserContext& ct, const Token& token, const std::string& str, size_t index) {
	return parseNumericLiteral<double>(ct, token, str, index, "floating", [&](const std::string& str, size_t* idx) {
		return std::stod(str, idx);
	});
}

static bool parseBooleanLiteral(ParserContext& ct, const Token& token, StringId sid) {
	if (sid == StringId::True)
		return true;
	if (sid == StringId::False)
		return false;
	ct.errorAtToken(ErrorLevel::Error, token, "invalid boolean constant", {});
	return false;
}

static bool parseUniversalCharacter(ParserContext& ct, const Token& token, const std::string& str, size_t& index, std::string& ret) {
	if (index + 4 > str.size()) {
		ct.errorAtInnerToken(ErrorLevel::Warning, token, index - 2, str.size(), "incomplete universal character name", {});
		return false;
	}
	size_t index0 = index;
	int code = 0;
	for (size_t i = 0; i < 4; i++, index++) {
		int digit;
		char c = str[index];
		if ('0' <= c && c <= '9')
			digit = c - '0';
		else if ('a' <= c && c <= 'f')
			digit = c - 'a' + 10;
		else if ('A' <= c && c <= 'F')
			digit = c - 'A' + 10;
		else {
			ct.errorAtInnerToken(ErrorLevel::Error, token, index, index + byteCount(str, index),
					"invalid digit in universal character", {});
			return false;
		}
		code = code * 16 + digit;
	}

	char buffer[4];
	size_t count = extractChar(static_cast<char32_t>(code), buffer);
	if (count == 0) {
		ct.errorAtInnerToken(ErrorLevel::Error, token, index0 - 2, index, "universal character out of range", {});
		return false;
	}
	ret.append(buffer, buffer + count);
	return true;
}

static std::string parseStringLiteral(ParserContext& ct, const Token& token, const std::string& str, size_t index) {
	char quot = str[index++];
	std::string ret;
	while (index < str.size()) {
		if (str[index] != '\\') {
			size_t pitch = byteCount(str, index);
			ret.append(str.cbegin() + index, str.cbegin() + index + pitch);
			index += pitch;
			continue;
		}

		// Checking index can be omitted since existence of next character was checked at lexical analysis
		switch (str[++index]) {
		case '\'':
		case '"':
			if (str[index] != quot)
				ct.errorAtInnerToken(ErrorLevel::Warning, token, index - 1, index + 1, "redundant character escape", {});
			CHATRA_FALLTHROUGH;

		case '\\':
			ret.append(1, str[index++]);
			break;

		case '0':  ret.append(1, '\0');  index++;  break;
		case 'a':  ret.append(1, '\a');  index++;  break;
		case 'b':  ret.append(1, '\b');  index++;  break;
		case 'f':  ret.append(1, '\f');  index++;  break;
		case 'n':  ret.append(1, '\n');  index++;  break;
		case 'r':  ret.append(1, '\r');  index++;  break;
		case 't':  ret.append(1, '\t');  index++;  break;
		case 'v':  ret.append(1, '\v');  index++;  break;

		case 'u': {
			if (!parseUniversalCharacter(ct, token, str, ++index, ret))
				return "";
			break;
		}

		default:
			ct.errorAtInnerToken(ErrorLevel::Warning, token, index, index + byteCount(str, index),
					"unknown escape sequence", {});
			return "";
		}
	}
	return ret;
}

static std::string parseRegexpStringLiteral(const std::string& str, size_t index) {
	char quot = str[index++];
	std::string ret;
	while (index < str.size()) {
		if (str[index] != quot) {
			size_t pitch = byteCount(str, index);
			ret.append(str.cbegin() + index, str.cbegin() + index + pitch);
			index += pitch;
		}
		else {
			ret.append(1, quot);
			index += 2;
		}
	}
	return ret;
}

static std::string parseRawStringLiteral(ParserContext& ct, const Token& token, const std::string& str, size_t index) {
	std::string prefix;
	while (index < str.size() && isSpace(str[index]))
		prefix.append(1, str[index++]);

	std::string ret;
	while (index < str.size()) {
		if (str[index] != '\n') {
			size_t pitch = byteCount(str, index);
			ret.append(str.cbegin() + index, str.cbegin() + index + pitch);
			index += pitch;
			continue;
		}
		ret.append(1, '\n');
		index++;
		if (index + prefix.size() > str.size() || !std::equal(prefix.cbegin(), prefix.cend(), str.cbegin() + index)) {
			ct.errorAtInnerToken(ErrorLevel::Error, token, index, index, "indent mismatch", {});
			return "";
		}
		index += prefix.size();
	}
	return ret;
}

static bool isLiteral(const Token& token) {
	return token.type == TokenType::Number || token.type == TokenType::String
			|| token.sid == StringId::Null || token.sid == StringId::True || token.sid == StringId::False;
}

static std::shared_ptr<Node> parseAsLiteral(ParserContext& ct, const Token& token) {
	auto& t = token.literal;

	std::unique_ptr<Literal> value(new Literal());

	if (token.type == TokenType::Number) {
		if (startsWith(t, 0, "0x")) {
			value->type = LiteralType::Int;
			value->vInt = parseIntLiteral(ct, token, t, 2, "hexadecimal", 16);
		}
		else if (startsWith(t, 0, "0b")) {
			value->type = LiteralType::Int;
			value->vInt = parseIntLiteral(ct, token, t, 2, "binary", 2);
		}
		else if (t.find('.') != std::string::npos) {
			value->type = LiteralType::Float;
			value->vFloat = parseFloatLiteral(ct, token, t, 0);
		}
		else {
			value->type = LiteralType::Int;
			value->vInt = parseIntLiteral(ct, token, t, 0, "decimal", 10);
		}
	}
	else if (token.type == TokenType::Name) {
		if (token.sid == StringId::Null)
			value->type = LiteralType::Null;
		else {
			value->type = LiteralType::Bool;
			value->vBool = parseBooleanLiteral(ct, token, token.sid);
		}
	}
	else if (token.type == TokenType::String) {
		value->type = LiteralType::String;
		if (startsWith(t, 0, "'") || startsWith(t, 0, "\""))
			 value->vString = parseStringLiteral(ct, token, t, 0);
		else if (startsWith(t, 0, "L'") || startsWith(t, 0, "L\"")) {
			value->type = LiteralType ::MultilingualString;
			value->vString = parseStringLiteral(ct, token, t, 1);
		}
		else if (startsWith(t, 0, "r'") || startsWith(t, 0, "r\""))
			value->vString = parseRegexpStringLiteral(t, 1);
		else if (startsWith(t, 0, "R<<<\n"))
			value->vString = parseRawStringLiteral(ct, token, t, 5);
	}
	else {
		ct.errorAtToken(ErrorLevel::Error, token, "internal error", {});
		throw AbortCompilingException();
	}

	std::shared_ptr<Node> n = std::make_shared<Node>();
	n->type = NodeType::Literal;
	n->tokens.push_back(&token);
	n->literalValue = std::move(value);
	return n;
}

static bool checkBracketPair(ParserContext& ct, const Token& token, std::vector<std::pair<StringId, const Token *>>& stack, StringId expected) {
	if (stack.empty()) {
		ct.errorAtToken(ErrorLevel::Error, token, std::string("extra '") + ct.getSt()->ref(token.sid) + "'", {});
		return false;
	}
	if (stack.back().first != expected) {
		ct.errorAtToken(ErrorLevel::Error, token, std::string("expected '") + ct.getSt()->ref(expected) + "'", {});
		ct.errorAtToken(ErrorLevel::Error, *stack.back().second, "started from here", {});
		return false;
	}
	stack.pop_back();
	return true;
}

template <class TokenPtrIterator, typename TopLevelDelimiter>
static TokenPtrIterator findExpressionBoundary(ParserContext& ct, TokenPtrIterator first, TokenPtrIterator last,
		TopLevelDelimiter topLevelDelimiter) {

	if (first == last)
		return first;

	std::vector<std::pair<StringId, const Token *>> stack;
	TokenPtrIterator it;
	for (it = first; it != last; it++) {
		if (stack.empty() && topLevelDelimiter(**it))
			break;

		if (matches(**it, TokenType::OpenBracket))
			stack.emplace_back((*it)->sid, *it);
		else if (matches(**it, TokenType::CloseBracket)) {
			switch ((*it)->sid) {
			case StringId::CloseParenthesis:
				if (!checkBracketPair(ct, **it, stack, StringId::OpenParenthesis))
					return first;
				break;
			case StringId::CloseBracket:
				if (!checkBracketPair(ct, **it, stack, StringId::OpenBracket))
					return first;
				break;
			case StringId::CloseBrace:
				if (!checkBracketPair(ct, **it, stack, StringId::OpenBrace))
					return first;
				break;
			default:
				ct.errorAtToken(ErrorLevel::Error, **it, "internal error", {});
				throw AbortCompilingException();
			}
		}
	}

	if (!stack.empty()) {
		ct.errorAtNextToken(ErrorLevel::Error, **(it == first ? first : it - 1),
				std::string("unmatched '") + ct.getSt()->ref(stack.back().first) + "'", {});
		return first;
	}

	return it;
}

enum class OpPt {
	Token,
	NameChain,
	Parenthesis,
	ParenthesisWithValue,
	Bracket,
	Brace,
	Expression
};

struct OperatorPatternElement final {
	OpPt type;
	StringId token;
	size_t subNodeIndex;

	OperatorPatternElement(OpPt type, size_t subNodeIndex)
			: type(type), token(StringId::Invalid), subNodeIndex(subNodeIndex) {}
	OperatorPatternElement(StringId token)
			: type(OpPt::Token), token(token), subNodeIndex(SIZE_MAX) {}
};

struct OperatorPattern final {
	Operator op;
	size_t subNodes;
	std::vector<OperatorPatternElement> elements;

	OperatorPattern() noexcept : op(defaultOp), subNodes(0) {}
	OperatorPattern(Operator op, size_t subNodes, std::vector<OperatorPatternElement> elements) noexcept
			: op(op), subNodes(subNodes), elements(std::move(elements)) {}
};

struct OperatorPatternGroup final {
	bool leftToRight;
	std::vector<OperatorPattern> patterns;

	OperatorPatternGroup(bool leftToRight, std::vector<OperatorPattern> patterns) noexcept
			: leftToRight(leftToRight), patterns(std::move(patterns)) {}
};

enum class OperatorType {
	Prefix, Postfix, Binary, Ternary, Special
};

struct OperatorAttributes final {
	OperatorType type;
	bool allowOverride;
	std::string description;

	OperatorAttributes() noexcept : type(enum_max<OperatorType>::value), allowOverride(false) {}
	OperatorAttributes(OperatorType type, bool allowOverride, std::string description = "") noexcept
			: type(type), allowOverride(allowOverride), description(std::move(description)) {}
};

static std::vector<OperatorPatternGroup> opPatterns;
static std::vector<OperatorAttributes> opAttrs;  // [Operator]
static OperatorPattern opPairPattern;

static void initializeOpTables() {
	opPatterns = {
			{true, {
					{Operator::Container, SubNode::Container_SubNodes, {
							{OpPt::NameChain, SubNode::Container_Class},
							{OpPt::Parenthesis, SubNode::Container_Args},
							{OpPt::Brace, SubNode::Container_Values}
					}},
					{Operator::Container, SubNode::Container_SubNodes, {
							{OpPt::NameChain, SubNode::Container_Class},
							{OpPt::Brace, SubNode::Container_Values}
					}},
					{Operator::Container, SubNode::Container_SubNodes, {
							{OpPt::Brace, SubNode::Container_Values}
					}},
					{Operator::Subscript, 2, {{OpPt::Expression, 0}, {OpPt::Bracket, 1}}},
					{Operator::Call, 2, {{OpPt::Expression, 0}, {OpPt::Parenthesis, 1}}},
					{Operator::Parenthesis, 1, {{OpPt::ParenthesisWithValue, 0}}},
					{Operator::ElementSelection, 2, {{OpPt::Expression, 0}, {StringId::OpElementSelection}, {OpPt::Expression, 1}}},
					{Operator::ElementSelection, 2, {{OpPt::Expression, 0}, {StringId::OpElementSelection}, {OpPt::ParenthesisWithValue, 1}}},
					{Operator::PostfixIncrement, 1, {{OpPt::Expression, 0}, {StringId::OpIncrement}}},
					{Operator::PostfixDecrement, 1, {{OpPt::Expression, 0}, {StringId::OpDecrement}}},
					{Operator::VarArg, 1, {{OpPt::Expression, 0}, {StringId::OpVarArg}}}
			}},
			{false, {
					{Operator::PrefixIncrement, 1, {{StringId::OpIncrement}, {OpPt::Expression, 0}}},
					{Operator::PrefixDecrement, 1, {{StringId::OpDecrement}, {OpPt::Expression, 0}}},
					{Operator::BitwiseComplement, 1, {{StringId::OpBitwiseComplement}, {OpPt::Expression, 0}}},
					{Operator::LogicalNot, 1, {{StringId::OpLogicalNot}, {OpPt::Expression, 0}}},
					{Operator::LogicalNot, 1, {{StringId::OpLogicalNotText}, {OpPt::Expression, 0}}},
					{Operator::UnaryPlus, 1, {{StringId::OpPlus}, {OpPt::Expression, 0}}},
					{Operator::UnaryMinus, 1, {{StringId::OpMinus}, {OpPt::Expression, 0}}},
					{Operator::TupleExtraction, 1, {{StringId::OpMultiplication}, {OpPt::Expression, 0}}},
					{Operator::FunctionObject, 1, {{StringId::OpBitwiseAnd}, {OpPt::Expression, 0}}},
					{Operator::Async, 1, {{StringId::OpAsync}, {OpPt::Expression, 0}}}
			}},
			{true, {
					{Operator::Multiplication, 2, {{OpPt::Expression, 0}, {StringId::OpMultiplication}, {OpPt::Expression, 1}}},
					{Operator::MultiplicationOv, 2, {{OpPt::Expression, 0}, {StringId::OpMultiplicationOv}, {OpPt::Expression, 1}}},
					{Operator::Division, 2, {{OpPt::Expression, 0}, {StringId::OpDivision}, {OpPt::Expression, 1}}},
					{Operator::DivisionFloor, 2, {{OpPt::Expression, 0}, {StringId::OpDivisionFloor}, {OpPt::Expression, 1}}},
					{Operator::DivisionCeiling, 2, {{OpPt::Expression, 0}, {StringId::OpDivisionCeiling}, {OpPt::Expression, 1}}},
					{Operator::Modulus, 2, {{OpPt::Expression, 0}, {StringId::OpModulus}, {OpPt::Expression, 1}}},
					{Operator::ModulusFloor, 2, {{OpPt::Expression, 0}, {StringId::OpModulusFloor}, {OpPt::Expression, 1}}},
					{Operator::ModulusCeiling, 2, {{OpPt::Expression, 0}, {StringId::OpModulusCeiling}, {OpPt::Expression, 1}}},
					{Operator::Exponent, 2, {{OpPt::Expression, 0}, {StringId::OpExponent}, {OpPt::Expression, 1}}}
			}},
			{true, {
					{Operator::Addition, 2, {{OpPt::Expression, 0}, {StringId::OpPlus}, {OpPt::Expression, 1}}},
					{Operator::AdditionOv, 2, {{OpPt::Expression, 0}, {StringId::OpAdditionOv}, {OpPt::Expression, 1}}},
					{Operator::Subtraction, 2, {{OpPt::Expression, 0}, {StringId::OpMinus}, {OpPt::Expression, 1}}},
					{Operator::SubtractionOv, 2, {{OpPt::Expression, 0}, {StringId::OpSubtractionOv}, {OpPt::Expression, 1}}}
			}},
			{true, {
					{Operator::LeftShift, 2, {{OpPt::Expression, 0}, {StringId::OpLeftShift}, {OpPt::Expression, 1}}},
					{Operator::RightShift, 2, {{OpPt::Expression, 0}, {StringId::OpRightShift}, {OpPt::Expression, 1}}},
					{Operator::UnsignedRightShift, 2, {{OpPt::Expression, 0}, {StringId::OpUnsignedRightShift}, {OpPt::Expression, 1}}}
			}},
			{true, {{Operator::BitwiseAnd, 2, {{OpPt::Expression, 0}, {StringId::OpBitwiseAnd}, {OpPt::Expression, 1}}}}},
			{true, {{Operator::BitwiseOr, 2, {{OpPt::Expression, 0}, {StringId::OpBitwiseOr}, {OpPt::Expression, 1}}}}},
			{true, {{Operator::BitwiseXor, 2, {{OpPt::Expression, 0}, {StringId::OpBitwiseXor}, {OpPt::Expression, 1}}}}},
			{true, {
					{Operator::LessThan, 2, {{OpPt::Expression, 0}, {StringId::OpLessThan}, {OpPt::Expression, 1}}},
					{Operator::GreaterThan, 2, {{OpPt::Expression, 0}, {StringId::OpGreaterThan}, {OpPt::Expression, 1}}},
					{Operator::LessThanOrEqualTo, 2, {{OpPt::Expression, 0}, {StringId::OpLessThanOrEqualTo}, {OpPt::Expression, 1}}},
					{Operator::GreaterThanOrEqualTo, 2, {{OpPt::Expression, 0}, {StringId::OpGreaterThanOrEqualTo}, {OpPt::Expression, 1}}},
					{Operator::InstanceOf, 2, {{OpPt::Expression, 0}, {StringId::OpInstanceOf}, {OpPt::Expression, 1}}}
			}},
			{true, {
					{Operator::EqualTo, 2, {{OpPt::Expression, 0}, {StringId::OpEqualTo}, {OpPt::Expression, 1}}},
					{Operator::NotEqualTo, 2, {{OpPt::Expression, 0}, {StringId::OpNotEqualTo}, {OpPt::Expression, 1}}}
			}},
			{true, {
					{Operator::LogicalAnd, 2, {{OpPt::Expression, 0}, {StringId::OpLogicalAnd}, {OpPt::Expression, 1}}},
					{Operator::LogicalAnd, 2, {{OpPt::Expression, 0}, {StringId::OpLogicalAndText}, {OpPt::Expression, 1}}}
			}},
			{true, {
					{Operator::LogicalOr, 2, {{OpPt::Expression, 0}, {StringId::OpLogicalOr}, {OpPt::Expression, 1}}},
					{Operator::LogicalOr, 2, {{OpPt::Expression, 0}, {StringId::OpLogicalOrText}, {OpPt::Expression, 1}}}
			}},
			{true, {
					{Operator::LogicalXor, 2, {{OpPt::Expression, 0}, {StringId::OpLogicalXor}, {OpPt::Expression, 1}}},
					{Operator::LogicalXor, 2, {{OpPt::Expression, 0}, {StringId::OpLogicalXorText}, {OpPt::Expression, 1}}}
			}},
			{false, {
					{Operator::Conditional, 3, {{OpPt::Expression, 0}, {StringId::OpConditional}, {OpPt::Expression, 1},
							{StringId::Colon}, {OpPt::Expression, 2}}}
			}},
			{false, {{Operator::Pair, 2, {{OpPt::Expression, 0}, {StringId::Colon}, {OpPt::Expression, 1}}}}},
			{true, {
					{Operator::Tuple, 2, {{OpPt::Expression, 0}, {StringId::Comma}, {OpPt::Expression, 1}}},
					{Operator::TupleDelimiter, 2, {{OpPt::Expression, 0}, {StringId::OpTupleDelimiter}, {OpPt::Expression, 1}}}
			}},
			{false, {  // To support foo(; a, b)
					{Operator::TupleDelimiter, 2, {{StringId::OpTupleDelimiter}, {OpPt::Expression, 1}}}
			}},
			{true, {  // To support foo(a, b;)
					{Operator::TupleDelimiter, 2, {{OpPt::Expression, 0}, {StringId::OpTupleDelimiter}}}
			}},
			{false, {
					{Operator::Assignment, 2, {{OpPt::Expression, 0}, {StringId::OpAssignment}, {OpPt::Expression, 1}}},
					{Operator::MultiplicationAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpMultiplicationAssignment}, {OpPt::Expression, 1}}},
					{Operator::MultiplicationOvAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpMultiplicationOvAssignment}, {OpPt::Expression, 1}}},
					{Operator::DivisionAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpDivisionAssignment}, {OpPt::Expression, 1}}},
					{Operator::DivisionFloorAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpDivisionFloorAssignment}, {OpPt::Expression, 1}}},
					{Operator::DivisionCeilingAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpDivisionCeilingAssignment}, {OpPt::Expression, 1}}},
					{Operator::ModulusAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpModulusAssignment}, {OpPt::Expression, 1}}},
					{Operator::ModulusFloorAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpModulusFloorAssignment}, {OpPt::Expression, 1}}},
					{Operator::ModulusCeilingAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpModulusCeilingAssignment}, {OpPt::Expression, 1}}},
					{Operator::ExponentAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpExponentAssignment}, {OpPt::Expression, 1}}},
					{Operator::AdditionAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpAdditionAssignment}, {OpPt::Expression, 1}}},
					{Operator::AdditionOvAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpAdditionOvAssignment}, {OpPt::Expression, 1}}},
					{Operator::SubtractionAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpSubtractionAssignment}, {OpPt::Expression, 1}}},
					{Operator::SubtractionOvAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpSubtractionOvAssignment}, {OpPt::Expression, 1}}},
					{Operator::LeftShiftAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpLeftShiftAssignment}, {OpPt::Expression, 1}}},
					{Operator::RightShiftAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpRightShiftAssignment}, {OpPt::Expression, 1}}},
					{Operator::UnsignedRightShiftAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpRightShiftUnsignedAssignment}, {OpPt::Expression, 1}}},
					{Operator::BitwiseAndAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpBitwiseAndAssignment}, {OpPt::Expression, 1}}},
					{Operator::BitwiseOrAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpBitwiseOrAssignment}, {OpPt::Expression, 1}}},
					{Operator::BitwiseXorAssignment, 2, {{OpPt::Expression, 0}, {StringId::OpBitwiseXorAssignment}, {OpPt::Expression, 1}}}
			}}
	};

	const std::vector<std::pair<Operator, OperatorAttributes>> sourceForOpAttrs = {
			{Operator::Container, {OperatorType::Special, false, "container initializer"}},
			{Operator::Subscript, {OperatorType::Special, false, "subscripting"}},
			{Operator::Call, {OperatorType::Special, false, "function call"}},
			{Operator::Parenthesis, {OperatorType::Special, false, "parenthesis"}},
			{Operator::ElementSelection, {OperatorType::Binary, false, "element selection (.)"}},
			{Operator::PostfixIncrement, {OperatorType::Postfix, true, "postfix increment (++)"}},
			{Operator::PostfixDecrement, {OperatorType::Postfix, true, "postfix decrement (--)"}},
			{Operator::VarArg, {OperatorType::Postfix, false, "variadic arguments (...)"}},

			{Operator::PrefixIncrement, {OperatorType::Prefix, true, "prefix increment (++)"}},
			{Operator::PrefixDecrement, {OperatorType::Prefix, true, "prefix decrement (--)"}},
			{Operator::BitwiseComplement, {OperatorType::Prefix, true, "bitwise complement (~)"}},
			{Operator::LogicalNot, {OperatorType::Prefix, true, "logical NOT (!, not)"}},
			{Operator::UnaryPlus, {OperatorType::Prefix, true, "unary plus (+)"}},
			{Operator::UnaryMinus, {OperatorType::Prefix, true, "unary minus (-)"}},
			{Operator::TupleExtraction, {OperatorType::Prefix, false, "tuple extraction (*)"}},
			{Operator::FunctionObject, {OperatorType::Prefix, false, "function object (&)"}},
			{Operator::Async, {OperatorType::Prefix, false, "asynchronous evaluation (async)"}},

			{Operator::Multiplication, {OperatorType::Binary, true, "multiplication (*)"}},
			{Operator::MultiplicationOv, {OperatorType::Binary, true, "multiplication with ignoring overflow (&*)"}},
			{Operator::Division, {OperatorType::Binary, true, "division (/)"}},
			{Operator::DivisionFloor, {OperatorType::Binary, true, "division with rounding down (/-)"}},
			{Operator::DivisionCeiling, {OperatorType::Binary, true, "division with rounding up (/+)"}},
			{Operator::Modulus, {OperatorType::Binary, true, "modulus (%)"}},
			{Operator::ModulusFloor, {OperatorType::Binary, true, "modulus with rounding down (%-)"}},
			{Operator::ModulusCeiling, {OperatorType::Binary, true, "modulus with rounding up (%+)"}},
			{Operator::Exponent, {OperatorType::Binary, true, "exponent (**)"}},

			{Operator::Addition, {OperatorType::Binary, true, "addition (+)"}},
			{Operator::AdditionOv, {OperatorType::Binary, true, "addition with ignoring overflow (&+)"}},
			{Operator::Subtraction, {OperatorType::Binary, true, "subtraction (-)"}},
			{Operator::SubtractionOv, {OperatorType::Binary, true, "subtraction with ignoring overflow (&-)"}},

			{Operator::LeftShift, {OperatorType::Binary, true, "left shift (<<)"}},
			{Operator::RightShift, {OperatorType::Binary, true, "right shift (>>)"}},
			{Operator::UnsignedRightShift, {OperatorType::Binary, true, "right shift with zero extension (>>>)"}},

			{Operator::BitwiseAnd, {OperatorType::Binary, true, "bitwise AND (&)"}},
			{Operator::BitwiseOr, {OperatorType::Binary, true, "bitwise OR (|)"}},
			{Operator::BitwiseXor, {OperatorType::Binary, true, "bitwise XOR (^)"}},

			{Operator::LessThan, {OperatorType::Binary, true, "less than (<)"}},
			{Operator::GreaterThan, {OperatorType::Binary, true, "greater than (>)"}},
			{Operator::LessThanOrEqualTo, {OperatorType::Binary, true, "less than or equal to (<=)"}},
			{Operator::GreaterThanOrEqualTo, {OperatorType::Binary, true, "greater than or equal to (>=)"}},
			{Operator::InstanceOf, {OperatorType::Binary, false, "instance of (is)"}},

			{Operator::EqualTo, {OperatorType::Binary, true, "equal to (==)"}},
			{Operator::NotEqualTo, {OperatorType::Binary, true, "not equal to (!=)"}},

			{Operator::LogicalAnd, {OperatorType::Binary, false, "logical AND (&&, and)"}},
			{Operator::LogicalOr, {OperatorType::Binary, false, "logical OR (||, or)"}},
			{Operator::LogicalXor, {OperatorType::Binary, false, "logical XOR (^^, xor)"}},

			{Operator::Conditional, {OperatorType::Ternary, false, "conditional (?:)"}},

			{Operator::Pair, {OperatorType::Binary, false, "pair (:)"}},

			{Operator::Tuple, {OperatorType::Binary, false, "tuple (,)"}},
			{Operator::TupleDelimiter, {OperatorType::Binary, false, "tuple delimiter (;)"}},

			{Operator::Assignment, {OperatorType::Binary, true, "assignment (=)"}},
			{Operator::MultiplicationAssignment, {OperatorType::Binary, true, "multiplication and assignment (*=)"}},
			{Operator::MultiplicationOvAssignment, {OperatorType::Binary, true, "multiplication with ignoring overflow and assignment (&*=)"}},
			{Operator::DivisionAssignment, {OperatorType::Binary, true, "division and assignment (/=)"}},
			{Operator::DivisionFloorAssignment, {OperatorType::Binary, true, "division with rounding down and assignment (/-=)"}},
			{Operator::DivisionCeilingAssignment, {OperatorType::Binary, true, "division with rounding up and assignment (/+=)"}},
			{Operator::ModulusAssignment, {OperatorType::Binary, true, "modulus and assignment (%=)"}},
			{Operator::ModulusFloorAssignment, {OperatorType::Binary, true, "modulus with rounding down and assignment (%-=)"}},
			{Operator::ModulusCeilingAssignment, {OperatorType::Binary, true, "modulus with rounding up and assignment (%+=)"}},
			{Operator::ExponentAssignment, {OperatorType::Binary, true, "exponent and assignment (**=)"}},
			{Operator::AdditionAssignment, {OperatorType::Binary, true, "addition and assignment (+=)"}},
			{Operator::AdditionOvAssignment, {OperatorType::Binary, true, "addition with ignoring overflow and assignment (&+=)"}},
			{Operator::SubtractionAssignment, {OperatorType::Binary, true, "subtraction and assignment (-=)"}},
			{Operator::SubtractionOvAssignment, {OperatorType::Binary, true, "subtraction with ignoring overflow and assignment (&-=)"}},
			{Operator::LeftShiftAssignment, {OperatorType::Binary, true, "left shift and assignment (<<=)"}},
			{Operator::RightShiftAssignment, {OperatorType::Binary, true, "right shift and assignment (>>=)"}},
			{Operator::UnsignedRightShiftAssignment, {OperatorType::Binary, true, "right shift with zero extension and assignment (>>>=)"}},
			{Operator::BitwiseAndAssignment, {OperatorType::Binary, true, "bitwise AND and assignment (&=)"}},
			{Operator::BitwiseOrAssignment, {OperatorType::Binary, true, "bitwise OR and assignment (|=)"}},
			{Operator::BitwiseXorAssignment, {OperatorType::Binary, true, "bitwise XOR and assignment (^=)"}}
	};

	size_t maxIndex = 0;
	for (auto& p : sourceForOpAttrs)
		maxIndex = std::max(maxIndex, static_cast<size_t>(p.first));
	opAttrs.resize(maxIndex + 1);
	for (auto& p : sourceForOpAttrs)
		opAttrs[static_cast<size_t>(p.first)] = p.second;

	opPairPattern = {Operator::Pair, 2, {{OpPt::Expression, 0}, {StringId::Colon}, {OpPt::Expression, 1}}};
}

static const OperatorAttributes& getOpAttr(Operator op) {
	chatra_assert(static_cast<size_t>(op) < opAttrs.size());
	return opAttrs[static_cast<size_t>(op)];
}

static bool isOperator(const std::shared_ptr<Node>& node, Operator op) {
	return node->op == op;
}

static bool matchesAsNameChain(const std::shared_ptr<Node>& node) {
	size_t count = 0;
	auto* n = node.get();
	for (; n->op == Operator::ElementSelection; count++) {
		if (n->subNodes[1]->type != NodeType::Name)
			return false;
		n = n->subNodes[0].get();
	}
	return count <= 2 && n->type == NodeType::Name;
}

static bool matches(const std::shared_ptr<Node>& node, const OperatorPatternElement& e) {
	switch (e.type) {
	case OpPt::Token:
		return node->type == NodeType::Unknown && node->tokens[0]->sid == e.token;

	case OpPt::NameChain:
		return matchesAsNameChain(node);

	case OpPt::Parenthesis:
		return node->type == NodeType::Unknown && matches(*node->tokens[0], StringId::OpenParenthesis);

	case OpPt::ParenthesisWithValue:
		return node->type == NodeType::Unknown && matches(*node->tokens[0], StringId::OpenParenthesis)
				&& node->subNodes[0];

	case OpPt::Bracket:
		return node->type == NodeType::Unknown && matches(*node->tokens[0], StringId::OpenBracket);

	case OpPt::Brace:
		return node->type == NodeType::Unknown && matches(*node->tokens[0], StringId::OpenBrace);

	case OpPt::Expression:
		return node->type != NodeType::Unknown;

	default:
		throw InternalError();
	}
}

static std::shared_ptr<Node> wrapByTuple(std::shared_ptr<Node> node, const Token* hintToken = nullptr) {
	if (node && isOperator(node, Operator::Tuple))
		return node;

	auto wrapNode = std::make_shared<Node>();
	wrapNode->type = NodeType::Operator;
	wrapNode->op = Operator::Tuple;
	if (node) {
		wrapNode->tokens = node->tokens;
		wrapNode->subNodes.push_back(std::move(node));
	}
	if (wrapNode->tokens.empty() && hintToken != nullptr)
		wrapNode->tokens.emplace_back(hintToken);
	return wrapNode;
}

template <class NodePtrIterator>
static std::shared_ptr<Node> extractOperator(NodePtrIterator first, NodePtrIterator it, const OperatorPattern& pattern) {

	if (!std::equal(it, it + pattern.elements.size(), pattern.elements.cbegin(),
			[](const decltype(*it)& node, const OperatorPatternElement& e) { return matches(node, e); }))
		return nullptr;

	std::shared_ptr<Node> n = std::make_shared<Node>();
	n->type = NodeType::Operator;
	n->op = pattern.op;

	// Special treatments for prefix operators.
	// Some prefix operator has partially same pattern as pattern for binary operators, such as +e (prefix) and e+e (binary).
	// Unfortunately this causes misinterpretation because these prefix operator has higher priority
	// than corresponding binary operator, so these prefix operator should be matched only in: {<first token> | operator} op expression
	auto& attr = getOpAttr(pattern.op);
	if (attr.type == OperatorType::Prefix) {
		if (first != it && ((*(it - 1))->type != NodeType::Unknown || (*(it - 1))->tokens[0]->type != TokenType::Operator))
			return nullptr;
	}

	n->subNodes.resize(pattern.subNodes);
	for (auto& e : pattern.elements) {
		n->tokens.insert(n->tokens.end(), (*it)->tokens.cbegin(), (*it)->tokens.cend());
		if (e.subNodeIndex != SIZE_MAX) {
			auto subNode = *it;
			if (subNode->type == NodeType::Unknown && matches(*subNode->tokens[0], TokenType::OpenBracket))
				subNode = wrapByTuple(subNode->subNodes[0], subNode->tokens.empty() ? nullptr : subNode->tokens[0]);
			n->subNodes[e.subNodeIndex] = std::move(subNode);
		}
		it++;
	}

	// Flatten tuples
	size_t flattenIndex = 0;
	if (n->op == Operator::TupleDelimiter) {
		// TD(n0, n1) -> T(n0, TD, n1), includes TD(T(*), n1) -> T(T(*), TD, n1)
		n->op = Operator::Tuple;
		auto& subNode = *n->subNodes.emplace(n->subNodes.cbegin() + 1, std::make_shared<Node>());
		subNode->type = NodeType::Operator;
		subNode->op = Operator::TupleDelimiter;
		subNode->tokens.push_back((*(it - 2))->tokens[0]);

		if (!n->subNodes[0]) {
			n->subNodes.erase(n->subNodes.cbegin() + 0);
			flattenIndex = 1;
		}
		else if (!n->subNodes[2])
			 n->subNodes.erase(n->subNodes.cbegin() + 2);
	}

	if (n->op == Operator::Tuple && isOperator(n->subNodes[flattenIndex], Operator::Tuple)) {
		// T(T(n0...nN), *) -> T(n0...nN, *)
		auto subNode = n->subNodes[flattenIndex];
		n->subNodes.insert(n->subNodes.erase(n->subNodes.begin() + flattenIndex),
				subNode->subNodes.cbegin(), subNode->subNodes.cend());
	}

	return n;
}

static std::shared_ptr<Node> createNullNode(const Token& templateToken) {
	auto n = std::make_shared<Node>();
	n->type = NodeType::Name;
	n->tokens.push_back(&templateToken);
	n->sid = StringId::Null;
	return n;
}

static std::shared_ptr<Node> createNameNode(const Token& token) {
	auto n = std::make_shared<Node>();
	n->type = NodeType::Name;
	n->tokens.push_back(&token);
	n->sid = token.sid;
	return n;
}

template <class TokenPtrIterator>
static std::vector<std::shared_ptr<Node>> parseNodes(ParserContext& ct, TokenPtrIterator first, TokenPtrIterator last) {

	std::vector<std::shared_ptr<Node>> nodes;
	for (auto it = first; it != last; ) {
		if (matches(**it, TokenType::OpenBracket)) {
			auto& n = addNode(nodes, nullptr, NodeType::Unknown);

			// This is inefficient since findExpressionBoundary() already checked correspondence of braces
			decltype(it) itPair;
			unsigned level = 0;
			for (itPair = it + 1; itPair != last; itPair++) {
				if (matches(**itPair, TokenType::OpenBracket))
					level++;
				else if (matches(**itPair, TokenType::CloseBracket)) {
					if (level == 0)
						break;
					level--;
				}
			}

			n->tokens.insert(n->tokens.end(), it, itPair + 1);
			n->subNodes.push_back(itPair == it + 1 ? nullptr : parseExpression(ct, it + 1, itPair));
			it = itPair + 1;
			continue;
		}

		if (isLiteral(**it)) {
			nodes.push_back(parseAsLiteral(ct, **it++));
			continue;
		}

		if (matches(**it, TokenType::Name)) {
			auto& n = addNode(nodes, nullptr, NodeType::Name);
			n->sid = (*it)->sid;
			n->tokens.push_back(*it++);
			continue;
		}

		if (matches(**it, TokenType::Operator)) {
			// Keep NodeType Unknown to distinguish processed or unprocessed nodes.
			addNode(nodes, nullptr, NodeType::Unknown)->tokens.push_back(*it++);
			continue;
		}

		ct.errorAtToken(ErrorLevel::Error, **it, "expected expression", {});
		nodes.clear();
		break;
	}
	return nodes;
}

template <class TokenPtrIterator>
static std::shared_ptr<Node> parseExpression(ParserContext& ct, TokenPtrIterator first, TokenPtrIterator last) {

	auto nodes = parseNodes(ct, first, last);
	if (nodes.empty())
		return createNullNode(**first);

	// Simple but may inefficient bottom-up parsing
	// TODO rewrite
	for (auto& group : opPatterns) {
		if (group.leftToRight) {
			for (auto it = nodes.begin(); it != nodes.end(); ) {
				bool matched = false;
				for (auto& pattern : group.patterns) {
					if (static_cast<size_t>(std::distance(it, nodes.end())) < pattern.elements.size())
						continue;
					auto n = extractOperator(nodes.begin(), it, pattern);
					if (!n)
						continue;
					it = nodes.erase(it, it + pattern.elements.size());
					it = nodes.insert(it, n);
					matched = true;
				}
				if (!matched)
					it++;
			}
		}
		else {
			for (auto it = nodes.end(); it != nodes.begin(); ) {
				bool matched = false;
				for (auto& pattern : group.patterns) {
					if (static_cast<size_t>(std::distance(nodes.begin(), it)) < pattern.elements.size())
						continue;
					auto n = extractOperator(nodes.begin(), it - pattern.elements.size(), pattern);
					if (!n)
						continue;
					it = nodes.erase(it - pattern.elements.size(), it);
					it = nodes.insert(it, n) + 1;
					matched = true;
				}
				if (!matched)
					it--;
			}
		}
	}

	if (nodes.empty() || nodes[0]->type == NodeType::Unknown) {
		ct.errorAtToken(ErrorLevel::Error, **first, "expected expression", {});
		return createNullNode(**first);
	}

	if (nodes.size() != 1) {
		if (!nodes[1]->tokens.empty())
			ct.errorAtToken(ErrorLevel::Error, *nodes[1]->tokens[0], "extra token in expression", {});
		else
			ct.errorAtToken(ErrorLevel::Error, **first, "extra token in expression starts from this token", {});
		return createNullNode(**first);
	}

	return nodes[0];
}

template <class TokenPtrIterator>
static std::shared_ptr<Node> parseOperatorParameter(ParserContext& ct, TokenPtrIterator first, TokenPtrIterator last) {

	auto nodes = parseNodes(ct, first, last);
	if (nodes.empty())
		return nullptr;

	// Group name:class pair
	for (auto it = nodes.begin(); static_cast<size_t>(std::distance(it, nodes.end())) >= opPairPattern.elements.size(); it++) {
		auto n = extractOperator(nodes.begin(), it, opPairPattern);
		if (n && n->subNodes[0]->type == NodeType::Name && n->subNodes[1]->type == NodeType::Name) {
			it = nodes.erase(it, it + opPairPattern.elements.size());
			it = nodes.insert(it, n);
		}
	}

	for (auto& group : opPatterns) {
		for (auto& pattern : group.patterns) {
			if (!getOpAttr(pattern.op).allowOverride)
				continue;
			if (nodes.size() < pattern.elements.size())
				continue;
			auto n = extractOperator(nodes.begin(), nodes.begin(), pattern);
			if (n)
				return n;
		}
	}

	ct.errorAtToken(ErrorLevel::Error, **first, "expected operator parameter", {});
	return createNullNode(**first);
}

enum class StPt {
	Token,
	Name,
	VariationName,  // .<variation>
	Label,
	LabelWithColon,  // <label>:
	StorageClassSpecifier,  // static
	Expression,
	TupleExpression,  // single top-level Pair operator should be wrapped by Tuple
	ParameterList,  // (<parameter list>)
	Qualifiers,  // as <qualifier>,...
	ClassList,
	VarDefinitions,  // bounded by "as"
	ForLoopVariables,  // bounded by "in"
	OperatorParameter
};

struct StatementPatternElement final {
	StPt type = enum_max<StPt>::value;
	StringId token = StringId::Invalid;
	bool required = false;
	size_t subNodeIndex = 0;
	std::string errorMessage;  // only for required field

	StatementPatternElement(StPt type, StringId token, bool required, size_t subNodeIndex, std::string errorMessage = "") noexcept
			: type(type), token(token), required(required), subNodeIndex(subNodeIndex), errorMessage(std::move(errorMessage)) {}
};

struct StatementAttributes final {
	size_t subNodes;
	size_t skipTokens;
	std::vector<StatementPatternElement> pattern;

	StatementAttributes() noexcept : subNodes(0), skipTokens(0) {}
	StatementAttributes(size_t subNodes, size_t skipTokens, std::vector<StatementPatternElement> pattern) noexcept
			: subNodes(subNodes), skipTokens(skipTokens), pattern(std::move(pattern)) {}
};

static std::vector<StatementAttributes> stAttrs;  // [NodeType]

static void initializeStTables() {
	const std::vector<std::pair<NodeType, StatementAttributes>> sourceForStAttrs = {
			{NodeType::Import, {SubNode::Import_SubNodes, 1, {
					{StPt::LabelWithColon, StringId::Invalid, false, SubNode::Import_Alias},
					{StPt::Name, StringId::Invalid, true, SubNode::Import_Package, "expected package name"}
			}}},
			{NodeType::Def, {SubNode::Def_SubNodes, 1, {
					{StPt::StorageClassSpecifier, StringId::Invalid, false, SubNode::Def_Static},
					{StPt::Name, StringId::Invalid, false, SubNode::Def_Name},
					{StPt::VariationName, StringId::Invalid, false, SubNode::Def_Variation},
					{StPt::ParameterList, StringId::Invalid, true, SubNode::Def_Parameter, "expected parameter list"},
					{StPt::VariationName, StringId::Invalid, false, SubNode::Def_Operator},
					{StPt::ParameterList, StringId::Invalid, false, SubNode::Def_OperatorParameter},
					{StPt::Token, StringId::Delete, false, SubNode::Def_Delete},
					{StPt::Qualifiers, StringId::Invalid, false, SubNode::Def_Qualifiers}
			}}},
			{NodeType::DefOperator, {1, 2, {
					{StPt::OperatorParameter, StringId::Invalid, true, 0, "expected operator parameter set"}
			}}},
			{NodeType::Class, {SubNode::Class_SubNodes, 1, {
					{StPt::StorageClassSpecifier, StringId::Invalid, false, SubNode::Class_Static},
					{StPt::Name, StringId::Invalid, true, SubNode::Class_Name, "expected class name"},
					{StPt::Token, StringId::Extends, false, SubNode::Class_Extends},
					{StPt::ClassList, StringId::Invalid, false, SubNode::Class_BaseClassList}
			}}},
			{NodeType::Expression, {1, 0, {
					{StPt::Expression, StringId::Invalid, true, 0, "expected expression"}
			}}},
			{NodeType::Sync, {0, 1, {}}},
			{NodeType::Touch, {1, 1, {
					{StPt::Expression, StringId::Invalid, true, 0, "expected expression"}
			}}},
			{NodeType::Var, {SubNode::Var_SubNodes, 1, {
					{StPt::StorageClassSpecifier, StringId::Invalid, false, SubNode::Var_Static},
					{StPt::VarDefinitions, StringId::Invalid, true, SubNode::Var_Definitions, "expected variable definitions"},
					{StPt::Qualifiers, StringId::Invalid, false, SubNode::Var_Qualifiers}
			}}},
			{NodeType::IfGroup, {0, 0, {}}},
			{NodeType::If, {1, 1, {
					{StPt::Expression, StringId::Invalid, true, 0, "expected expression"}
			}}},
			{NodeType::ElseIf, {1, 2, {
					{StPt::Expression, StringId::Invalid, true, 0, "expected expression"}
			}}},
			{NodeType::Else, {0, 1, {}}},
			{NodeType::For, {SubNode::For_SubNodes, 0, {
					{StPt::LabelWithColon, StringId::Invalid, false, SubNode::For_Label},
					{StPt::Token, StringId::For, true, SIZE_MAX  /* internal error */},
					{StPt::LabelWithColon, StringId::Invalid, false, SubNode::For_Iterator},
					{StPt::ForLoopVariables, StringId::Invalid, false, SubNode::For_LoopVariable},
					{StPt::Token, StringId::In, true, SIZE_MAX, "expected \"in\""},
					{StPt::Expression, StringId::Invalid, true, SubNode::For_Iterable, "expected expression"}
			}}},
			{NodeType::While, {SubNode::While_SubNodes, 0, {
					{StPt::LabelWithColon, StringId::Invalid, false, SubNode::While_Label},
					{StPt::Token, StringId::While, true, SIZE_MAX  /* internal error */},
					{StPt::Expression, StringId::Invalid, true, SubNode::While_Condition, "expected expression"}
			}}},
			{NodeType::Switch, {1, 1, {
					{StPt::Expression, StringId::Invalid, true, 0, "expected expression"}
			}}},
			{NodeType::Case, {1, 1, {
					{StPt::Expression, StringId::Invalid, true, 0, "expected expression"}
			}}},
			{NodeType::Default, {0, 1, {}}},
			{NodeType::Do, {0, 1, {}}},
			{NodeType::Catch, {SubNode::Catch_SubNodes, 1, {
					{StPt::LabelWithColon, StringId::Invalid, false, SubNode::Catch_Label},
					{StPt::ClassList, StringId::Invalid, true, SubNode::Catch_ClassList, "expected exception-class list"}
			}}},
			{NodeType::Finally, {0, 1, {}}},
			{NodeType::Break, {1, 1, {
					{StPt::Label, StringId::Invalid, false, 0}
			}}},
			{NodeType::Continue, {1, 1, {
					{StPt::Label, StringId::Invalid, false, 0}
			}}},
			{NodeType::Return, {1, 1, {
					{StPt::TupleExpression, StringId::Invalid, false, 0}
			}}},
			{NodeType::Throw, {1, 1, {
					{StPt::Expression, StringId::Invalid, false, 0}
			}}}
	};

	size_t maxIndex = 0;
	for (auto& p : sourceForStAttrs)
		maxIndex = std::max(maxIndex, static_cast<size_t>(p.first));
	stAttrs.resize(maxIndex + 1);
	for (auto& p : sourceForStAttrs)
		stAttrs[static_cast<size_t>(p.first)] = p.second;
}

static const StatementAttributes& getStAttr(NodeType type) {
	chatra_assert(static_cast<size_t>(type) < stAttrs.size());
	return stAttrs[static_cast<size_t>(type)];
}

static bool checkParameterNameDuplicated(ParserContext &ct, std::unordered_set<StringId>& usedNames,
		const std::shared_ptr<Node>& nameNode) {
	StringId name = nameNode->sid;
	if (usedNames.count(name) != 0) {
		ct.errorAtToken(ErrorLevel::Error, *nameNode->tokens[0], "redefinition of parameter \"${0}\"", {ct.getSt()->ref(name)});
		return false;
	}
	usedNames.emplace(name);
	return true;
}

static bool checkAsClass(const std::shared_ptr<Node>& node) {
	return matchesAsNameChain(node);
}

static bool checkAsClassOrLiteral(const std::shared_ptr<Node>& node) {
	return node->type == NodeType::Literal
			|| ((isOperator(node, Operator::UnaryMinus) || isOperator(node, Operator::UnaryPlus))
					&& node->subNodes[0]->type == NodeType::Literal)
			|| checkAsClass(node);
}

static bool checkAsParameterList(ParserContext &ct, const std::shared_ptr<Node>& node) {
	bool delimiterFound = false;
	bool defaultArgFound = false;
	bool varArgFound = false;
	std::unordered_set<StringId> usedNames;
	for (auto& n : node->subNodes) {
		if (isOperator(n, Operator::TupleDelimiter)) {
			if (delimiterFound) {
				ct.errorAtToken(ErrorLevel::Error, *n->tokens[0], "duplicated \";\"", {});
				return false;
			}
			delimiterFound = true;
			varArgFound = false;
			continue;
		}

		if (varArgFound) {
			ct.errorAtToken(ErrorLevel::Error, *n->tokens[0], "variadic arguments should be last of parameters", {});
			return false;
		}

		if (isOperator(n, Operator::VarArg)) {
			if (n->subNodes[0]->type != NodeType::Name) {
				ct.errorAtToken(ErrorLevel::Error, *n->subNodes[0]->tokens[0], "expected parameter name", {});
				return false;
			}
			if (!checkParameterNameDuplicated(ct, usedNames, n->subNodes[0]))
				return false;
			varArgFound = true;
			continue;
		}

		if (n->type == NodeType::Name) {
			if (!delimiterFound && defaultArgFound) {
				ct.errorAtToken(ErrorLevel::Error, *n->tokens[0],
						"parameter without default parameter cannot be placed after the one with default parameter in array-style parameters", {});
				return false;
			}
			if (!checkParameterNameDuplicated(ct, usedNames, n))
				return false;
			continue;
		}

		if (!isOperator(n, Operator::Pair)) {
			ct.errorAtToken(ErrorLevel::Error, *n->tokens[0], *n->tokens.back(), "expected parameter name", {});
			return false;
		}

		if (!checkParameterNameDuplicated(ct, usedNames, n->subNodes[0]))
			return false;

		auto& nValue = n->subNodes[1];
		if (isOperator(nValue, Operator::Call)) {
			if (!checkAsClass(nValue->subNodes[0])) {
				ct.errorAtToken(ErrorLevel::Error, *nValue->tokens[0], *n->tokens.back(), "expected constructor call", {});
				return false;
			}
			defaultArgFound = true;
		}
		else if (!checkAsClassOrLiteral(nValue)) {
			ct.errorAtToken(ErrorLevel::Error, *nValue->tokens[0], *nValue->tokens.back(), "expected class name or default value", {});
			return false;
		}
	}

	return true;
}

static bool checkAsQualifiers(ParserContext& ct, const std::shared_ptr<Node>& node) {
	for (auto& n : node->subNodes) {
		if (checkAsClass(n))
			continue;
		if (isOperator(n, Operator::Call))
			continue;

		ct.errorAtToken(ErrorLevel::Error, *n->tokens[0], *n->tokens.back(), "expected qualifiers", {});
		return false;
	}
	return true;
}

static bool checkAsClassList(ParserContext& ct, const std::shared_ptr<Node>& node) {
	for (auto& n : node->subNodes) {
		if (checkAsClass(n))
			continue;
		ct.errorAtToken(ErrorLevel::Error, *n->tokens[0], *n->tokens.back(), "expected class names", {});
		return false;
	}
	return true;
}

static bool checkAsVarDefinitions(ParserContext& ct, const std::shared_ptr<Node>& node) {
	for (auto& n : node->subNodes) {
		if (n->type == NodeType::Name)
			continue;
		if (isOperator(n, Operator::Pair) && n->subNodes[0]->type == NodeType::Name)
			continue;
		ct.errorAtToken(ErrorLevel::Error, *n->tokens[0], *n->tokens.back(), "expected name [: value]", {});
		return false;
	}
	return true;
}

static bool checkAsOperatorParameter(ParserContext& ct, const std::shared_ptr<Node>& node) {
	std::unordered_set<StringId> usedNames;
	for (auto& n : node->subNodes) {
		if (n->type == NodeType::Name) {
			if (!checkParameterNameDuplicated(ct, usedNames, n))
				return false;
			continue;
		}
		if (isOperator(n, Operator::Pair)) {
			if (n->subNodes[0]->type != NodeType::Name) {
				ct.errorAtToken(ErrorLevel::Error, *n->subNodes[0]->tokens[0], "expected parameter name", {});
				return false;
			}
			if (!checkParameterNameDuplicated(ct, usedNames, n->subNodes[0]))
				return false;
			if (!checkAsClass(n->subNodes[1])) {
				ct.errorAtToken(ErrorLevel::Error, *n->subNodes[1]->tokens[0], "expected class name", {});
				return false;
			}
			continue;
		}
		ct.errorAtToken(ErrorLevel::Error, *n->tokens[0], *n->tokens.back(), "expected operator parameter", {});
		return false;
	}
	return true;
}

template <class TokenPtrIterator, typename TopLevelDelimiter>
static std::shared_ptr<Node> extractExpression(ParserContext& ct, TokenPtrIterator& it, TokenPtrIterator last,
		TopLevelDelimiter topLevelDelimiter) {

	auto itBoundary = findExpressionBoundary(ct, it, last, std::move(topLevelDelimiter));
	if (itBoundary == it)
		return nullptr;
	auto n = parseExpression(ct, it, itBoundary);
	it = itBoundary;
	return n;
}

template <class TokenPtrIterator>
static std::shared_ptr<Node> extractStatementPiece(ParserContext& ct, TokenPtrIterator& it, TokenPtrIterator last,
		const StatementPatternElement& e) {

	chatra_assert(it != last);
	auto it0 = it;
	auto leftTokens = static_cast<size_t>(std::distance(it, last));

	switch (e.type) {
	case StPt::Token:
		if (matches(**it, e.token))
			return createNameNode(**it++);
		break;

	case StPt::Name:
		if (matches(**it, TokenType::Name))
			return createNameNode(**it++);
		break;

	case StPt::VariationName:
		if (leftTokens >= 2 && matches(**it, StringId::OpElementSelection) && matches(**(it + 1), TokenType::Name)) {
			// Variation name is distinguishable from reserved words since it is always used with prefixed ".";
			// No checks performed.
			auto n = createNameNode(**(it + 1));
			it += 2;
			return n;
		}
		break;

	case StPt::Label:
		if (matches(**it, TokenType::Name))
			return createNameNode(**it++);
		break;

	case StPt::LabelWithColon:
		if (leftTokens >= 2 && matches(**it, TokenType::Name) && matches(**(it + 1), StringId::Colon)) {
			auto n = createNameNode(**it);
			it += 2;
			return n;
		}
		break;

	case StPt::StorageClassSpecifier:
		// Currently only supports "static"
		if (matches(**it, StringId::Static))
			return wrapByTuple(createNameNode(**it++));
		break;

	case StPt::Expression: {
		auto n = extractExpression(ct, it, last, [](const Token& token) { (void)token; return false; });
		if (!n)
			break;
		return n;
	}

	case StPt::TupleExpression: {
		auto n = extractExpression(ct, it, last, [](const Token& token) { (void)token; return false; });
		if (!n)
			break;
		if (isOperator(n, Operator::Pair))
			n = wrapByTuple(n);
		return n;
	}

	case StPt::ParameterList:
		if (matches(**it, StringId::OpenParenthesis)) {
			auto itBoundary = findExpressionBoundary(ct, it + 1, last, [](const Token& token) {
					return matches(token, StringId::CloseParenthesis); });
			// Empty parameter list means "void" function, so it should be accepted
			if (itBoundary == last || !matches(**itBoundary, StringId::CloseParenthesis))
				break;
			if (itBoundary == it + 1) {
				auto itHint = it;
				it += 2;
				return wrapByTuple(nullptr, *itHint);
			}

			auto n = wrapByTuple(parseExpression(ct, it + 1, itBoundary));
			if (!checkAsParameterList(ct, n))
				break;
			it = itBoundary + 1;
			return n;
		}
		break;

	case StPt::Qualifiers:
		if (leftTokens >= 2 && matches(**it, StringId::As)) {
			auto itExp = it + 1;
			auto n = extractExpression(ct, itExp, last, [](const Token& token) { (void)token; return false; });
			if (!n)
				break;
			n = wrapByTuple(n);
			if (!checkAsQualifiers(ct, n))
				break;
			it = itExp;
			return n;
		}
		break;

	case StPt::ClassList: {
		auto n = extractExpression(ct, it, last, [](const Token& token) { (void)token; return false; });
		if (!n)
			break;
		n = wrapByTuple(n);
		if (!checkAsClassList(ct, n))
			break;
		return n;
	}

	case StPt::VarDefinitions: {
		auto n = extractExpression(ct, it, last, [](const Token& token) { return matches(token, StringId::As); });
		if (!n)
			break;
		n = wrapByTuple(n);
		if (!checkAsVarDefinitions(ct, n))
			break;
		return n;
	}

	case StPt::ForLoopVariables: {
		auto n = extractExpression(ct, it, last, [](const Token& token) { return matches(token, StringId::In); });
		if (!n)
			break;
		n = wrapByTuple(n);
		// if (!checkAsForLoopVariables(ct, n))
		// 	break;
		return n;
	}

	case StPt::OperatorParameter:
		if (matches(**it, StringId::OpenParenthesis)) {
			auto itBoundary = findExpressionBoundary(ct, it + 1, last, [](const Token& token) {
				return matches(token, StringId::CloseParenthesis); });
			if (itBoundary == it + 1 || itBoundary == last || !matches(**itBoundary, StringId::CloseParenthesis))
				break;

			auto n = parseOperatorParameter(ct, it + 1, itBoundary);
			if (!checkAsOperatorParameter(ct, n))
				break;
			it = itBoundary + 1;
			return n;
		}
		break;
	}

	if (e.required) {
		chatra_assert(!e.errorMessage.empty());
		ct.errorAtToken(ErrorLevel::Error, **it0, e.errorMessage, {});
	}

	return nullptr;
}

static bool checkCombination(ParserContext& ct, const std::shared_ptr<Node>& n,
		size_t subNode0, size_t subNode1, const std::string& message) {

	if(static_cast<bool>(n->subNodes[subNode0]) == static_cast<bool>(n->subNodes[subNode1]))
		return true;

	auto& firstToken = *n->subNodes[n->subNodes[subNode0] ? subNode0 : subNode1]->tokens[0];
	auto& lastToken = *n->subNodes[n->subNodes[subNode0] ? subNode0 : subNode1]->tokens.back();
	ct.errorAtToken(ErrorLevel::Error, firstToken, lastToken, message, {});
	return false;
}

static bool qualifyDef(ParserContext& ct, const std::shared_ptr<Node>& n) {
	if (!n->subNodes[SubNode::Def_Qualifiers])
		return true;

	for (auto& nSub : n->subNodes[SubNode::Def_Qualifiers]->subNodes) {
		if (nSub->type == NodeType::Name && nSub->sid == StringId::Native) {
			n->flags |= NodeFlags::Native;
			continue;
		}
		ct.errorAtToken(ErrorLevel::Error, *nSub->tokens[0], *nSub->tokens.back(), "expected qualifiers", {});
		return false;
	}
	return true;
}

template <class InputIterator, class Predicate>
static InputIterator find_it(InputIterator first, InputIterator last, Predicate predicate) {
	for ( ; first != last; first++) {
		if (predicate(first, last))
			return first;
	}
	return last;
}

static const Token* addToken(ParserContext& ct, const std::shared_ptr<Node>& node, const Token& templateToken,
		TokenType type, std::string token) {

	chatra_assert(type != TokenType::Number && type != TokenType::String);

	node->additionalTokens.emplace_back(new Token(
			templateToken.line, templateToken.index, templateToken.first, templateToken.last,
			type, add(ct.getSt(), std::move(token))));

	return node->additionalTokens.back().get();
}

// key: Node* or Token*
using AnnotationMap = std::unordered_map<const void*, std::vector<std::shared_ptr<Node>>>;

static AnnotationMap parseAnnotations(ParserContext& ct, const std::shared_ptr<Node>& node) {
	AnnotationMap ret;
	std::vector<std::shared_ptr<Node>> pending;

	for (auto it = node->tokens.begin(); it != node->tokens.end(); ) {
		if (std::distance(it, node->tokens.end()) < 2 || !matches(**it, StringId::Annotation)) {
			if (!pending.empty()) {
				ret.emplace(*it, pending);
				pending.clear();
			}
			it++;
			continue;
		}

		if (matches(**(it + 1), TokenType::Name)) {
			pending.push_back(parseExpression(ct, it + 1, it + 2));
			it = node->tokens.erase(it, it + 2);
		}
		else if (matches(**(it + 1), StringId::OpenBrace)) {
			auto itLast = findExpressionBoundary(ct, it + 2, node->tokens.end(), [](const Token& token) {
				return matches(token, StringId::CloseBrace); });
			if (itLast == it + 2 || itLast == node->tokens.end() || !matches(**itLast, StringId::CloseBrace)) {
				ct.errorAtNextToken(ErrorLevel::Error, **(it + 1), "expected annotation", {});
				break;
			}
			auto n = wrapByTuple(parseExpression(ct, it + 2, itLast));
			pending.insert(pending.end(), n->subNodes.cbegin(), n->subNodes.cend());
			it = node->tokens.erase(it, itLast + 1);
		}
		else {
			ct.errorAtToken(ErrorLevel::Error, **it, "expected annotation name or \"{\"", {});
			break;
		}
	}

	if (!pending.empty()) {
		if (ret.empty())
			ret.emplace(nullptr, std::move(pending));
		else
			ct.errorAtToken(ErrorLevel::Error, *pending[0]->tokens[0], *pending.back()->tokens.back(), "expected target of annotation", {});
	}
	return ret;
}

static AnnotationMap parseInnerAnnotations(ParserContext& ct, Node* node) {
	AnnotationMap aMap;
	std::vector<std::shared_ptr<Node>> pending;
	for (auto it = node->blockNodes.begin(); it != node->blockNodes.end(); ) {
		auto a = parseAnnotations(ct, *it);
		if (!a.empty() && (*it)->tokens.empty()) {
			it = node->blockNodes.erase(it);
			auto nodes = a[nullptr];
			pending.insert(pending.end(), nodes.cbegin(), nodes.cend());
		}
		else {
			if (!pending.empty()) {
				aMap.emplace(it->get(), pending);
				pending.clear();
			}
			aMap.insert(a.cbegin(), a.cend());
			it++;
		}
	}
	return aMap;
}

static void mapAnnotations(ParserContext& ct, AnnotationMap& aMap, Node* node) {
	std::unordered_map<const void*, std::pair<size_t, std::shared_ptr<Node>>> map;
	for (auto& e : aMap)
		map.emplace(e.first, std::make_pair(SIZE_MAX, nullptr));

	std::deque<std::shared_ptr<Node>> pendingNodes;
	for (auto& n : node->blockNodes) {
		pendingNodes.push_back(n);
		if (n && n->type == NodeType::IfGroup)
			pendingNodes.insert(pendingNodes.end(), n->blockNodes.cbegin(), n->blockNodes.cend());
	}

	while (!pendingNodes.empty()) {
		auto n = pendingNodes[0];
		pendingNodes.pop_front();
		if (!n)
			continue;

		auto itNode = map.find(n.get());
		if (itNode != map.end()) {
			itNode->second.first = 0;
			itNode->second.second = n;
		}

		for (auto& token : n->tokens) {
			auto itToken = map.find(token);
			if (itToken != map.end() && itToken->second.first >= n->tokens.size()) {
				itToken->second.first = n->tokens.size();
				itToken->second.second = n;
			}
		}

		pendingNodes.insert(pendingNodes.end(), n->subNodes.cbegin(), n->subNodes.cend());
	}

	for (auto& e : map) {
		auto annotations = aMap[e.first];
		if (e.second.first == SIZE_MAX)
			ct.errorAtToken(ErrorLevel::Error, *annotations[0]->tokens[0], "no entity found associated with the annotations start from here", {});
		else
			e.second.second->annotations.insert(e.second.second->annotations.end(), annotations.cbegin(), annotations.cend());
	}
}

static void transferAnnotations(AnnotationMap& aMap, const void* from, const void* to) {
	chatra_assert(aMap.count(to) == 0);
	auto it = aMap.find(from);
	if (it == aMap.end())
		return;
	auto value = it->second;
	aMap.erase(it);
	aMap.emplace(to, std::move(value));
}

template <class NodePtrContainer, class NodePtrContainerIterator>
static bool replaceListComprehension(ParserContext& ct, AnnotationMap& aMap, NodePtrContainer& nodes, NodePtrContainerIterator& it) {

	// list comprehension: {for [<iterator>:] [<loop variable>] in <iterable>: <expression>}

	auto& n = (*it);
	auto itFirst = find_it(n->tokens.begin(), n->tokens.end(), [&](decltype(n->tokens.begin()) first, decltype(n->tokens.begin()) last) {
		return std::distance(first, last) >= 7  // {, for, in, <iterable>, :, <expression>, }
				&& matches(**first, StringId::OpenBrace) && matches(**(first + 1), StringId::For);
	});
	if (itFirst == n->tokens.end())
		return false;

	auto itLast = findExpressionBoundary(ct, itFirst + 1, n->tokens.end(), [](const Token& token) {
		return matches(token, StringId::CloseBrace); });
	if (itLast == itFirst + 1 || itLast == n->tokens.end())
		return false;

	// Find "in", followed by iterable expression and colon
	auto itIn = std::find_if(itFirst, itLast, [](const Token*& token) { return matches(*token, StringId::In); });
	if (itIn == itLast)
		return false;

	auto itColon = findExpressionBoundary(ct, itIn + 1, itLast, [](const Token& token) {
		return matches(token, StringId::Colon); });
	if (itColon == itIn + 1 || itColon == itLast)
		return false;

	// Construct nodes
	auto varName = ct.allocateGeneratedVar();
	auto n0 = std::make_shared<Node>();
	n0->type = NodeType::Expression;
	n0->line = n->line;
	n0->tokens.push_back(addToken(ct, n0, **itFirst, TokenType::Name, varName));
	n0->tokens.push_back(addToken(ct, n0, **itFirst, TokenType::Operator, ":"));
	n0->tokens.push_back(addToken(ct, n0, **itFirst, TokenType::OpenBracket, "{"));
	n0->tokens.push_back(addToken(ct, n0, **itFirst, TokenType::CloseBracket, "}"));
	transferAnnotations(aMap, *itFirst, n0.get());

	auto n1 = std::make_shared<Node>();
	n1->type = NodeType::For;
	n1->line = n->line;
	n1->tokens.insert(n1->tokens.end(), itFirst + 1, itColon);
	n1->blockNodesState = NodeState::Structured;

	auto& n2 = addNode(n1->blockNodes, n->line, NodeType::Expression);
	n2->tokens.push_back(addToken(ct, n2, **itFirst, TokenType::Name, varName));
	n2->tokens.push_back(addToken(ct, n2, **itFirst, TokenType::Operator, "."));
	n2->tokens.push_back(addToken(ct, n2, **itFirst, TokenType::Name, "add"));
	n2->tokens.push_back(addToken(ct, n2, **itFirst, TokenType::OpenBracket, "("));
	n2->tokens.insert(n2->tokens.end(), itColon + 1, itLast);
	n2->tokens.push_back(addToken(ct, n2, **itFirst, TokenType::CloseBracket, ")"));

	auto replaced = addToken(ct, n, **itFirst, TokenType::Name, varName);
	n->tokens.insert(n->tokens.erase(itFirst, itLast + 1), replaced);

	it = nodes.insert(it, std::move(n1));
	it = nodes.insert(it, std::move(n0));
	return true;
}

template <class NodePtrContainer, class NodePtrContainerIterator>
static bool replaceWhere(ParserContext& ct, AnnotationMap& aMap, NodePtrContainer& nodes, NodePtrContainerIterator& it) {

	// where: <sentence> where LF {<placeholder>: <expression> LF }...

	auto& n = (*it);
	auto itFirst = find_it(n->tokens.begin(), n->tokens.end(), [&](decltype(n->tokens.begin()) first, decltype(n->tokens.begin()) last) {
		return std::distance(first, last) >= 2 && (*first)->line.lock()->lineNo != (*(first + 1))->line.lock()->lineNo
				&& matches(**first, StringId::Where);
	});
	if (itFirst == n->tokens.end())
		return false;

	std::vector<std::shared_ptr<Node>> additionalNodes;
	std::unordered_map<StringId, std::shared_ptr<Node>> nodeMap;  // placeholder -> Node
	std::unordered_map<StringId, std::string> varMap;  // placeholder -> generated variable
	for (auto itDef = itFirst + 1; itDef != n->tokens.end(); ) {
		if (std::distance(itDef, n->tokens.end()) < 3
				|| !matches(**itDef, TokenType::Name) || !matches(**(itDef + 1), StringId::Colon)) {
			ct.errorAtToken(ErrorLevel::Error, **itDef, "expected name : value pair", {});
			return false;
		}
		auto itNext = findExpressionBoundary(ct, itDef + 2, n->tokens.end(), [](const Token& token) {
			return matches(token, StringId::Comma); });
		if (itNext == itDef + 2)
			return false;

		StringId placeholder = (*itDef)->sid;
		auto varName = ct.allocateGeneratedVar();
		varMap.emplace(placeholder, varName);

		auto& n0 = addNode(additionalNodes, n->line, NodeType::Expression);
		n0->tokens.push_back(addToken(ct, n0, **itDef, TokenType::Name, varName));
		n0->tokens.push_back(addToken(ct, n0, **itDef, TokenType::Operator, ":"));
		n0->tokens.push_back(addToken(ct, n0, **itDef, TokenType::OpenBracket, "("));
		n0->tokens.insert(n0->tokens.end(), itDef + 2, itNext);
		n0->tokens.push_back(addToken(ct, n0, **itDef, TokenType::CloseBracket, ")"));
		nodeMap.emplace(placeholder, n0);

		transferAnnotations(aMap, *itDef, n0.get());
		itDef = (itNext == n->tokens.end() ? itNext : itNext + 1);
	}

	n->tokens.erase(itFirst, n->tokens.end());
	for (auto itRef = n->tokens.begin(); itRef != n->tokens.end(); itRef++) {
		if (!matches(**itRef, TokenType::Name))
			continue;
		auto placeholder = (*itRef)->sid;
		auto itMap = varMap.find(placeholder);
		if (itMap == varMap.end())
			continue;
		if (itRef != n->tokens.begin() && matches(**(itRef - 1), StringId::OpElementSelection))
			continue;

		auto replaced = addToken(ct, nodeMap[placeholder], **itRef, TokenType::Name, itMap->second);
		transferAnnotations(aMap, *itRef, replaced);
		itRef = n->tokens.insert(n->tokens.erase(itRef), replaced);
	}

	for (auto itNode = additionalNodes.rbegin(); itNode != additionalNodes.rend(); itNode++)
		it = nodes.insert(it, *itNode);
	return true;
}

template <class NodePtrContainer, class NodePtrContainerIterator>
static bool replaceLambda(ParserContext& ct, AnnotationMap& aMap, Node* node,
		NodePtrContainer& nodes, NodePtrContainerIterator& it) {

	// lambda: (<parameter list>) { <sentences> }

	auto& n = (*it);
	for (auto itFirst = n->tokens.begin(); itFirst != n->tokens.end(); itFirst++) {
		if (std::distance(itFirst, n->tokens.end()) < 4)
			break;
		if (!matches(**itFirst, StringId::OpenParenthesis))
			continue;
		if (itFirst != n->tokens.begin() && matches(**(itFirst - 1), TokenType::Name))
			continue;
		auto itArgLast = findExpressionBoundary(ct, itFirst + 1, n->tokens.end(), [](const Token& token) {
			return matches(token, StringId::CloseParenthesis); });
		if (std::distance(itArgLast, n->tokens.end()) < 3 || !matches(**itArgLast, StringId::CloseParenthesis))
			continue;
		if (!matches(**(itArgLast + 1), StringId::OpenBrace))
			continue;
		auto itBodyLast = findExpressionBoundary(ct, itArgLast + 2, n->tokens.end(), [](const Token& token) {
			return matches(token, StringId::CloseBrace); });
		if (itBodyLast == n->tokens.end() || !matches(**itBodyLast, StringId::CloseBrace))
			continue;

		auto varName = ct.allocateGeneratedVar();
		StringId varNameId = add(ct.getSt(), varName);

		auto n0 = std::make_shared<Node>();
		n0->type = NodeType::Def;
		n0->line = n->line;
		n0->tokens.push_back(addToken(ct, n0, **itFirst, TokenType::Name, "def"));
		n0->tokens.push_back(addToken(ct, n0, **itFirst, TokenType::Name, varName));
		n0->tokens.push_back(addToken(ct, n0, **itFirst, TokenType::OpenBracket, "("));
		n0->tokens.insert(n0->tokens.end(), itFirst + 1, itArgLast);
		n0->tokens.push_back(addToken(ct, n0, **itFirst, TokenType::CloseBracket, ")"));
		n0->sid = varNameId;
		n0->blockNodesState = NodeState::Structured;
		transferAnnotations(aMap, *itFirst, n0.get());

		auto& n1 = addNode(n0->blockNodes, n->line, NodeType::Return);
		n1->tokens.push_back(addToken(ct, n1, **itFirst, TokenType::Name, "return"));
		n1->tokens.insert(n1->tokens.end(), itArgLast + 2, itBodyLast);

		auto itReplace = n->tokens.erase(itFirst, itBodyLast + 1);
		itReplace = n->tokens.insert(itReplace, addToken(ct, n, **itFirst, TokenType::Name, varName));
		n->tokens.insert(itReplace, addToken(ct, n, **itFirst, TokenType::Operator, "&"));

		it = nodes.insert(it, n0);
		node->symbols.push_back(std::move(n0));
		return true;
	}
	return false;
}

static void replaceSyntaxSugar(ParserContext& ct, AnnotationMap& aMap, Node* node) {
	for (auto it = node->blockNodes.begin(); it != node->blockNodes.end(); ) {
		if (replaceListComprehension(ct, aMap, node->blockNodes, it))
			continue;
		if (replaceWhere(ct, aMap, node->blockNodes, it))
			continue;
		if (replaceLambda(ct, aMap, node, node->blockNodes, it))
			continue;
		it++;
	}
}

void parseInnerNode(IErrorReceiver& errorReceiver,
		std::shared_ptr<StringTable>& sTable, Node* node, bool recursive) {

	chatra_assert(node->blockNodes.empty() || node->blockNodesState == NodeState::Structured);

	ParserContext ct(errorReceiver, sTable);

	auto aMap = parseInnerAnnotations(ct, node);
	replaceSyntaxSugar(ct, aMap, node);

	std::vector<std::shared_ptr<Node>> outNodes;
	for (auto& n : node->blockNodes) {
		auto& attr = getStAttr(n->type);

		if (n->line && n->line->containsError) {
			ct.errorAtLine(ErrorLevel::Error, n->line, "interpreter reached at the line which contains error on lexical analysis", {});
			throw AbortCompilingException();
		}

		// Parse subNodes
		n->subNodes.resize(attr.subNodes);

		auto it = n->tokens.cbegin() + attr.skipTokens;
		bool hasError = false;
		for (auto& e : attr.pattern) {
			if (it == n->tokens.cend()) {
				if (!e.required)
					continue;
				chatra_assert(!e.errorMessage.empty());
				ct.errorAtNextToken(ErrorLevel::Error, *n->tokens.back(), e.errorMessage, {});
				hasError = true;
				break;
			}

			auto subNode = extractStatementPiece(ct, it, n->tokens.cend(), e);
			if (subNode) {
				if (e.subNodeIndex != SIZE_MAX) {
					chatra_assert(e.subNodeIndex < n->subNodes.size() && !n->subNodes[e.subNodeIndex]);
					n->subNodes[e.subNodeIndex] = std::move(subNode);
				}
			}
			else if (e.required) {
				hasError = true;
				break;
			}
		}
		if (hasError)
			continue;

		if (it != n->tokens.cend()) {
			ct.errorAtToken(ErrorLevel::Error, **it, "extra token", {});
			continue;
		}

		// Combination check
		if (n->type == NodeType::Def && !checkCombination(ct, n,
				SubNode::Def_Operator, SubNode::Def_OperatorParameter, "expected \".name(parameters)\""))
			continue;
		if (n->type == NodeType::Class && !checkCombination(ct, n,
				SubNode::Class_Extends, SubNode::Class_BaseClassList, "expected \"extends base-classes,...\""))
			continue;
		if (n->type == NodeType::Def && n->subNodes[SubNode::Def_Delete] && !n->blockNodes.empty()) {
			ct.errorAtLine(ErrorLevel::Error, n->blockNodes[0]->line, "deleted method cannot have nested block", {});
			continue;
		}
		if (n->type == NodeType::Def && n->subNodes[SubNode::Def_Delete] &&
				(n->sid == StringId::Init || n->sid == StringId::Deinit)) {
			ct.errorAtToken(ErrorLevel::Error, *n->subNodes[SubNode::Def_Delete]->tokens[0],
					R"("init" and "deinit" cannot be deleted method)", {});
			continue;
		}
		if (n->type == NodeType::Def && n->subNodes[SubNode::Def_Static]) {
			ct.errorAtToken(ErrorLevel::Warning, *n->subNodes[SubNode::Def_Static]->tokens[0],
					"static qualifier is reserved keyword, currently not working", {});
		}
		if (n->type == NodeType::Class && n->subNodes[SubNode::Class_Static]) {
			ct.errorAtToken(ErrorLevel::Warning, *n->subNodes[SubNode::Class_Static]->tokens[0],
					"static qualifier is reserved keyword, currently not working", {});
		}

		// Additional process (set flags, etc)
		if (n->type == NodeType::Def && !qualifyDef(ct, n))
			continue;

		outNodes.push_back(n);

		if (!recursive) {
			if (n->type == NodeType::Sync || n->type == NodeType::IfGroup) {
				parseInnerNode(errorReceiver, sTable, n.get(), false);
				n->blockNodesState = NodeState::Parsed;
			}
		}
	}

	node->blockNodes = outNodes;
	mapAnnotations(ct, aMap, node);

	if (recursive) {
		for (auto& n : node->blockNodes) {
			parseInnerNode(errorReceiver, sTable, n.get(), true);
			n->blockNodesState = NodeState::Parsed;
		}
	}

	// node->blockNodesState = NodeState::Parsed;
}

void initializeParser() {
	initializeLineTypes();
	initializeOpTables();
	initializeStTables();
}

void errorAtNode(IErrorReceiver& errorReceiver, ErrorLevel level, Node* node,
		const std::string& message, const std::vector<std::string>& args) {

	chatra_assert(!message.empty());

	if (node == nullptr) {
		errorReceiver.error(level, "", UnknownLine, "", SIZE_MAX, SIZE_MAX, message, args);
		return;
	}

	chatra_assert(!node->tokens.empty());
	auto& firstToken = *node->tokens[0];
	auto firstLine = firstToken.line.lock();
	chatra_assert(firstLine);

	size_t first = firstToken.first;
	size_t last = (firstToken.last == SIZE_MAX ? 0 : firstToken.last);
	for (size_t i = 1; i < node->tokens.size(); i++) {
		auto token = *node->tokens[i];
		auto line = token.line.lock();
		if (firstLine.get() != line.get())
			break;
		first = std::min(first, token.first);
		last = std::max(last, token.last == SIZE_MAX ? 0 : token.last);
	}
	if (first == SIZE_MAX || last == 0) {
		first = SIZE_MAX;
		last = SIZE_MAX;
	}

	errorReceiver.error(level, firstLine->fileName, firstLine->lineNo, firstLine->line, first, last, message, args);
}

std::string getOpDescription(Operator op) {
	return getOpAttr(op).description;
}

#ifndef CHATRA_NDEBUG
static const char* toString(NodeState v) {
	switch (v) {
	case NodeState::Grouped:  return "Grouped";
	case NodeState::Structured:  return "Structured";
	case NodeState::Parsed:  return "Parsed";
	default:  return "(invalid)";
	}
}

static const char* toString(NodeType v) {
	switch (v) {
	case NodeType::Unknown:  return "Unknown";
	case NodeType::ScriptRoot:  return "ScriptRoot";

	case NodeType::Import:  return "Import";
	case NodeType::Def:  return "Def";
	case NodeType::DefOperator:  return "DefOperator";
	case NodeType::Class:  return "Class";
	case NodeType::Expression:  return "Expression";
	case NodeType::Sync:  return "Sync";
	case NodeType::Touch:  return "Touch";
	case NodeType::Var:  return "Var";
	case NodeType::IfGroup:  return "IfGroup";
	case NodeType::If:  return "If";
	case NodeType::ElseIf:  return "ElseIf";
	case NodeType::Else:  return "Else";
	case NodeType::For:  return "For";
	case NodeType::While:  return "While";
	case NodeType::Switch:  return "Switch";
	case NodeType::Case:  return "Case";
	case NodeType::Default:  return "Default";
	case NodeType::Do:  return "Do";
	case NodeType::Catch:  return "Catch";
	case NodeType::Finally:  return "Finally";
	case NodeType::Break:  return "Break";
	case NodeType::Continue:  return "Continue";
	case NodeType::Return:  return "Return";
	case NodeType::Throw:  return "Throw";

	case NodeType::Literal:  return "Literal";
	case NodeType::Name:  return "Name";
	case NodeType::Operator:  return "Operator";

	default:  return "(invalid)";
	}
}

static void dumpIndent(unsigned level) {
	for (unsigned i = 0; i < level; i++)
		printf("    ");
}

static void dump(const std::shared_ptr<StringTable>& sTable, const std::shared_ptr<Node>& node,
		unsigned level, bool shortStyle) {

	dumpIndent(level);
	if (!node) {
		printf("Node{(null)}\n");
		return;
	}
	printf("Node{type=%s, lineNo=%u", toString(node->type), node->line ? node->line->lineNo : 0);
	if (node->sid != StringId::Invalid)
		printf(", name=\"%s\"", sTable->ref(node->sid).c_str());
	if (node->type == NodeType::Operator)
		printf(", op=%d \"%s\"", static_cast<unsigned>(node->op), getOpAttr(node->op).description.c_str());
	if (!node->blockNodes.empty())
		printf(", blockNodesState=%s", toString(node->blockNodesState));
	if (shortStyle) {
		printf("}\n");
		return;
	}

	if (node->line)
		printf(", line=\"%s\"", node->line->line.c_str());

	if (!node->tokens.empty()) {
		printf(", tokens=\"");
		for (size_t i = 0; i < node->tokens.size(); i++) {
			printf("%s%s", i == 0 ? "" : " ",
					!node->tokens[i]->literal.empty() ? node->tokens[i]->literal.c_str() : sTable->ref(node->tokens[i]->sid).c_str());
		}
		printf("\"");
	}

	if (node->literalValue) {
		printf(", literalValue=");
		switch (node->literalValue->type) {
		case LiteralType::Null:  printf("null");  break;
		case LiteralType::Bool:  printf("%s(Bool)", node->literalValue->vBool ? "true" : "false");  break;
		case LiteralType::Int:  printf("%lld(Int)", static_cast<long long>(node->literalValue->vInt));  break;
		case LiteralType::Float:  printf("%g(Float)", node->literalValue->vFloat);  break;
		case LiteralType::String:  printf("\"%s\"(String)", node->literalValue->vString.c_str());  break;
		case LiteralType::MultilingualString:  printf("\"%s\"(MultilingualString)", node->literalValue->vString.c_str());  break;
		}
	}

	if (!node->symbols.empty()) {
		printf(", symbols=[\n");
		for (auto& n : node->symbols) {
			dumpIndent(level);
			printf("\"%s\": ", sTable->ref(n->sid).c_str());
			dump(sTable, n, 0, true);
		}
		dumpIndent(level);
		printf("]");
	}

	printf("}\n");

	for (auto& n : node->subNodes)
		dump(sTable, n, level + 2, false);

	if (!node->annotations.empty()) {
		dumpIndent(level + 2);
		printf("@annotations:\n");
		for (auto& n : node->annotations)
			dump(sTable, n, level + 2, false);
	}

	for (auto& n : node->blockNodes)
		dump(sTable, n, level + 1, false);
}

void dump(const std::shared_ptr<StringTable>& sTable, const std::shared_ptr<Node>& node) {
	dump(sTable, node, 0, false);
}
#endif // !CHATRA_NDEBUG

}  // namespace chatra
