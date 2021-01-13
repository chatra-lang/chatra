/*
 * Programming language 'Chatra' reference implementation
 *
 * Copyright(C) 2021 Chatra Project Team
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

#ifndef CHATRA_CHATRA_UTILS_H
#define CHATRA_CHATRA_UTILS_H

#include "chatra.h"
#include <cassert>
#include <type_traits>

#ifdef CHATRA_NDEBUG
	#define chatra_assert(e)  ((void)0)
#else
	#define chatra_assert(e)  assert(e)
#endif

#if defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER)
	#define CHATRA_MAYBE_GCC   __GNUC__
#endif

#ifndef CHATRA_WHEN
	#define CHATRA_WHEN(...)  typename std::enable_if<__VA_ARGS__, std::nullptr_t>::type = nullptr
#endif
#define CHATRA_HAS_TYPE(...)  std::is_same<__VA_ARGS__, __VA_ARGS__>::value
#define CHATRA_TYPE_EXISTS(...)  CHATRA_WHEN(std::is_same<__VA_ARGS__, __VA_ARGS__>::value)

#if defined(__clang__)
	#define CHATRA_FALLTHROUGH  [[clang::fallthrough]]
	#define CHATRA_FALLTHROUGH_DEFINED
#endif

#if defined(CHATRA_MAYBE_GCC)
	#if __GNUC__ >= 7
		#define CHATRA_FALLTHROUGH  [[gnu::fallthrough]]
		#define CHATRA_FALLTHROUGH_DEFINED
	#endif
#endif

#ifndef CHATRA_FALLTHROUGH_DEFINED
	#define CHATRA_FALLTHROUGH
#endif

#endif //CHATRA_CHATRA_UTILS_H
