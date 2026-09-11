#include "mangrove/data/DataBase.h"

#include "mangrove/Mangrove.h"

#include "ll/api/data/KeyValueDB.h"

#include <charconv>
#include <exception>
#include <system_error>

namespace mangrove::data {
namespace {

/// `{88,67}` -> `"88,67"`
std::string encodeIntList(std::vector<int> const& values) {
    std::string result;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) result.push_back(',');
        result += std::to_string(values[i]);
    }
    return result;
}

/// `"88,67"` -> `{88,67}`；非法输入返回 `std::nullopt`
std::optional<std::vector<int>> decodeIntList(std::string_view text) {
    std::vector<int> values;
    size_t           start = 0;
    while (start <= text.size()) {
        auto const separator = text.find(',', start);
        auto const piece     = text.substr(start, separator - start);
        if (piece.empty()) return std::nullopt;

        int value{};
        auto const [end, error] = std::from_chars(piece.data(), piece.data() + piece.size(), value);
        if (error != std::errc{} || end != piece.data() + piece.size()) return std::nullopt;

        values.push_back(value);
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    if (values.empty()) return std::nullopt;
    return values;
}

auto& logger() { return Mangrove::getInstance().getSelf().getLogger(); }

} // namespace

DataBase::DataBase() = default;

DataBase::~DataBase() = default;

bool DataBase::open(std::filesystem::path const& backendDir) {
    if (mDatabase) return true;
    try {
        mDatabase = std::make_unique<ll::data::KeyValueDB>(backendDir);
    } catch (std::exception const& e) {
        mDatabase.reset();
        logger().error("Failed to open the key-value database: {}", e.what());
        return false;
    } catch (...) {
        mDatabase.reset();
        logger().error("Failed to open the key-value database");
        return false;
    }
    return true;
}

void DataBase::close() { mDatabase.reset(); }

bool DataBase::isOpen() const { return mDatabase != nullptr; }

std::optional<std::string> DataBase::get(std::string_view key) const {
    if (!mDatabase) return std::nullopt;
    return mDatabase->get(key);
}

bool DataBase::set(std::string_view key, std::string_view value) {
    if (!mDatabase) return false;
    if (value.empty()) return remove(key);
    return mDatabase->set(key, value);
}

bool DataBase::remove(std::string_view key) {
    if (!mDatabase) return false;
    return mDatabase->del(key);
}

std::optional<std::vector<int>> DataBase::getIntList(std::string_view key) const {
    auto const stored = get(key);
    if (!stored) return std::nullopt;
    return decodeIntList(*stored);
}

bool DataBase::setIntList(std::string_view key, std::vector<int> const& values) {
    if (values.empty()) return remove(key);
    return set(key, encodeIntList(values));
}

} // namespace mangrove::data
