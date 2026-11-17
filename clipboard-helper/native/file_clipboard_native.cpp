#include "file_clipboard_native.h"
#include "file_clipboard_wire.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shldisp.h>
#include <wtsapi32.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <limits>
#include <mutex>
#include <set>
#include <thread>

namespace streaming::file_clipboard {
namespace {
CLIPFORMAT descriptor_format() { return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW")); }
CLIPFORMAT contents_format() { return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents")); }
CLIPFORMAT effect_format() { return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Preferred DropEffect")); }

HRESULT global_medium(std::size_t size, STGMEDIUM *medium, const std::function<void(void *)> &fill) {
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
  if (!memory) return E_OUTOFMEMORY;
  void *data = GlobalLock(memory);
  if (!data) { GlobalFree(memory); return E_OUTOFMEMORY; }
  fill(data);
  GlobalUnlock(memory);
  medium->tymed = TYMED_HGLOBAL;
  medium->hGlobal = memory;
  medium->pUnkForRelease = nullptr;
  return S_OK;
}

class file_stream final : public IStream {
public:
  file_stream(std::shared_ptr<offer> value, std::size_t index, std::uint64_t position = 0)
      : value_(std::move(value)), index_(index), position_(position) {
    CoCreateFreeThreadedMarshaler(static_cast<IStream *>(this), &marshaler_);
  }
  ~file_stream() { if (marshaler_) marshaler_->Release(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id == IID_IUnknown || id == IID_ISequentialStream || id == IID_IStream) {
      *out = static_cast<IStream *>(this); AddRef(); return S_OK;
    }
    if (id == IID_IMarshal && marshaler_) return marshaler_->QueryInterface(id, out);
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override { auto n = --references_; if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE Read(void *buffer, ULONG count, ULONG *read) override {
    if (read) *read = 0;
    if (!buffer && count) return STG_E_INVALIDPOINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!value_->valid.load()) return STG_E_REVERTED;
    const auto size = value_->entries[index_].size;
    ULONG total = 0;
    while (total < count && position_ < size) {
      ULONG wanted = static_cast<ULONG>(std::min<std::uint64_t>(
          std::min<ULONG>(max_chunk_bytes, count - total), size - position_));
      ULONG received = 0;
      HRESULT status = value_->source->read_at(index_, position_,
          static_cast<unsigned char *>(buffer) + total, wanted, received);
      if (FAILED(status)) { if (read) *read = total; return status; }
      if (received == 0 || received > wanted) { if (read) *read = total; return STG_E_READFAULT; }
      position_ += received;
      total += received;
      if (!value_->valid.load()) { if (read) *read = total; return STG_E_REVERTED; }
    }
    if (read) *read = total;
    return total == count ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *written) override {
    if (written) *written = 0;
    return STG_E_ACCESSDENIED;
  }
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *new_position) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t base = 0;
    if (origin == STREAM_SEEK_CUR) base = position_;
    else if (origin == STREAM_SEEK_END) base = value_->entries[index_].size;
    else if (origin != STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
    if (move.QuadPart < 0) {
      const auto magnitude = static_cast<std::uint64_t>(-(move.QuadPart + 1)) + 1;
      if (magnitude > base) return STG_E_INVALIDFUNCTION;
      position_ = base - magnitude;
    } else {
      if (static_cast<std::uint64_t>(move.QuadPart) > UINT64_MAX - base) return STG_E_INVALIDFUNCTION;
      position_ = base + static_cast<std::uint64_t>(move.QuadPart);
    }
    if (new_position) new_position->QuadPart = position_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
  HRESULT STDMETHODCALLTYPE CopyTo(IStream *destination, ULARGE_INTEGER count,
                                    ULARGE_INTEGER *read, ULARGE_INTEGER *written) override {
    if (!destination) return STG_E_INVALIDPOINTER;
    if (read) read->QuadPart = 0;
    if (written) written->QuadPart = 0;
    std::vector<unsigned char> buffer(max_chunk_bytes);
    std::uint64_t remaining = count.QuadPart;
    while (remaining) {
      ULONG got = 0, sent = 0;
      HRESULT status = Read(buffer.data(), static_cast<ULONG>(std::min<std::uint64_t>(remaining, buffer.size())), &got);
      if (FAILED(status)) return status;
      if (read) read->QuadPart += got;
      if (got) {
        HRESULT write_status = destination->Write(buffer.data(), got, &sent);
        if (written) written->QuadPart += sent;
        if (FAILED(write_status)) return write_status;
        if (sent != got) return STG_E_WRITEFAULT;
        remaining -= got;
      }
      if (status == S_FALSE) return S_OK;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
  HRESULT STDMETHODCALLTYPE Stat(STATSTG *stat, DWORD flags) override {
    if (!stat) return STG_E_INVALIDPOINTER;
    std::memset(stat, 0, sizeof(*stat));
    const auto &item = value_->entries[index_];
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = item.size;
    stat->mtime = item.last_write;
    stat->grfMode = STGM_READ | STGM_SHARE_DENY_NONE;
    if (!(flags & STATFLAG_NONAME)) {
      const auto bytes = (item.relative_path.size() + 1) * sizeof(wchar_t);
      stat->pwcsName = static_cast<wchar_t *>(CoTaskMemAlloc(bytes));
      if (!stat->pwcsName) return E_OUTOFMEMORY;
      std::memcpy(stat->pwcsName, item.relative_path.c_str(), bytes);
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(IStream **out) override {
    if (!out) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    *out = new (std::nothrow) file_stream(value_, index_, position_);
    return *out ? S_OK : E_OUTOFMEMORY;
  }
private:
  std::atomic<ULONG> references_ {1};
  std::shared_ptr<offer> value_;
  std::size_t index_;
  std::uint64_t position_;
  std::mutex mutex_;
  IUnknown *marshaler_ = nullptr;
};

class data_object final : public IDataObject, public IDataObjectAsyncCapability {
public:
  explicit data_object(std::shared_ptr<offer> value): value_(std::move(value)) {
    // IDataObject stays STA-marshalled. Free-threaded content IStreams remain
    // separate: OLE's clipboard wrapper lost the registered descriptor format
    // when this IDataObject itself used the free-threaded marshaler.
    formats_ = {{{ descriptor_format(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
                 { contents_format(), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM },
                 { effect_format(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL }}};
  }
  ~data_object() = default;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id == IID_IUnknown || id == IID_IDataObject) *out = static_cast<IDataObject *>(this);
    else if (id == __uuidof(IDataObjectAsyncCapability)) *out = static_cast<IDataObjectAsyncCapability *>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override { auto n = --references_; if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override {
    if (!format || !medium) return E_POINTER;
    std::memset(medium, 0, sizeof(*medium));
    if (!value_->valid.load()) return STG_E_REVERTED;
    HRESULT supported = QueryGetData(format);
    if (FAILED(supported)) return supported;
    if (format->cfFormat == descriptor_format()) {
      const auto size = offsetof(FILEGROUPDESCRIPTORW, fgd) + value_->entries.size() * sizeof(FILEDESCRIPTORW);
      return global_medium(size, medium, [&](void *memory) {
        auto *group = static_cast<FILEGROUPDESCRIPTORW *>(memory);
        group->cItems = static_cast<UINT>(value_->entries.size());
        for (std::size_t i = 0; i < value_->entries.size(); ++i) {
          const auto &item = value_->entries[i];
          auto &descriptor = group->fgd[i];
          descriptor.dwFlags = FD_ATTRIBUTES | FD_UNICODE | FD_WRITESTIME | FD_PROGRESSUI;
          descriptor.dwFileAttributes = item.attributes;
          descriptor.ftLastWriteTime = item.last_write;
          if (!item.directory()) {
            descriptor.dwFlags |= FD_FILESIZE;
            descriptor.nFileSizeHigh = static_cast<DWORD>(item.size >> 32);
            descriptor.nFileSizeLow = static_cast<DWORD>(item.size);
          }
          std::memcpy(descriptor.cFileName, item.relative_path.c_str(),
                      (item.relative_path.size() + 1) * sizeof(wchar_t));
        }
      });
    }
    if (format->cfFormat == effect_format()) {
      return global_medium(sizeof(DWORD), medium, [](void *memory) {
        *static_cast<DWORD *>(memory) = DROPEFFECT_COPY;
      });
    }
    if (format->cfFormat == contents_format()) {
      if (format->lindex < 0 || static_cast<std::size_t>(format->lindex) >= value_->entries.size() ||
          value_->entries[format->lindex].directory()) return DV_E_LINDEX;
      medium->pstm = new (std::nothrow) file_stream(value_, static_cast<std::size_t>(format->lindex));
      if (!medium->pstm) return E_OUTOFMEMORY;
      medium->tymed = TYMED_ISTREAM;
      return S_OK;
    }
    return DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return DATA_E_FORMATETC; }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override {
    if (!format) return E_POINTER;
    if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    if (format->cfFormat == descriptor_format() || format->cfFormat == effect_format())
      return (format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;
    if (format->cfFormat == contents_format())
      return (format->tymed & TYMED_ISTREAM) ? S_OK : DV_E_TYMED;
    return DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override {
    if (out) out->ptd = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *format, STGMEDIUM *medium, BOOL release) override {
    // Completion notifications never authorize deleting source files.
    if (!format || !medium) return E_POINTER;
    const auto performed = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Performed DropEffect"));
    const auto pasted = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Paste Succeeded"));
    if (format->cfFormat != performed && format->cfFormat != pasted) return DV_E_FORMATETC;
    if (release) ReleaseStgMedium(medium);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override {
    if (direction != DATADIR_GET) return E_NOTIMPL;
    return SHCreateStdEnumFmtEtc(static_cast<UINT>(formats_.size()), formats_.data(), out);
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL value) override { asynchronous_ = value != FALSE; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL *out) override { if (!out) return E_POINTER; *out = asynchronous_ ? TRUE : FALSE; return S_OK; }
  HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx *) override { in_operation_ = true; return S_OK; }
  HRESULT STDMETHODCALLTYPE InOperation(BOOL *out) override { if (!out) return E_POINTER; *out = in_operation_ ? TRUE : FALSE; return S_OK; }
  HRESULT STDMETHODCALLTYPE EndOperation(HRESULT, IBindCtx *, DWORD) override { in_operation_ = false; return S_OK; }
private:
  std::atomic<ULONG> references_ {1};
  std::shared_ptr<offer> value_;
  std::array<FORMATETC, 3> formats_ {};
  std::atomic<bool> asynchronous_ {true};
  std::atomic<bool> in_operation_ {false};
};
}

bool valid_relative_path(const std::wstring &path) {
  if (path.empty() || path.size() >= MAX_PATH || path.front() == L'\\' || path.front() == L'/') return false;
  std::size_t begin = 0;
  while (begin < path.size()) {
    auto end = path.find(L'\\', begin);
    if (end == std::wstring::npos) end = path.size();
    auto part = path.substr(begin, end - begin);
    if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
    for (std::size_t i = 0; i < part.size(); ++i) {
      wchar_t c = part[i];
      if (c < 32 || std::wstring(L"<>:\"/|?*").find(c) != std::wstring::npos) return false;
      if (c >= 0xd800 && c <= 0xdbff) {
        if (++i == part.size() || part[i] < 0xdc00 || part[i] > 0xdfff) return false;
      } else if (c >= 0xdc00 && c <= 0xdfff) return false;
    }
    auto stem = part.substr(0, part.find(L'.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return std::towupper(c); });
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") &&
          ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' || stem[3] == L'\u00b2' || stem[3] == L'\u00b3'))) return false;
    if (end == path.size()) return true;
    begin = end + 1;
    if (begin == path.size()) return false;
  }
  return false;
}

HRESULT create_data_object(std::shared_ptr<offer> value, IDataObject **result) {
  if (!result) return E_POINTER;
  *result = nullptr;
  if (!value || !value->source || value->entries.empty() || value->entries.size() > max_entries) return E_INVALIDARG;
  if (!wire::valid_manifest(value->entries)) return E_INVALIDARG;
  *result = new (std::nothrow) data_object(std::move(value));
  return *result ? S_OK : E_OUTOFMEMORY;
}

struct publisher::impl {
  std::mutex mutex;
  std::condition_variable ready;
  std::thread thread;
  HWND window = nullptr;
  bool initialized = false;
  HRESULT status = E_PENDING;
  IDataObject *current = nullptr;
  std::shared_ptr<offer> current_offer;
  publisher::clipboard_change observer;
  DWORD last_sequence = 0;
  unsigned clipboard_retries = 0;
  bool locked = false;
  void changed(HWND window) {
    if (!observer) return;
    DWORD sequence = GetClipboardSequenceNumber();
    if (sequence == last_sequence) return;
    if (current && OleIsCurrentClipboard(current) == S_OK) { last_sequence = sequence; return; }
    if (current_offer) current_offer->valid = false;
    if (locked || !interactive_desktop_available()) {
      last_sequence = sequence;
      observer(sequence, {});
      return;
    }
    if (!OpenClipboard(window)) {
      if (++clipboard_retries <= 10) SetTimer(window, 1, 100, nullptr);
      return;
    }
    std::vector<std::wstring> paths;
    bool copy = true;
    HANDLE effect = GetClipboardData(effect_format());
    if (effect && GlobalSize(effect) >= sizeof(DWORD)) {
      if (auto *value = static_cast<DWORD *>(GlobalLock(effect))) {
        copy = (*value & DROPEFFECT_MOVE) == 0;
        GlobalUnlock(effect);
      }
    }
    if (copy && IsClipboardFormatAvailable(CF_HDROP)) {
      const bool shell_files = IsClipboardFormatAvailable(RegisterClipboardFormatW(L"Shell IDList Array"));
      const bool image = IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_DIBV5) ||
                         IsClipboardFormatAvailable(RegisterClipboardFormatW(L"PNG"));
      if (image && !shell_files) copy = false;
    }
    if (copy && IsClipboardFormatAvailable(CF_HDROP)) {
      HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
      UINT count = drop ? DragQueryFileW(drop, 0xffffffffu, nullptr, 0) : 0;
      std::size_t path_characters = 0;
      if (count <= max_entries) {
        for (UINT i = 0; i < count; ++i) {
          UINT length = DragQueryFileW(drop, i, nullptr, 0);
          if (!length || length > 32766 || length > wire::max_manifest_bytes / sizeof(wchar_t) - path_characters) { paths.clear(); break; }
          path_characters += length;
          std::vector<wchar_t> path(length + 1);
          if (DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())) != length) { paths.clear(); break; }
          paths.emplace_back(path.data());
        }
      }
    }
    CloseClipboard();
    clipboard_retries = 0;
    last_sequence = sequence;
    observer(sequence, std::move(paths));
  }
  struct request { std::shared_ptr<offer> value; HRESULT result = E_PENDING; };
  static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto *self = reinterpret_cast<impl *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      self = static_cast<impl *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self && message == WM_APP + 1) {
      auto &call = *reinterpret_cast<request *>(lparam);
      IDataObject *object = nullptr;
      bool changed_clipboard = false;
      call.result = call.value ? create_data_object(call.value, &object) : S_OK;
      if (SUCCEEDED(call.result)) {
        if (object) { call.result = OleSetClipboard(object); changed_clipboard = SUCCEEDED(call.result); }
        else if (self->current && OleIsCurrentClipboard(self->current) == S_OK) {
          call.result = OleSetClipboard(nullptr); changed_clipboard = SUCCEEDED(call.result);
        }
      }
      if (SUCCEEDED(call.result)) {
        if (self->current_offer) self->current_offer->valid = false;
        if (self->current) self->current->Release();
        self->current = object;
        self->current_offer = call.value;
        if (changed_clipboard) self->last_sequence = GetClipboardSequenceNumber();
      } else if (object) object->Release();
      return 0;
    }
    if (self && message == WM_APP + 2) { PostQuitMessage(0); return 0; }
    if (self && message == WM_APP + 3) {
      self->observer = *reinterpret_cast<publisher::clipboard_change *>(lparam);
      self->last_sequence = GetClipboardSequenceNumber();
      return 0;
    }
    if (self && message == WM_CLIPBOARDUPDATE) { self->changed(window); return 0; }
    if (self && message == WM_TIMER && wparam == 1) { KillTimer(window, 1); self->changed(window); return 0; }
    if (self && message == WM_TIMER && wparam == 2) {
      const bool unavailable = !interactive_desktop_available();
      if (unavailable && !self->locked) {
        self->locked = true;
        request clear;
        SendMessageW(window, WM_APP + 1, 0, reinterpret_cast<LPARAM>(&clear));
        if (self->observer) self->observer(GetClipboardSequenceNumber(), {});
      } else if (!unavailable) self->locked = false;
      return 0;
    }
    if (self && message == WM_WTSSESSION_CHANGE) {
      if (wparam == WTS_SESSION_LOCK || wparam == WTS_SESSION_LOGOFF || wparam == WTS_CONSOLE_DISCONNECT || wparam == WTS_REMOTE_DISCONNECT) {
        self->locked = true;
        if (self->current_offer) self->current_offer->valid = false;
        request clear;
        SendMessageW(window, WM_APP + 1, 0, reinterpret_cast<LPARAM>(&clear));
        if (self->observer) self->observer(GetClipboardSequenceNumber(), {});
      } else if (wparam == WTS_SESSION_UNLOCK || wparam == WTS_SESSION_LOGON || wparam == WTS_CONSOLE_CONNECT || wparam == WTS_REMOTE_CONNECT) self->locked = false;
      return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }
  impl() {
    thread = std::thread([this] {
      HRESULT apartment = OleInitialize(nullptr);
      HWND created = nullptr;
      if (SUCCEEDED(apartment)) {
        WNDCLASSW type {};
        type.lpfnWndProc = procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"StreamingFileClipboardSTA";
        if (RegisterClassW(&type) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
          created = CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, type.hInstance, this);
        }
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        window = created;
        initialized = true;
        status = FAILED(apartment) ? apartment : created ? S_OK : HRESULT_FROM_WIN32(GetLastError());
      }
      ready.notify_all();
      if (FAILED(apartment)) return;
      if (!created) { OleUninitialize(); return; }
      AddClipboardFormatListener(created);
      WTSRegisterSessionNotification(created, NOTIFY_FOR_THIS_SESSION);
      SetTimer(created, 2, 250, nullptr);
      MSG message;
      while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
      }
      if (current) {
        if (OleIsCurrentClipboard(current) == S_OK) OleSetClipboard(nullptr);
        current->Release(); current = nullptr;
      }
      WTSUnRegisterSessionNotification(created);
      RemoveClipboardFormatListener(created);
      DestroyWindow(created);
      OleUninitialize();
    });
    std::unique_lock<std::mutex> lock(mutex);
    ready.wait(lock, [&] { return initialized; });
  }
  ~impl() {
    if (thread.joinable()) {
      if (window) {
        request clear;
        SendMessageW(window, WM_APP + 1, 0, reinterpret_cast<LPARAM>(&clear));
        PostMessageW(window, WM_APP + 2, 0, 0);
      }
      thread.join();
    }
  }
};

publisher::publisher(): state_(std::make_unique<impl>()) {}
publisher::~publisher() = default;
HRESULT publisher::publish(std::shared_ptr<offer> value) {
  if (FAILED(state_->status)) return state_->status;
  impl::request call {std::move(value), E_PENDING};
  // SendMessage dispatches incoming nonqueued messages while waiting. A bare
  // condition-variable wait here would deadlock if this caller owned the
  // previous clipboard and OLE sent it WM_DESTROYCLIPBOARD during replacement.
  SendMessageW(state_->window, WM_APP + 1, 0, reinterpret_cast<LPARAM>(&call));
  return call.result;
}
void publisher::clear() { publish({}); }
void publisher::observe(clipboard_change callback) {
  if (SUCCEEDED(state_->status)) SendMessageW(state_->window, WM_APP + 3, 0, reinterpret_cast<LPARAM>(&callback));
}
}
