#include "fileclipboardsync.h"
#include "native/file_clipboard_wire.h"
#include "backend/pairedcertificate.h"
#include "streaming/clipboardlogging.h"
#include <QEventLoop>
#include <QNetworkReply>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <mutex>

namespace fc = streaming::file_clipboard;
namespace wire = fc::wire;
namespace {
QByteArray array(const wire::bytes& data) { return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size())); }
wire::bytes bytes(const QByteArray& data) { return wire::bytes(data.begin(), data.end()); }
struct Transport {
    ClipboardSyncHostContext context;
    QByteArray session;
    std::atomic<bool> valid {true};
};
QNetworkRequest request(const ClipboardSyncHostContext& context, const QByteArray& session, const QString& tail)
{
    QUrl url;
    url.setScheme(QStringLiteral("https")); url.setHost(context.address); url.setPort(context.httpsPort);
    url.setPath(QStringLiteral("/api/v1/clipboard/files/") + tail);
    QNetworkRequest result(url);
    auto ssl = QSslConfiguration::defaultConfiguration();
    ssl.setLocalCertificate(context.clientCertificate); ssl.setPrivateKey(context.clientPrivateKey);
    result.setSslConfiguration(ssl);
    result.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    result.setRawHeader("Content-Type", "application/octet-stream");
    result.setRawHeader("X-Clipboard-Session", session);
    result.setTransferTimeout(18000);
    return result;
}
void pin(QNetworkAccessManager* manager, QObject* parent, const QSslCertificate& certificate)
{
    PairedCertificate::enforce(manager, parent, certificate);
    QObject::connect(manager, &QNetworkAccessManager::sslErrors, parent,
                     [certificate](QNetworkReply* reply, const QList<QSslError>& errors) {
        if (PairedCertificate::canIgnoreErrors(certificate, errors)) reply->ignoreSslErrors(errors);
    });
}
bool accepted(QNetworkReply* reply, const QSslCertificate& pinned)
{
    return reply->error() == QNetworkReply::NoError &&
        PairedCertificate::matches(pinned, reply->sslConfiguration().peerCertificate());
}
// Each COM reader uses its own event loop. It never waits for the clipboard/UI
// thread, including when Explorer calls IStream while that thread is in OLE.
HRESULT remoteRead(const std::shared_ptr<Transport>& transport, const wire::read_query& query,
                   void* buffer, ULONG count, ULONG& received)
{
    received = 0;
    if (!transport->valid || !fc::interactive_desktop_available()) return STG_E_REVERTED;
    QNetworkAccessManager manager; pin(&manager, &manager, transport->context.serverCertificate);
    auto reply = manager.post(request(transport->context, transport->session, QStringLiteral("read")),
                              array(wire::encode({wire::kind::request, 0, wire::query(query)})));
    reply->setReadBufferSize(fc::max_chunk_bytes + 4);
    QByteArray content; bool overflow = false;
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [&] {
        content += reply->readAll();
        if (content.size() > static_cast<qsizetype>(fc::max_chunk_bytes + 4)) { overflow = true; reply->abort(); }
    });
    QEventLoop loop; QTimer timer, cancellation; timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(&cancellation, &QTimer::timeout, reply, [transport, reply] { if (!transport->valid) reply->abort(); });
    cancellation.start(100);
    timer.start(18000); if (!reply->isFinished()) loop.exec();
    content += reply->readAll();
    if (!transport->valid || overflow || !accepted(reply, transport->context.serverCertificate) ||
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return STG_E_READFAULT;
    return wire::reply(bytes(content), buffer, count, received);
}
struct RemoteReader final : fc::reader {
    std::shared_ptr<Transport> transport; std::uint64_t generation;
    std::timed_mutex gate;
    HRESULT read_at(std::size_t index, std::uint64_t offset, void* buffer, ULONG count, ULONG& got) override {
        got = 0;
        std::unique_lock<std::timed_mutex> lock(gate, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::seconds(20))) return STG_E_INUSE;
        return remoteRead(transport, {generation, static_cast<std::uint32_t>(index), offset, count}, buffer, count, got);
    }
};
}

struct FileClipboardSync::Impl {
    FileClipboardSync* owner; ClipboardSyncHostContext context;
    std::unique_ptr<QNetworkAccessManager> manager;
    QTimer retry; QPointer<QNetworkReply> events;
    std::shared_ptr<Transport> transport;
    std::atomic<std::uint64_t> generation {1};
    std::atomic<bool> stopping {false};
    std::mutex mutex; std::shared_ptr<fc::source_files> source;
    fc::publisher publisher;
    std::unique_ptr<wire::workers> workers;
    bool opening = false, unsupported = false;
    wire::message pendingOffer;
    bool havePendingOffer = false;
    explicit Impl(FileClipboardSync* object, ClipboardSyncHostContext value): owner(object), context(std::move(value)) {
        manager = std::make_unique<QNetworkAccessManager>(); pin(manager.get(), owner, context.serverCertificate);
        workers = std::make_unique<wire::workers>();
        QObject::connect(&retry, &QTimer::timeout, owner, [this] { if (!transport) open(); else poll(); });
        retry.setInterval(1500); retry.start();
        publisher.observe([this](std::uint32_t, std::vector<std::wstring> paths) {
            auto epoch = ++generation;
            { std::lock_guard<std::mutex> lock(mutex); source.reset(); }
            QMetaObject::invokeMethod(owner, [this] { havePendingOffer = false; send({wire::kind::offer, 0, {}}); }, Qt::QueuedConnection);
            if (paths.empty()) return;
            workers->post([this, epoch, paths = std::move(paths)] {
                std::shared_ptr<fc::source_files> collected;
                auto status = fc::source_files::collect(paths, [this, epoch] { return stopping || generation != epoch; }, collected);
                if (FAILED(status)) {
                    if (generation == epoch && !stopping) ClipboardLog::warn("FileClipboard: cannot prepare file selection (0x%08lx)", static_cast<unsigned long>(status));
                    return;
                }
                if (stopping || generation != epoch) return;
                auto manifest = wire::manifest(collected->entries()); if (manifest.empty()) return;
                QMetaObject::invokeMethod(owner, [this, epoch, collected, manifest = std::move(manifest)]() mutable {
                    if (stopping || generation != epoch) return;
                    { std::lock_guard<std::mutex> lock(mutex); source = collected; }
                    wire::message value {wire::kind::offer, epoch, std::move(manifest)};
                    if (transport) send(std::move(value));
                    else { pendingOffer = std::move(value); havePendingOffer = true; }
                }, Qt::QueuedConnection);
            });
        });
        QTimer::singleShot(250, owner, [this] { open(); });
    }
    ~Impl() {
        stopping = true; retry.stop(); generation++;
        publisher.observe({}); publisher.clear();
        if (transport) transport->valid = false;
        if (events) events->abort();
        workers.reset();
        manager.reset();
    }
    void reset() {
        if (transport) transport->valid = false;
        transport.reset(); generation++;
        havePendingOffer = false;
        { std::lock_guard<std::mutex> lock(mutex); source.reset(); }
        publisher.clear();
    }
    void bounded(QNetworkReply* reply, qsizetype limit) {
        reply->setReadBufferSize(limit + 1);
        QObject::connect(reply, &QNetworkReply::readyRead, owner, [reply, limit] {
            if (reply->bytesAvailable() > limit) reply->abort();
        });
    }
    void open() {
        if (opening || unsupported || stopping) return;
        opening = true;
        auto reply = manager->post(request(context, {}, QStringLiteral("open")), QByteArray()); bounded(reply, 128);
        QObject::connect(reply, &QNetworkReply::finished, owner, [this, reply] {
            opening = false;
            auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            auto key = reply->readAll();
            if (status == 404) unsupported = true;
            if (accepted(reply, context.serverCertificate) && status == 200 && key.size() == 64) {
                transport = std::make_shared<Transport>(); transport->context = context; transport->session = key;
                ClipboardLog::info("FileClipboard: connected (copy only, files + folders)");
                if (havePendingOffer) {
                    if (pendingOffer.id == generation) send(std::move(pendingOffer));
                    havePendingOffer = false;
                }
                poll();
            }
            reply->deleteLater();
        });
    }
    void poll() {
        if (events || !transport || stopping) return;
        auto current = transport;
        auto reply = manager->get(request(context, current->session, QStringLiteral("events"))); events = reply;
        bounded(reply, wire::max_manifest_bytes + 16);
        QObject::connect(reply, &QNetworkReply::finished, owner, [this, current, reply] {
            if (events == reply) events = nullptr;
            auto body = reply->readAll();
            auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            bool okay = current == transport && accepted(reply, context.serverCertificate);
            if (okay && status == 200) {
                wire::message value;
                if (wire::decode(bytes(body), value)) receive(std::move(value)); else okay = false;
            } else if (status != 204) okay = false;
            reply->deleteLater();
            if (stopping || current != transport) return;
            if (!okay) { reset(); return; }
            QTimer::singleShot(0, owner, [this] { poll(); });
        });
    }
    void send(wire::message value) {
        if (!transport || stopping) return;
        auto current = transport;
        auto reply = manager->post(request(context, current->session, QStringLiteral("send")), array(wire::encode(value)));
        bounded(reply, 1024);
        QObject::connect(reply, &QNetworkReply::finished, owner, [this, current, reply] {
            auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (current == transport && (status == 403 || status == 410)) reset();
            reply->deleteLater();
        });
    }
    void receive(wire::message value) {
        if (value.type == wire::kind::offer) {
            if (!value.id) { publisher.clear(); return; }
            generation++; havePendingOffer = false;
            { std::lock_guard<std::mutex> lock(mutex); source.reset(); }
            auto offer = std::make_shared<fc::offer>();
            if (!wire::manifest(value.body, offer->entries) || !fc::interactive_desktop_available()) return;
            auto reader = std::make_shared<RemoteReader>(); reader->transport = transport; reader->generation = value.id;
            offer->source = std::move(reader);
            if (FAILED(publisher.publish(std::move(offer)))) ClipboardLog::warn("FileClipboard: cannot publish remote file list");
        } else if (value.type == wire::kind::request) {
            wire::read_query query; std::shared_ptr<fc::source_files> local;
            { std::lock_guard<std::mutex> lock(mutex); local = source; }
            if (!wire::query(value.body, query) || !local || !wire::within(query, generation, local->entries())) {
                send({wire::kind::reply, value.id, wire::reply(STG_E_REVERTED)}); return;
            }
            auto id = value.id;
            if (!workers->post([this, local, query, id] {
                wire::bytes data(query.count); ULONG got = 0;
                auto status = generation == query.generation ? local->read_at(query.index, query.offset, data.data(), query.count, got) : STG_E_REVERTED;
                if (generation != query.generation) status = STG_E_REVERTED;
                auto body = wire::reply(status, data.data(), SUCCEEDED(status) ? got : 0);
                QMetaObject::invokeMethod(owner, [this, id, body = std::move(body)]() mutable {
                    if (!stopping) send({wire::kind::reply, id, std::move(body)});
                }, Qt::QueuedConnection);
            })) send({wire::kind::reply, id, wire::reply(STG_E_INUSE)});
        }
    }
};
FileClipboardSync::FileClipboardSync(const ClipboardSyncHostContext& context, QObject* parent): QObject(parent), state(std::make_unique<Impl>(this, context)) {}
FileClipboardSync::~FileClipboardSync() = default;
void FileClipboardSync::clearRemote() { state->publisher.clear(); }
