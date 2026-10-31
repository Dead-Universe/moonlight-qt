#include "file_clipboard_native.h"
#include "file_clipboard_wire.h"
#include <filesystem>
#include <set>

namespace streaming::file_clipboard {
namespace {
std::wstring extended(const std::wstring &path) {
  if (path.rfind(L"\\\\?\\", 0) == 0) return path;
  if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
  return L"\\\\?\\" + path;
}
struct handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct ordinal_less {
  bool operator()(const std::wstring &a, const std::wstring &b) const {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
  }
};
HRESULT full_path(const std::wstring &input, std::wstring &output) {
  DWORD count = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
  if (!count || count > 32767) return E_INVALIDARG;
  std::vector<wchar_t> buffer(count);
  if (!GetFullPathNameW(input.c_str(), count, buffer.data(), nullptr)) return HRESULT_FROM_WIN32(GetLastError());
  output = buffer.data();
  while (output.size() > 3 && output.back() == L'\\') output.pop_back();
  auto parent = std::filesystem::path(output);
  while (!parent.empty()) {
    auto attributes = GetFileAttributesW(extended(parent.wstring()).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(GetLastError());
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    auto next = parent.parent_path();
    if (next == parent) break;
    parent = next;
  }
  return S_OK;
}
}

bool interactive_desktop_available() {
  DWORD session = MAXDWORD;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || session != WTSGetActiveConsoleSessionId()) return false;
  HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!desktop) return false;
  wchar_t name[128] {};
  DWORD bytes = 0;
  BOOL success = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &bytes);
  CloseDesktop(desktop);
  return success && CompareStringOrdinal(name, -1, L"Default", -1, TRUE) == CSTR_EQUAL;
}

struct source_files::impl {
  struct fingerprint {
    std::wstring path;
    BY_HANDLE_FILE_INFORMATION information {};
  };
  std::vector<entry> entries;
  std::vector<fingerprint> files;
  std::set<std::wstring, ordinal_less> names;
  std::size_t path_characters = 0;
  HRESULT append(const std::wstring &path, const std::wstring &relative,
                 const std::function<bool()> &cancelled, unsigned depth) {
    if (cancelled && cancelled()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (depth > 64 || entries.size() >= max_entries || !valid_relative_path(relative)) return E_INVALIDARG;
    if (path.size() > wire::max_manifest_bytes / sizeof(wchar_t) - path_characters) return E_OUTOFMEMORY;
    path_characters += path.size();
    if (!names.insert(relative).second) return HRESULT_FROM_WIN32(ERROR_DUP_NAME);
    WIN32_FILE_ATTRIBUTE_DATA attributes {};
    if (!GetFileAttributesExW(extended(path).c_str(), GetFileExInfoStandard, &attributes)) return HRESULT_FROM_WIN32(GetLastError());
    if (attributes.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    entry descriptor;
    descriptor.relative_path = relative;
    descriptor.attributes = attributes.dwFileAttributes &
        (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_SYSTEM);
    if (!descriptor.attributes) descriptor.attributes = FILE_ATTRIBUTE_NORMAL;
    descriptor.last_write = attributes.ftLastWriteTime;
    fingerprint identity;
    identity.path = extended(path);
    if (!descriptor.directory()) {
      handle file;
      file.value = CreateFileW(identity.path.c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (file.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
      if (!GetFileInformationByHandle(file.value, &identity.information)) return HRESULT_FROM_WIN32(GetLastError());
      if (identity.information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
      descriptor.size = (static_cast<std::uint64_t>(identity.information.nFileSizeHigh) << 32) | identity.information.nFileSizeLow;
      descriptor.last_write = identity.information.ftLastWriteTime;
    }
    entries.push_back(descriptor);
    files.push_back(std::move(identity));
    if (!descriptor.directory()) return S_OK;
    WIN32_FIND_DATAW found {};
    HANDLE search = FindFirstFileW(extended(path + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
      DWORD error = GetLastError();
      return error == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
    }
    std::set<std::wstring, ordinal_less> children;
    do {
      if (std::wstring(found.cFileName) != L"." && std::wstring(found.cFileName) != L"..") children.insert(found.cFileName);
      if (children.size() + entries.size() > max_entries || (cancelled && cancelled())) {
        FindClose(search); return HRESULT_FROM_WIN32(ERROR_CANCELLED);
      }
    } while (FindNextFileW(search, &found));
    DWORD error = GetLastError();
    FindClose(search);
    if (error != ERROR_NO_MORE_FILES) return HRESULT_FROM_WIN32(error);
    for (const auto &child : children) {
      HRESULT status = append(path + L"\\" + child, relative + L"\\" + child, cancelled, depth + 1);
      if (FAILED(status)) return status;
    }
    return S_OK;
  }
};

source_files::source_files(): state_(std::make_unique<impl>()) {}
source_files::~source_files() = default;
const std::vector<entry> &source_files::entries() const { return state_->entries; }
HRESULT source_files::collect(const std::vector<std::wstring> &paths,
                              const std::function<bool()> &cancelled,
                              std::shared_ptr<source_files> &result) {
  result.reset();
  if (paths.empty() || paths.size() > max_entries || !interactive_desktop_available()) return E_ACCESSDENIED;
  auto collection = std::shared_ptr<source_files>(new source_files);
  for (const auto &path : paths) {
    std::wstring full;
    HRESULT status = full_path(path, full);
    if (FAILED(status)) return status;
    auto name = std::filesystem::path(full).filename().wstring();
    status = collection->state_->append(full, name, cancelled, 0);
    if (FAILED(status)) return status;
  }
  result = std::move(collection);
  return S_OK;
}
HRESULT source_files::read_at(std::size_t index, std::uint64_t offset,
                             void *buffer, ULONG requested, ULONG &received) {
  received = 0;
  if (!interactive_desktop_available()) return E_ACCESSDENIED;
  if (index >= state_->entries.size() || state_->entries[index].directory() || requested > max_chunk_bytes) return E_INVALIDARG;
  const auto &descriptor = state_->entries[index];
  if (offset > descriptor.size || requested > descriptor.size - offset) return E_INVALIDARG;
  const auto &identity = state_->files[index];
  handle file;
  file.value = CreateFileW(identity.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
  BY_HANDLE_FILE_INFORMATION current {};
  if (!GetFileInformationByHandle(file.value, &current)) return HRESULT_FROM_WIN32(GetLastError());
  const auto &before = identity.information;
  if ((current.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
      current.dwVolumeSerialNumber != before.dwVolumeSerialNumber || current.nFileIndexHigh != before.nFileIndexHigh ||
      current.nFileIndexLow != before.nFileIndexLow || current.nFileSizeHigh != before.nFileSizeHigh ||
      current.nFileSizeLow != before.nFileSizeLow || CompareFileTime(&current.ftLastWriteTime, &before.ftLastWriteTime) != 0) return STG_E_REVERTED;
  LARGE_INTEGER position; position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(file.value, position, nullptr, FILE_BEGIN)) return HRESULT_FROM_WIN32(GetLastError());
  if (!ReadFile(file.value, buffer, requested, &received, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
  return received == requested ? S_OK : STG_E_READFAULT;
}
}
