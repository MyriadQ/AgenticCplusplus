#include "agenticpp/memory_store.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "agenticpp/cosine.hpp"
#include "agenticpp/unique_fd.hpp"

namespace agenticpp {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kSnapshotMagic = "AGNTMEM1";
constexpr std::uint32_t kSnapshotVersion = 1;
constexpr std::uint32_t kMaxRecords = 1'000'000;
constexpr std::uint32_t kMaxString = 64 * 1024 * 1024;

static_assert(std::endian::native == std::endian::little,
              "memory snapshot format assumes a little-endian host");

struct StoreState {
    std::unordered_map<std::uint64_t, std::shared_ptr<const FixRecord>> records;
    std::vector<std::uint64_t> ids;
    std::vector<float> vectors;
    std::unordered_map<std::uint64_t, std::size_t> row_by_id;
};

std::int64_t to_ns(Timestamp at) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
}

Timestamp from_ns(std::int64_t ns) {
    return Timestamp{std::chrono::nanoseconds{ns}};
}

template <class T>
void write_pod(std::ostream& out, T value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <class T>
Result<T> read_pod(std::istream& in, std::string_view what) {
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(T))) {
        return Result<T>::err(1, "corrupt snapshot: short " + std::string{what});
    }
    return Result<T>::ok(value);
}

Result<void> write_string(std::ostream& out, std::string_view text) {
    if (text.size() > kMaxString) {
        return Result<void>::err(2, "snapshot string too long");
    }
    write_pod<std::uint32_t>(out, static_cast<std::uint32_t>(text.size()));
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        return Result<void>::err(3, "snapshot write failed");
    }
    return Result<void>::ok();
}

Result<std::string> read_string(std::istream& in, std::string_view what) {
    auto length = read_pod<std::uint32_t>(in, what);
    if (!length.has_value()) {
        return Result<std::string>::err(length.error());
    }
    if (length.value() > kMaxString) {
        return Result<std::string>::err(4, "corrupt snapshot: string too long");
    }
    std::string value(length.value(), '\0');
    if (!value.empty()) {
        in.read(value.data(), static_cast<std::streamsize>(value.size()));
        if (in.gcount() != static_cast<std::streamsize>(value.size())) {
            return Result<std::string>::err(5, "corrupt snapshot: short string");
        }
    }
    return Result<std::string>::ok(std::move(value));
}

void put_record(StoreState& state, std::uint64_t id, const Vectorizer& vectorizer,
                FixRecord record) {
    const auto existing = state.row_by_id.find(id);
    std::vector<float> vector(Vectorizer::kDim, 0.0f);
    vectorizer.embed(record.template_text, std::span<float>{vector});

    if (existing != state.row_by_id.end()) {
        const std::size_t row = existing->second;
        std::copy(vector.begin(), vector.end(),
                  state.vectors.begin() + static_cast<std::ptrdiff_t>(row) * Vectorizer::kDim);
        state.records[id] = std::make_shared<const FixRecord>(std::move(record));
        return;
    }

    const std::size_t row = state.ids.size();
    state.ids.push_back(id);
    state.row_by_id.emplace(id, row);
    state.vectors.insert(state.vectors.end(), vector.begin(), vector.end());
    state.records.emplace(id, std::make_shared<const FixRecord>(std::move(record)));
}

Result<void> load_snapshot(const fs::path& path, StoreState& state) {
    if (path.empty() || !fs::exists(path)) {
        return Result<void>::ok();
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<void>::err(6, "cannot open memory snapshot: " + path.string());
    }

    char magic[8]{};
    input.read(magic, sizeof(magic));
    if (input.gcount() != static_cast<std::streamsize>(sizeof(magic)) ||
        std::string_view{magic, sizeof(magic)} != kSnapshotMagic) {
        return Result<void>::err(7, "bad memory snapshot magic");
    }

    auto version = read_pod<std::uint32_t>(input, "version");
    auto dim = read_pod<std::uint32_t>(input, "dim");
    auto count = read_pod<std::uint32_t>(input, "count");
    auto reserved = read_pod<std::uint32_t>(input, "reserved");
    if (!version.has_value() || !dim.has_value() || !count.has_value() ||
        !reserved.has_value()) {
        return Result<void>::err(8, "corrupt memory snapshot header");
    }
    if (version.value() != kSnapshotVersion) {
        return Result<void>::err(9, "unsupported memory snapshot version");
    }
    if (dim.value() != Vectorizer::kDim) {
        return Result<void>::err(10, "memory snapshot dimension mismatch");
    }
    if (count.value() > kMaxRecords) {
        return Result<void>::err(11, "memory snapshot record count too large");
    }

    state.ids.reserve(count.value());
    state.vectors.reserve(static_cast<std::size_t>(count.value()) * Vectorizer::kDim);
    state.records.reserve(count.value());
    state.row_by_id.reserve(count.value());

    for (std::uint32_t i = 0; i < count.value(); ++i) {
        auto id = read_pod<std::uint64_t>(input, "id");
        auto text = read_string(input, "template_text");
        auto nargs = read_pod<std::uint32_t>(input, "nargs");
        if (!id.has_value() || !text.has_value() || !nargs.has_value()) {
            return Result<void>::err(12, "corrupt memory snapshot record");
        }
        if (nargs.value() > 1024) {
            return Result<void>::err(13, "corrupt memory snapshot: too many argv entries");
        }

        FixRecord record;
        record.template_text = std::move(text.value());
        for (std::uint32_t arg = 0; arg < nargs.value(); ++arg) {
            auto value = read_string(input, "argv");
            if (!value.has_value()) {
                return Result<void>::err(14, "corrupt memory snapshot argv");
            }
            record.argv.push_back(std::move(value.value()));
        }
        auto purpose = read_string(input, "purpose");
        auto successes = read_pod<std::uint32_t>(input, "successes");
        auto failures = read_pod<std::uint32_t>(input, "failures");
        auto last_seen = read_pod<std::int64_t>(input, "last_seen");
        if (!purpose.has_value() || !successes.has_value() || !failures.has_value() ||
            !last_seen.has_value()) {
            return Result<void>::err(15, "corrupt memory snapshot counters");
        }
        record.purpose = std::move(purpose.value());
        record.successes = successes.value();
        record.failures = failures.value();
        record.last_seen = from_ns(last_seen.value());

        std::vector<float> vector(Vectorizer::kDim, 0.0f);
        for (std::size_t d = 0; d < Vectorizer::kDim; ++d) {
            auto value = read_pod<float>(input, "vector");
            if (!value.has_value()) {
                return Result<void>::err(16, "corrupt memory snapshot vector");
            }
            vector[d] = value.value();
        }

        const std::size_t row = state.ids.size();
        state.ids.push_back(id.value());
        state.row_by_id.emplace(id.value(), row);
        state.vectors.insert(state.vectors.end(), vector.begin(), vector.end());
        state.records.emplace(id.value(),
                              std::make_shared<const FixRecord>(std::move(record)));
    }

    return Result<void>::ok();
}

Result<void> save_snapshot(const fs::path& path, const StoreState& state) {
    if (path.empty()) {
        return Result<void>::ok();
    }
    if (state.ids.size() > kMaxRecords) {
        return Result<void>::err(20, "too many records for memory snapshot");
    }

    std::error_code directory_error;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), directory_error);
        if (directory_error) {
            return Result<void>::err(21, "cannot create snapshot directory: " +
                                             directory_error.message());
        }
    }

    const fs::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Result<void>::err(22, "cannot create snapshot temp file");
    }

    output.write(kSnapshotMagic.data(), static_cast<std::streamsize>(kSnapshotMagic.size()));
    write_pod<std::uint32_t>(output, kSnapshotVersion);
    write_pod<std::uint32_t>(output, static_cast<std::uint32_t>(Vectorizer::kDim));
    write_pod<std::uint32_t>(output, static_cast<std::uint32_t>(state.ids.size()));
    write_pod<std::uint32_t>(output, 0);

    for (std::size_t row = 0; row < state.ids.size(); ++row) {
        const std::uint64_t id = state.ids[row];
        const auto found = state.records.find(id);
        if (found == state.records.end() || found->second == nullptr) {
            output.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return Result<void>::err(23, "snapshot record missing from map");
        }
        const FixRecord& record = *found->second;

        write_pod<std::uint64_t>(output, id);
        auto text = write_string(output, record.template_text);
        if (!text.has_value()) {
            output.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return text;
        }
        write_pod<std::uint32_t>(output, static_cast<std::uint32_t>(record.argv.size()));
        for (const std::string& arg : record.argv) {
            auto written = write_string(output, arg);
            if (!written.has_value()) {
                output.close();
                std::error_code ignored;
                fs::remove(temporary, ignored);
                return written;
            }
        }
        auto purpose = write_string(output, record.purpose);
        if (!purpose.has_value()) {
            output.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return purpose;
        }
        write_pod<std::uint32_t>(output, record.successes);
        write_pod<std::uint32_t>(output, record.failures);
        write_pod<std::int64_t>(output, to_ns(record.last_seen));
        for (std::size_t d = 0; d < Vectorizer::kDim; ++d) {
            write_pod<float>(output, state.vectors[row * Vectorizer::kDim + d]);
        }
    }

    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return Result<void>::err(24, "snapshot write failed");
    }
    output.close();

    UniqueFd file{::open(temporary.c_str(), O_RDONLY | O_CLOEXEC)};
    if (!file || ::fsync(file.get()) != 0) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return Result<void>::err(25, std::string{"snapshot fsync: "} + std::strerror(errno));
    }

    std::error_code rename_error;
    fs::rename(temporary, path, rename_error);
    if (rename_error) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return Result<void>::err(26, "snapshot rename: " + rename_error.message());
    }

    const fs::path directory = path.parent_path().empty() ? fs::path{"."} : path.parent_path();
    UniqueFd dir{::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (dir) {
        (void)::fsync(dir.get());
    }
    return Result<void>::ok();
}

void apply_event(StoreState& state, const Vectorizer& vectorizer,
                 const MemoryJournalEvent& event) {
    if (event.kind == MemoryJournalEvent::Kind::Upsert) {
        FixRecord record;
        record.template_text = event.template_text;
        record.argv = event.argv;
        record.purpose = event.purpose;
        record.successes = event.successes;
        record.failures = event.failures;
        record.last_seen = event.last_seen;
        put_record(state, event.template_id, vectorizer, std::move(record));
        return;
    }

    const auto found = state.records.find(event.template_id);
    if (found == state.records.end() || found->second == nullptr) {
        return;
    }
    FixRecord updated = *found->second;
    updated.successes = event.successes;
    updated.failures = event.failures;
    updated.last_seen = event.last_seen;
    state.records[event.template_id] = std::make_shared<const FixRecord>(std::move(updated));
}

}  // namespace

MemoryStore::MemoryStore() : MemoryStore(Vectorizer{}, MemoryStoreConfig{}, 1024) {}

MemoryStore::MemoryStore(std::size_t reserve)
    : MemoryStore(Vectorizer{}, MemoryStoreConfig{}, reserve) {}

MemoryStore::MemoryStore(const Vectorizer& vectorizer, std::size_t reserve)
    : MemoryStore(vectorizer, MemoryStoreConfig{}, reserve) {}

MemoryStore::MemoryStore(const Vectorizer& vectorizer, MemoryStoreConfig config,
                         std::size_t reserve)
    : vectorizer_(vectorizer),
      config_(std::move(config)),
      journal_(config_.journal_path) {
    if (reserve > 0) {
        records_.reserve(reserve);
        ids_.reserve(reserve);
        row_by_id_.reserve(reserve);
        vectors_.reserve(reserve * Vectorizer::kDim);
    }
    if (config_.persistent && config_.journal_path.empty() && !config_.snapshot_path.empty()) {
        // A caller may specify only the snapshot path. Keep memory_journal's
        // path consistent with the documented data/memory.bin layout.
        std::filesystem::path derived = config_.snapshot_path;
        derived.replace_extension(".journal");
        config_.journal_path = derived;
        journal_ = MemoryJournal{config_.journal_path};
    }
}

Result<void> MemoryStore::load() {
    if (!config_.persistent) {
        return Result<void>::ok();
    }
    if (config_.snapshot_path.empty()) {
        return Result<void>::err(30, "persistent memory has no snapshot path");
    }

    StoreState state;
    auto snapshot = load_snapshot(config_.snapshot_path, state);
    if (!snapshot.has_value()) {
        return snapshot;
    }

    auto events = journal_.load();
    if (!events.has_value()) {
        return Result<void>::err(events.error());
    }
    for (const MemoryJournalEvent& event : events.value()) {
        apply_event(state, vectorizer_, event);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    records_ = std::move(state.records);
    ids_ = std::move(state.ids);
    vectors_ = std::move(state.vectors);
    row_by_id_ = std::move(state.row_by_id);
    journal_events_ = events.value().size();
    return Result<void>::ok();
}

Result<void> MemoryStore::upsert(std::uint64_t id, std::string_view text, FixRecord record) {
    record.template_text = std::string{text};
    record.last_seen = record.last_seen == Timestamp{} ? std::chrono::system_clock::now()
                                                       : record.last_seen;

    std::vector<float> vector(Vectorizer::kDim, 0.0f);
    vectorizer_.embed(record.template_text, std::span<float>{vector});

    std::lock_guard<std::mutex> lock(mutex_);

    if (config_.persistent && config_.write_through) {
        auto appended = journal_.append_upsert(id, record.template_text, record.argv,
                                               record.purpose, record.successes,
                                               record.failures, record.last_seen);
        if (!appended.has_value()) {
            return appended;
        }
        ++journal_events_;
    }

    auto owner = std::make_shared<const FixRecord>(std::move(record));
    const auto existing = row_by_id_.find(id);
    if (existing != row_by_id_.end()) {
        const std::size_t row = existing->second;
        std::copy(vector.begin(), vector.end(),
                  vectors_.begin() + static_cast<std::ptrdiff_t>(row) * Vectorizer::kDim);
        records_[id] = std::move(owner);
    } else {
        const std::size_t row = ids_.size();
        ids_.push_back(id);
        row_by_id_.emplace(id, row);
        vectors_.insert(vectors_.end(), vector.begin(), vector.end());
        records_.emplace(id, std::move(owner));
    }

    (void)maybe_compact_locked();
    return Result<void>::ok();
}

Result<void> MemoryStore::record_outcome(std::uint64_t id, bool success) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end() || found->second == nullptr) {
        return Result<void>::ok();
    }

    FixRecord updated = *found->second;
    if (success) {
        ++updated.successes;
    } else {
        ++updated.failures;
    }
    updated.last_seen = std::chrono::system_clock::now();

    if (config_.persistent && config_.write_through) {
        auto appended = journal_.append_outcome(id, updated.successes, updated.failures,
                                                updated.last_seen);
        if (!appended.has_value()) {
            return appended;
        }
        ++journal_events_;
    }

    records_[id] = std::make_shared<const FixRecord>(std::move(updated));
    (void)maybe_compact_locked();
    return Result<void>::ok();
}

std::vector<Hit> MemoryStore::search(std::string_view query, std::size_t k) const {
    std::vector<Hit> hits;
    if (k == 0) {
        return hits;
    }

    std::vector<float> query_vector(Vectorizer::kDim, 0.0f);
    vectorizer_.embed(query, std::span<float>{query_vector});

    float query_norm_squared = 0.0f;
    for (const float value : query_vector) {
        query_norm_squared += value * value;
    }
    if (query_norm_squared <= 0.0f) {
        return hits;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (ids_.empty()) {
        return hits;
    }

    const std::size_t limit = std::min(k, ids_.size());
    std::vector<CosineHit> scored(limit);
    const std::size_t used =
        cosine_topk(query_vector, vectors_, ids_, ids_.size(), Vectorizer::kDim, scored);
    hits.reserve(used);
    for (std::size_t i = 0; i < used; ++i) {
        const auto found = records_.find(scored[i].template_id);
        if (found == records_.end() || found->second == nullptr) {
            continue;
        }
        hits.push_back(Hit{scored[i].template_id, scored[i].score, found->second.get(),
                           found->second});
    }

    std::sort(hits.begin(), hits.end(), [](const Hit& left, const Hit& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        if (left.record->failures != right.record->failures) {
            return left.record->failures < right.record->failures;
        }
        if (left.record->successes != right.record->successes) {
            return left.record->successes > right.record->successes;
        }
        return left.template_id < right.template_id;
    });
    return hits;
}

std::size_t MemoryStore::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

Result<void> MemoryStore::maybe_compact_locked() {
    if (config_.compact_after_events == 0 || journal_events_ < config_.compact_after_events) {
        return Result<void>::ok();
    }
    return compact_locked();
}

Result<void> MemoryStore::compact() {
    std::lock_guard<std::mutex> lock(mutex_);
    return compact_locked();
}

void MemoryStore::set_write_through(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.write_through = enabled;
}

Result<void> MemoryStore::compact_locked() {
    if (!config_.persistent || !config_.write_through) {
        return Result<void>::ok();
    }

    StoreState state;
    state.records = records_;
    state.ids = ids_;
    state.vectors = vectors_;
    state.row_by_id = row_by_id_;

    auto saved = save_snapshot(config_.snapshot_path, state);
    if (!saved.has_value()) {
        return saved;
    }
    auto cleared = journal_.clear();
    if (!cleared.has_value()) {
        return cleared;
    }
    journal_events_ = 0;
    return Result<void>::ok();
}

}  // namespace agenticpp
