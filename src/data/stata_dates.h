// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — Stata daily-date arithmetic (%td), single-sourced.
//
// BUG-371. Stata's daily date is a signed DAY COUNT with 1960-01-01 == 0, so
// 1959-12-31 is -1. `mdy(M,D,Y)` builds one and year()/month()/day()/dow()/
// doy()/quarter()/halfyear() take one apart.
//
// This header exists because TWO expression layers must agree to the digit:
// cobra's `frame.cpp` parser (what `gen`/`replace`/`if` run) and PIET's
// `exprfns.cpp` pyeval registry (what `display`/`scalar` and the string-mutation
// evaluator run). A second copy of a calendar is a drift waiting to happen, and
// this campaign has already paid for that lesson once (is_ts_prefix, BUG-369).
//
// NOT merged with piet/native/include/piet/stata_format.h's civilFromDays: that
// one is the DISPLAY layer, with its own documented flooring rules for rendering
// a fractional or negative %td cell. Same calendar, different contract; they are
// cross-checked by test rather than by sharing code.
#pragma once
#include "core/ident.h"  // non-ASCII bytes are identifier bytes (Unicode names)

#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace cecon {

// 1970-01-01 expressed in Stata days (ten years, three of them leap).
inline constexpr int64_t kStataEpochShift = 3653;

// Proleptic-Gregorian civil <-> days-since-1970 (Howard Hinnant's algorithms).
inline int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                                    // [0, 399]
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   // [0, 365]
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            // [0, 146096]
    return era * 146097 + doe - 719468;
}

inline void civil_from_days(int64_t z, int64_t& y, int64_t& m, int64_t& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;                                       // [0, 146096]
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                // [0, 365]
    const int64_t mp = (5 * doy + 2) / 153;                                     // [0, 11]
    d = doy - (153 * mp + 2) / 5 + 1;                                           // [1, 31]
    m = mp + (mp < 10 ? 3 : -9);                                                // [1, 12]
    y = yoe + era * 400 + (m <= 2);
}

enum DatePart { DP_year, DP_month, DP_day, DP_dow, DP_doy, DP_quarter, DP_half };

// Stata's %td calendar is 01jan0100 .. 31dec9999 (fn.pdf: "Domain e_d: e_d dates
// 01jan0100 to 31dec9999 (integers -679,350 to 2,936,549)"). Captured, StataNow
// 19.5 (internal notes w2y_fn_datetime_probe): mdy(1,1,100) is
// -679350, mdy(12,31,99) is missing, year(-679351) and year(2936550) are missing.
inline constexpr int64_t kStataDayMin = -679350;
inline constexpr int64_t kStataDayMax = 2936549;

// mdy(M,D,Y) -> Stata daily date, or NaN (missing).
//
// The round-trip at the end is not belt-and-braces: days_from_civil happily
// NORMALISES a date that does not exist (Feb 30 becomes Mar 1), and returning
// that would be a wrong number rather than an error. Stata returns missing for
// an impossible date, so the only safe test is "does it come back as what went
// in".
//
// Each argument is TRUNCATED first, as Stata does (w2y_fn_datetime fd_mdy_edges2:
// mdy(1.5,1,2000) = mdy(1,1.9,2000) = mdy(1,1,2000.5) = 14610; probe p_mdy_d299:
// mdy(2,29.5,2000) = 14669), and the year must lie in 100..9999 (p_mdy_99,
// p_mdy_10000). This is the builder the mdy() FUNCTION uses; the literal and
// string-mask paths call it only with whole numbers.
inline double stata_mdy(double mm, double dd, double yy) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    if (std::isnan(mm) || std::isnan(dd) || std::isnan(yy)) return NA;
    if (!std::isfinite(mm) || !std::isfinite(dd) || !std::isfinite(yy)) return NA;
    mm = std::trunc(mm); dd = std::trunc(dd); yy = std::trunc(yy);
    if (mm < 1 || mm > 12 || dd < 1 || dd > 31) return NA;
    if (yy < 100 || yy > 9999) return NA;
    const int64_t m = static_cast<int64_t>(mm), d = static_cast<int64_t>(dd),
                  y = static_cast<int64_t>(yy);
    const int64_t z = days_from_civil(y, m, d);
    int64_t ry = 0, rm = 0, rd = 0;
    civil_from_days(z, ry, rm, rd);
    if (ry != y || rm != m || rd != d) return NA;  // the date did not exist
    return static_cast<double>(z + kStataEpochShift);
}

// The inverse family. A fractional %td value floors (Stata renders 42.9 as the
// same day as 42), and missing in means missing out. A floored day outside the
// calendar is missing (probe p_year_-679350.5, p_year_2936550, p_doy_2936550),
// except dow(), which Stata answers past the upper end (p_dow_2936550 = 6) but not
// below the lower one (p_dow_-679351).
inline double stata_date_part(double t, DatePart part) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    if (std::isnan(t)) return NA;
    const double ft = std::floor(t);
    if (!std::isfinite(ft) || std::fabs(ft) > 4.0e6) return NA;  // far outside any real calendar
    if (ft < static_cast<double>(kStataDayMin)) return NA;
    if (part != DP_dow && ft > static_cast<double>(kStataDayMax)) return NA;
    const int64_t td = static_cast<int64_t>(ft);
    const int64_t z = td - kStataEpochShift;
    int64_t y = 0, m = 0, d = 0;
    civil_from_days(z, y, m, d);
    switch (part) {
        case DP_year: return static_cast<double>(y);
        case DP_month: return static_cast<double>(m);
        case DP_day: return static_cast<double>(d);
        // Stata's dow() is 0=Sunday. Day 0 (1960-01-01) was a Friday, hence +5.
        case DP_dow: {
            int64_t w = (td + 5) % 7;
            if (w < 0) w += 7;
            return static_cast<double>(w);
        }
        case DP_doy: return static_cast<double>(z - days_from_civil(y, 1, 1) + 1);
        case DP_quarter: return static_cast<double>((m - 1) / 3 + 1);
        case DP_half: return static_cast<double>(m <= 6 ? 1 : 2);
    }
    return NA;
}


// ===================================================================
// AER scope 2, B3. The rest of Stata's date/time scales, single-sourced
// for the same reason the daily calendar above is: two evaluator layers
// must agree to the digit.
//
// Every constant below was MEASURED against StataNow 19.5 BE rather than
// recalled -- tests/validate/b3_funcs_oracle.{do,json} is the capture, and
// the five prose prescriptions it overturned are written up in
// internal note ONESHOT-AER-SCOPE2-PLAN-20260907.md section 7.7.
//
// The scales, with the oracle key that pins each:
//   %tm  months  since 1960m1  == 0   (ym.2020.3   -> 722)
//   %tq  quarters since 1960q1 == 0   (yq.2020.1   -> 240)
//   %th  halves  since 1960h1  == 0   (yh.2020.1   -> 120)
//   %tw  weeks   since 1960w1  == 0   (yw.2020.10  -> 3129)
//   %ty  the calendar year ITSELF, not an offset (yofd.epoch -> 1960)
//   %tc  milliseconds since 1960-01-01 00:00:00.000
//   %tC  the same, plus inserted leap seconds     (Cofd -> cofd + 27000 in 2020)
//
// Stata's week is not an ISO week: weeks 1..51 are seven days from 1 January
// and week 52 absorbs the remaining 8 or 9. wofd(31dec1959) == -1 is what pins
// that -- a naive doy/7 would answer 52 and put the day in the wrong year.

inline constexpr int64_t kMsPerDay = 86400000LL;

// Floored division/modulus. Ordinary C division truncates toward zero, which
// would put 1959 dates in 1960: dofm(-1) is 1959m12 (-31), not 1960m1.
inline int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
inline int64_t floor_mod(int64_t a, int64_t b) {
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

// A period argument is floored the way stata_date_part floors a %td, and any
// non-finite or absurd input is missing rather than a plausible wrong number.
inline bool period_arg(double v, int64_t& out, double limit = 4.0e8) {
    if (std::isnan(v)) return false;
    const double f = std::floor(v);
    if (!std::isfinite(f) || std::fabs(f) > limit) return false;
    out = static_cast<int64_t>(f);
    return true;
}

// ---- builders: (year, period) -> scale value ----------------------
// Out-of-range periods are MISSING, not wrapped: ym(2020,13) and ym(2020,0)
// both answer `.` (oracle ym.ovf / ym.zero).
//
// Two builders, because Stata has two. The ym()/yq()/yh()/yw() FUNCTIONS are
// linear in their arguments and take years 1000..9999 only (fn.pdf "Domain Y:
// integers 1000 to 9999"; captured: ym(2000,2.5) = 481.5, ym(2000.5,2) = 487,
// yq(2020.5,3) = 244, ym(1960,0.99) and ym(1960,12.5) missing, ym(100,1),
// ym(101,1) and ym(9999.5,1) missing, ym(9999,12) = 96479 -- w2y_fn_datetime
// fd_ym_edges/fd_yq_real and the probe's p_ym_* rows). The bound is checked on
// the arguments as given, so a fractional year or period is not floored first.
inline double stata_y_period(double yy, double pp, int64_t per_year) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    if (std::isnan(yy) || std::isnan(pp) || !std::isfinite(yy) || !std::isfinite(pp)) return NA;
    if (pp < 1.0 || pp > static_cast<double>(per_year)) return NA;
    if (yy < 1000.0 || yy > 9999.0) return NA;
    return (yy - 1960.0) * static_cast<double>(per_year) + (pp - 1.0);
}
inline double stata_ym(double y, double m) { return stata_y_period(y, m, 12); }
inline double stata_yq(double y, double q) { return stata_y_period(y, q, 4); }
inline double stata_yh(double y, double h) { return stata_y_period(y, h, 2); }
inline double stata_yw(double y, double w) { return stata_y_period(y, w, 52); }

// The literal tm()/tq()/th()/tw() pseudo-functions and the monthly()/quarterly()
// string translators build from whole numbers parsed out of text, and they are
// NOT held to the function's 1000 floor: tm(0100m1) is -22320 and tq(0100q1) is
// -7440 (probe p_lit_2, p_lit_4). This keeps the builder those paths always had.
inline double stata_y_period_exact(double yy, double pp, int64_t per_year) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    int64_t y = 0, p = 0;
    if (!period_arg(yy, y, 9999.0) || !period_arg(pp, p)) return NA;
    if (yy != std::floor(yy) || pp != std::floor(pp)) return NA;
    if (p < 1 || p > per_year) return NA;
    if (y < -9999 || y > 9999) return NA;
    return static_cast<double>((y - 1960) * per_year + (p - 1));
}

// ---- %td -> coarser scale ----------------------------------------
enum DateScale { DS_month, DS_quarter, DS_half, DS_year, DS_week };

// BUG-1374: wofd() of a floored day outside the calendar is not missing in
// StataBE 19.5 (mofd/qofd/hofd/yofd are). It answers a week of year 1 whose
// index is the day's distance from 2147483643, wrapped to a signed 32-bit
// value, floored by 7 and clamped at 51 like any week -- and missing once the
// day itself leaves the 32-bit range. No calendar gives these numbers; the
// model is fitted to the capture and reproduces every captured day (lane
// expr-sem x1b/a14-a16.do): wofd(-679351), wofd(-2000000) and wofd(-1e9)
// -101817, wofd(-2147483648) -101868, wofd(2936550..2936552) -306465739,
// wofd(2936553) -306465738, wofd(5000000) -306170960, wofd(1e9) -164028103,
// wofd(2e9) -21170960, wofd(2147483647) -101868, wofd(2147483648) and
// wofd(-1e10) missing.
inline double stata_week_outside_calendar(int64_t td) {
    if (td < INT32_MIN || td > INT32_MAX) return std::numeric_limits<double>::quiet_NaN();
    const int64_t off = floor_mod(td - 2147483643LL + 2147483648LL, 4294967296LL) - 2147483648LL;
    const int64_t w = floor_div(off, 7);
    return static_cast<double>((1 - 1960) * 52 + (w < 51 ? w : 51));
}

inline double stata_scale_of_day(double t, DateScale scale) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    int64_t td = 0;
    if (!period_arg(t, td, scale == DS_week ? 2147483648.0 : 4.0e6)) return NA;
    // mofd/qofd/hofd/yofd of a floored day outside the calendar are missing
    // (probe p_mofd_-679351, p_yofd_2936550, p_hofd_-1000000).
    if (td < kStataDayMin || td > kStataDayMax)
        return scale == DS_week ? stata_week_outside_calendar(td) : NA;
    int64_t y = 0, m = 0, d = 0;
    civil_from_days(td - kStataEpochShift, y, m, d);
    switch (scale) {
        case DS_month:   return static_cast<double>((y - 1960) * 12 + (m - 1));
        case DS_quarter: return static_cast<double>((y - 1960) * 4 + (m - 1) / 3);
        case DS_half:    return static_cast<double>((y - 1960) * 2 + (m <= 6 ? 0 : 1));
        case DS_year:    return static_cast<double>(y);  // %ty IS the year
        case DS_week: {
            const int64_t doy = td - kStataEpochShift - days_from_civil(y, 1, 1);  // 0-based
            const int64_t w = doy / 7 < 51 ? doy / 7 : 51;  // week 52 absorbs the tail
            return static_cast<double>((y - 1960) * 52 + w);
        }
    }
    return NA;
}

// week() is the 1..52 number inside the year, the same clamp seen from the
// other side: week(15mar2020) == 11.
inline double stata_week_of_day(double t) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    int64_t td = 0;
    if (!period_arg(t, td, 4.0e6)) return NA;
    if (td < kStataDayMin || td > kStataDayMax) return NA;  // p_week_-679351, p_week_2936550
    int64_t y = 0, m = 0, d = 0;
    civil_from_days(td - kStataEpochShift, y, m, d);
    const int64_t doy = td - kStataEpochShift - days_from_civil(y, 1, 1);
    return static_cast<double>((doy / 7 < 51 ? doy / 7 : 51) + 1);
}

// ---- coarser scale -> first %td of that period --------------------
inline double stata_day_of_scale(double v, DateScale scale) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    int64_t p = 0;
    if (!period_arg(v, p)) return NA;
    int64_t y = 0, mon = 1;
    switch (scale) {
        case DS_month:   y = 1960 + floor_div(p, 12); mon = floor_mod(p, 12) + 1; break;
        case DS_quarter: y = 1960 + floor_div(p, 4);  mon = floor_mod(p, 4) * 3 + 1; break;
        case DS_half:    y = 1960 + floor_div(p, 2);  mon = floor_mod(p, 2) * 6 + 1; break;
        case DS_year:    y = p;                       mon = 1; break;
        case DS_week: {
            const int64_t yy = 1960 + floor_div(p, 52);
            const double jan1 = stata_mdy(1, 1, static_cast<double>(yy));
            if (std::isnan(jan1)) return NA;
            return jan1 + 7.0 * static_cast<double>(floor_mod(p, 52));
        }
    }
    return stata_mdy(static_cast<double>(mon), 1, static_cast<double>(y));
}

// ---- leap seconds (%tC) ------------------------------------------
// MEASURED, not recalled: `Cofd(mdy(m,1,y)) - cofd(mdy(m,1,y))` was captured at
// every half-year boundary from 1960 to 2026 and the 27 steps below are the
// points where that difference moved. An insertion at the END of 1972-06-30 is
// in force from 1972-07-01, which is why the table is keyed by the first day the
// larger offset applies. [[measurement-reading-traps]]: a leap table written
// from memory is exactly the kind of constant that is wrong in one row and
// silently shifts one hour boundary by a second.
struct LeapStep { int16_t year; int8_t month; int8_t seconds; };
inline const LeapStep* leap_table(int& n) {
    static const LeapStep kSteps[] = {
        {1972, 7, 1},  {1973, 1, 2},  {1974, 1, 3},  {1975, 1, 4},  {1976, 1, 5},
        {1977, 1, 6},  {1978, 1, 7},  {1979, 1, 8},  {1980, 1, 9},  {1981, 7, 10},
        {1982, 7, 11}, {1983, 7, 12}, {1985, 7, 13}, {1988, 1, 14}, {1990, 1, 15},
        {1991, 1, 16}, {1992, 7, 17}, {1993, 7, 18}, {1994, 7, 19}, {1996, 1, 20},
        {1997, 7, 21}, {1999, 1, 22}, {2006, 1, 23}, {2009, 1, 24}, {2012, 7, 25},
        {2015, 7, 26}, {2017, 1, 27},
    };
    n = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));
    return kSteps;
}

// Cumulative inserted seconds in force on Stata day `td`.
inline int64_t leap_seconds_on_day(int64_t td) {
    int n = 0;
    const LeapStep* t = leap_table(n);
    int64_t acc = 0;
    for (int i = 0; i < n; ++i) {
        const int64_t start =
            days_from_civil(t[i].year, t[i].month, 1) + kStataEpochShift;
        if (td >= start) acc = t[i].seconds; else break;
    }
    return acc;
}

// ---- %td <-> %tc <-> %tC -----------------------------------------
// The %tc line covers the same calendar: 01jan0100 00:00:00.000 ..
// 31dec9999 23:59:59.999 (probe: dofc(-58695840000000) = -679350 and
// dofc(-58695840000001) missing; hh(253717919999999) = 23 and
// hh(253717920000000) missing).
inline constexpr double kStataClockMin = -58695840000000.0;
inline constexpr double kStataClockMax = 253717919999999.0;
inline bool stata_clock_in_range(double c) {
    return !std::isnan(c) && c >= kStataClockMin && c <= kStataClockMax;
}

// cofd() checks its argument against the calendar AS GIVEN, before flooring:
// cofd(-679350.5) and cofd(2936549.9) are missing (probe p_cofd_*).
inline double stata_cofd(double t) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    int64_t td = 0;
    if (!period_arg(t, td, 4.0e6)) return NA;
    if (t < static_cast<double>(kStataDayMin) || t > static_cast<double>(kStataDayMax)) return NA;
    return static_cast<double>(td) * static_cast<double>(kMsPerDay);
}
inline double stata_dofc(double c) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    if (!stata_clock_in_range(c)) return NA;
    return std::floor(c / static_cast<double>(kMsPerDay));
}
inline double stata_Cofd(double t) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    int64_t td = 0;
    if (!period_arg(t, td, 4.0e6)) return NA;
    return static_cast<double>(td) * static_cast<double>(kMsPerDay) +
           1000.0 * static_cast<double>(leap_seconds_on_day(td));
}
// The inverse needs one correction pass: the naive day is at most a second off,
// and a second can only carry across a day boundary once.
inline double stata_dofC(double C) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    if (std::isnan(C) || !std::isfinite(C)) return NA;
    double d = std::floor(C / static_cast<double>(kMsPerDay));
    for (int pass = 0; pass < 2; ++pass) {
        if (std::fabs(d) > 4.0e6) return NA;
        const int64_t lead = leap_seconds_on_day(static_cast<int64_t>(d));
        d = std::floor((C - 1000.0 * static_cast<double>(lead)) /
                       static_cast<double>(kMsPerDay));
    }
    return d;
}

// ---- time-of-day ---------------------------------------------------
enum ClockPart { CLK_hour, CLK_minute, CLK_second };

// `leap` selects the %tC family (hhC/mmC/ssC): the argument is first shifted
// back onto the %tc line by the leap seconds in force that day, then read the
// same way. A timestamp landing INSIDE an inserted second reads as :59 here,
// where Stata renders :60 -- documented rather than faked, since no corpus line
// asks for it.
//
// A %tc value outside the calendar is missing (probe p_hh_-58695840000001,
// p_mm_1e16), and ss() keeps the milliseconds: ss(253717919999999) is 59.999
// (fn.pdf "Range: 0.000 to 59.999").
inline double stata_clock_part(double c, ClockPart part, bool leap) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    if (std::isnan(c) || !std::isfinite(c)) return NA;
    if (!leap && !stata_clock_in_range(c)) return NA;
    double ms = c;
    if (leap) {
        const double d = stata_dofC(c);
        if (std::isnan(d)) return NA;
        ms = c - 1000.0 * static_cast<double>(leap_seconds_on_day(static_cast<int64_t>(d)));
    }
    const double day_ms = ms - std::floor(ms / static_cast<double>(kMsPerDay)) *
                                   static_cast<double>(kMsPerDay);
    const int64_t within = static_cast<int64_t>(std::floor(day_ms));
    switch (part) {
        case CLK_hour:   return static_cast<double>(within / 3600000);
        case CLK_minute: return static_cast<double>((within / 60000) % 60);
        case CLK_second: return static_cast<double>(within % 60000) / 1000.0;
    }
    return NA;
}

// ---- constructors --------------------------------------------------
// hms/dhms validate their components instead of normalising them: hms(25,0,0),
// hms(0,0,90) and hms(-1,0,0) are all missing in Stata. Seconds may be
// fractional (dhms(0,0,0,0.5) is 500 ms), so only the whole part is bounded.
inline double stata_hms_ms(double h, double m, double s) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    if (std::isnan(h) || std::isnan(m) || std::isnan(s)) return NA;
    if (h != std::floor(h) || m != std::floor(m)) return NA;
    if (h < 0 || h > 23 || m < 0 || m > 59) return NA;
    if (!(s >= 0.0) || !(s < 60.0)) return NA;
    return ((h * 60.0 + m) * 60.0 + s) * 1000.0;
}
inline double stata_hms(double h, double m, double s) { return stata_hms_ms(h, m, s); }
inline double stata_dhms(double d, double h, double m, double s) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    const double within = stata_hms_ms(h, m, s);
    if (std::isnan(within) || std::isnan(d) || !std::isfinite(d)) return NA;
    if (d != std::floor(d) || std::fabs(d) > 4.0e6) return NA;
    return d * static_cast<double>(kMsPerDay) + within;
}

// ---- millisecond unit conversions ----------------------------------
// Pure scalings, missing-propagating. Named the way Stata names them, which is
// why there is no days()/msofdays(): those do not exist (oracle: rc 133).
inline double ms_mul(double v, double factor) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    if (std::isnan(v) || !std::isfinite(v)) return NA;
    return v * factor;
}
// The other direction DIVIDES. Multiplying by a reciprocal is not the same
// number: 1.0/3600000.0 is inexact in binary, so hours(3600000) came back as
// 0.9999999999999999 until this was a division. Caught by the header's own
// check against the captured oracle before any of it was wired up.
inline double ms_div(double v, double divisor) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    if (std::isnan(v) || !std::isfinite(v)) return NA;
    return v / divisor;
}
inline double stata_msofseconds(double v) { return ms_mul(v, 1000.0); }
inline double stata_msofminutes(double v) { return ms_mul(v, 60000.0); }
inline double stata_msofhours(double v)   { return ms_mul(v, 3600000.0); }
inline double stata_seconds(double v)     { return ms_div(v, 1000.0); }
inline double stata_minutes(double v)     { return ms_div(v, 60000.0); }
inline double stata_hours(double v)       { return ms_div(v, 3600000.0); }


// ===================================================================
// AER scope 2, B3 stage (b). The LITERAL pseudo-functions.
//
// `td(15mar2020)`, `tq(2005q1)`, `tc(12:00)` and their single-letter synonyms
// do not take a value -- they take an unquoted date LITERAL, and Stata rejects a
// variable there: `gen x = td(t)` is r(198) "td(x) invalid" while `dofm(t)` is
// fine. That is why these cannot be a row in a function table: the argument has
// to be lifted before the expression parser ever sees it, in BOTH evaluators.
// The corpus leans on them 76 times (tq 35, td 22, tm 14, tc 4, q 8, d 3).
//
// Accepted spellings, all measured against StataNow 19.5 BE:
//   day      15mar2020 / 15Mar2020 / 15MAR2020 / 15 March 2020
//   clock    <day> <hh>:<mm>[:<ss>[.fff]]   or a bare  <hh>:<mm>[:<ss>[.fff]]
//   month    2020m3 / 2020M3      quarter 2020q1 / 2020Q1
//   week     2020w10              half    2020h1
//   year     2020
// Case is irrelevant for the scale letter and for month names.
enum class LiteralScale { Day, Clock, ClockLeap, Month, Quarter, Week, Half, Year };

inline int literal_month_name(const std::string& w) {
    static const char* kN[12] = {"jan","feb","mar","apr","may","jun",
                                 "jul","aug","sep","oct","nov","dec"};
    if (w.size() < 3) return 0;
    char k[3];
    for (int i = 0; i < 3; ++i) k[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(w[i])));
    for (int i = 0; i < 12; ++i)
        if (k[0]==kN[i][0] && k[1]==kN[i][1] && k[2]==kN[i][2]) return i + 1;
    return 0;
}

// Splits into digit runs and letter runs; every other byte is a separator, except
// ':' and '.' which are remembered so a time and a fraction can be reassembled.
struct LiteralTok { std::string text; bool numeric; char lead; };

inline std::vector<LiteralTok> literal_tokens(const std::string& s) {
    std::vector<LiteralTok> out;
    char lead = ' ';
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isdigit(c)) {
            size_t j = i;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
            out.push_back({s.substr(i, j - i), true, lead});
            lead = ' '; i = j;
        } else if (std::isalpha(c)) {
            size_t j = i;
            while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) ++j;
            out.push_back({s.substr(i, j - i), false, lead});
            lead = ' '; i = j;
        } else {
            if (c == ':' || c == '.') lead = static_cast<char>(c);
            ++i;
        }
    }
    return out;
}

inline double stata_datetime_literal(const std::string& text, LiteralScale scale) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    // Stata reads a literal with its blanks taken out, as it reads every bracketed
    // expression (core/expr_blanks.h): tc(01jan2010 12:00:00) is the text
    // 01jan201012:00:00. The year after a month name is at most four digits and
    // the rest of a longer run is the hour: tc(1jan20101:00) is 1jan2010 1:00,
    // tc(1jan10000 1:00) is 1jan1000 1:00, tc(1jan999 1:00) and td(1jan20101)
    // are invalid (StataBE 19.5).
    std::string squeezed;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c))) squeezed.push_back(c);
    std::vector<LiteralTok> t = literal_tokens(squeezed);
    if (t.size() >= 3 && t[0].numeric && !t[1].numeric && t[2].numeric &&
        t[2].text.size() > 4 && literal_month_name(t[1].text) > 0) {
        const LiteralTok hour{t[2].text.substr(4), true, ' '};
        t[2].text.resize(4);
        t.insert(t.begin() + 3, hour);
    }
    if (t.empty()) return NA;
    auto num = [](const std::string& d) {
        double v = 0; for (char c : d) v = v * 10 + (c - '0'); return v;
    };

    if (scale == LiteralScale::Year) {
        if (t.size() != 1 || !t[0].numeric) return NA;
        return num(t[0].text);
    }
    if (scale == LiteralScale::Month || scale == LiteralScale::Quarter ||
        scale == LiteralScale::Week || scale == LiteralScale::Half) {
        // <year><letter><period>, the letter optional if a separator was used.
        std::vector<double> nums;
        for (const LiteralTok& k : t) {
            if (k.numeric) nums.push_back(num(k.text));
            else if (k.text.size() != 1) return NA;   // a word here is not a scale letter
        }
        if (nums.size() != 2) return NA;
        switch (scale) {
            case LiteralScale::Month:   return stata_y_period_exact(nums[0], nums[1], 12);
            case LiteralScale::Quarter: return stata_y_period_exact(nums[0], nums[1], 4);
            case LiteralScale::Week:    return stata_y_period_exact(nums[0], nums[1], 52);
            default:                    return stata_y_period_exact(nums[0], nums[1], 2);
        }
    }

    // Day and clock share the date half.
    size_t i = 0;
    double day = 0.0;
    bool have_day = false;
    if (t.size() >= 3 && t[0].numeric && !t[1].numeric && t[2].numeric &&
        literal_month_name(t[1].text) > 0) {
        day = stata_mdy(literal_month_name(t[1].text), num(t[0].text), num(t[2].text));
        if (std::isnan(day)) return NA;
        have_day = true;
        i = 3;
    }
    if (scale == LiteralScale::Day) return have_day && i == t.size() ? day : NA;

    // Clock: hh:mm[:ss[.fff]] [am|pm]. A bare time means 1960-01-01, which is
    // what tc(12:00) == 43200000 pins. A trailing meridian is honoured the way
    // clock() honours it: tc(10:30am) = 37800000, tc(10:30 pm) = 81000000 (probe
    // p_lit_16, p_lit_17).
    double h = 0, m = 0, sec = 0;
    int meridian = 0;  // 0 none, 1 am, 2 pm
    if (!t.empty() && !t.back().numeric && i < t.size() - 1) {
        std::string w = t.back().text;
        for (char& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (w == "am" || w == "pm") meridian = (w == "pm") ? 2 : 1;
    }
    const size_t tend = t.size() - (meridian ? 1 : 0);
    if (i < tend) {
        if (!t[i].numeric) return NA;
        h = num(t[i].text); ++i;
        if (i >= tend || !t[i].numeric || t[i].lead != ':') return NA;
        m = num(t[i].text); ++i;
        if (i < tend && t[i].numeric && t[i].lead == ':') {
            sec = num(t[i].text); ++i;
            if (i < tend && t[i].numeric && t[i].lead == '.') {
                double f = num(t[i].text);
                for (size_t k = 0; k < t[i].text.size(); ++k) f /= 10.0;
                sec += f; ++i;
            }
        }
    }
    if (i != tend) return NA;
    if (meridian == 2 && h < 12) h += 12;
    if (meridian == 1 && h == 12) h = 0;
    const double within = stata_hms(h, m, sec);
    if (std::isnan(within)) return NA;
    const double base = (scale == LiteralScale::ClockLeap) ? stata_Cofd(day) : stata_cofd(day);
    if (std::isnan(base)) return NA;
    return base + within;
}

// The name table both parsers consult. Returns false if `name` is an ordinary
// function; a true answer means "lift the argument text, do not parse it".
inline bool literal_scale_for(const std::string& name, LiteralScale& out) {
    if (name == "td" || name == "d") { out = LiteralScale::Day; return true; }
    if (name == "tc")                { out = LiteralScale::Clock; return true; }
    if (name == "tC")                { out = LiteralScale::ClockLeap; return true; }
    if (name == "tm" || name == "m") { out = LiteralScale::Month; return true; }
    if (name == "tq" || name == "q") { out = LiteralScale::Quarter; return true; }
    if (name == "tw" || name == "w") { out = LiteralScale::Week; return true; }
    if (name == "th" || name == "h") { out = LiteralScale::Half; return true; }
    // NOT "ty": measured rc 133, it does not exist in Stata. Accepting it here
    // would be us answering a name Stata refuses.
    if (name == "y")                 { out = LiteralScale::Year; return true; }
    return false;
}

// Every literal pseudo-function call in an expression's TEXT, for the PIET
// surfaces that must see one before an expression parser does (the macro/arith
// whitelist refuses the ':' of tc(10:30); generate must refuse tq(2010q5) with
// Stata's r(198) before the engine reports it as an internal error). Matches
// what both lexers lift: a name from literal_scale_for, not part of a longer
// identifier or a `.member`, then optional blanks and a balanced (...).
// Double-quoted strings are skipped. visit(name, raw, begin, end) gets the
// argument text and the byte span [begin, end) of the whole call; returning
// false stops the scan.
template <typename Visit>
inline void for_each_datetime_literal(const std::string& s, Visit visit) {
    auto word = [](char c) {
        return cecon::ident::isIdentByte(c);
    };
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') {
            const size_t close = s.find('"', i + 1);
            if (close == std::string::npos) return;
            i = close + 1;
            continue;
        }
        if (!cecon::ident::isIdentStartByte(c) ||
            (i > 0 && (word(s[i - 1]) || s[i - 1] == '.'))) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < s.size() && word(s[j])) ++j;
        const std::string name = s.substr(i, j - i);
        LiteralScale ls;
        size_t k = j;
        while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
        if (!literal_scale_for(name, ls) || k >= s.size() || s[k] != '(') {
            i = j;
            continue;
        }
        size_t q = k + 1;
        int depth = 1;
        while (q < s.size() && depth) {
            if (s[q] == '(') ++depth;
            else if (s[q] == ')') --depth;
            if (depth) ++q;
        }
        if (depth) return;  // unbalanced: the expression parser reports it
        if (!visit(name, s.substr(k + 1, q - k - 1), i, q + 1)) return;
        i = q + 1;
    }
}

}  // namespace cecon
