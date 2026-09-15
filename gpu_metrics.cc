#include <napi.h>

#include <windows.h>
#include <dxgi1_2.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

constexpr size_t kMaxPids = 128;
constexpr DWORD kMaxCounterInfoBytes = 64 * 1024;
constexpr DWORD kMaxExpandedPathChars = 256 * 1024;
constexpr size_t kMaxConcretePathsPerWildcard = 512;
constexpr size_t kMaxConcreteCounterHandles = 8192;
constexpr size_t kMaxEngineRows = 4096;

enum Availability : int {
    kAvailable = 1,
    kNotSupported = 2,
    kNotInitialized = 3,
    kProviderError = 5
};

using CounterHandles = std::unordered_map<std::wstring, PDH_HCOUNTER>;

struct CounterGroup {
    std::wstring englishWildcard;
    std::wstring localizedWildcard;
    CounterHandles handles;
};

struct PidCounters {
    DWORD pid = 0;
    CounterGroup engine;
    CounterGroup dedicated;
    CounterGroup shared;
};

struct CounterRefreshResult {
    bool hasAnyHandle = false;
    bool failed = false;
    bool truncated = false;
};

struct EngineValue {
    std::wstring luid;
    std::wstring engineType;
    double utilization = 0.0;
};

struct ProcessMemoryValue {
    DWORD pid = 0;
    double dedicatedKiB = 0.0;
    double sharedKiB = 0.0;
    bool hasDedicated = false;
    bool hasShared = false;
};

struct AdapterIdentityValue {
    std::wstring luid;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
};

struct SampleData {
    int availability = kProviderError;
    std::vector<EngineValue> engines;
    std::vector<ProcessMemoryValue> processMemory;
    std::vector<AdapterIdentityValue> adapterIdentities;
    std::string reason;
};

class ProviderState {
public:
    ProviderState() = default;
    ~ProviderState() { dispose(); }

    SampleData sample(const std::vector<DWORD>& pids) {
        if (disposed_.load(std::memory_order_acquire)) return disposedSample();
        std::lock_guard<std::mutex> lock(mutex_);
        if (disposed_.load(std::memory_order_relaxed)) return disposedSample();
        SampleData result;
        const bool hasAdapterIdentities = collectAdapterIdentities(result.adapterIdentities);
        const std::vector<DWORD> normalized = normalizePids(pids);
        if (normalized.empty()) {
            result.availability = kNotInitialized;
            result.reason = "no_target_pids";
            return result;
        }

        bool queryCreated = false;
        CounterRefreshResult refresh;
        if (!ensurePdh(normalized, queryCreated, refresh)) {
            result.availability = refresh.failed ? kProviderError : kNotInitialized;
            result.reason = refresh.failed ? "pdh_query_unavailable" : "pdh_no_matching_instances";
            return result;
        }

        const PDH_STATUS collectStatus = PdhCollectQueryData(query_);
        if (collectStatus != ERROR_SUCCESS) {
            result.availability = kProviderError;
            result.reason = "pdh_collect_failed";
            return result;
        }
        if (queryCreated || !baselineReady_) {
            baselineReady_ = true;
            result.availability = kNotInitialized;
            result.reason = "pdh_baseline_pending";
            return result;
        }

        std::unordered_map<std::wstring, double> engineNodeTotals;
        bool hasValidEngineCounter = false;
        bool hasValidMemoryCounter = false;
        for (const auto& entry : counters_) {
            const PidCounters& counters = entry.second;
            hasValidEngineCounter = readEngineCounters(counters.engine.handles, engineNodeTotals) || hasValidEngineCounter;
            ProcessMemoryValue memory;
            memory.pid = counters.pid;
            memory.hasDedicated = readMemoryCounters(counters.dedicated.handles, memory.dedicatedKiB);
            memory.hasShared = readMemoryCounters(counters.shared.handles, memory.sharedKiB);
            hasValidMemoryCounter = memory.hasDedicated || memory.hasShared || hasValidMemoryCounter;
            if (memory.hasDedicated || memory.hasShared) result.processMemory.push_back(memory);
        }

        // Sum target-process contexts per physical engine, clamp each engine
        // to 100%, then report the busiest physical engine per adapter/type.
        // This mirrors the useful "busy engine" signal without pretending
        // independent hardware engines can be added into one percentage.
        std::unordered_map<std::wstring, double> engineTypeMaximums;
        for (const auto& entry : engineNodeTotals) {
            const size_t lastSplit = entry.first.rfind(L'|');
            if (lastSplit == std::wstring::npos) continue;
            const std::wstring adapterAndType = entry.first.substr(0, lastSplit);
            const double physicalEngineBusy = std::clamp(entry.second, 0.0, 100.0);
            auto found = engineTypeMaximums.find(adapterAndType);
            if (found == engineTypeMaximums.end()) {
                engineTypeMaximums.emplace(adapterAndType, physicalEngineBusy);
            } else {
                found->second = std::max(found->second, physicalEngineBusy);
            }
        }
        result.engines.reserve(engineTypeMaximums.size());
        for (const auto& entry : engineTypeMaximums) {
            const size_t split = entry.first.find(L'|');
            if (split == std::wstring::npos) continue;
            result.engines.push_back({entry.first.substr(0, split), entry.first.substr(split + 1), entry.second});
        }

        if (!hasValidEngineCounter && !hasValidMemoryCounter) {
            result.availability = kNotInitialized;
            result.reason = "pdh_no_valid_samples";
            return result;
        }

        result.availability = kAvailable;
        if (refresh.truncated) {
            result.reason = "partial_counter_cap_no_adapter_memory";
        } else if (refresh.failed) {
            result.reason = "partial_counter_refresh_no_adapter_memory";
        } else if (!hasAdapterIdentities) {
            result.reason = "partial_no_adapter_identity_or_memory";
        } else if (!hasValidEngineCounter) {
            result.reason = "partial_process_memory_only_no_adapter_memory";
        } else if (!hasValidMemoryCounter) {
            result.reason = "partial_engine_only_no_adapter_memory";
        } else {
            result.reason = "adapter_memory_not_supported";
        }
        return result;
    }

    void requestDispose() { disposed_.store(true, std::memory_order_release); }

    void dispose() {
        requestDispose();
        std::lock_guard<std::mutex> lock(mutex_);
        closePdh();
    }

private:
    std::mutex mutex_;
    std::atomic<bool> disposed_{false};
    PDH_HQUERY query_ = nullptr;
    std::unordered_map<DWORD, PidCounters> counters_;
    size_t concreteCounterHandleCount_ = 0;
    bool baselineReady_ = false;

    static SampleData disposedSample() {
        SampleData result;
        result.availability = kNotSupported;
        result.reason = "native_gpu_metrics_provider_disposed";
        return result;
    }

    static std::vector<DWORD> normalizePids(const std::vector<DWORD>& input) {
        std::vector<DWORD> result;
        std::unordered_set<DWORD> seen;
        for (DWORD pid : input) {
            if (pid == 0 || !seen.insert(pid).second) continue;
            result.push_back(pid);
            if (result.size() == kMaxPids) break;
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    bool ensurePdh(
        const std::vector<DWORD>& pids,
        bool& queryCreated,
        CounterRefreshResult& refresh
    ) {
        queryCreated = false;
        if (!query_) {
            if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
                query_ = nullptr;
                refresh.failed = true;
                return false;
            }
            queryCreated = true;
            baselineReady_ = false;
        }

        const std::unordered_set<DWORD> requested(pids.begin(), pids.end());
        for (auto iterator = counters_.begin(); iterator != counters_.end();) {
            if (requested.find(iterator->first) != requested.end()) {
                ++iterator;
                continue;
            }
            removePidCounters(iterator->second);
            iterator = counters_.erase(iterator);
        }

        for (DWORD pid : pids) {
            auto found = counters_.find(pid);
            if (found == counters_.end()) {
                const std::wstring pidText = std::to_wstring(pid);
                PidCounters counters;
                counters.pid = pid;
                counters.engine.englishWildcard =
                    L"\\GPU Engine(pid_" + pidText + L"_*)\\Utilization Percentage";
                counters.dedicated.englishWildcard =
                    L"\\GPU Process Memory(pid_" + pidText + L"_*)\\Dedicated Usage";
                counters.shared.englishWildcard =
                    L"\\GPU Process Memory(pid_" + pidText + L"_*)\\Shared Usage";
                found = counters_.emplace(pid, std::move(counters)).first;
            }
            mergeRefresh(refresh, refreshCounterGroup(found->second.engine));
            mergeRefresh(refresh, refreshCounterGroup(found->second.dedicated));
            mergeRefresh(refresh, refreshCounterGroup(found->second.shared));
        }
        refresh.hasAnyHandle = concreteCounterHandleCount_ > 0;
        return refresh.hasAnyHandle;
    }

    static void mergeRefresh(CounterRefreshResult& target, const CounterRefreshResult& value) {
        target.hasAnyHandle = target.hasAnyHandle || value.hasAnyHandle;
        target.failed = target.failed || value.failed;
        target.truncated = target.truncated || value.truncated;
    }

    CounterRefreshResult refreshCounterGroup(CounterGroup& group) {
        CounterRefreshResult result;
        if (group.localizedWildcard.empty() && !localizeEnglishWildcard(group.englishWildcard, group.localizedWildcard)) {
            result.failed = true;
            result.hasAnyHandle = !group.handles.empty();
            return result;
        }

        std::vector<std::wstring> concretePaths;
        bool noInstances = false;
        if (!expandLocalizedWildcard(group.localizedWildcard, concretePaths, result.truncated, noInstances)) {
            result.failed = !noInstances;
            if (!noInstances) {
                result.hasAnyHandle = !group.handles.empty();
                return result;
            }
        }

        const std::unordered_set<std::wstring> requested(concretePaths.begin(), concretePaths.end());
        for (auto iterator = group.handles.begin(); iterator != group.handles.end();) {
            if (requested.find(iterator->first) != requested.end()) {
                ++iterator;
                continue;
            }
            PdhRemoveCounter(iterator->second);
            if (concreteCounterHandleCount_ > 0) --concreteCounterHandleCount_;
            iterator = group.handles.erase(iterator);
        }

        for (const std::wstring& path : concretePaths) {
            if (group.handles.find(path) != group.handles.end()) continue;
            if (concreteCounterHandleCount_ >= kMaxConcreteCounterHandles) {
                result.truncated = true;
                break;
            }
            PDH_HCOUNTER handle = nullptr;
            if (PdhAddCounterW(query_, path.c_str(), 0, &handle) != ERROR_SUCCESS || !handle) {
                result.failed = true;
                continue;
            }
            group.handles.emplace(path, handle);
            ++concreteCounterHandleCount_;
        }
        result.hasAnyHandle = !group.handles.empty();
        return result;
    }

    bool localizeEnglishWildcard(const std::wstring& englishWildcard, std::wstring& localizedWildcard) {
        PDH_HCOUNTER temporary = nullptr;
        if (PdhAddEnglishCounterW(query_, englishWildcard.c_str(), 0, &temporary) != ERROR_SUCCESS || !temporary) {
            return false;
        }
        DWORD bytes = 0;
        PDH_STATUS status = PdhGetCounterInfoW(temporary, FALSE, &bytes, nullptr);
        if (status != PDH_MORE_DATA || bytes == 0 || bytes > kMaxCounterInfoBytes) {
            PdhRemoveCounter(temporary);
            return false;
        }
        std::vector<uint8_t> buffer(bytes);
        auto* info = reinterpret_cast<PDH_COUNTER_INFO_W*>(buffer.data());
        status = PdhGetCounterInfoW(temporary, FALSE, &bytes, info);
        if (status == ERROR_SUCCESS && info->szFullPath) localizedWildcard.assign(info->szFullPath);
        PdhRemoveCounter(temporary);
        return status == ERROR_SUCCESS && !localizedWildcard.empty();
    }

    static bool expandLocalizedWildcard(
        const std::wstring& localizedWildcard,
        std::vector<std::wstring>& paths,
        bool& truncated,
        bool& noInstances
    ) {
        DWORD characters = 0;
        PDH_STATUS status = PdhExpandWildCardPathW(
            nullptr, localizedWildcard.c_str(), nullptr, &characters, 0
        );
        if (isNoInstanceStatus(status)) {
            noInstances = true;
            return false;
        }
        if (status != PDH_MORE_DATA || characters == 0 || characters > kMaxExpandedPathChars) {
            truncated = characters > kMaxExpandedPathChars;
            return false;
        }
        std::vector<wchar_t> buffer(characters);
        status = PdhExpandWildCardPathW(
            nullptr, localizedWildcard.c_str(), buffer.data(), &characters, 0
        );
        if (isNoInstanceStatus(status)) {
            noInstances = true;
            return false;
        }
        if (status != ERROR_SUCCESS || characters == 0 || characters > buffer.size()) return false;

        size_t offset = 0;
        while (offset < characters && buffer[offset] != L'\0') {
            size_t length = 0;
            while (offset + length < characters && buffer[offset + length] != L'\0') ++length;
            if (offset + length >= characters) return false;
            if (length > 0) {
                if (paths.size() >= kMaxConcretePathsPerWildcard) {
                    truncated = true;
                    break;
                }
                paths.emplace_back(buffer.data() + offset, length);
            }
            offset += length + 1;
        }
        return true;
    }

    static bool isNoInstanceStatus(PDH_STATUS status) {
        return status == PDH_CSTATUS_NO_INSTANCE || status == PDH_CSTATUS_NO_OBJECT || status == PDH_NO_DATA;
    }

    void removeCounterGroup(CounterGroup& group) {
        for (const auto& entry : group.handles) PdhRemoveCounter(entry.second);
        concreteCounterHandleCount_ = group.handles.size() > concreteCounterHandleCount_
            ? 0
            : concreteCounterHandleCount_ - group.handles.size();
        group.handles.clear();
    }

    void removePidCounters(PidCounters& counters) {
        removeCounterGroup(counters.engine);
        removeCounterGroup(counters.dedicated);
        removeCounterGroup(counters.shared);
    }

    void closePdh() {
        for (auto& entry : counters_) removePidCounters(entry.second);
        counters_.clear();
        concreteCounterHandleCount_ = 0;
        baselineReady_ = false;
        if (query_) PdhCloseQuery(query_);
        query_ = nullptr;
    }

    static bool readEngineCounters(
        const CounterHandles& counters,
        std::unordered_map<std::wstring, double>& output
    ) {
        bool hasValidValue = false;
        for (const auto& entry : counters) {
            PDH_FMT_COUNTERVALUE formatted{};
            if (PdhGetFormattedCounterValue(
                    entry.second, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &formatted
                ) != ERROR_SUCCESS || !isPdhValueValid(formatted.CStatus)) {
                continue;
            }
            const double value = formatted.doubleValue;
            if (!std::isfinite(value)) continue;
            const std::wstring luid = parseLuid(entry.first);
            const std::wstring engineType = parseEngineType(entry.first);
            const std::wstring engineNode = parseEngineNode(entry.first);
            if (luid.empty() || engineType.empty() || engineNode.empty()) continue;
            hasValidValue = true;
            const std::wstring key = luid + L"|" + engineType + L"|" + engineNode;
            const auto found = output.find(key);
            if (found == output.end()) {
                if (output.size() >= kMaxEngineRows) continue;
                output.emplace(key, std::max(0.0, value));
            } else {
                found->second = std::min(10000.0, found->second + std::max(0.0, value));
            }
        }
        return hasValidValue;
    }

    static bool readMemoryCounters(const CounterHandles& counters, double& value) {
        uint64_t totalBytes = 0;
        bool hasValidValue = false;
        for (const auto& entry : counters) {
            PDH_FMT_COUNTERVALUE formatted{};
            if (PdhGetFormattedCounterValue(entry.second, PDH_FMT_LARGE, nullptr, &formatted) != ERROR_SUCCESS ||
                !isPdhValueValid(formatted.CStatus)) {
                continue;
            }
            const LONGLONG bytes = formatted.largeValue;
            if (bytes < 0) continue;
            hasValidValue = true;
            const uint64_t unsignedBytes = static_cast<uint64_t>(bytes);
            totalBytes = unsignedBytes > std::numeric_limits<uint64_t>::max() - totalBytes
                ? std::numeric_limits<uint64_t>::max()
                : totalBytes + unsignedBytes;
        }
        if (!hasValidValue) return false;
        value = static_cast<double>(totalBytes) / 1024.0;
        return true;
    }

    static bool isPdhValueValid(DWORD status) {
        return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA;
    }

    static std::wstring parseLuid(const std::wstring& instance) {
        const size_t start = instance.find(L"_luid_");
        if (start == std::wstring::npos) return {};
        const size_t valueStart = start + 6;
        size_t end = instance.find(L"_phys_", valueStart);
        if (end == std::wstring::npos) end = instance.find(L"_eng_", valueStart);
        if (end == std::wstring::npos || end <= valueStart) return {};
        return canonicalizeLuid(instance.substr(valueStart, end - valueStart));
    }

    static std::wstring parseEngineType(const std::wstring& instance) {
        const size_t start = instance.find(L"_engtype_");
        if (start == std::wstring::npos) return {};
        const size_t valueStart = start + 9;
        size_t valueEnd = valueStart;
        while (valueEnd < instance.size()) {
            const wchar_t character = instance[valueEnd];
            if (!iswalnum(character) && character != L'_') break;
            valueEnd += 1;
        }
        if (valueEnd == valueStart) return {};
        std::wstring value = instance.substr(valueStart, valueEnd - valueStart);
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(towlower(character));
        });
        if (value == L"3d") return L"3d";
        if (value == L"compute") return L"compute";
        if (value == L"copy") return L"copy";
        if (value == L"videodecode") return L"video_decode";
        if (value == L"videoencode") return L"video_encode";
        return L"unknown";
    }

    static std::wstring parseEngineNode(const std::wstring& instance) {
        const size_t start = instance.find(L"_phys_");
        const size_t end = instance.find(L"_engtype_");
        if (start == std::wstring::npos || end == std::wstring::npos || end <= start + 1) return {};
        const std::wstring node = instance.substr(start + 1, end - start - 1);
        return node.size() <= 64 ? node : std::wstring();
    }

    static std::wstring luidToString(const LUID& luid) {
        std::wstringstream stream;
        stream << std::hex << std::nouppercase << static_cast<uint32_t>(luid.HighPart) << L"_"
               << static_cast<uint32_t>(luid.LowPart);
        return stream.str();
    }

    static std::wstring canonicalizeLuid(const std::wstring& value) {
        const size_t separator = value.find(L'_');
        if (separator == std::wstring::npos) return {};
        const std::wstring highText = value.substr(0, separator);
        const std::wstring lowText = value.substr(separator + 1);
        wchar_t* highEnd = nullptr;
        wchar_t* lowEnd = nullptr;
        const unsigned long high = wcstoul(highText.c_str(), &highEnd, 0);
        const unsigned long low = wcstoul(lowText.c_str(), &lowEnd, 0);
        if (!highEnd || *highEnd != L'\0' || !lowEnd || *lowEnd != L'\0') return {};
        std::wstringstream stream;
        stream << std::hex << std::nouppercase << static_cast<uint32_t>(high) << L"_"
               << static_cast<uint32_t>(low);
        return stream.str();
    }

    static bool collectAdapterIdentities(std::vector<AdapterIdentityValue>& output) {
        IDXGIFactory1* rawFactory = nullptr;
        if (CreateDXGIFactory1(IID_PPV_ARGS(&rawFactory)) != S_OK || !rawFactory) return false;
        for (UINT index = 0; index < 64; ++index) {
            IDXGIAdapter1* adapter = nullptr;
            const HRESULT enumeration = rawFactory->EnumAdapters1(index, &adapter);
            if (enumeration == DXGI_ERROR_NOT_FOUND) break;
            if (enumeration != S_OK || !adapter) continue;
            DXGI_ADAPTER_DESC1 description{};
            if (adapter->GetDesc1(&description) == S_OK) {
                output.push_back({luidToString(description.AdapterLuid), description.VendorId, description.DeviceId});
            }
            adapter->Release();
        }
        rawFactory->Release();
        return !output.empty();
    }
};

ProviderState& state() {
    static ProviderState value;
    return value;
}

std::string toUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr
    );
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr
    );
    return result;
}

std::vector<DWORD> readPids(const Napi::CallbackInfo& info) {
    std::vector<DWORD> pids;
    if (info.Length() == 0 || !info[0].IsObject()) return pids;
    const Napi::Object input = info[0].As<Napi::Object>();
    const Napi::Value raw = input.Get("pids");
    if (!raw.IsArray()) return pids;
    const Napi::Array array = raw.As<Napi::Array>();
    for (uint32_t index = 0; index < array.Length() && pids.size() < kMaxPids; ++index) {
        const Napi::Value item = array.Get(index);
        if (!item.IsNumber()) continue;
        const double value = item.As<Napi::Number>().DoubleValue();
        if (!std::isfinite(value) || value <= 0 || value > 0xFFFFFFFF || std::floor(value) != value) continue;
        pids.push_back(static_cast<DWORD>(value));
    }
    return pids;
}

Napi::Object sampleToObject(Napi::Env env, const SampleData& data) {
    Napi::Object result = Napi::Object::New(env);
    result.Set("availability", Napi::Number::New(env, data.availability));
    if (!data.reason.empty()) result.Set("reason", Napi::String::New(env, data.reason));

    Napi::Array engines = Napi::Array::New(env, data.engines.size());
    for (size_t index = 0; index < data.engines.size(); ++index) {
        const EngineValue& value = data.engines[index];
        Napi::Object item = Napi::Object::New(env);
        item.Set("luid", Napi::String::New(env, toUtf8(value.luid)));
        item.Set("engineType", Napi::String::New(env, toUtf8(value.engineType)));
        item.Set("utilizationPercent", Napi::Number::New(env, value.utilization));
        engines.Set(index, item);
    }
    result.Set("engines", engines);

    Napi::Array processMemory = Napi::Array::New(env, data.processMemory.size());
    for (size_t index = 0; index < data.processMemory.size(); ++index) {
        const ProcessMemoryValue& value = data.processMemory[index];
        Napi::Object item = Napi::Object::New(env);
        item.Set("pid", Napi::Number::New(env, value.pid));
        if (value.hasDedicated) item.Set("dedicatedKiB", Napi::Number::New(env, value.dedicatedKiB));
        if (value.hasShared) item.Set("sharedKiB", Napi::Number::New(env, value.sharedKiB));
        processMemory.Set(index, item);
    }
    result.Set("processMemory", processMemory);

    Napi::Array identities = Napi::Array::New(env, data.adapterIdentities.size());
    for (size_t index = 0; index < data.adapterIdentities.size(); ++index) {
        const AdapterIdentityValue& value = data.adapterIdentities[index];
        Napi::Object item = Napi::Object::New(env);
        item.Set("luid", Napi::String::New(env, toUtf8(value.luid)));
        item.Set("vendorId", Napi::Number::New(env, value.vendorId));
        item.Set("deviceId", Napi::Number::New(env, value.deviceId));
        identities.Set(index, item);
    }
    result.Set("adapterIdentities", identities);
    result.Set("adapters", Napi::Array::New(env));
    return result;
}

class SampleWorker final : public Napi::AsyncWorker {
public:
    SampleWorker(Napi::Env env, std::vector<DWORD> pids)
        : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)), pids_(std::move(pids)) {}

    Napi::Promise promise() const { return deferred_.Promise(); }

    void Execute() override { data_ = state().sample(pids_); }

    void OnOK() override { deferred_.Resolve(sampleToObject(Env(), data_)); }

    void OnError(const Napi::Error& error) override { deferred_.Reject(error.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    std::vector<DWORD> pids_;
    SampleData data_;
};

class DisposeWorker final : public Napi::AsyncWorker {
public:
    explicit DisposeWorker(Napi::Env env) : Napi::AsyncWorker(env) {}

    void Execute() override { state().dispose(); }
};

Napi::Value sample(const Napi::CallbackInfo& info) {
    auto* worker = new SampleWorker(info.Env(), readPids(info));
    const Napi::Promise promise = worker->promise();
    worker->Queue();
    return promise;
}

Napi::Value dispose(const Napi::CallbackInfo& info) {
    // Prevent queued samples from reopening the query immediately, then do
    // potentially blocking PDH handle cleanup away from Electron's Main loop.
    state().requestDispose();
    auto* worker = new DisposeWorker(info.Env());
    worker->Queue();
    return info.Env().Undefined();
}

} // namespace

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("sample", Napi::Function::New(env, sample));
    exports.Set("dispose", Napi::Function::New(env, dispose));
    return exports;
}

NODE_API_MODULE(gpu_metrics, Init)
