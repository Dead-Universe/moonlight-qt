#pragma once
#include "streaming/clipboardsync.h"
#include <memory>
class FileClipboardSync final : public QObject
{
public:
    explicit FileClipboardSync(const ClipboardSyncHostContext& context, QObject* parent = nullptr);
    ~FileClipboardSync() override;
    // Releases only this engine's remote virtual files, preserving a local copy.
    void clearRemote();
private:
    struct Impl;
    std::unique_ptr<Impl> state;
};
