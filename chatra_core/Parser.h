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

#ifndef CHATRA_PARSER_H
#define CHATRA_PARSER_H

#include "Internal.h"
#include "StringTable.h"
#include "LexicalAnalyzer.h"
#include "Serialize.h"

namespace chatra {

enum class NodeState {
	// Partially identified line types (only applied to Class or Def).
	// Nested lines stay on Raw state except inner classes or nested functions.
	// Interpreter can find code point for class reference or function call.
	Grouped,
	// Fully identified line types and correctly distinguished between wrapped lines and block.
	// Interpreter can find path for running and keep unused lines as Raw state.
	Structured,
	// Fully parsed
	Parsed,
};

enum class NodeType : uint_least8_t {
	Unknown = 0,  // Not parsed at all. state should be Raw
	ScriptRoot = 1,

	// Statement
	Import = 2,  // import [<alias>:] <package name>
	Def = 3,  // def [static] [<name>] [.<variation>] (<parameter list>) [.<method operator>(<method operator parameter list>)] [delete] [as <qualifier>,...]
	DefOperator = 4,  // def operator (<operator parameters>)
	Class = 5,  // class [static] <name> [extends <base class list>]
	Expression = 6,
	Sync = 7,  // sync
	Touch = 8,  // touch <variables>
	Var = 9,  // var [static] <definitions> [as <qualifier>,...]
	IfGroup = 10,  // {If, ElseIf, ..., Else}
	If = 11,  // if <condition>
	ElseIf = 12,  // else if <condition>
	Else = 13,  // else
	For = 14,  // [<label>:] for [<iterator>:] [<loop variables>] in <iterable>
	While = 15,  // [<label>:] while <condition>
	Switch = 16,  // switch <target> {Case, ..., Default, Catch, Finally}
	Case = 17, // case <case condition>
	Default = 18,  // default
	Do = 19,  // do
	Catch = 20,  // catch [<label>:] <exception type>,...
	Finally = 21,  // finally
	Break = 22,  // break [<label>]
	Continue = 23,  // continue [<label>]
	Return = 24,  // return [<expression>]
	Throw = 25,  // throw [<expression>]

	// Expression
	Literal = 26,
	Name = 27,  // including "self", "super", "null"
	Operator = 28,
};

constexpr size_t NumberOfNodeTypes = 29;

enum class LiteralType {
	Null, Bool, Int, Float, String, MultilingualString
};

struct Literal final {
	LiteralType type;
	union {
		bool vBool;
		uint64_t vInt;
		double vFloat;
	};
	std::string vString;
};

enum class Operator : uint_least8_t {
	// Group 0: special operators and postfix (left to right)
	Container = 0,  // [[<package>.] <container class> ["(" <container class args> ")"]] "{" <expression> "}"
	Subscript = 1,  // <expression> "[" <expression> "]"
	Call = 2,  // <expression> "(" <expression> ")"
	Parenthesis = 3,  // "(" <expression> ")"
	ElementSelection = 4,  // a . b
	PostfixIncrement = 5,  // a++
	PostfixDecrement = 6,  // a--
	VarArg = 7,  // a...

	// Group 1: prefix (right to left)
	PrefixIncrement = 8,  // ++a
	PrefixDecrement = 9,  // --a
	BitwiseComplement = 10,  // ~a
	LogicalNot = 11,  // !a, not a
	UnaryPlus = 12,  // +a
	UnaryMinus = 13,  // -a
	TupleExtraction = 14,  // *a (only allowed at directly under Tuple/Pair)
	FunctionObject = 15,  // &a
	Async = 16,  // async a

	// Group 2: multipliers (left to right)
	Multiplication = 17,  // a * b
	MultiplicationOv = 18,  // a &* b
	Division = 19,  // a / b
	DivisionFloor = 20,  // a /- b
	DivisionCeiling = 21,  // a /+ b
	Modulus = 22,  // a % b
	ModulusFloor = 23,  // a % b
	ModulusCeiling = 24,  // a % b
	Exponent = 25,  // a ** b

	// Group 3: additions (left to right)
	Addition = 26,  // a + b
	AdditionOv = 27,  // a &+ b
	Subtraction = 28,  // a - b
	SubtractionOv = 29,  // a &- b

	// Group 4: shifts (left to right)
	LeftShift = 30,  // a << b
	RightShift = 31,  // a >> b
	UnsignedRightShift = 32,  // a >>> b

	// Group 5: bitwise AND (left to right)
	BitwiseAnd = 33,  // a & b

	// Group 6: bitwise OR (left to right)
	BitwiseOr = 34,  // a | b

	// Group 7: bitwise XOR (left to right)
	BitwiseXor = 35,  // a ^ b

	// Group 8: relations (left to right)
	LessThan = 36,  // a < b
	GreaterThan = 37,  // a > b
	LessThanOrEqualTo = 38,  // a <= b
	GreaterThanOrEqualTo = 39,  // a >= b
	InstanceOf = 40,  // a is b

	// Group 9: equality (left to right)
	EqualTo = 41,  // a == b
	NotEqualTo = 42,  // a != b

	// Group 10: logical AND (left to right)
	LogicalAnd = 43,  // a and b

	// Group 11: logical OR (left to right)
	LogicalOr = 44,  // a or b

	// Group 12: logical XOR (left to right)
	LogicalXor = 45,  // a xor b

	// Group 13: ternary operator (right to left)
	Conditional = 46,   // a ? b : c

	// Group 14: pair operator (right to left)
	Pair = 47,  // a : b

	// Group 15: tuple (left to right)
	Tuple = 48,  // a , b
	TupleDelimiter = 49,  // a ; b

	// Group 16: assignments (right to left)
	Assignment = 50,  // a = b
	MultiplicationAssignment = 51,  // a *= b
	MultiplicationOvAssignment = 52,  // a &*= b
	DivisionAssignment = 53,  // a /= b
	DivisionFloorAssignment = 54,  // a /-= b
	DivisionCeilingAssignment = 55,  // a /+= b
	ModulusAssignment = 56,  // a %= b
	ModulusFloorAssignment = 57,  // a %-= b
	ModulusCeilingAssignment = 58,  // a %+= b
	ExponentAssignment = 59,  // a **= b
	AdditionAssignment = 60,  // a += b
	AdditionOvAssignment = 61,  // a &+= b
	SubtractionAssignment = 62,  // a -= b
	SubtractionOvAssignment = 63,  // a &-= b
	LeftShiftAssignment = 64,  // a <<= b
	RightShiftAssignment = 65,  // a >>= b
	UnsignedRightShiftAssignment = 66,  // a >>>= b
	BitwiseAndAssignment = 67,  // a &= b
	BitwiseOrAssignment = 68,  // a |= b
	BitwiseXorAssignment = 69,  // a ^= b
};

constexpr size_t NumberOfOperators = 70;
constexpr Operator defaultOp = enum_max<Operator>::value;

namespace SubNode {
	constexpr size_t Import_Alias = 0;
	constexpr size_t Import_Package = 1;
	constexpr size_t Import_SubNodes = 2;

	constexpr size_t Def_Static = 0;
	constexpr size_t Def_Name = 1;
	constexpr size_t Def_Variation = 2;
	constexpr size_t Def_Parameter = 3;
	constexpr size_t Def_Operator = 4;
	constexpr size_t Def_OperatorParameter = 5;
	constexpr size_t Def_Delete = 6;
	constexpr size_t Def_Qualifiers = 7;
	constexpr size_t Def_SubNodes = 8;

	constexpr size_t Class_Static = 0;
	constexpr size_t Class_Name = 1;
	constexpr size_t Class_Extends = 2;
	constexpr size_t Class_BaseClassList = 3;
	constexpr size_t Class_SubNodes = 4;

	constexpr size_t Var_Static = 0;
	constexpr size_t Var_Definitions = 1;
	constexpr size_t Var_Qualifiers = 2;
	constexpr size_t Var_SubNodes = 3;

	// Unlike others, these values are not order of appearance; For_LoopVariable should be 0 for optimizing code.
	// (see Thread::processFor_CallNext())
	constexpr size_t For_LoopVariable = 0;
	constexpr size_t For_Label = 1;
	constexpr size_t For_Iterator = 2;
	constexpr size_t For_Iterable = 3;
	constexpr size_t For_SubNodes = 4;

	constexpr size_t While_Label = 0;
	constexpr size_t While_Condition = 1;
	constexpr size_t While_SubNodes = 2;

	constexpr size_t Catch_Label = 0;
	constexpr size_t Catch_ClassList = 1;
	constexpr size_t Catch_SubNodes = 2;

	constexpr size_t Container_Class = 0;
	constexpr size_t Container_Args = 1;
	constexpr size_t Container_Values = 2;
	constexpr size_t Container_SubNodes = 3;
}

struct Node final {
	NodeType type = NodeType::Unknown;

	// [!Parsed] reference to source line (first line if wrapped)
	std::shared_ptr<Line> line;

	// [type = statement] token list combined from wrapped line
	// [Parsed, type = !statement] token range corresponding to this Node
	std::vector<const Token*> tokens;

	// Flags used for improve runtime performance
	uint32_t flags = 0;

	// [Class, Def] name for entity (not includes variation name)
	// [DefOperator] "operator"
	// [For, While] label name if exists
	// [Name] name represented by tokens[0] unless script has an error.
	StringId sid = StringId::Invalid;

	// [Literal] value
	std::unique_ptr<Literal> literalValue;
	// [Operator] operator type
	Operator op = defaultOp;

	std::atomic<NodeState> blockNodesState = {NodeState::Grouped};
	std::vector<std::shared_ptr<Node>> blockNodes;

	std::vector<std::shared_ptr<Node>> subNodes;
	std::vector<std::shared_ptr<Node>> annotations;

	// Def, DefOperator, Class, Var (including under sync block)
	std::vector<std::shared_ptr<Node>> symbols;

	// Tokens generated for constructing syntax sugar
	std::vector<std::unique_ptr<Token>> additionalTokens;
};

namespace NodeFlags {
	constexpr uint32_t Native = 0x1;
	constexpr uint32_t ThreadSpecial = 0x2;
	constexpr uint32_t InitialNode = 0x4;
}

struct ParserWorkingSet final {
	unsigned generatedVarCount = 0;
};

std::shared_ptr<Node> groupScript(IErrorReceiver& errorReceiver,
		std::shared_ptr<StringTable>& sTable, const std::vector<std::shared_ptr<Line>> &lines);

void structureInnerNode(IErrorReceiver& errorReceiver,
		std::shared_ptr<StringTable>& sTable, Node* node, bool recursive);

// note: This function does not change node->blockNodesState
void parseInnerNode(ParserWorkingSet& ws, IErrorReceiver& errorReceiver,
		std::shared_ptr<StringTable>& sTable, Node* node, bool recursive);

void initializeParser();

void errorAtNode(IErrorReceiver& errorReceiver, ErrorLevel level, Node* node,
		const std::string& message, const std::vector<std::string>& args);

std::string getOpDescription(Operator op);

#ifndef NDEBUG
void dump(const std::shared_ptr<StringTable>& sTable, const std::shared_ptr<Node>& node);
#endif // !NDEBUG


}  // namespace chatra

CHATRA_ENUM_HASH(chatra::NodeType)
CHATRA_ENUM_HASH(chatra::Operator)

#endif //CHATRA_PARSER_H
