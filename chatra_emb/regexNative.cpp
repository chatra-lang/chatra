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
using namespace chatraEmb;

// note: <regex> in C++11 only supports char or wchar_t;
// This specification does not have sufficient capabilities to support unicode string.
// To avoid troubles with using <regex> such as pre/post conversions or surrogate pairs,
// we use external library "SRELL".
//
// SRELL
// http://www.akenotsuki.com/misc/srell/

#if defined(__GNUC__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
	#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include "regexNative_srell.h"

#if defined(__GNUC__)
	#pragma GCC diagnostic pop
#endif


namespace chatraEmbRegex {

static const char* script =
#include "regex.cha"
;

enum class Type {
	Pattern, Match
};

enum class SearchType {
	Search, Match
};

struct NativeData : public cha::INativePtr {
	Type type;
	explicit NativeData(Type type) : type(type) {}
};

struct Pattern : public NativeData {
	SpinLock lock;
	std::string pattern;
	srell::regex_constants::syntax_option_type flags = srell::regex_constants::ECMAScript;
	std::unique_ptr<srell::u8cregex> re;
	Pattern() : NativeData(Type::Pattern) {}
};

struct Match : public NativeData {
	SpinLock lock;
	std::string pattern;
	srell::regex_constants::syntax_option_type flags = srell::regex_constants::ECMAScript;
	std::string str;
	SearchType searchType = SearchType::Search;
	srell::u8csmatch m;
	srell::regex_constants::match_flag_type mFlags = srell::regex_constants::match_default;
	bool matched = false;
	Match() : NativeData(Type::Match) {}
};

struct RegexPackageInterface : public cha::IPackage {
	std::vector<uint8_t> saveNativePtr(cha::PackageContext& pct, cha::INativePtr* ptr) override {
		(void)pct;
		std::vector<uint8_t> buffer;

		auto* data = static_cast<NativeData*>(ptr);
		writeInt(buffer, static_cast<uint64_t>(data->type));

		switch (data->type) {
		case Type::Pattern: {
			auto* p = static_cast<Pattern*>(ptr);
			writeString(buffer, p->pattern);
			writeInt(buffer, static_cast<uint64_t>(p->flags));
			writeInt(buffer, p->re ? 1 : 0);
			break;
		}

		case Type::Match: {
			auto* m = static_cast<Match*>(ptr);
			writeString(buffer, m->pattern);
			writeInt(buffer, static_cast<uint64_t>(m->flags));
			writeString(buffer, m->str);
			writeInt(buffer, static_cast<uint64_t>(m->searchType));
			writeInt(buffer, static_cast<uint64_t>(m->mFlags));
			writeInt(buffer, m->matched ? 1 : 0);
			break;
		}
		}
		return buffer;
	}

	cha::INativePtr* restoreNativePtr(cha::PackageContext& pct, const std::vector<uint8_t>& stream) override {
		(void)pct;
		size_t offset = 0;

		auto type = readInt<Type>(stream, offset);
		switch (type) {
		case Type::Pattern: {
			auto* p = new Pattern();
			p->pattern = readString(stream, offset);
			p->flags = readInt<srell::regex_constants::syntax_option_type>(stream, offset);
			if (readInt<int>(stream, offset))
				p->re.reset(new srell::u8cregex(p->pattern, p->flags));
			return p;
		}

		case Type::Match: {
			auto* m = new Match();
			m->pattern = readString(stream, offset);
			m->flags = readInt<srell::regex_constants::syntax_option_type>(stream, offset);
			m->str = readString(stream, offset);
			m->searchType = readInt<SearchType>(stream, offset);
			m->mFlags = readInt<srell::regex_constants::match_flag_type>(stream, offset);
			m->matched = (readInt<int>(stream, offset) != 0);
			if (m->matched) {
				srell::u8cregex re(m->pattern, m->flags);
				switch (m->searchType) {
				case SearchType::Search:
					srell::regex_search(m->str, m->m, re, m->mFlags);
					break;
				case SearchType::Match:
					srell::regex_match(m->str, m->m, re, m->mFlags);
					break;
				}
			}
			return m;
		}

		default:
			throw cha::NativeException();
		}
	}
};

static void processException(srell::regex_error& ex) {
	const char* message = "internal error";
	switch (ex.code()) {
	case srell::regex_constants::error_escape:
		message = "The expression contained an invalid escaped character, or a trailing escape";  break;
	case srell::regex_constants::error_backref:
		message = "The expression contained an invalid back reference";  break;
	case srell::regex_constants::error_brack:
		message = "The expression contained mismatched brackets";  break;
	case srell::regex_constants::error_paren:
		message = "The expression contained mismatched parentheses";  break;
	case srell::regex_constants::error_brace:
		message = "The expression contained mismatched braces";  break;
	case srell::regex_constants::error_badbrace:
		message = "The expression contained an invalid range between braces";  break;
	case srell::regex_constants::error_range:
		message = "The expression contained an invalid character range";  break;
	case srell::regex_constants::error_badrepeat:
		message = "The expression contained a repeat specifier that was not preceded by a valid regular expression";  break;
	case srell::regex_constants::error_complexity:
		message = "The complexity of an attempted match against a regular expression exceeded a pre-set level";  break;
	case srell::regex_constants::error_stack:
		message = "There was insufficient memory to determine whether the regular expression could match the specified character sequence";  break;
	}
	throw cha::IllegalArgumentException(message);
}

static void compile(cha::Ct& ct) {
	auto* p = new Pattern();
	ct.at(0).setNative(p);

	p->pattern = ct.at(1).get<std::string>();

	auto flags = ct.at(2).get<unsigned>();
	if (flags & 1U)
		p->flags |= srell::regex_constants::multiline;
	if (flags & 2U)
		p->flags |= srell::regex_constants::icase;
	if (flags & 4U)
		p->flags |= srell::regex_constants::dotall;

	try {
		p->re.reset(new srell::u8cregex(p->pattern, p->flags));
	}
	catch (srell::regex_error& ex) {
		processException(ex);
	}
}

static Pattern* derefSelfAsPattern(cha::Ct& ct) {
	auto* p = ct.self<Pattern>();
	if (p == nullptr || !p->re)
		throw cha::IllegalArgumentException("invalid Pattern object");
	return p;
}

static void parseCommonMatchFlags(srell::regex_constants::match_flag_type& mFlags, unsigned flags) {
	if (flags & 8U)
		mFlags |= srell::regex_constants::match_not_bol;
	if (flags & 0x10U)
		mFlags |= srell::regex_constants::match_not_eol;
	if (flags & 0x20U)
		mFlags |= srell::regex_constants::match_not_bow;
	if (flags & 0x40U)
		mFlags |= srell::regex_constants::match_not_eow;
	if (flags & 0x80U)
		mFlags |= srell::regex_constants::match_not_null;
	if (flags & 0x100U)
		mFlags |= srell::regex_constants::match_continuous;
}

template <typename Pred>
static void searchOrMatch(cha::Ct& ct, SearchType searchType, Pred pred) {
	auto* p = derefSelfAsPattern(ct);
	std::lock_guard<SpinLock> lock(p->lock);

	auto* m = new Match();
	ct.at(0).setNative(m);

	m->pattern = p->pattern;
	m->flags = p->flags;
	m->str = ct.at(1).get<std::string>();

	auto flags = ct.at(2).get<unsigned>();
	parseCommonMatchFlags(m->mFlags, flags);

	m->searchType = searchType;
	try {
		m->matched = pred(p, m);
		ct.set(m->matched);
	}
	catch (srell::regex_error& ex) {
		processException(ex);
	}
}

static void search(cha::Ct& ct) {
	searchOrMatch(ct, SearchType::Search, [](Pattern* p, Match* m) {
		return srell::regex_search(m->str, m->m, *p->re, m->mFlags);
	});
}

static void match(cha::Ct& ct) {
	searchOrMatch(ct, SearchType::Match, [](Pattern* p, Match* m) {
		return srell::regex_match(m->str, m->m, *p->re, m->mFlags);
	});
}

static void replace(cha::Ct& ct) {
	auto* p = derefSelfAsPattern(ct);
	std::lock_guard<SpinLock> lock(p->lock);

	auto str = ct.at(0).get<std::string>();
	auto format = ct.at(1).get<std::string>();

	auto flags = ct.at(2).get<unsigned>();
	srell::regex_constants::match_flag_type mFlags = srell::regex_constants::format_first_only;
	parseCommonMatchFlags(mFlags, flags);
	if (flags & 0x200U)
		mFlags &= ~srell::regex_constants::format_first_only;
	if (flags & 0x400U)
		mFlags |= srell::regex_constants::format_no_copy;

	try {
		auto replaced =  srell::regex_replace(str, *p->re, format, mFlags);
		ct.set(replaced);
	}
	catch (srell::regex_error& ex) {
		processException(ex);
	}
}

static Match* derefSelfAsMatch(cha::Ct& ct) {
	auto* m = ct.self<Match>();
	if (m == nullptr)
		throw cha::IllegalArgumentException("invalid Match object");
	return m;
}

static size_t getPositionOrThrow(cha::Ct& ct, Match* m) {
	auto position = ct.at(0).get<size_t>();
	if (position >= m->m.size())
		throw cha::IllegalArgumentException("position is out of range");
	return position;
}

static void patternSize(cha::Ct& ct) {
	auto* m = derefSelfAsMatch(ct);
	std::lock_guard<SpinLock> lock(m->lock);

	ct.set(!m->matched ? 0 : m->m.size());
}

static size_t countChar(const std::string& str, size_t offset) {
	size_t count = 0;
	for (size_t i = 0; i < offset; i++) {
		if ((static_cast<unsigned>(str[i]) & 0x80U) == 0
				|| (static_cast<unsigned>(str[i]) & 0xC0U) == 0xC0U)
			count++;
	}
	return count;
}

static void patternPosition(cha::Ct& ct) {
	auto* m = derefSelfAsMatch(ct);
	std::lock_guard<SpinLock> lock(m->lock);

	auto position = getPositionOrThrow(ct, m);
	ct.set(countChar(m->str, m->m.position(position)));
}

static void patternLength(cha::Ct& ct) {
	auto* m = derefSelfAsMatch(ct);
	std::lock_guard<SpinLock> lock(m->lock);

	auto position = getPositionOrThrow(ct, m);
	auto str = m->m.str(position);
	ct.set(countChar(str, str.length()));
}

static void patternStr(cha::Ct& ct) {
	auto* m = derefSelfAsMatch(ct);
	std::lock_guard<SpinLock> lock(m->lock);

	auto position = getPositionOrThrow(ct, m);
	ct.set(m->m.str(position));
}

cha::PackageInfo packageInfo() {
	return {{{"regex", script}}, {
			{compile, "_native_compile"},
			{search, "Pattern", "_native_search"},
			{match, "Pattern", "_native_match"},
			{replace, "Pattern", "_native_replace"},
			{patternSize, "Match", "_native_size"},
			{patternPosition, "Match", "_native_position"},
			{patternLength, "Match", "_native_length"},
			{patternStr, "Match", "_native_str"},
	}, std::make_shared<RegexPackageInterface>()};
}

} // namespace chatraEmbRegex
