#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace streaming::file_clipboard {
inline constexpr std::size_t max_entries = 20000;
inline constexpr ULONG max_chunk_bytes = 1024 * 1024;

struct entry {
  std::wstring relative_path;
  std::uint64_t size = 0;
  DWORD attributes = FILE_ATTRIBUTE_NORMAL;
  FILETIME last_write {};
  bool directory() const { return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
};

// The transport must authenticate the offer and supply bounded reads. No
// destination path or arbitrary source path is accepted by this interface.
class reader {
public:
  virtual ~reader() = default;
  virtual HRESULT read_at(std::size_t index, std::uint64_t offset,
                          void *buffer, ULONG requested, ULONG &received) = 0;
};

struct offer {
  std::vector<entry> entries;
  std::shared_ptr<reader> source;
  std::atomic<bool> valid {true};
};

bool valid_relative_path(const std::wstring &path);
HRESULT create_data_object(std::shared_ptr<offer> value, IDataObject **result);
bool interactive_desktop_available();

class source_files final : public reader {
public:
  static HRESULT collect(const std::vector<std::wstring> &paths,
                         const std::function<bool()> &cancelled,
                         std::shared_ptr<source_files> &result);
  ~source_files();
  const std::vector<entry> &entries() const;
  HRESULT read_at(std::size_t index, std::uint64_t offset,
                  void *buffer, ULONG requested, ULONG &received) override;
private:
  struct impl;
  source_files();
  std::unique_ptr<impl> state_;
};

// Owns a dedicated OLE STA. Network/source callbacks may run on COM worker
// threads and must not depend on this thread's message pump to make progress.
class publisher {
public:
  publisher();
  ~publisher();
  publisher(const publisher &) = delete;
  publisher &operator=(const publisher &) = delete;
  HRESULT publish(std::shared_ptr<offer> value);
  void clear();
  using clipboard_change = std::function<void(std::uint32_t, std::vector<std::wstring>)>;
  void observe(clipboard_change callback);
private:
  struct impl;
  std::unique_ptr<impl> state_;
};
}
