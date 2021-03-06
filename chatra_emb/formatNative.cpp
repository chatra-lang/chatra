/*
 * Programming language 'Chatra' reference implementation
 *
 * Copyright(C) 2020 Chatra Project Team
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

#include "EmbInternal.h"
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cmath>

namespace chatra {
namespace emb {
namespace format {

static const char* script =
#include "format.cha"
;

// Format specifier:
// %{<|>}[+][0][<width>][.<precision>]<type>
// or
// %{[<key>:]{<|>}[+][0][<width>][.<precision>]{""|<type>}[;<options,...>]}
//
// type := s|d|x|X|e|E|f|F|g|G|a|A
// options :=
//   m=<decimal marker (default = '.')>
//   i=<digit group separators for integer part (default = ''; typical = '%c')>
//   f=<digit group separators for fractional part (default = '', typical = ' ')>
//   %c in value field means ','

template <char targetCharacter>
static constexpr bool isChar(char c) {
	return c == targetCharacter;
}

static constexpr bool isDigit(char c) {
	return '0' <= c && c <= '9';
}

static constexpr bool isNotDigit(char c) {
	return !isDigit(c);
}

static constexpr bool isHexDigit(char c) {
	return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F')
			|| c == 'x' || c == 'X';
}

static constexpr bool isAlpha(char c) {
	return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z');
}

template <typename Pred>
static size_t findOffset(const std::string& str, size_t first, size_t last, Pred pred) {
	return static_cast<size_t>(std::distance(str.cbegin(), std::find_if(str.cbegin() + first, str.cbegin() + last, pred)));
}

static std::string restoreValue(const std::string& value) {
	std::string out;
	for (size_t i = 0; i < value.length(); i++) {
		if (value[i] == '%' && i + 1 < value.length()) {
			if (value[i + 1] == 'c') {
				out.push_back(',');
				i++;
				continue;
			}
			else if (value[i + 1] == '%') {
				out.push_back('%');
				i++;
				continue;
			}
		}
		out.push_back(value[i]);
	}
	return out;
}

#if defined(__clang__)
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
#if defined(CHATRA_MAYBE_GCC)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif

template <class Type>
static std::string convert(size_t specifierIndex, char* spec, size_t specSize, const char* type, Type value) {
	std::strcpy(spec + specSize, type);
	auto length = std::snprintf(nullptr, 0, spec, value);
	if (length < 0)
		throw IllegalArgumentException("failed to apply format specifier #%u",
				static_cast<unsigned>(specifierIndex));
	std::vector<char> buffer(static_cast<size_t>(length) + 1);
	std::snprintf(buffer.data(), buffer.size(), spec, value);
	return std::string(buffer.data());
}

#if defined(__clang__)
	#pragma clang diagnostic pop
#endif
#if defined(CHATRA_MAYBE_GCC)
	#pragma GCC diagnostic pop
#endif

static std::string toNegative(const std::string& valueString) {
	auto it = std::find_if(valueString.cbegin(), valueString.cend(), [](char c) {
		return isDigit(c) || c == '+';
	});
	chatra_assert(it != valueString.cend());

	auto offset = static_cast<size_t>(std::distance(valueString.cbegin(), it));
	auto ret = valueString;

	if (*it == '+') {
		ret[offset] = '-';
		return ret;
	}

	if (offset != 0) {
		ret[offset - 1] = '-';
		return ret;
	}

	if (*it == '0' && valueString.length() != 1) {
		ret[0] = '-';
		return ret;
	}

	return '-' + valueString;
}

template <typename IsDigitPred>
static void postProcess(std::string& valueString,
		const std::string& dMarker, const std::string& dsInt, const std::string& dsFrac,
		IsDigitPred isDigitPred) {

	auto marker = findOffset(valueString, 0, valueString.length(), isChar<'.'>);

	if (!dsInt.empty()) {
		auto _dsInt = restoreValue(dsInt);
		auto iInt0 = findOffset(valueString, 0, valueString.length(), isDigitPred);
		auto iInt1 = findOffset(valueString, iInt0, valueString.length(), [&](char c) { return !isDigitPred(c); });
		for (size_t j = 3; j < iInt1 - iInt0; j += 3) {
			valueString.insert(iInt1 - j, _dsInt);
			marker += _dsInt.length();
			for (size_t k = 0; k < _dsInt.length() && valueString[0] == ' '; k++) {
				valueString.erase(0, 1);
				marker--;
			}
		}
	}

	if (marker != valueString.length()) {
		if (!dsFrac.empty()) {
			auto _dsFrac = restoreValue(dsFrac);
			auto iFrac = findOffset(valueString, marker + 1, valueString.length(),
					[&](char c) { return !isDigitPred(c); });
			for (size_t j = (iFrac - (marker + 1)) / 3 * 3; j >= 3; j -= 3)
				valueString.insert(marker + 1 + j, _dsFrac);
		}

		if (!dMarker.empty()) {
			auto _dMarker = restoreValue(dMarker);
			valueString.replace(marker, 1, _dMarker);
		}
	}
}

static void format(Ct& ct) {
	auto f = ct.at(0).get<std::string>();
	auto& indexes = ct.at(1);
	auto& values = ct.at(2);
	auto arrayArgs = ct.at(3).get<size_t>();

	std::unordered_map<std::string, size_t> keyedIndexes;
	if (indexes.size() != 0 && indexes.at(indexes.size() - 1).isString()) {
		for (size_t  i = 0; i < indexes.size(); i++) {
			auto& index = indexes.at(i);
			if (index.isString())
				keyedIndexes.emplace(index.get<std::string>(), i);
		}
	}

	std::string out;
	out.reserve(f.length());

	char spec[28 + 1];  // %{+|-|0}<width(<4G)><.precision(<4G)><length(<=2)><type>
	spec[0] = '%';
	std::string dMarker, dsInt, dsFrac;

	size_t specifierIndex = 0;
	size_t arrayValueIndex = 0;
	for (size_t i = 0; i < f.length(); i++) {
		if (f[i] != '%') {
			out += f[i];
			continue;
		}
		if (i + 1 < f.length() && f[i + 1] == '%') {
			out += '%';
			i++;
			continue;
		}

		specifierIndex++;
		size_t valueIndex = std::numeric_limits<size_t>::max();

		// Find boundary of format specifier
		if (i + 1 >= f.length())
			throw IllegalArgumentException("unterminated format specifier at end of text");

		size_t i0, i1;
		if (f[i + 1] == '{') {
			i0 = i + 2;
			i1 = findOffset(f, i0, f.length(), isChar<'}'>);
			if (i1 >= f.length())
				throw IllegalArgumentException(
						"unterminated format specifier #%u (started from offset %+u)",
						static_cast<unsigned>(specifierIndex), static_cast<unsigned>(i));
			i = i1;
		}
		else {
			i0 = i + 1;
			i1 = findOffset(f, i0, f.length(), isAlpha);
			if (i1 >= f.length())
				throw IllegalArgumentException(
						"unterminated format specifier #%u (started from offset %+u)",
						static_cast<unsigned>(specifierIndex), static_cast<unsigned>(i));
			i = i1++;
		}

		// Format string
		size_t specSize = 1;
		char type = 's';

		dMarker = ".";
		dsInt.clear();
		dsFrac.clear();
		bool requiresPostProcess = false;

		size_t iSpec = i0;
		auto t0 = findOffset(f, iSpec, i1, isChar<':'>);
		if (t0 != i1) {
			auto firstChar = f[iSpec];
			if (isDigit(firstChar) || firstChar == '-') {
				errno = 0;
				auto index = std::strtol(f.data() + iSpec, nullptr, 10);
				if (errno == ERANGE)
					throw IllegalArgumentException(
							"at format specifier #%u: index is out of range",
							static_cast<unsigned>(specifierIndex));
				valueIndex = static_cast<size_t>(index >= 0 ? index : arrayArgs + index);
				if (valueIndex >= arrayArgs)
					throw IllegalArgumentException(
							"at format specifier #%u: specified index (%ld) is out of range",
							static_cast<unsigned>(specifierIndex), index);
			}
			else {
				auto key = f.substr(iSpec, t0 - iSpec);
				auto it = keyedIndexes.find(key);
				if (it == keyedIndexes.cend())
					throw IllegalArgumentException(
							"at format specifier #%u: specified key (%s) is not present on arguments list",
							static_cast<unsigned>(specifierIndex), key.data());
				valueIndex = it->second;
			}
			iSpec = t0 + 1;
		}

		if (valueIndex == std::numeric_limits<size_t>::max()) {
			valueIndex = arrayValueIndex++;
			if (valueIndex >= arrayArgs)
				throw IllegalArgumentException(
						"at format specifier #%u: specified index (%ld) is out of range",
						static_cast<unsigned>(specifierIndex), valueIndex);
		}

		if (f[iSpec] == '<') {
			spec[specSize++] = '-';
			iSpec++;
		}

		if (f[iSpec] == '>')
			iSpec++;

		if (f[iSpec] == '+') {
			spec[specSize++] = '+';
			iSpec++;
		}

		if (f[iSpec] == '0') {
			spec[specSize++] = '0';
			iSpec++;
		}

		if (isDigit(f[iSpec])) {
			auto length = findOffset(f, iSpec, i1, isNotDigit) - iSpec;
			if (length > 10)
				throw IllegalArgumentException(
						"at format specifier #%u: width field is out of range",
						static_cast<unsigned>(specifierIndex));
			std::strncpy(spec + specSize, f.data() + iSpec, length);
			specSize += length;
			iSpec += length;
		}

		bool hasPrecision = false;
		if (f[iSpec] == '.' && iSpec + 1 < i1 && isDigit(f[iSpec + 1])) {
			auto length = findOffset(f, iSpec + 1, i1, isNotDigit) - iSpec;
			if (length > 1 + 10)
				throw IllegalArgumentException(
						"at format specifier #%u: precision field is out of range",
						static_cast<unsigned>(specifierIndex));
			std::strncpy(spec + specSize, f.data() + iSpec, length);
			specSize += length;
			iSpec += length;
			hasPrecision = true;
		}

		if (isAlpha(f[iSpec]))
			type = f[iSpec++];

		if (f[iSpec] == ';') {
			iSpec++;
			while (iSpec < i1) {
				auto last = findOffset(f, iSpec, i1, isChar<','>);
				auto sep = findOffset(f, iSpec + 1, last, isChar<'='>);
				if (last == sep)
					throw IllegalArgumentException(
							"at format specifier #%u: option field requires 'key=value' form",
							static_cast<unsigned>(specifierIndex));

				if (sep - iSpec == 1 && !std::strncmp(f.data() + iSpec, "m", 1)) {
					dMarker.assign(f.data() + sep + 1, f.data() + last);
					requiresPostProcess = true;
				}
				else if (sep - iSpec == 1 && !std::strncmp(f.data() + iSpec, "i", 1)) {
					dsInt.assign(f.data() + sep + 1, f.data() + last);
					requiresPostProcess = true;
				}
				else if (sep - iSpec == 1 && !std::strncmp(f.data() + iSpec, "f", 1)) {
					dsFrac.assign(f.data() + sep + 1, f.data() + last);
					requiresPostProcess = true;
				}
				else
					throw IllegalArgumentException(
							"at format specifier #%u: unknown option (%s)",
							static_cast<unsigned>(specifierIndex), f.substr(iSpec, sep - iSpec).data());

				iSpec = last + 1;
			}
		}

		auto& value = values.at(valueIndex);
		std::string valueString;

		if (type == 's') {
			if (value.isInt())
				type = 'd';
			else if (value.isFloat())
				type = 'g';
		}

		bool typeMismatch = false;
		switch (type) {
		case 's':
			if (value.isNull())
				valueString = convert(specifierIndex, spec, specSize, "s", "null");
			else if (value.isBool())
				valueString = convert(specifierIndex, spec, specSize, "s", value.getBool() ? "true" : "false");
			else if (value.isString())
				valueString = convert(specifierIndex, spec, specSize, "s", value.getString().data());
			else
				typeMismatch = true;
			requiresPostProcess = false;
			break;

		case 'd':
			if (value.isNull())
				valueString = convert(specifierIndex, spec, specSize, "d", 0);
			else if (value.isInt())
				valueString = convert(specifierIndex, spec, specSize, "lld", static_cast<long long>(value.getInt()));
			else if (value.isFloat())
				valueString = convert(specifierIndex, spec, specSize, "lld", static_cast<long long>(value.getFloat()));
			else if (value.isBool())
				valueString = convert(specifierIndex, spec, specSize, "d", value.getBool() ? 1 : 0);
			else
				typeMismatch = true;
			break;

		case 'x':
		case 'X': {
			char typeString[4] = {'l', 'l', type, '\0'};
			if (value.isNull())
				valueString = convert(specifierIndex, spec, specSize, "d", 0);
			else if (value.isInt()) {
				if (value.getInt() >= 0)
					valueString = convert(specifierIndex, spec, specSize, typeString, static_cast<long long>(value.getInt()));
				else {
					valueString = convert(specifierIndex, spec, specSize, typeString, static_cast<long long>(-value.getInt()));
					valueString = toNegative(valueString);
				}
			}
			else if (value.isFloat()) {
				if (!std::signbit(value.getFloat()))
					valueString = convert(specifierIndex, spec, specSize, typeString, static_cast<long long>(value.getFloat()));
				else {
					valueString = convert(specifierIndex, spec, specSize, typeString, static_cast<long long>(-value.getFloat()));
					valueString = toNegative(valueString);
				}
			}
			else if (value.isBool())
				valueString = convert(specifierIndex, spec, specSize, "d", value.getBool() ? 1 : 0);
			else
				typeMismatch = true;
			break;
		}

		case 'a':
		case 'A':
			if (!hasPrecision) {
				std::strcpy(spec + specSize, ".6");
				specSize += 2;
			}
			// CHATRA_FALLTHROUGH

		case 'e':
		case 'E':
		case 'f':
		case 'F':
		case 'g':
		case 'G': {
			char typeString[2] = {type, '\0'};
			if (value.isNull())
				valueString = convert(specifierIndex, spec, specSize, typeString, 0.0);
			else if (value.isInt())
				valueString = convert(specifierIndex, spec, specSize, typeString, static_cast<double>(value.getInt()));
			else if (value.isFloat())
				valueString = convert(specifierIndex, spec, specSize, typeString, static_cast<double>(value.getFloat()));
			else if (value.isBool())
				valueString = convert(specifierIndex, spec, specSize, typeString, value.getBool() ? 1.0 : 0.0);
			else
				typeMismatch = true;
			break;
		}

		default:
			throw IllegalArgumentException(
					"at format specifier #%u: unknown type name (%c)",
					static_cast<unsigned>(specifierIndex), type);
		}

		if (typeMismatch) {
			throw IllegalArgumentException(
					"at format specifier #%u: type '%c' cannot be used with specified value",
					static_cast<unsigned>(specifierIndex), type);
		}

		// Post-processing (adding/replacing marker or separators)
		if (requiresPostProcess) {
			switch (type) {
			case 'x':
			case 'X':
			case 'a':
			case 'A':
				postProcess(valueString, dMarker, dsInt, dsFrac, isHexDigit);
				break;
			default:
				postProcess(valueString, dMarker, dsInt, dsFrac, isDigit);
				break;
			}
		}

		out += valueString;
	}

	ct.setString(out);
}

PackageInfo packageInfo() {
	std::vector<Script> scripts = {{"format", script}};
	std::vector<HandlerInfo> handlers = {
			{format, "_native_format"}
	};
	return {scripts, handlers, nullptr};
}

}  // namespace format
}  // namespace emb
}  // namespace chatra
