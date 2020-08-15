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

#include "chatra.h"
#include <cmath>

namespace chatra {
namespace emb {
namespace math {

static const char* script =
#include "math.cha"
;

static double getDouble(Ct& ct, size_t index = 0) {
	if (ct[index].isInt())
		return static_cast<double>(ct[index].getInt());
	if (ct[index].isFloat())
		return ct[index].getFloat();
	throw IllegalArgumentException("passing argument #%zu has non-numeric value", index);
}

static int getInt(Ct& ct, size_t index = 0) {
	if (ct[index].isInt())
		return static_cast<int>(ct[index].getInt());
	throw IllegalArgumentException("passing argument #%zu is not integer", index);
}

static void math_sin(Ct& ct) { ct.setFloat(std::sin(getDouble(ct))); }
static void math_cos(Ct& ct) { ct.setFloat(std::cos(getDouble(ct))); }
static void math_tan(Ct& ct) { ct.setFloat(std::tan(getDouble(ct))); }
static void math_asin(Ct& ct) { ct.setFloat(std::asin(getDouble(ct))); }
static void math_acos(Ct& ct) { ct.setFloat(std::acos(getDouble(ct))); }
static void math_atan(Ct& ct) { ct.setFloat(std::atan(getDouble(ct))); }

static void math_atan2(Ct& ct) {
	ct.setFloat(std::atan2(getDouble(ct, 0), getDouble(ct, 1)));
}

static void math_sinh(Ct& ct) { ct.setFloat(std::sinh(getDouble(ct))); }
static void math_cosh(Ct& ct) { ct.setFloat(std::cosh(getDouble(ct))); }
static void math_tanh(Ct& ct) { ct.setFloat(std::tanh(getDouble(ct))); }
static void math_asinh(Ct& ct) { ct.setFloat(std::asinh(getDouble(ct))); }
static void math_acosh(Ct& ct) { ct.setFloat(std::acosh(getDouble(ct))); }
static void math_atanh(Ct& ct) { ct.setFloat(std::atanh(getDouble(ct))); }

static void math_exp(Ct& ct) { ct.setFloat(std::exp(getDouble(ct))); }
static void math_exp2(Ct& ct) { ct.setFloat(std::exp2(getDouble(ct))); }
static void math_expm1(Ct& ct) { ct.setFloat(std::expm1(getDouble(ct))); }
static void math_logn(Ct& ct) { ct.setFloat(std::log(getDouble(ct))); }
static void math_log10(Ct& ct) { ct.setFloat(std::log10(getDouble(ct))); }
static void math_log1p(Ct& ct) { ct.setFloat(std::log1p(getDouble(ct))); }
static void math_log2(Ct& ct) { ct.setFloat(std::log2(getDouble(ct))); }

static void math_ldexp(Ct& ct) {
	ct.setFloat(std::ldexp(getDouble(ct, 0), getInt(ct, 1)));
}

static void math_frexp_fraction(Ct& ct) {
	int vExp;
	ct.setFloat(std::frexp(getDouble(ct), &vExp));
}

static void math_frexp_exponent(Ct& ct) {
	int vExp;
	std::frexp(getDouble(ct), &vExp);
	ct.setInt(vExp);
}

static void math_ilogb(Ct& ct) { ct.setInt(std::ilogb(getDouble(ct))); }
static void math_logb(Ct& ct) { ct.setFloat(std::logb(getDouble(ct))); }

static void math_modf_integral(Ct& ct) {
	double integral;
	std::modf(getDouble(ct), &integral);
	ct.setFloat(integral);
}

static void math_modf_fractional(Ct& ct) {
	double integral;
	ct.setFloat(std::modf(getDouble(ct), &integral));
}

static void math_pow(Ct& ct) {
	ct.setFloat(std::pow(getDouble(ct, 0), getDouble(ct, 1)));
}

static void math_sqrt(Ct& ct) { ct.setFloat(std::sqrt(getDouble(ct))); }
static void math_cbrt(Ct& ct) { ct.setFloat(std::cbrt(getDouble(ct))); }

static void math_hypot(Ct& ct) {
	ct.setFloat(std::hypot(getDouble(ct, 0), getDouble(ct, 1)));
}

static void math_abs(Ct& ct) {
	if (ct[0].isInt())
		ct.setInt(std::abs(ct[0].getInt()));
	else if (ct[0].isFloat())
		ct.setFloat(std::abs(ct[0].getFloat()));
	else
		throw IllegalArgumentException("passing argument #%zu has non-numeric value", index);
}

static void math_erf(Ct& ct) { ct.setFloat(std::erf(getDouble(ct))); }
static void math_erfc(Ct& ct) { ct.setFloat(std::erfc(getDouble(ct))); }
static void math_tgamma(Ct& ct) { ct.setFloat(std::tgamma(getDouble(ct))); }
static void math_lgamma(Ct& ct) { ct.setFloat(std::lgamma(getDouble(ct))); }

static void math_ceil(Ct& ct) { ct.setFloat(std::ceil(getDouble(ct))); }
static void math_floor(Ct& ct) { ct.setFloat(std::floor(getDouble(ct))); }
static void math_trunc(Ct& ct) { ct.setFloat(std::trunc(getDouble(ct))); }
static void math_round(Ct& ct) { ct.setFloat(std::round(getDouble(ct))); }

static void math_mod(Ct& ct) {
	ct.setFloat(std::fmod(getDouble(ct, 0), getDouble(ct, 1)));
}

static void math_remainder(Ct& ct) {
	ct.setFloat(std::remainder(getDouble(ct, 0), getDouble(ct, 1)));
}

static void math_nextafter(Ct& ct) {
	ct.setFloat(std::nextafter(getDouble(ct, 0), getDouble(ct, 1)));
}

static void math_isfinite(Ct& ct) { ct.setBool(std::isfinite(getDouble(ct))); }
static void math_isinf(Ct& ct) { ct.setBool(std::isinf(getDouble(ct))); }
static void math_isnan(Ct& ct) { ct.setBool(std::isnan(getDouble(ct))); }
static void math_isnormal(Ct& ct) { ct.setBool(std::isnormal(getDouble(ct))); }
static void math_signbit(Ct& ct) { ct.setBool(std::signbit(getDouble(ct))); }


PackageInfo packageInfo() {
	std::vector<Script> scripts = {{"math", script}};
	std::vector<HandlerInfo> handlers = {
			{math_sin, "sin"},
			{math_cos, "cos"},
			{math_tan, "tan"},
			{math_asin, "asin"},
			{math_acos, "acos"},
			{math_atan, "atan"},
			{math_atan2, "atan2"},

			{math_sinh, "sinh"},
			{math_cosh, "cosh"},
			{math_tanh, "tanh"},
			{math_asinh, "asinh"},
			{math_acosh, "acosh"},
			{math_atanh, "atanh"},

			{math_exp, "exp"},
			{math_exp2, "exp2"},
			{math_expm1, "expm1"},
			{math_logn, "logn"},
			{math_log10, "log10"},
			{math_log1p, "log1p"},
			{math_log2, "log2"},

			{math_ldexp, "ldexp"},
			{math_frexp_fraction, "_native_frexp_fraction"},
			{math_frexp_exponent, "_native_frexp_exponent"},
			{math_ilogb, "ilogb"},
			{math_logb, "logb"},
			{math_modf_integral, "_native_modf_integral"},
			{math_modf_fractional, "_native_modf_fractional"},

			{math_pow, "pow"},
			{math_sqrt, "sqrt"},
			{math_cbrt, "cbrt"},
			{math_hypot, "hypot"},
			{math_abs, "abs"},

			{math_erf, "erf"},
			{math_erfc, "erfc"},
			{math_tgamma, "tgamma"},
			{math_lgamma, "lgamma"},

			{math_ceil, "ceil"},
			{math_floor, "floor"},
			{math_trunc, "trunc"},
			{math_round, "round"},

			{math_mod, "mod"},
			{math_remainder, "remainder"},
			{math_nextafter, "nextafter"},

			{math_isfinite, "isfinite"},
			{math_isinf, "isinf"},
			{math_isnan, "isnan"},
			{math_isnormal, "isnormal"},
			{math_signbit, "signbit"},
	};
	return {scripts, handlers, nullptr};
}

}  // namespace math
}  // namespace emb
}  // namespace chatra
