#pragma once
#include "file_clipboard_native.h"
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <set>
#include <thread>

namespace streaming::file_clipboard::wire {
using bytes = std::vector<unsigned char>;
inline constexpr std::size_t max_manifest_bytes = 16 * 1024 * 1024;
enum class kind : std::uint32_t { hello = 1, offer = 2, request = 3, reply = 4, publish = 5 };
struct message { kind type {}; std::uint64_t id = 0; bytes body; };
inline void number(bytes &out, std::uint64_t value, unsigned count) {
  for (unsigned i = 0; i < count; ++i) out.push_back(static_cast<unsigned char>(value >> (i * 8)));
}
struct decoder {
  const unsigned char *data; std::size_t size, position = 0;
  explicit decoder(const bytes &in): data(in.data()), size(in.size()) {}
  bool number(std::uint64_t &value, unsigned count) {
    if (count > 8 || count > size - position) return false;
    value = 0;
    for (unsigned i = 0; i < count; ++i) value |= std::uint64_t(data[position++]) << (i * 8);
    return true;
  }
  bool done() const { return position == size; }
};
inline bytes encode(const message &value) {
  if (value.body.size() > max_manifest_bytes) return {};
  bytes out; out.reserve(16 + value.body.size());
  number(out, static_cast<std::uint32_t>(value.type), 4);
  number(out, value.id, 8); number(out, value.body.size(), 4);
  out.insert(out.end(), value.body.begin(), value.body.end());
  return out;
}
inline bool decode(const bytes &data, message &value) {
  decoder input(data); std::uint64_t type, size;
  if (!input.number(type, 4) || !input.number(value.id, 8) || !input.number(size, 4) ||
      type < 1 || type > 5 || size > max_manifest_bytes || size != data.size() - input.position) return false;
  value.type = static_cast<kind>(type);
  value.body.assign(data.begin() + input.position, data.end()); return true;
}
struct ordinal_less {
  bool operator()(const std::wstring &a, const std::wstring &b) const {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
  }
};
inline bool valid_manifest(const std::vector<entry> &entries) {
  if (entries.empty() || entries.size() > max_entries) return false;
  std::set<std::wstring, ordinal_less> names, directories;
  for (const auto &item : entries) {
    if (!valid_relative_path(item.relative_path) || !names.insert(item.relative_path).second ||
        item.size > static_cast<std::uint64_t>(INT64_MAX) || (item.directory() && item.size)) return false;
    const auto slash = item.relative_path.rfind(L'\\');
    if (slash != std::wstring::npos && !directories.count(item.relative_path.substr(0, slash))) return false;
    if (item.attributes & ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                            FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NORMAL)) return false;
    if (item.directory()) directories.insert(item.relative_path);
  }
  return true;
}
inline bytes manifest(const std::vector<entry> &entries) {
  if (!valid_manifest(entries)) return {};
  bytes out; number(out, entries.size(), 4);
  for (const auto &item : entries) {
    number(out, item.relative_path.size(), 2);
    for (wchar_t character : item.relative_path) number(out, static_cast<std::uint16_t>(character), 2);
    number(out, item.size, 8); number(out, item.attributes, 4);
    number(out, (std::uint64_t(item.last_write.dwHighDateTime) << 32) | item.last_write.dwLowDateTime, 8);
    if (out.size() > max_manifest_bytes) return {};
  }
  return out;
}
inline bool manifest(const bytes &body, std::vector<entry> &entries) {
  entries.clear(); if (body.size() > max_manifest_bytes) return false;
  decoder input(body); std::uint64_t count, length, value;
  if (!input.number(count, 4) || !count || count > max_entries) return false;
  entries.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    entry item;
    if (!input.number(length, 2) || !length || length >= MAX_PATH) return false;
    for (std::uint64_t j = 0; j < length; ++j) {
      if (!input.number(value, 2)) return false;
      item.relative_path.push_back(static_cast<wchar_t>(value));
    }
    if (!input.number(item.size, 8) || !input.number(value, 4)) return false;
    item.attributes = static_cast<DWORD>(value);
    if (!input.number(value, 8)) return false;
    item.last_write.dwLowDateTime = static_cast<DWORD>(value);
    item.last_write.dwHighDateTime = static_cast<DWORD>(value >> 32);
    entries.push_back(std::move(item));
  }
  return input.done() && valid_manifest(entries);
}
struct read_query { std::uint64_t generation = 0; std::uint32_t index = 0; std::uint64_t offset = 0; std::uint32_t count = 0; };
inline bytes query(const read_query &value) {
  bytes out; number(out, value.generation, 8); number(out, value.index, 4);
  number(out, value.offset, 8); number(out, value.count, 4); return out;
}
inline bool query(const bytes &body, read_query &value) {
  decoder input(body); std::uint64_t index, count;
  if (!input.number(value.generation, 8) || !input.number(index, 4) || !input.number(value.offset, 8) ||
      !input.number(count, 4) || !input.done() || count > max_chunk_bytes) return false;
  value.index = static_cast<std::uint32_t>(index); value.count = static_cast<std::uint32_t>(count); return true;
}
inline bool within(const read_query &value, std::uint64_t generation, const std::vector<entry> &entries) {
  return value.generation && value.generation == generation && value.index < entries.size() &&
      !entries[value.index].directory() && value.offset <= entries[value.index].size &&
      value.count <= max_chunk_bytes && value.count <= entries[value.index].size - value.offset;
}
inline bytes reply(HRESULT status, const void *data = nullptr, std::size_t size = 0) {
  bytes out; number(out, static_cast<std::uint32_t>(status), 4);
  if (data && size <= max_chunk_bytes) out.insert(out.end(), static_cast<const unsigned char *>(data), static_cast<const unsigned char *>(data) + size);
  return out;
}
inline HRESULT reply(const bytes &body, void *data, ULONG count, ULONG &received) {
  received = 0; decoder input(body); std::uint64_t status;
  if (!input.number(status, 4) || body.size() - 4 > count) return STG_E_READFAULT;
  HRESULT result = static_cast<HRESULT>(status);
  if (FAILED(result)) return result;
  received = static_cast<ULONG>(body.size() - 4);
  if (received != count) return STG_E_READFAULT;
  if (received) std::memcpy(data, body.data() + 4, received);
  return S_OK;
}

// Two workers and a bounded queue keep file operations away from UI and video threads.
class workers {
public:
  workers() { for (unsigned i = 0; i < 2; ++i) threads_.emplace_back([this] { run(); }); }
  ~workers() {
    { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; queue_.clear(); }
    ready_.notify_all(); for (auto &thread : threads_) thread.join();
  }
  bool post(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_ || queue_.size() >= 8) return false;
    queue_.push_back(std::move(task)); ready_.notify_one(); return true;
  }
private:
  void run() {
    for (;;) {
      std::function<void()> task;
      { std::unique_lock<std::mutex> lock(mutex_); ready_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_) return;
        task = std::move(queue_.front()); queue_.pop_front(); }
      try { task(); } catch (...) { /* A failed offer must not terminate the helper. */ }
    }
  }
  std::mutex mutex_; std::condition_variable ready_; bool stop_ = false;
  std::deque<std::function<void()>> queue_; std::vector<std::thread> threads_;
};
}
