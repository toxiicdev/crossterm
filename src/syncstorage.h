#pragma once
// Where the encrypted sync file (syncvault.h) is kept. Every storage only reads and writes that one file; the
// content is ciphertext, so none of them can read the data.
//
// Writes are conditional: write(data, revision) succeeds only while the stored file is still the one read with
// that revision (empty: none existed), otherwise Conflict and the caller reads, merges and tries again. WebDAV
// uses ETags and Gist its version id; the others compare the SHA-256 of the content right before writing.
//
// Settings (JSON):
//   {"type":"folder","path":<file or folder>}                       folder of a sync client: Dropbox, OneDrive,
//                                                                    iCloud Drive, Google Drive, Proton Drive...
//   {"type":"webdav","url":<https file URL>,"user":..,"password":..}
//   {"type":"sftp","session":<SSH session object>,"path":<remote file>}      key or saved password, never a prompt
//   {"type":"ftps","host":..,"port":..,"user":..,"password":..,"security":"explicit"|"implicit","path":..,
//    "fingerprint":<SHA-256 of the trusted certificate>}
//   {"type":"gist","token":<GitHub token, gist scope>,"gist":<id, empty: created on first write>}
// "api" (gist) and plain "http" (webdav) are accepted for 127.0.0.1/localhost only: tests and local servers.
#include <QJsonObject>
#include <functional>
#include <memory>

namespace SyncStorage
{
inline const char *const FileName = "crossterm-sync.ctvault";

struct Result
{
    enum Status
    {
        Ok,
        NotFound, // read: no sync file yet
        Conflict, // write: the file changed since it was read
        Failed
    } status = Failed;
    QByteArray data;     // read: the file
    QString revision;    // read and write: what the next conditional write expects
    QString error;       // Failed: message for the user
    QJsonObject updated; // settings to save after this call (the id of a Gist created by the first write)
    QString fingerprint; // FTPS: SHA-256 of a certificate that is not the trusted one (ask the user)
};
using Done = std::function<void(const Result &)>;

class Storage
{
public:
    virtual ~Storage() = default;
    virtual void read(Done done) = 0;
    // Writes data if the stored file is still at revision (empty: no file yet); Conflict otherwise.
    virtual void write(const QByteArray &data, const QString &revision, Done done) = 0;
};

// The storage described by settings, or nullptr with *error (missing field, unsupported type...).
std::unique_ptr<Storage> create(const QJsonObject &settings, QString *error = nullptr);
// Revision of storages without versions of their own: "sha256:<hex>".
QString contentRevision(const QByteArray &data);
} // namespace SyncStorage
