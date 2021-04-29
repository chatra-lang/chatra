/*
 * Programming language 'Chatra' reference implementation
 *
 * Copyright(C) 2019-2021 Chatra Project Team
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

#ifndef CHATRA_STRING_TABLE_H
#define CHATRA_STRING_TABLE_H

#include "Internal.h"
#include "Serialize.h"

namespace chatra {

enum class StringId : uint_least32_t {
	// Brackets
	OpenParenthesis,
	CloseParenthesis,
	OpenBracket,
	CloseBracket,
	OpenBrace,
	CloseBrace,

	// Markers
	Annotation,  // "@"

	// Keywords
	Import,
	Def,
	Static,
	Delete,
	As,
	Operator,
	Class,
	Extends,
	Sync,
	Touch,
	Var,
	If,
	Else,
	For,
	In,
	While,
	Switch,
	Case,
	Default,
	Do,
	Catch,
	Finally,
	Break,
	Continue,
	Return,
	Throw,
	Where,

	// Operators
	Colon,
	Comma,
	OpElementSelection,
	OpIncrement,
	OpDecrement,
	OpVarArg,
	OpBitwiseComplement,
	OpLogicalNot,  // "!"
	OpLogicalNotText,  // "not"
	OpAsync,
	OpPlus,
	OpMinus,
	OpMultiplication,
	OpMultiplicationOv,
	OpDivision,
	OpDivisionFloor,
	OpDivisionCeiling,
	OpModulus,
	OpModulusFloor,
	OpModulusCeiling,
	OpExponent,
	OpAdditionOv,
	OpSubtractionOv,
	OpLeftShift,
	OpRightShift,
	OpUnsignedRightShift,
	OpLessThan,
	OpGreaterThan,
	OpLessThanOrEqualTo,
	OpGreaterThanOrEqualTo,
	OpInstanceOf,
	OpEqualTo,
	OpNotEqualTo,
	OpBitwiseAnd,
	OpBitwiseOr,
	OpBitwiseXor,
	OpLogicalAnd,
	OpLogicalAndText,
	OpLogicalOr,
	OpLogicalOrText,
	OpLogicalXor,
	OpLogicalXorText,
	OpConditional,  // "?"
	OpTupleDelimiter,
	OpAssignment,
	OpMultiplicationAssignment,
	OpMultiplicationOvAssignment,
	OpDivisionAssignment,
	OpDivisionFloorAssignment,
	OpDivisionCeilingAssignment,
	OpModulusAssignment,
	OpModulusFloorAssignment,
	OpModulusCeilingAssignment,
	OpExponentAssignment,
	OpAdditionAssignment,
	OpAdditionOvAssignment,
	OpSubtractionAssignment,
	OpSubtractionOvAssignment,
	OpLeftShiftAssignment,
	OpRightShiftAssignment,
	OpRightShiftUnsignedAssignment,
	OpBitwiseAndAssignment,
	OpBitwiseOrAssignment,
	OpBitwiseXorAssignment,

	// Other keyword-like names
	Set,
	Self,
	Super,
	Init,
	Deinit,
	Native,

	Null,
	True,
	False,
	Bool,
	Int,
	Float,
	Iterator,
	KeyedIterator,
	IndexSet,
	RangeIterator,
	KeyedRangeIterator,
	Range,
	ArrayIterator,
	ArrayKeyedIterator,
	DictIterator,
	DictKeyedIterator,
	Sequence,
	VariableLengthSequence,
	ArrayView,
	Async,
	String,
	Array,
	Dict,
	_ReflectNodeShared,
	_ReflectNode,

	// Exceptions
	Exception,
	RuntimeException,
	ArithmeticException,

	ParserErrorException,
	UnsupportedOperationException,
	PackageNotFoundException,
	ClassNotFoundException,
	MemberNotFoundException,
	IncompleteExpressionException,
	TypeMismatchException,
	NullReferenceException,
	OverflowException,
	DivideByZeroException,
	IllegalArgumentException,
	NativeException,

	// Special IDs
	Empty,  // Empty but valid name
	Parser,  // Pseudo object for (global) parser lock
	FinalizerObjects,  // Array of objects which are waiting for calling deinit()
	FinalizerTemporary,  // Used for keeping reference to de-initializing object during copy
	PackageObject,
	PackageInitializer,
	CapturedScope,  // (MemoryManagement) link to capture object from Scope or CapturedScope
	Rvalue,  // (Runtime) used by TemporaryObject
	SourceObject,  // (Runtime) used by TemporaryObject
	Captured,  // (Runtime) link to CapturedScope
	AnyArgs,  // Wildcard for ArgumentMatcher::matches()
	AnyString,
	EmptyTuple,
	EmbeddedFunctions,

	Tuple,
	TemporaryTuple,
	TupleAssignmentMap,
	ClassObject,
	FunctionObject,
	ForIterator,

	a,  // Array
	d,  // Dict

	// Names used by embedded functions and scripts
	iterator,
	keyedIterator,
	hasNext,
	next,
	equals,
	has,
	toArray,
	toDict,

	a0, a1, a2, r, m0, m1, m2, m3, t0, t1, t2, t3,
	key,
	value,
	first,
	last,
	step,
	ascend,
	reverse,
	log,
	dump,
	gc,
	_native_time,
	time,
	clock,
	_native_sleep,
	sleep,
	timeout,
	relative,
	_native_wait,
	wait,
	type,
	objectId,
	_native_compile,
	_check,
	_checkCmd,
	_incrementTestTimer,

	finished,
	result,
	toString,
	fromString,
	fromChar,
	size,
	add,
	insert,
	remove,
	clear,
	clone,
	sub,
	append,
	keys,
	values,
	position,
	get,
	_native_updated,
	_native_add,
	_native_insert,
	_native_append,
	_native_at,
	_native_sub,
	_shared,
	_block,
	_sub,

	PredefinedStringIds,
	Invalid = std::numeric_limits<uint_least32_t>::max()
};

class StringTable {
private:
	unsigned baseVersion;
	unsigned version;
	std::unordered_map<std::string, StringId> strToId;
	std::vector<std::string> idToStr;

protected:
	StringTable() noexcept;
	StringTable(const StringTable& r) noexcept;

	void restore(Reader& r);

public:
	void save(Writer& w) const;

private:
	StringId add(std::string str);

public:
	unsigned getVersion() const {
		return version;
	}

	bool has(StringId id) const {
		return static_cast<size_t>(id) < idToStr.size();
	}

	std::string ref(StringId id) const {
		#ifndef CHATRA_NDEBUG
			if (id == StringId::Invalid)
				return "#invalid";
		#endif  // !CHATRA_NDEBUG
		chatra_assert(static_cast<size_t>(id) < idToStr.size());
		return idToStr[static_cast<size_t>(id)];
	}

	StringId find(const std::string& str) const {
		auto it = strToId.find(str);
		return it == strToId.end() ? StringId::Invalid : it->second;
	}

	std::shared_ptr<StringTable> copy() const;

	bool isDirty() const;

	void clearDirty();

	static std::shared_ptr<StringTable> newInstance();
	static std::shared_ptr<StringTable> newInstance(Reader& r);

	friend StringId add(std::shared_ptr<StringTable>& sTable, std::string str);

#ifndef CHATRA_NDEBUG
	size_t validIdCount() const {
		return idToStr.size();
	}
#endif  // !CHATRA_NDEBUG
};

StringId add(std::shared_ptr<StringTable>& sTable, std::string str);

} // namespace chatra

CHATRA_ENUM_HASH(chatra::StringId)

#endif //CHATRA_STRING_TABLE_H
