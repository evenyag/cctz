// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   https://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.

#ifndef CCTZ_CIVIL_TIME_DETAIL_H_
#define CCTZ_CIVIL_TIME_DETAIL_H_

#include <cstdint>
#include <limits>
#include <ostream>
#include <type_traits>

// Disable constexpr support unless we are in C++14 mode.
#if __cpp_constexpr >= 201304 || (defined(_MSC_VER) && _MSC_VER >= 1910)
#define CONSTEXPR_D constexpr  // data
#define CONSTEXPR_F constexpr  // function
#define CONSTEXPR_M constexpr  // member
#else
#define CONSTEXPR_D const
#define CONSTEXPR_F inline
#define CONSTEXPR_M
#endif

namespace cctz {

// Support years that at least span the range of 64-bit time_t values.
using year_t = std::int_fast64_t;

// Type alias that indicates an argument is not normalized (e.g., the
// constructor parameters and operands/results of addition/subtraction).
using diff_t = std::int_fast64_t;

namespace detail {

// Type aliases that indicate normalized argument values.
using month_t = std::int_fast8_t;   // [1:12]
using day_t = std::int_fast8_t;     // [1:31]
using hour_t = std::int_fast8_t;    // [0:23]
using minute_t = std::int_fast8_t;  // [0:59]
using second_t = std::int_fast8_t;  // [0:59]

// Normalized civil-time fields: Y-M-D HH:MM:SS.
struct fields {
  CONSTEXPR_M fields(year_t year, month_t month, day_t day,
                     hour_t hour, minute_t minute, second_t second)
      : y(year), m(month), d(day), hh(hour), mm(minute), ss(second) {}
  std::int_least64_t y;
  std::int_least8_t m;
  std::int_least8_t d;
  std::int_least8_t hh;
  std::int_least8_t mm;
  std::int_least8_t ss;
};

struct second_tag {};
struct minute_tag : second_tag {};
struct hour_tag : minute_tag {};
struct day_tag : hour_tag {};
struct month_tag : day_tag {};
struct year_tag : month_tag {};

////////////////////////////////////////////////////////////////////////

// Field normalization (without avoidable overflow).

namespace impl {

// 是否闰年
CONSTEXPR_F bool is_leap_year(year_t y) noexcept {
  return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}
CONSTEXPR_F int year_index(year_t y, month_t m) noexcept {
  // 如果超过 2 月则 y 需要 + 1 ，这个处理逻辑和 days_per_year() 一致，这里还不太理解为啥 ?
  // 可能主要是一种打表的逻辑，方便后面计算从当年当月到现在过去了多少天，因此就跟计算 days_per_year 一样，考虑 2 月
  // y 计算模 400 后余下多少，得到 yi
  const int yi = static_cast<int>((y + (m > 2)) % 400);
  // yi 保证是正数，如果小于 0 会加上 400 ，因为是下标，负数直接加 400 得到正数的下标即可
  return yi < 0 ? yi + 400 : yi;
}
// 这里最早是一个数组，直接根据 yi 作为下标取数组里的值，里面的值只会是 0 或者 1
// 当时注释如下
// > The number of days in the 100 years starting in the mod-400 index year,
// > stored as a 36524-deficit value (i.e., 0 == 36524, 1 == 36525).
// 这里改为用 const fn 实现了
CONSTEXPR_F int days_per_century(int yi) noexcept {
  return 36524 + (yi == 0 || yi > 300);
}
// 同理是计算往后走 4 年过去了多少天
// > The number of days in the 4 years starting in the mod-400 index year,
// > stored as a 1460-deficit value (i.e., 0 == 1460, 1 == 1461).
// 这里也是用 const fn 实现了
CONSTEXPR_F int days_per_4years(int yi) noexcept {
  return 1460 + (yi == 0 || yi > 300 || (yi - 1) % 100 < 96);
}
// 计算从 y 这年 m 月到下一年这个月过了多少天，需要考虑月份
CONSTEXPR_F int days_per_year(year_t y, month_t m) noexcept {
  // 如果 2 月以后，则 m > 2 是 1 ， y + 1 即今年是闰年的话，就过了 366 天
  return is_leap_year(y + (m > 2)) ? 366 : 365;
}
CONSTEXPR_F int days_per_month(year_t y, month_t m) noexcept {
  CONSTEXPR_D int k_days_per_month[1 + 12] = {
      -1, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31  // non leap year
  };
  return k_days_per_month[m] + (m == 2 && is_leap_year(y));
}

// cd 是对 d 的增量，到这里除了 d 部分，其他部分都标准化了
// 需要注意底下用的的常量
// - 每 400 年就正好有 146097 天 (Gregorian cycle of 400 years has exactly 146,097 days)
CONSTEXPR_F fields n_day(year_t y, month_t m, diff_t d, diff_t cd,
                         hour_t hh, minute_t mm, second_t ss) noexcept {
  // 计算 y 模 400 年余下多少年，记录到 ey 中， ey 后续会用来累计 cd 和 d 都合并并转成标准化的 d 后有多少年，
  // 而不需要对原来的 y 做计算，这其实也是一种避免 overflow 的手段
  // 详见 https://github.com/google/cctz/pull/152
  year_t ey = y % 400;
  // 记录原来 y 模 400 年余下多少年到 oey ，大意应该就是 original ey
  const year_t oey = ey;
  // 计算 cd 可以对应到多少个 400 年，并累加到 ey 中
  ey += (cd / 146097) * 400;
  // 计算 cd 扣掉若干 400 年后剩下多少天
  cd %= 146097;
  if (cd < 0) {
    // 如果 cd 为负了，需要保证是正数，即从 ey 中减去 400 年，加上 146097 天
    ey -= 400;
    cd += 146097;
  }
  // 同样的逻辑，从 d 中扣除掉所有可扣的 400 年，加到 ey 中
  ey += (d / 146097) * 400;
  // d 中余下的天数再加上 cd 得到所有余下的天数
  d = d % 146097 + cd;
  // 以下逻辑对 d 进行处理，将 d 减少到 146097 以下
  if (d > 0) {
    // 如果 d 大于 0
    if (d > 146097) {
      // 如果 d 大于 146097 ，我们知道 d 和 cd 之前都应该小于 146097 ，因此加起来最多不会超过 2 * 146097
      // 扣掉 400 年对应的天数， ey 加上 400 年
      ey += 400;
      d -= 146097;
    }
  } else {
    // 否则说明 d 小于 0
    if (d > -365) {
      // We often hit the previous year when stepping a civil time backwards,
      // so special case it to avoid counting up by 100/4/1-year chunks.
      // 如果 d 比 -365 要大
      // 这里是一个特殊优化，因为使用的时候经常看能需要看上一年，比如去年的今天，这时我们走一个特殊路径
      // 避免走到后面的各种复杂逻辑
      // ey 减 1 往前一年
      ey -= 1;
      // 计算前一年这个月距离今年有几天， d 补上该年的天数
      d += days_per_year(ey, m);
    } else {
      // 否则，需要将 d 转为正数， ey 扣掉 400 年， d 加上 146097 天
      ey -= 400;
      d += 146097;
    }
  }
  // 到这里 d 应该都是 146097 以下了，同时也保证是正数
  if (d > 365) {
    // 如果 d 大于 365 ，则说明肯定超过 1 年
    // 计算 ey 这个月在 400 年周期里对应的位置（index），其实就是方便后面打表，计算从 ey 往后走经过多少天
    int yi = year_index(ey, m);  // Index into Gregorian 400 year cycle.
    // 循环处理，每次计算是否可以再往后一个世纪（100 年）
    for (;;) {
      // 计算 yi 往后 100 年（一个世纪）过去了多少天
      int n = days_per_century(yi);
      // 如果 d 小于 n 说明没法往后前进 100 年了，跳出循环
      if (d <= n) break;
      // d 减去 n ，也就是过去 100 年的天数
      d -= n;
      // ey 往后 100 年，即一个世纪
      ey += 100;
      // yi 也需要增加 100
      yi += 100;
      // 如果 yi >= 400 了再将下标对齐到 400 以内
      if (yi >= 400) yi -= 400;
    }
    // 接下来循环处理，每次往前 4 年
    for (;;) {
      // 计算往后 4 年会过去多少天
      int n = days_per_4years(yi);
      // 如果 d 小于 n 则跳出去
      if (d <= n) break;
      // d 减去 n ，即 4 年的天数
      d -= n;
      // ey 往后走 4 年
      ey += 4;
      // yi 往后 4 年
      yi += 4;
      // 对齐到 400 内
      if (yi >= 400) yi -= 400;
    }
    // 接下来每次往前 1 年
    for (;;) {
      int n = days_per_year(ey, m);
      if (d <= n) break;
      d -= n;
      ++ey;
    }
  }
  // 到这里 d 已经小于 365 天了
  if (d > 28) {
    // 如果 d > 28 则超过了一个月，按月往前进
    for (;;) {
      // 计算 ey 这个月有多少天
      int n = days_per_month(ey, m);
      // 如果 d 已经小于 n 了，说明 d 已经位于这一月内了
      if (d <= n) break;
      // 减去这个月的天数
      d -= n;
      // 月份递增，如果超过 12 月则增加一年
      if (++m > 12) {
        ++ey;
        m = 1;
      }
    }
  }
  // 通过前面的计算，我们计算出 y 的增量并加到了 ey 中，因此通过 ey - oey 就能拿到实际
  // 应该累加多少年到 y ，然后构造出最终的 fields
  return fields(y + (ey - oey), m, static_cast<day_t>(d), hh, mm, ss);
}
// cd 是对 d 的增量
CONSTEXPR_F fields n_mon(year_t y, diff_t m, diff_t d, diff_t cd,
                         hour_t hh, minute_t mm, second_t ss) noexcept {
  if (m != 12) {
    // m 的范围是 1 ~ 12 ，如果 m 不等于 12
    // 首先计算 m 对应了多少年，将这部分加到 y
    y += m / 12;
    // 去掉年的部分后计算 m 余下几个月
    m %= 12;
    if (m <= 0) {
      // 如果为负数，则需要标准化为正数，扣去一年，然后加上 12 月
      y -= 1;
      m += 12;
    }
  }
  // 调用 n_day 继续处理，注意这里 cd 还会继续往下传
  return n_day(y, static_cast<month_t>(m), d, cd, hh, mm, ss);
}
// cd 是对 d 的增量
CONSTEXPR_F fields n_hour(year_t y, diff_t m, diff_t d, diff_t cd,
                          diff_t hh, minute_t mm, second_t ss) noexcept {
  // 计算 hh 对应多少天
  cd += hh / 24;
  // 余下多少小时
  hh %= 24;
  if (hh < 0) {
    // 如果小时还是负的，需要转为正数，减一天然后加 24h
    cd -= 1;
    hh += 24;
  }
  // 用 n_mon 对余下部分做标准化
  return n_mon(y, m, d, cd, static_cast<hour_t>(hh), mm, ss);
}
// ch 是 hh 的增量
CONSTEXPR_F fields n_min(year_t y, diff_t m, diff_t d, diff_t hh, diff_t ch,
                         diff_t mm, second_t ss) noexcept {
  // 计算 mm 对应了多少小时
  ch += mm / 60;
  // 转为小时后余下多少分钟
  mm %= 60;
  if (mm < 0) {
    // 如果分钟是负的，我们要转为正数做标准化，因此先减 1h ，然后加 60 分钟
    ch -= 1;
    mm += 60;
  }
  // 接下来用 n_hour 对 hh 部分做标准化
  // 同理， `hh / 24 + ch / 24` 算出 cd 即对 d 的增量，然后 `hh % 24 + ch % 24`
  // 得到余下的小时数
  return n_hour(y, m, d, hh / 24 + ch / 24, hh % 24 + ch % 24,
                static_cast<minute_t>(mm), ss);
}
CONSTEXPR_F fields n_sec(year_t y, diff_t m, diff_t d, diff_t hh, diff_t mm,
                         diff_t ss) noexcept {
  // Optimization for when (non-constexpr) fields are already normalized.
  // 优先针对标准化后（normalized）的数值进行处理，大部分情况这些值应该都是标准化后的
  if (0 <= ss && ss < 60) {
    // 这里变量都是 n 开头应该就是取标准化之意， ss 范围不标准
    const second_t nss = static_cast<second_t>(ss);
    if (0 <= mm && mm < 60) {
      // mm 范围不标准
      const minute_t nmm = static_cast<minute_t>(mm);
      if (0 <= hh && hh < 24) {
        // hh 范围不标准
        const hour_t nhh = static_cast<hour_t>(hh);
        if (1 <= d && d <= 28 && 1 <= m && m <= 12) {
          // d 和 m 的范围也标准
          const day_t nd = static_cast<day_t>(d);
          const month_t nm = static_cast<month_t>(m);
          // 所有值都标准，直接构造 fields
          return fields(y, nm, nd, nhh, nmm, nss);
        }
        // 否则 d 和 m 可能范围不标准，用 n_mon 对 m 进行标准化
        return n_mon(y, m, d, 0, nhh, nmm, nss);
      }
      // hh 可能不标准， 用 m_hour 对 hh 进行标准化
      // `hh / 24` 计算 cd 即天的增量， `hh % 24` 计算转成天后余下几小时
      return n_hour(y, m, d, hh / 24, hh % 24, nmm, nss);
    }
    // mm 不标准，用 m_min 对 mm 做标准化
    // `mm / 60` 计算 ch 即小时的增量，然后 `mm % 60` 拿到余下几分钟
    return n_min(y, m, d, hh, mm / 60, mm % 60, nss);
  }
  // 这里 ss 是不标准的，先对 ss 做标准化
  // 首先计算 ss 有几分钟， cm 感觉就是类似 count_minute 的意思
  diff_t cm = ss / 60;
  // 接下来是 ss 还余下几秒
  ss %= 60;
  if (ss < 0) {
    // 当然 ss 可能是负数，如果 ss 取模后还是负的，我们这里需要将 ss 变为正数来进行标准化，这时候 cm 要减去 1 分钟， ss 加上 60s
    cm -= 1;
    ss += 60;
  }
  // 然后也是调用 n_min 对 mm 标准化
  // 这里 `mm / 60 + cm / 60` 是计算这个分钟值对应有多少小时，而 `mm % 60 + cm % 60` 是计算转为小时后余下多少分钟
  // 分开计算再加起来应该是为了避免 overflow ，但是这也存在 mm + cm 加起来反而超过 60min 的情况没有处理
  return n_min(y, m, d, hh, mm / 60 + cm / 60, mm % 60 + cm % 60,
               static_cast<second_t>(ss));
}

}  // namespace impl

////////////////////////////////////////////////////////////////////////

// Increments the indicated (normalized) field by "n".
// step() 会对 fields 进行递增
CONSTEXPR_F fields step(second_tag, fields f, diff_t n) noexcept {
  // 递增 n 秒实际上重新根据 n_sec() 计算 fields
  return impl::n_sec(f.y, f.m, f.d, f.hh, f.mm + n / 60, f.ss + n % 60);
}
CONSTEXPR_F fields step(minute_tag, fields f, diff_t n) noexcept {
  return impl::n_min(f.y, f.m, f.d, f.hh + n / 60, 0, f.mm + n % 60, f.ss);
}
CONSTEXPR_F fields step(hour_tag, fields f, diff_t n) noexcept {
  return impl::n_hour(f.y, f.m, f.d + n / 24, 0, f.hh + n % 24, f.mm, f.ss);
}
CONSTEXPR_F fields step(day_tag, fields f, diff_t n) noexcept {
  // 递增 n 天直接将 n 作为 cd 传入 n_day() 即可
  return impl::n_day(f.y, f.m, f.d, n, f.hh, f.mm, f.ss);
}
CONSTEXPR_F fields step(month_tag, fields f, diff_t n) noexcept {
  return impl::n_mon(f.y + n / 12, f.m + n % 12, f.d, 0, f.hh, f.mm, f.ss);
}
CONSTEXPR_F fields step(year_tag, fields f, diff_t n) noexcept {
  return fields(f.y + n, f.m, f.d, f.hh, f.mm, f.ss);
}

////////////////////////////////////////////////////////////////////////

namespace impl {

// Returns (v * f + a) but avoiding intermediate overflow when possible.
// 实际上就是计算 (v * f + a) ，不过用了一些技巧避免溢出
CONSTEXPR_F diff_t scale_add(diff_t v, diff_t f, diff_t a) noexcept {
  return (v < 0) ? ((v + 1) * f + a) - f : ((v - 1) * f + a) + f;
}

// Map a (normalized) Y/M/D to the number of days before/after 1970-01-01.
// Probably overflows for years outside [-292277022656:292277026595].
// 将 y/m/d 转为 1970-01-01 以后的天数
//
// 这里的计算逻辑非常复杂，详细原理可以参考 https://howardhinnant.github.io/date_algorithms.html
// 实现还是非常精妙的，这里有一些需要注意的地方:
// > These algorithms internally assume that March 1 is the first day of the year.
// > This is convenient because it puts the leap day, Feb. 29 as the last day of the year,
// > or actually the preceding year. That is, Feb. 15, 2000, is considered by this algorithm
// > to be the 15th day of the last month of the year 1999.
//
// 一些概念
// - era: In these algorithms, an era is a 400 year period. As it turns out, the civil
// calendar exactly repeats itself every 400 years. And so these algorithms will first
// compute the era of a year/month/day triple, or the era of a serial date,
// and then factor the era out of the computation.
// - yoe: The year of the era (yoe). This is always in the range [0, 399].
// - doe: The day of the era. This is always in the range [0, 146096].
//
// era 和日期之间的规律
// ```
// era start date  end date
// -2  -0800-03-01 -0400-02-29
// -1  -0400-03-01 0000-02-29
//  0  0000-03-01  0400-02-29
//  1  0400-03-01  0800-02-29
//  2  0800-03-01  1200-02-29
//  3  1200-03-01  1600-02-29
//  4  1600-03-01  2000-02-29
//  5  2000-03-01  2400-02-29
// ```
// 可以看到对于非负的年份， era = y/400 ，而对于负数，则需要注意 [-400, -1] 对应 -1 的 era
CONSTEXPR_F diff_t ymd_ord(year_t y, month_t m, day_t d) noexcept {
  // 这里认为 3 月 1 日是一年的开始，所以这里做了处理，如果 m <=2 则 y - 1 得到 eyear
  const diff_t eyear = (m <= 2) ? y - 1 : y;
  // 计算 era ，这里负数减 399 保证计算得到正确的 era
  const diff_t era = (eyear >= 0 ? eyear : eyear - 399) / 400;
  // yoe 只要将 eyear 减下 era * 400 就能得到了，对照上面 era 和日期的规律还是不难理解的
  // 需要注意的是前面的计算方式保证这里 yoe 是 >= 0 的
  const diff_t yoe = eyear - era * 400;
  // 这个公式比较复杂，这里不做太多展开
  const diff_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  // 拿到了 yoe (year-of-era) 和 doy (day-of-year) 之后，对于每年 * 365 ，然后处理下闰年，即每
  // 4 年 + 1 ，然后每 100 年 - 1
  const diff_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  // 这里减 719468 是为了让时间从 1970-01-01 开始而不是从 0000-03-01 开始
  return era * 146097 + doe - 719468;
}

// Returns the difference in days between two normalized Y-M-D tuples.
// ymd_ord() will encounter integer overflow given extreme year values,
// yet the difference between two such extreme values may actually be
// small, so we take a little care to avoid overflow when possible by
// exploiting the 146097-day cycle.
// 计算 y1/m1/d1 - y2/m2/d2
CONSTEXPR_F diff_t day_difference(year_t y1, month_t m1, day_t d1,
                                  year_t y2, month_t m2, day_t d2) noexcept {
  // 计算 y1/y2 模 400 年余下的部分， c4 应该是推测 4 century 的意思
  const diff_t a_c4_off = y1 % 400;
  const diff_t b_c4_off = y2 % 400;
  // 将 y1 和 y2 对齐到 400 年之后计算之间的差值到 c4_diff
  diff_t c4_diff = (y1 - a_c4_off) - (y2 - b_c4_off);
  // 用模 400 年余下的部分，通过 `ymd_ord()` 转换成天数然后计算天数之间的差值得到 delta
  diff_t delta = ymd_ord(a_c4_off, m1, d1) - ymd_ord(b_c4_off, m2, d2);
  if (c4_diff > 0 && delta < 0) {
    // 如果 c4_diff 大于 0 ， delta 小于 0 ，则说明是大减小需要将 delta 转为正数
    // 不过个人感觉这里和下面加 1 个 146097 应该就够了，这里为何是 2 倍呢 ???
    delta += 2 * 146097;
    c4_diff -= 2 * 400;
  } else if (c4_diff < 0 && delta > 0) {
    // 如果 c4_diff 小于 0 ， delta 大于 0 ，则说明是小减大，将 delta 转为负数
    delta -= 2 * 146097;
    c4_diff += 2 * 400;
  }
  // 用 c4_diff 和 delta 重新计算得到最终的差值
  return (c4_diff / 400 * 146097) + delta;
}

}  // namespace impl

// Returns the difference between fields structs using the indicated unit.
CONSTEXPR_F diff_t difference(year_tag, fields f1, fields f2) noexcept {
  // 年之间直接计算差值
  return f1.y - f2.y;
}
CONSTEXPR_F diff_t difference(month_tag, fields f1, fields f2) noexcept {
  // 计算 f1 和 f2 年之间的差距，换算成月，然后再加上月之间的差距
  return impl::scale_add(difference(year_tag{}, f1, f2), 12, (f1.m - f2.m));
}
CONSTEXPR_F diff_t difference(day_tag, fields f1, fields f2) noexcept {
  // 调用 day_difference() 计算差距的天数，天数的差距计算麻烦写，需要专门的函数
  return impl::day_difference(f1.y, f1.m, f1.d, f2.y, f2.m, f2.d);
}
CONSTEXPR_F diff_t difference(hour_tag, fields f1, fields f2) noexcept {
  // 计算差距的天数并换算成小时，然后再加上小时之间的差
  return impl::scale_add(difference(day_tag{}, f1, f2), 24, (f1.hh - f2.hh));
}
CONSTEXPR_F diff_t difference(minute_tag, fields f1, fields f2) noexcept {
  // 计算差距的小时数并换算成分钟，然后再加上分钟之间的差
  return impl::scale_add(difference(hour_tag{}, f1, f2), 60, (f1.mm - f2.mm));
}
CONSTEXPR_F diff_t difference(second_tag, fields f1, fields f2) noexcept {
  // 计算差距的分钟数并换算成秒数，然后再加上秒之间的差
  return impl::scale_add(difference(minute_tag{}, f1, f2), 60, f1.ss - f2.ss);
}

////////////////////////////////////////////////////////////////////////

// Aligns the (normalized) fields struct to the indicated field.
CONSTEXPR_F fields align(second_tag, fields f) noexcept {
  return f;
}
CONSTEXPR_F fields align(minute_tag, fields f) noexcept {
  return fields{f.y, f.m, f.d, f.hh, f.mm, 0};
}
CONSTEXPR_F fields align(hour_tag, fields f) noexcept {
  return fields{f.y, f.m, f.d, f.hh, 0, 0};
}
CONSTEXPR_F fields align(day_tag, fields f) noexcept {
  return fields{f.y, f.m, f.d, 0, 0, 0};
}
CONSTEXPR_F fields align(month_tag, fields f) noexcept {
  return fields{f.y, f.m, 1, 0, 0, 0};
}
CONSTEXPR_F fields align(year_tag, fields f) noexcept {
  return fields{f.y, 1, 1, 0, 0, 0};
}

////////////////////////////////////////////////////////////////////////

template <typename T>
class civil_time {
 public:
  explicit CONSTEXPR_M civil_time(year_t y, diff_t m = 1, diff_t d = 1,
                                  diff_t hh = 0, diff_t mm = 0,
                                  diff_t ss = 0) noexcept
      : civil_time(impl::n_sec(y, m, d, hh, mm, ss)) {}

  CONSTEXPR_M civil_time() noexcept : f_{1970, 1, 1, 0, 0, 0} {}
  civil_time(const civil_time&) = default;
  civil_time& operator=(const civil_time&) = default;

  // Conversion between civil times of different alignment. Conversion to
  // a more precise alignment is allowed implicitly (e.g., day -> hour),
  // but conversion where information is discarded must be explicit
  // (e.g., second -> minute).
  template <typename U, typename S>
  using preserves_data =
      typename std::enable_if<std::is_base_of<U, S>::value>::type;
  template <typename U>
  CONSTEXPR_M civil_time(const civil_time<U>& ct,
                         preserves_data<T, U>* = nullptr) noexcept
      : civil_time(ct.f_) {}
  template <typename U>
  explicit CONSTEXPR_M civil_time(const civil_time<U>& ct,
                                  preserves_data<U, T>* = nullptr) noexcept
      : civil_time(ct.f_) {}

  // Factories for the maximum/minimum representable civil_time.
  static CONSTEXPR_F civil_time (max)() {
    const auto max_year = (std::numeric_limits<std::int_least64_t>::max)();
    return civil_time(max_year, 12, 31, 23, 59, 59);
  }
  static CONSTEXPR_F civil_time (min)() {
    const auto min_year = (std::numeric_limits<std::int_least64_t>::min)();
    return civil_time(min_year, 1, 1, 0, 0, 0);
  }

  // Field accessors.  Note: All but year() return an int.
  CONSTEXPR_M year_t year() const noexcept { return f_.y; }
  CONSTEXPR_M int month() const noexcept { return f_.m; }
  CONSTEXPR_M int day() const noexcept { return f_.d; }
  CONSTEXPR_M int hour() const noexcept { return f_.hh; }
  CONSTEXPR_M int minute() const noexcept { return f_.mm; }
  CONSTEXPR_M int second() const noexcept { return f_.ss; }

  // Assigning arithmetic.
  CONSTEXPR_M civil_time& operator+=(diff_t n) noexcept {
    return *this = *this + n;
  }
  CONSTEXPR_M civil_time& operator-=(diff_t n) noexcept {
    return *this = *this - n;
  }
  CONSTEXPR_M civil_time& operator++() noexcept {
    return *this += 1;
  }
  CONSTEXPR_M civil_time operator++(int) noexcept {
    const civil_time a = *this;
    ++*this;
    return a;
  }
  CONSTEXPR_M civil_time& operator--() noexcept {
    return *this -= 1;
  }
  CONSTEXPR_M civil_time operator--(int) noexcept {
    const civil_time a = *this;
    --*this;
    return a;
  }

  // Binary arithmetic operators.
  friend CONSTEXPR_F civil_time operator+(civil_time a, diff_t n) noexcept {
    // 加操作通过 step 实现，进行 step 时会传入 T 来控制精度
    return civil_time(step(T{}, a.f_, n));
  }
  friend CONSTEXPR_F civil_time operator+(diff_t n, civil_time a) noexcept {
    return a + n;
  }
  friend CONSTEXPR_F civil_time operator-(civil_time a, diff_t n) noexcept {
    // 减操作实际上可以转换为反过来 step() ，这里考虑了 min 转为正数会溢出，因此当等于 min 时需要特殊处理
    return n != (std::numeric_limits<diff_t>::min)()
               ? civil_time(step(T{}, a.f_, -n))
               : civil_time(step(T{}, step(T{}, a.f_, -(n + 1)), 1));
  }
  friend CONSTEXPR_F diff_t operator-(civil_time lhs, civil_time rhs) noexcept {
    return difference(T{}, lhs.f_, rhs.f_);
  }

 private:
  // All instantiations of this template are allowed to call the following
  // private constructor and access the private fields member.
  template <typename U>
  friend class civil_time;

  // The designated constructor that all others eventually call.
  explicit CONSTEXPR_M civil_time(fields f) noexcept : f_(align(T{}, f)) {}

  fields f_;
};

// Disallows difference between differently aligned types.
// auto n = civil_day(...) - civil_hour(...);  // would be confusing.
template <typename T, typename U>
CONSTEXPR_F diff_t operator-(civil_time<T>, civil_time<U>) = delete;

using civil_year = civil_time<year_tag>;
using civil_month = civil_time<month_tag>;
using civil_day = civil_time<day_tag>;
using civil_hour = civil_time<hour_tag>;
using civil_minute = civil_time<minute_tag>;
using civil_second = civil_time<second_tag>;

////////////////////////////////////////////////////////////////////////

// Relational operators that work with differently aligned objects.
// Always compares all six fields.
template <typename T1, typename T2>
CONSTEXPR_F bool operator<(const civil_time<T1>& lhs,
                           const civil_time<T2>& rhs) noexcept {
  return (lhs.year() < rhs.year() ||
          (lhs.year() == rhs.year() &&
           (lhs.month() < rhs.month() ||
            (lhs.month() == rhs.month() &&
             (lhs.day() < rhs.day() ||
              (lhs.day() == rhs.day() &&
               (lhs.hour() < rhs.hour() ||
                (lhs.hour() == rhs.hour() &&
                 (lhs.minute() < rhs.minute() ||
                  (lhs.minute() == rhs.minute() &&
                   (lhs.second() < rhs.second())))))))))));
}
template <typename T1, typename T2>
CONSTEXPR_F bool operator<=(const civil_time<T1>& lhs,
                            const civil_time<T2>& rhs) noexcept {
  return !(rhs < lhs);
}
template <typename T1, typename T2>
CONSTEXPR_F bool operator>=(const civil_time<T1>& lhs,
                            const civil_time<T2>& rhs) noexcept {
  return !(lhs < rhs);
}
template <typename T1, typename T2>
CONSTEXPR_F bool operator>(const civil_time<T1>& lhs,
                           const civil_time<T2>& rhs) noexcept {
  return rhs < lhs;
}
template <typename T1, typename T2>
CONSTEXPR_F bool operator==(const civil_time<T1>& lhs,
                            const civil_time<T2>& rhs) noexcept {
  return lhs.year() == rhs.year() && lhs.month() == rhs.month() &&
         lhs.day() == rhs.day() && lhs.hour() == rhs.hour() &&
         lhs.minute() == rhs.minute() && lhs.second() == rhs.second();
}
template <typename T1, typename T2>
CONSTEXPR_F bool operator!=(const civil_time<T1>& lhs,
                            const civil_time<T2>& rhs) noexcept {
  return !(lhs == rhs);
}

////////////////////////////////////////////////////////////////////////

enum class weekday {
  monday,
  tuesday,
  wednesday,
  thursday,
  friday,
  saturday,
  sunday,
};

// 这里主要通过打表来计算 weekday ，从 1970-01-01 开始的 weekday 都是 7 天的规律循环的，不过中间的计算逻辑做了非常多次
// 的优化，因此比较晦涩，具体细节其实也没完全想清楚，不过核心还是打表
// 更早的改动在 https://github.com/google/cctz/commit/0a5cc5000ee04ff6fd16d57cea1626a5d0b471d4
// 改之前的实现在这里，可以方便理解逻辑
// ```
// CONSTEXPR_F weekday get_weekday(const civil_day& cd) noexcept {
//   CONSTEXPR_D weekday k_weekday_by_thu_off[] = {
//       weekday::thursday,  weekday::friday,  weekday::saturday,
//       weekday::sunday,    weekday::monday,  weekday::tuesday,
//       weekday::wednesday,
//   };
//   return k_weekday_by_thu_off[((cd - civil_day()) % 7 + 7) % 7];
// }
// ```
CONSTEXPR_F weekday get_weekday(const civil_second& cs) noexcept {
  CONSTEXPR_D weekday k_weekday_by_mon_off[13] = {
      weekday::monday,    weekday::tuesday,  weekday::wednesday,
      weekday::thursday,  weekday::friday,   weekday::saturday,
      weekday::sunday,    weekday::monday,   weekday::tuesday,
      weekday::wednesday, weekday::thursday, weekday::friday,
      weekday::saturday,
  };
  CONSTEXPR_D int k_weekday_offsets[1 + 12] = {
      -1, 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4,
  };
  // 2400 这个是在这个 commit 里来的 https://github.com/google/cctz/commit/776e781b05c976d59664744d7a8f0d830e84039c
  // 主要是将年份映射到一个等价比较小的年份来避免溢出
  // 原来的实现为
  // ```
  // year_t wd = cd.year() - (cd.month() < 3);
  // if (wd >= 0) {
  //   wd += wd / 4 - wd / 100 + wd / 400;
  // } else {
  //   wd += (wd - 3) / 4 - (wd - 99) / 100 + (wd - 399) / 400;
  // }
  // wd += k_weekday_offsets[cd.month()] + cd.day();
  // return k_weekday_by_sun_off[(wd % 7 + 7) % 7];
  // ```
  year_t wd = 2400 + (cs.year() % 400) - (cs.month() < 3);
  wd += wd / 4 - wd / 100 + wd / 400;
  wd += k_weekday_offsets[cs.month()] + cs.day();
  // 这里通过扩大 offset 数组来避免一次 % ，详见 https://github.com/google/cctz/commit/b14d4c984f9ca5bcc55c4e5b7e3818b23d70c004
  return k_weekday_by_mon_off[wd % 7 + 6];
}

////////////////////////////////////////////////////////////////////////

CONSTEXPR_F civil_day next_weekday(civil_day cd, weekday wd) noexcept {
  CONSTEXPR_D weekday k_weekdays_forw[14] = {
      weekday::monday,    weekday::tuesday,  weekday::wednesday,
      weekday::thursday,  weekday::friday,   weekday::saturday,
      weekday::sunday,    weekday::monday,   weekday::tuesday,
      weekday::wednesday, weekday::thursday, weekday::friday,
      weekday::saturday,  weekday::sunday,
  };
  // 拿到 cd 对应星期几
  weekday base = get_weekday(cd);
  for (int i = 0;; ++i) {
    // 遍历上面的数组，直到发现星期数等于 wd
    if (base == k_weekdays_forw[i]) {
      for (int j = i + 1;; ++j) {
        // 往后面一直迭代直到下个星期 wd
        if (wd == k_weekdays_forw[j]) {
          // 这时 j - i 就是从 cd 到下个 wd 过得天数，然后通过 cd 的加法运算即可
          return cd + (j - i);
        }
      }
    }
  }
}

CONSTEXPR_F civil_day prev_weekday(civil_day cd, weekday wd) noexcept {
  CONSTEXPR_D weekday k_weekdays_back[14] = {
      weekday::sunday,   weekday::saturday,  weekday::friday,
      weekday::thursday, weekday::wednesday, weekday::tuesday,
      weekday::monday,   weekday::sunday,    weekday::saturday,
      weekday::friday,   weekday::thursday,  weekday::wednesday,
      weekday::tuesday,  weekday::monday,
  };
  // 原理和 next_weekday() 类似
  weekday base = get_weekday(cd);
  for (int i = 0;; ++i) {
    if (base == k_weekdays_back[i]) {
      for (int j = i + 1;; ++j) {
        if (wd == k_weekdays_back[j]) {
          return cd - (j - i);
        }
      }
    }
  }
}

CONSTEXPR_F int get_yearday(const civil_second& cs) noexcept {
  CONSTEXPR_D int k_month_offsets[1 + 12] = {
      -1, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334,
  };
  // 如果已经在 2 月后了，并且今年是闰年，则 2 月有 29 天， feb29 为 1
  const int feb29 = (cs.month() > 2 && impl::is_leap_year(cs.year()));
  // 打表，从表中拿到天数，加上 2 月可能要多加的 1 天和这个月所在的天数
  return k_month_offsets[cs.month()] + feb29 + cs.day();
}

////////////////////////////////////////////////////////////////////////

std::ostream& operator<<(std::ostream& os, const civil_year& y);
std::ostream& operator<<(std::ostream& os, const civil_month& m);
std::ostream& operator<<(std::ostream& os, const civil_day& d);
std::ostream& operator<<(std::ostream& os, const civil_hour& h);
std::ostream& operator<<(std::ostream& os, const civil_minute& m);
std::ostream& operator<<(std::ostream& os, const civil_second& s);
std::ostream& operator<<(std::ostream& os, weekday wd);

}  // namespace detail
}  // namespace cctz

#undef CONSTEXPR_M
#undef CONSTEXPR_F
#undef CONSTEXPR_D

#endif  // CCTZ_CIVIL_TIME_DETAIL_H_
