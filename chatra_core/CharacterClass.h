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

#ifndef CHATRA_CHARACTER_CLASS_H
#define CHATRA_CHARACTER_CLASS_H

namespace chatra {

static constexpr bool isSpace(char c) {
	return c == ' ' || c == '\t';
}

static constexpr bool isNotSpace(char c) {
	return !isSpace(c);
}

static constexpr bool isBeginningOfNumber(char c) {
	return '0' <= c && c <= '9';
}

static constexpr bool isPartOfNumber(char c) {
	return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F') || c == 'x' || c == '_'
			|| c == '.' || c == '+' || c == '-';  // +/- are used for exponent (e.g. 1e-5 = 0.00001)
}

static constexpr bool isBeginningOfOperator(char c) {
	return c == '!' || c == '%' || c == '^' || c == '&' || c == '*' || c == '-' || c == '+' || c == '=' || c == '|'
			|| c == '~' || c == ':' || c == ';' || c == ',' || c == '.' || c == '<' || c == '>' || c == '?' || c == '/';
}

static constexpr bool isPartOfOperator(char c) {
	return c == '!' || c == '%' || c == '^' || c == '&' || c == '*' || c == '-' || c == '+' || c == '=' || c == '|'
			|| c == '~' || c == '.' || c == '<' || c == '>' || c == '?' || c == '/';
}

static constexpr bool isSpecialCharacters(char c) {
	return isBeginningOfOperator(c) || c == '@' || c == '#' || c == '$' || c == '\\' || c == '`' || c == '\'' || c == '"'
			|| c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}';
}

static constexpr bool isBeginningOfName(char c) {
	return !isSpace(c) && !isBeginningOfNumber(c) && !isSpecialCharacters(c);
}

static constexpr bool isPartOfName(char c) {
	return !isSpace(c) && !isSpecialCharacters(c);
}

}  // namespace chatra

#endif //CHATRA_CHARACTER_CLASS_H
