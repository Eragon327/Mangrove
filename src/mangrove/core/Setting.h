#pragma once

#include <algorithm>
#include <cmath>
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
    [[nodiscard]] double             defaultValue() const { return mDefaultValue; }

    /// 当前值。开关用 0 / 1 表示，数值就是数值本身。
    [[nodiscard]] double value() const { return mValue; }

    /// 当前值（开关语义）
    [[nodiscard]] bool enabled() const { return mValue != 0.0; }

    void setValue(double value) { mValue = sanitize(value); }

    /// 回到默认值
    void reset() { mValue = sanitize(mDefaultValue); }

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
