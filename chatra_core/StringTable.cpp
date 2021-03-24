/*
 * Programming language 'Chatra' reference implementation
 *
 * Copyright(C) 2019,2021 Chatra Project Team
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

#include "StringTable.h"

namespace chatra {

StringTable::StringTable() noexcept {
	baseVersion = version = 0;
	strToId = {
			{"(", StringId::OpenParenthesis},
			{")", StringId::CloseParenthesis},
			{"[", StringId::OpenBracket},
			{"]", StringId::CloseBracket},
			{"{", StringId::OpenBrace},
			{"}", StringId::CloseBrace},

			{"@", StringId::Annotation},

			{"import", StringId::Import},
			{"def", StringId::Def},
			{"static", StringId::Static},
			{"delete", StringId::Delete},
			{"as", StringId::As},
			{"operator", StringId::Operator},
			{"class", StringId::Class},
			{"extends", StringId::Extends},
			{"sync", StringId::Sync},
			{"touch", StringId::Touch},
			{"var", StringId::Var},
			{"if", StringId::If},
			{"else", StringId::Else},
			{"for", StringId::For},
			{"in", StringId::In},
			{"while", StringId::While},
			{"switch", StringId::Switch},
			{"case", StringId::Case},
			{"default", StringId::Default},
			{"do", StringId::Do},
			{"catch", StringId::Catch},
			{"finally", StringId::Finally},
			{"break", StringId::Break},
			{"continue", StringId::Continue},
			{"return", StringId::Return},
			{"throw", StringId::Throw},
			{"where", StringId::Where},

			{":", StringId::Colon},
			{",", StringId::Comma},
			{".", StringId::OpElementSelection},
			{"++", StringId::OpIncrement},
			{"--", StringId::OpDecrement},
			{"...", StringId::OpVarArg},
			{"~", StringId::OpBitwiseComplement},
			{"!", StringId::OpLogicalNot},
			{"not", StringId::OpLogicalNotText},
			{"async", StringId::OpAsync},
			{"+", StringId::OpPlus},
			{"-", StringId::OpMinus},
			{"*", StringId::OpMultiplication},
			{"&*", StringId::OpMultiplicationOv},
			{"/", StringId::OpDivision},
			{"/-", StringId::OpDivisionFloor},
			{"/+", StringId::OpDivisionCeiling},
			{"%", StringId::OpModulus},
			{"%-", StringId::OpModulusFloor},
			{"%+", StringId::OpModulusCeiling},
			{"**", StringId::OpExponent},
			{"&+", StringId::OpAdditionOv},
			{"&-", StringId::OpSubtractionOv},
			{"<<", StringId::OpLeftShift},
			{">>", StringId::OpRightShift},
			{">>>", StringId::OpUnsignedRightShift},
			{"<", StringId::OpLessThan},
			{">", StringId::OpGreaterThan},
			{"<=", StringId::OpLessThanOrEqualTo},
			{">=", StringId::OpGreaterThanOrEqualTo},
			{"is", StringId::OpInstanceOf},
			{"==", StringId::OpEqualTo},
			{"!=", StringId::OpNotEqualTo},
			{"&", StringId::OpBitwiseAnd},
			{"|", StringId::OpBitwiseOr},
			{"^", StringId::OpBitwiseXor},
			{"&&", StringId::OpLogicalAnd},
			{"and", StringId::OpLogicalAndText},
			{"||", StringId::OpLogicalOr},
			{"or", StringId::OpLogicalOrText},
			{"^^", StringId::OpLogicalXor},
			{"xor", StringId::OpLogicalXorText},
			{"?", StringId::OpConditional},
			{";", StringId::OpTupleDelimiter},
			{"=", StringId::OpAssignment},
			{"*=", StringId::OpMultiplicationAssignment},
			{"&*=", StringId::OpMultiplicationOvAssignment},
			{"/=", StringId::OpDivisionAssignment},
			{"/-=", StringId::OpDivisionFloorAssignment},
			{"/+=", StringId::OpDivisionCeilingAssignment},
			{"%=", StringId::OpModulusAssignment},
			{"%-=", StringId::OpModulusFloorAssignment},
			{"%+=", StringId::OpModulusCeilingAssignment},
			{"**=", StringId::OpExponentAssignment},
			{"+=", StringId::OpAdditionAssignment},
			{"&+=", StringId::OpAdditionOvAssignment},
			{"-=", StringId::OpSubtractionAssignment},
			{"&-=", StringId::OpSubtractionOvAssignment},
			{"<<=", StringId::OpLeftShiftAssignment},
			{">>=", StringId::OpRightShiftAssignment},
			{">>>=", StringId::OpRightShiftUnsignedAssignment},
			{"&=", StringId::OpBitwiseAndAssignment},
			{"|=", StringId::OpBitwiseOrAssignment},
			{"^=", StringId::OpBitwiseXorAssignment},

			{"set", StringId::Set},
			{"self", StringId::Self},
			{"super", StringId::Super},
			{"init", StringId::Init},
			{"deinit", StringId::Deinit},
			{"native", StringId::Native},

			{"null", StringId::Null},
			{"true", StringId::True},
			{"false", StringId::False},
			{"Bool", StringId::Bool},
			{"Int", StringId::Int},
			{"Float", StringId::Float},
			{"Iterator", StringId::Iterator},
			{"KeyedIterator", StringId::KeyedIterator},
			{"IndexSet", StringId::IndexSet},
			{"RangeIterator", StringId::RangeIterator},
			{"KeyedRangeIterator", StringId::KeyedRangeIterator},
			{"Range", StringId::Range},
			{"ArrayIterator", StringId::ArrayIterator},
			{"ArrayKeyedIterator", StringId::ArrayKeyedIterator},
			{"DictIterator", StringId::DictIterator},
			{"DictKeyedIterator", StringId::DictKeyedIterator},
			{"Sequence", StringId::Sequence},
			{"VariableLengthSequence", StringId::VariableLengthSequence},
			{"ArrayView", StringId::ArrayView},
			{"Async", StringId::Async},
			{"String", StringId::String},
			{"Array", StringId::Array},
			{"Dict", StringId::Dict},
			{"_ReflectNodeShared", StringId::_ReflectNodeShared},
			{"_ReflectNode", StringId::_ReflectNode},

			{"Exception", StringId::Exception},
			{"RuntimeException", StringId::RuntimeException},
			{"ArithmeticException", StringId::ArithmeticException},

			{"ParserErrorException", StringId::ParserErrorException},
			{"UnsupportedOperationException", StringId::UnsupportedOperationException},
			{"PackageNotFoundException", StringId::PackageNotFoundException},
			{"ClassNotFoundException", StringId::ClassNotFoundException},
			{"MemberNotFoundException", StringId::MemberNotFoundException},
			{"IncompleteExpressionException", StringId::IncompleteExpressionException},
			{"TypeMismatchException", StringId::TypeMismatchException},
			{"NullReferenceException", StringId::NullReferenceException},
			{"OverflowException", StringId::OverflowException},
			{"DivideByZeroException", StringId::DivideByZeroException},
			{"IllegalArgumentException", StringId::IllegalArgumentException},
			{"NativeException", StringId::NativeException},

			{"#empty", StringId::Empty},
			{"#parser", StringId::Parser},
			{"#finalizerObjects", StringId::FinalizerObjects},
			{"#finalizerTemporary", StringId::FinalizerTemporary},
			{"#packageObject", StringId::PackageObject},
			{"#packageInitializer", StringId::PackageInitializer},
			{"#capturedScope", StringId::CapturedScope},
			{"#rvalue", StringId::Rvalue},
			{"#sourceObject", StringId::SourceObject},
			{"#captured", StringId::Captured},
			{"#anyArgs", StringId::AnyArgs},
			{"#anyString", StringId::AnyString},
			{"#emptyTuple", StringId::EmptyTuple},
			{"#embeddedFunctions", StringId::EmbeddedFunctions},

			{"#tuple", StringId::Tuple},
			{"#temporaryTuple", StringId::TemporaryTuple},
			{"#tupleAssignmentMap", StringId::TupleAssignmentMap},
			{"#functionObject", StringId::FunctionObject},
			{"#forIterator", StringId::ForIterator},

			{"a", StringId::a},
			{"d", StringId::d},

			{"iterator", StringId::iterator},
			{"keyedIterator", StringId::keyedIterator},
			{"hasNext", StringId::hasNext},
			{"next", StringId::next},
			{"equals", StringId::equals},
			{"has", StringId::has},
			{"toArray", StringId::toArray},
			{"toDict", StringId::toDict},

			{"a0", StringId::a0},
			{"a1", StringId::a1},
			{"a2", StringId::a2},
			{"r", StringId::r},
			{"m0", StringId::m0},
			{"m1", StringId::m1},
			{"m2", StringId::m2},
			{"m3", StringId::m3},
			{"t0", StringId::t0},
			{"t1", StringId::t1},
			{"t2", StringId::t2},
			{"t3", StringId::t3},
			{"key", StringId::key},
			{"value", StringId::value},
			{"first", StringId::first},
			{"last", StringId::last},
			{"step", StringId::step},
			{"ascend", StringId::ascend},
			{"reverse", StringId::reverse},
			{"log", StringId::log},
			{"dump", StringId::dump},
			{"gc", StringId::gc},
			{"_native_time", StringId::_native_time},
			{"time", StringId::time},
			{"clock", StringId::clock},
			{"_native_sleep", StringId::_native_sleep},
			{"sleep", StringId::sleep},
			{"timeout", StringId::timeout},
			{"relative", StringId::relative},
			{"_native_wait", StringId::_native_wait},
			{"wait", StringId::wait},
			{"type", StringId::type},
			{"objectId", StringId::objectId},
			{"_native_compile", StringId::_native_compile},
			{"_check", StringId::_check},
			{"_checkCmd", StringId::_checkCmd},
			{"_incrementTestTimer", StringId::_incrementTestTimer},

			{"finished", StringId::finished},
			{"result", StringId::result},
			{"toString", StringId::toString},
			{"fromString", StringId::fromString},
			{"fromChar", StringId::fromChar},
			{"size", StringId::size},
			{"add", StringId::add},
			{"insert", StringId::insert},
			{"remove", StringId::remove},
			{"clear", StringId::clear},
			{"clone", StringId::clone},
			{"sub", StringId::sub},
			{"append", StringId::append},
			{"keys", StringId::keys},
			{"values", StringId::values},
			{"position", StringId::position},
			{"get", StringId::get},
			{"_native_updated", StringId::_native_updated},
			{"_native_add", StringId::_native_add},
			{"_native_insert", StringId::_native_insert},
			{"_native_append", StringId::_native_append},
			{"_native_at", StringId::_native_at},
			{"_native_sub", StringId::_native_sub},
			{"_shared", StringId::_shared},
			{"_block", StringId::_block},
			{"_sub", StringId::_sub}
	};
	idToStr.resize(static_cast<size_t>(StringId::PredefinedStringIds));
	for (auto& e : strToId)
		idToStr[static_cast<size_t>(e.second)] = e.first;
}

StringTable::StringTable(const StringTable& r) noexcept {
	baseVersion = r.baseVersion;
	version = r.version;
	strToId = r.strToId;
	idToStr = r.idToStr;
}

void StringTable::restore(Reader& r) {
	chatra_assert(idToStr.size() == static_cast<size_t>(StringId::PredefinedStringIds));
	r.in(baseVersion);
	r.in(version);
	r.inList([&]() { add(r.read<std::string>()); });
}

void StringTable::save(Writer& w) const {
	w.out(baseVersion);
	w.out(version);
	w.out(idToStr.cbegin() + static_cast<size_t>(StringId::PredefinedStringIds), idToStr.cend(),
			[&](const std::string& value) { w.out(value); });
}

StringId StringTable::add(std::string str) {
	auto id = static_cast<StringId>(idToStr.size());
	strToId.emplace(str, id);
	idToStr.push_back(std::move(str));
	return id;
}

struct StringTableBridge final : StringTable {
	explicit StringTableBridge() noexcept : StringTable() {}
	explicit StringTableBridge(const StringTable& r) noexcept : StringTable(r) {}
};

std::shared_ptr<StringTable> StringTable::copy() const {
	return std::make_shared<StringTableBridge>(*this);
}

bool StringTable::isDirty() const {
	return baseVersion != version;
}

void StringTable::clearDirty() {
	baseVersion = version;
}

std::shared_ptr<StringTable> StringTable::newInstance() {
	return std::make_shared<StringTableBridge>();
}

std::shared_ptr<StringTable> StringTable::newInstance(Reader& r) {
	auto ret = std::make_shared<StringTableBridge>();
	ret->restore(r);
	return std::static_pointer_cast<StringTable>(ret);
}

StringId add(std::shared_ptr<StringTable>& sTable, std::string str) {
	auto it = sTable->strToId.find(str);
	if (it != sTable->strToId.end())
		return it->second;

	if (sTable->version == sTable->baseVersion) {
		sTable = std::make_shared<StringTableBridge>(*sTable);
		sTable->version++;
	}

	return sTable->add(std::move(str));
}

} // namespace chatra
