#include "ItchReplay.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

const std::size_t kHeaderSize = 128;
const std::size_t kRecordSize = 24;
const char        kMagic[8]   = {'I', 'T', 'C', 'H', 'A', 'C', 'T', '1'};
const std::size_t kBatch      = 64 * 1024;   // records read per fread

// The file is written little-endian with no padding, so every field is copied out by its
// byte offset rather than by casting the buffer to a struct. That way the reader does not
// depend on how this compiler happens to lay a struct out.
template <typename T>
T field(const unsigned char* buffer, std::size_t offset) {
    T value;
    std::memcpy(&value, buffer + offset, sizeof(T));
    return value;
}

// Header offsets, in the order analysis/itch_extract.py packs them.
const std::size_t kOffVersion  = 8,  kOffRecord  = 12, kOffSymbol  = 16, kOffDate     = 24;
const std::size_t kOffPrefill  = 32, kOffActions = 40, kOffMessages = 48, kOffPeakLive = 56;
const std::size_t kOffMinPrice = 64, kOffMaxPrice = 72, kOffAdds = 80, kOffCancels = 88;
const std::size_t kOffModifies = 96, kOffFirstNs = 104, kOffLastNs = 112;

// A fixed-length text field, with the padding spaces removed.
std::string text(const unsigned char* buffer, std::size_t offset, std::size_t size) {
    std::string value(reinterpret_cast<const char*>(buffer + offset), size);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) value.pop_back();
    return value;
}

// One 24-byte record -> one Action. The record's type byte is 0 add, 1 cancel, 2 modify,
// matching ACTION_ADD / ACTION_CANCEL / ACTION_MODIFY in the extractor.
Action toAction(const unsigned char* record) {
    Action action{};
    unsigned char kind = record[0];
    action.type = kind == 0 ? ActionType::Add : (kind == 1 ? ActionType::Cancel : ActionType::Modify);

    action.order.id       = field<std::uint64_t>(record, 16);
    action.order.side     = record[1] == 0 ? Side::Buy : Side::Sell;
    action.order.type     = OrderType::Limit;      // ITCH only publishes resting limit orders
    action.order.price    = field<std::int64_t>(record, 8);
    action.order.quantity = field<std::uint32_t>(record, 4);
    action.order.timestamp = 0;                    // the book stamps its own arrival time
    return action;
}

// RAII so the file is closed on every path, including the throws below.
struct File {
    std::FILE* handle;
    explicit File(const std::string& path) : handle(std::fopen(path.c_str(), "rb")) {}
    ~File() { if (handle) std::fclose(handle); }
};

}  // namespace

ItchFile readItchFile(const std::string& path, std::size_t maxActions) {
    File file(path);
    if (!file.handle) throw std::runtime_error("cannot open " + path);

    unsigned char header[kHeaderSize];
    if (std::fread(header, 1, kHeaderSize, file.handle) != kHeaderSize)
        throw std::runtime_error(path + ": too short to be an action file");
    if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0)
        throw std::runtime_error(path + ": not an action file (bad magic)");
    if (field<std::uint32_t>(header, kOffVersion) != 1)
        throw std::runtime_error(path + ": unsupported action file version");
    if (field<std::uint32_t>(header, kOffRecord) != kRecordSize)
        throw std::runtime_error(path + ": unexpected record size");

    ItchFile out;
    out.symbol   = text(header, kOffSymbol, 8);
    out.date     = text(header, kOffDate, 8);
    out.messages = field<std::uint64_t>(header, kOffMessages);
    out.peakLive = field<std::uint64_t>(header, kOffPeakLive);
    out.minPrice = field<std::int64_t>(header, kOffMinPrice);
    out.maxPrice = field<std::int64_t>(header, kOffMaxPrice);
    out.adds     = field<std::uint64_t>(header, kOffAdds);
    out.cancels  = field<std::uint64_t>(header, kOffCancels);
    out.modifies = field<std::uint64_t>(header, kOffModifies);
    out.firstNs  = field<std::int64_t>(header, kOffFirstNs);
    out.lastNs   = field<std::int64_t>(header, kOffLastNs);

    std::uint64_t prefillCount = field<std::uint64_t>(header, kOffPrefill);
    std::uint64_t actionCount  = field<std::uint64_t>(header, kOffActions);
    if (maxActions > 0 && actionCount > maxActions) actionCount = maxActions;

    out.prefill.reserve(static_cast<std::size_t>(prefillCount));
    out.flow.reserve(static_cast<std::size_t>(actionCount));

    // The prefill block comes first, then the measured block. Read both in batches so the
    // whole file is never in memory twice.
    std::vector<unsigned char> buffer(kBatch * kRecordSize);
    for (int phase = 0; phase < 2; ++phase) {
        std::vector<Action>& into = phase == 0 ? out.prefill : out.flow;
        std::uint64_t        left = phase == 0 ? prefillCount : actionCount;
        while (left > 0) {
            std::size_t want = static_cast<std::size_t>(left < kBatch ? left : kBatch);
            std::size_t got  = std::fread(buffer.data(), kRecordSize, want, file.handle);
            if (got != want) throw std::runtime_error(path + ": file is shorter than its header says");
            for (std::size_t i = 0; i < got; ++i) into.push_back(toAction(&buffer[i * kRecordSize]));
            left -= got;
        }
    }

    // If maxActions cut the flow short, the counts in the header describe the whole day and
    // no longer describe what was loaded, so recount them.
    if (out.flow.size() != field<std::uint64_t>(header, kOffActions)) {
        out.messages = out.flow.size();
        out.adds = out.cancels = out.modifies = 0;
        for (const Action& a : out.flow) {
            if (a.type == ActionType::Add)         ++out.adds;
            else if (a.type == ActionType::Cancel) ++out.cancels;
            else                                   ++out.modifies;
        }
    }
    return out;
}
