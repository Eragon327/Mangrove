#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace mangrove::core {

/// 一条可持久化的设置项。
///
/// 功能只负责「声明我有哪些设置」，界面渲染和读写配置都由框架按 `kind()` 统一处理：
/// 给功能加一条设置 = 加一个成员，不需要写任何界面代码。
class Setting {
public:
    /// 控件类型
    enum class Kind {
        Toggle, ///< 开关：值域 {0, 1}
        Number, ///< 数值：区间 + 步长，界面默认是滑条，可以切成输入框直接打字
    };

    Setting(Setting const&)            = delete;
    Setting& operator=(Setting const&) = delete;

    [[nodiscard]] Kind               kind() const { return mKind; }
    [[nodiscard]] std::string const& id() const { return mId; }
    [[nodiscard]] double             min() const { return mMin; }
    [[nodiscard]] double             max() const { return mMax; }
    [[nodiscard]] double             step() const { return mStep; }

    /// 当前值。开关用 0 / 1 表示，数值就是数值本身。
    [[nodiscard]] double value() const { return mValue; }

    /// 当前值（开关语义）
    [[nodiscard]] bool enabled() const { return mValue != 0.0; }

    /// 当前值是否就是默认值。
    /// 「只存 diff」（等于默认值就不写库）和界面上的「重置」按钮都靠它 ——
    /// 所以这条判据只有这一份实现，不要在调用方再写一遍 `abs(值 - 默认值) < eps`。
    [[nodiscard]] bool isDefault() const { return std::abs(mValue - mDefaultValue) <= kSameValueEpsilon; }

    void setValue(double value) { mValue = sanitize(value); }

    /// 回到默认值
    void reset() { mValue = sanitize(mDefaultValue); }

    /// 覆盖声明时的区间（框架配置 `config/Config.json` 用）。
    /// @param min,max,step 为空表示这一头维持原样
    /// @note 给数值项用；默认值也跟着过一遍区间 —— 否则「区间把默认值排除在外」时，
    ///       当前值会显得「和默认不同」，于是往设置库里写一条毫无意义的 diff。
    void overrideRange(std::optional<double> min, std::optional<double> max, std::optional<double> step) {
        if (min) mMin = *min;
        if (max) mMax = *max;
        if (step) mStep = *step;

        // 手写反了、写了负数也认：调过来 / 按「不吸附」处理，别让区间空着
        if (!(mMin <= mMax)) std::swap(mMin, mMax);
        if (!(mStep >= 0.0)) mStep = 0.0;

        mDefaultValue = sanitize(mDefaultValue);
        mValue        = sanitize(mValue);
    }

protected:
    Setting(Kind kind, std::string id, double min, double max, double step, double defaultValue)
    : mKind(kind),
      mId(std::move(id)),
      mMin(min),
      mMax(max),
      mStep(step),
      // 传参而不是读成员：否则这里就隐式依赖成员声明顺序，谁把声明顺序调一下就成 UB
      mDefaultValue(sanitize(kind, min, max, step, defaultValue, min)),
      mValue(mDefaultValue) {}

private:
    /// 「算不算等于默认值」的容差：滑条是浮点数，过完步长吸附未必能精确相等
    static constexpr double kSameValueEpsilon = 1e-9;

    /// 值域收敛：非有限值取 @p fallback，数值按步长吸附，最后夹进区间。
    /// @note 参数式而非读成员，构造期间也能安全使用。
    [[nodiscard]] static double
    sanitize(Kind kind, double min, double max, double step, double value, double fallback) {
        if (!std::isfinite(value)) return fallback;
        if (kind == Kind::Number && step > 0.0) value = std::round(value / step) * step;
        return std::clamp(value, min, max);
    }

    [[nodiscard]] double sanitize(double value) const {
        // 运行时写进来的非法值（NaN、越界）退回默认值
        return sanitize(mKind, mMin, mMax, mStep, value, mDefaultValue);
    }

    Kind        mKind;
    std::string mId;
    double      mMin;
    double      mMax;
    double      mStep;
    double      mDefaultValue;
    double      mValue;
};

/// 开关，值域 {0, 1}
class ToggleSetting : public Setting {
public:
    explicit ToggleSetting(std::string id, bool defaultValue = false)
    : Setting(Kind::Toggle, std::move(id), 0.0, 1.0, 1.0, defaultValue ? 1.0 : 0.0) {}
};

/// 数值：区间 + 步长（步长为 0 表示不吸附，可取区间内任意值）。
///
/// 界面默认画成滑条，右侧按钮可以切成输入框直接打字 —— 两者是同一个控件的两种编辑方式，
/// 所以这里是**一个**类型而不是两个。
class NumberSetting : public Setting {
public:
    NumberSetting(std::string id, double min, double max, double step, double defaultValue)
    : Setting(Kind::Number, std::move(id), min, max, step, defaultValue) {}
};

} // namespace mangrove::core
