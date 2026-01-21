#include "library/tfm/tfmtrackmodel.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include "library/tfm/tfmapiclient.h"
#include "library/trackcollectionmanager.h"
#include "track/track.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("TFMTrackModel");
const char* kSettingsNamespace = "TFMTrackModel";
const QString kTFMTracksTable = QStringLiteral("tfm_tracks");
const int kDownloadTimeoutMs = 60000; // 60 second timeout for downloads
} // anonymous namespace

TFMTrackModel::TFMTrackModel(QObject* parent,
                             TrackCollectionManager* pTrackCollectionManager,
                             QSharedPointer<BaseTrackCache> trackSource,
                             tfm::TFMApiClient* pApiClient)
        : BaseExternalTrackModel(parent,
                                 pTrackCollectionManager,
                                 kSettingsNamespace,
                                 kTFMTracksTable,
                                 trackSource),
          m_pApiClient(pApiClient) {
    // Set up cache directory - use forward slashes for consistency
    m_cacheDir = QDir::cleanPath(
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/tfm_tracks");
    QDir().mkpath(m_cacheDir);

    kLogger.info() << "TFM track cache directory:" << m_cacheDir;
}

TFMTrackModel::~TFMTrackModel() {
}

TrackPointer TFMTrackModel::getTrack(const QModelIndex& index) const {
    QString artist = getFieldString(index, ColumnCache::COLUMN_LIBRARYTABLE_ARTIST);
    QString title = getFieldString(index, ColumnCache::COLUMN_LIBRARYTABLE_TITLE);
    QString album = getFieldString(index, ColumnCache::COLUMN_LIBRARYTABLE_ALBUM);
    QString genre = getFieldString(index, ColumnCache::COLUMN_LIBRARYTABLE_GENRE);
    float bpm = getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_BPM).toFloat();

    QString location = getTrackLocation(index);
    QString trackName = title.isEmpty() ?
            index.sibling(index.row(), fieldIndex("title")).data().toString() : title;

    if (location.isEmpty()) {
        kLogger.warning() << "Track has no location";
        return TrackPointer();
    }

    // Check if it's a URL - if so, we need to download it first
    if (location.startsWith("http://") || location.startsWith("https://")) {
        // Get the external_id (MongoDB ObjectId) for cache path - more unique than integer id
        QString externalId = index.sibling(index.row(), fieldIndex("external_id")).data().toString();
        if (externalId.isEmpty()) {
            // Fallback to integer id if external_id not available
            externalId = index.sibling(index.row(), fieldIndex("id")).data().toString();
        }
        // Sanitize externalId - replace path separators with underscores for valid filename
        QString sanitizedId = externalId;
        sanitizedId.replace('/', '_');
        sanitizedId.replace('\\', '_');
        sanitizedId.replace(':', '_');

        QString fileExt = getFileExtension(trackName, location);
        // Avoid double extension if sanitizedId already ends with the extension
        if (sanitizedId.endsWith(fileExt, Qt::CaseInsensitive)) {
            sanitizedId = sanitizedId.left(sanitizedId.length() - fileExt.length());
        }
        // Use forward slash for path consistency on all platforms
        QString localPath = m_cacheDir + "/" + sanitizedId + fileExt;

        // Get expected file size from database (from API response)
        qint64 expectedSize = index.sibling(index.row(), fieldIndex("file_size")).data().toLongLong();

        // Check if we have a cached version with correct size
        QFileInfo cachedFile(localPath);
        if (cachedFile.exists()) {
            qint64 cachedSize = cachedFile.size();
            // Validate size: must be > 1KB and match expected size (if known)
            bool sizeValid = cachedSize > 1000;
            bool sizeMatches = (expectedSize <= 0) || (cachedSize == expectedSize);

            if (sizeValid && sizeMatches) {
                kLogger.info() << "Using cached track:" << localPath << "size:" << cachedSize;
                location = localPath;
            } else {
                // Cached file is invalid - remove it
                if (!sizeValid) {
                    kLogger.warning() << "Cached file too small, removing:" << localPath
                                      << "size:" << cachedSize;
                } else {
                    kLogger.warning() << "Cached file size mismatch, removing:" << localPath
                                      << "cached:" << cachedSize << "expected:" << expectedSize;
                }
                QFile::remove(localPath);
            }
        }

        if (location != localPath) {  // No valid cache or it was removed
            // Try to get local_path from the model - must be a valid file path (not "/" or similar)
            QString storedLocalPath = index.sibling(index.row(), fieldIndex("local_path")).data().toString();
            if (storedLocalPath.length() > 5 && QFileInfo(storedLocalPath).isFile()) {
                kLogger.info() << "Using stored local path:" << storedLocalPath;
                location = storedLocalPath;
            } else {
                // Download the track before loading
                kLogger.info() << "Downloading track from:" << location << "to:" << localPath
                               << "expected size:" << expectedSize;
                QString downloadedPath = downloadTrackSync(location, localPath, expectedSize);
                if (!downloadedPath.isEmpty()) {
                    kLogger.info() << "Track downloaded successfully to:" << downloadedPath;
                    location = downloadedPath;
                } else {
                    kLogger.warning() << "Failed to download track from:" << location;
                    return TrackPointer();
                }
            }
        }
    }

    bool track_already_in_library = false;
    TrackPointer pTrack = m_pTrackCollectionManager->getOrAddTrack(
            TrackRef::fromFilePath(location),
            &track_already_in_library);

    if (pTrack) {
        // Set metadata from TFM if track is new
        if (!track_already_in_library) {
            pTrack->setArtist(artist);
            pTrack->setTitle(title);
            pTrack->setAlbum(album);
            updateTrackGenre(pTrack.get(), genre);
            if (bpm > 0) {
                pTrack->trySetBpm(bpm);
            }
        }
    } else {
        kLogger.warning() << "Failed to load TFM track from" << location;
    }

    return pTrack;
}

QString TFMTrackModel::getTrackLocation(const QModelIndex& index) const {
    // First check if there's a valid local_path that exists as a file
    QString localPath = index.sibling(index.row(), fieldIndex("local_path")).data().toString();
    // Must be a real path (not "/", "D:/", etc.) and exist as a file
    if (localPath.length() > 5 && QFileInfo(localPath).isFile()) {
        return localPath;
    }

    // Use file_url (download URL) - better for downloading full file
    QString fileUrl = index.sibling(index.row(), fieldIndex("file_url")).data().toString();
    if (!fileUrl.isEmpty() && fileUrl.startsWith("http")) {
        return fileUrl;
    }

    // Fall back to location (stream URL)
    QString location = index.sibling(index.row(), fieldIndex("location")).data().toString();
    if (!location.isEmpty() && location.startsWith("http")) {
        return location;
    }

    kLogger.warning() << "getTrackLocation: No valid location found. local_path:" << localPath
                      << "file_url:" << fileUrl << "location:" << location;
    return QString();
}

QString TFMTrackModel::resolveLocation(const QString& nativeLocation) const {
    // Handle both URLs and local paths
    if (nativeLocation.startsWith("http://") || nativeLocation.startsWith("https://")) {
        // Return URL as-is
        return nativeLocation;
    }
    return QDir::fromNativeSeparators(nativeLocation);
}

TrackModel::Capabilities TFMTrackModel::getCapabilities() const {
    return Capability::AddToTrackSet |
            Capability::AddToAutoDJ |
            Capability::LoadToDeck |
            Capability::LoadToPreviewDeck |
            Capability::LoadToSampler |
            Capability::Sorting;
}

bool TFMTrackModel::needsDownload(const QString& location) const {
    if (location.startsWith("http://") || location.startsWith("https://")) {
        return true;
    }
    return !QFile::exists(location);
}

QString TFMTrackModel::getCachePath(const QString& trackId) const {
    return m_cacheDir + "/" + trackId + ".mp3";
}

QString TFMTrackModel::downloadTrackSync(const QString& url, const QString& destPath, qint64 expectedSize) const {
    kLogger.info() << "Starting download from" << url << "expected size:" << expectedSize;

    QUrl qurl(url);
    QNetworkRequest netRequest;
    netRequest.setUrl(qurl);
    netRequest.setRawHeader("Accept", "*/*");
    // Follow redirects automatically
    netRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                           QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = m_downloadManager.get(netRequest);

    // Set up event loop with timeout
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    timer.start(kDownloadTimeoutMs);
    loop.exec();

    if (timer.isActive()) {
        timer.stop();
    } else {
        // Timeout occurred
        kLogger.warning() << "Download timed out for" << url;
        reply->abort();
        reply->deleteLater();
        return QString();
    }

    if (reply->error() != QNetworkReply::NoError) {
        kLogger.warning() << "Download error:" << reply->errorString();
        reply->deleteLater();
        return QString();
    }

    // Check HTTP status code
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode != 200) {
        kLogger.warning() << "Download failed with HTTP status:" << statusCode;
        reply->deleteLater();
        return QString();
    }

    // Check Content-Type to ensure it's not an error page
    QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    if (contentType.contains("text/html", Qt::CaseInsensitive)) {
        kLogger.warning() << "Server returned HTML instead of audio file. Content-Type:" << contentType;
        reply->deleteLater();
        return QString();
    }

    // Get Content-Length header before reading data
    qint64 contentLength = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();

    // Read file data
    QByteArray data = reply->readAll();
    reply->deleteLater();

    if (data.isEmpty()) {
        kLogger.warning() << "Downloaded file is empty";
        return QString();
    }

    // Validate size against Content-Length header
    if (contentLength > 0 && data.size() != contentLength) {
        kLogger.warning() << "Downloaded size mismatch with Content-Length: expected" << contentLength << "got" << data.size();
        if (data.size() < contentLength * 0.99) {
            kLogger.warning() << "Download appears truncated (Content-Length), aborting";
            return QString();
        }
    }

    // Validate size against expected size from API (more reliable than Content-Length)
    if (expectedSize > 0 && data.size() != expectedSize) {
        kLogger.warning() << "Downloaded size mismatch with API size: expected" << expectedSize << "got" << data.size();
        if (data.size() < expectedSize * 0.99) {  // Allow 1% tolerance
            kLogger.warning() << "Download appears truncated (API size), aborting";
            return QString();
        }
    }

    // Basic validation: check for common audio file headers
    bool validAudio = false;
    if (data.size() >= 4) {
        // Check for FLAC header "fLaC"
        if (data.startsWith("fLaC")) {
            validAudio = true;
        }
        // Check for MP3 (ID3 tag or frame sync)
        else if (data.startsWith("ID3") || (static_cast<unsigned char>(data[0]) == 0xFF && (static_cast<unsigned char>(data[1]) & 0xE0) == 0xE0)) {
            validAudio = true;
        }
        // Check for OGG "OggS"
        else if (data.startsWith("OggS")) {
            validAudio = true;
        }
        // Check for WAV "RIFF"
        else if (data.startsWith("RIFF")) {
            validAudio = true;
        }
    }

    if (!validAudio) {
        kLogger.warning() << "Downloaded file doesn't appear to be a valid audio file. First bytes:"
                          << data.left(16).toHex();
        // Don't return - let Mixxx try to decode it anyway
    }

    QFile file(destPath);
    if (!file.open(QIODevice::WriteOnly)) {
        kLogger.warning() << "Failed to open file for writing:" << destPath;
        return QString();
    }

    qint64 written = file.write(data);
    file.flush();  // Ensure data is flushed to disk
    file.close();

    if (written != data.size()) {
        kLogger.warning() << "Failed to write complete file, wrote" << written << "of" << data.size();
        QFile::remove(destPath);
        return QString();
    }

    // Verify the file was written correctly by checking its size
    QFileInfo fileInfo(destPath);
    if (fileInfo.size() != data.size()) {
        kLogger.warning() << "File size verification failed: expected" << data.size() << "got" << fileInfo.size();
        QFile::remove(destPath);
        return QString();
    }

    kLogger.info() << "Successfully downloaded" << data.size() << "bytes to" << destPath
                   << "(Content-Type:" << contentType << ")";
    return destPath;
}

QString TFMTrackModel::getFileExtension(const QString& trackName, const QString& url) const {
    // Try to get extension from track name first
    if (!trackName.isEmpty()) {
        int dotPos = trackName.lastIndexOf('.');
        if (dotPos > 0) {
            QString ext = trackName.mid(dotPos).toLower();
            if (ext == ".mp3" || ext == ".flac" || ext == ".wav" ||
                ext == ".ogg" || ext == ".m4a" || ext == ".aac" ||
                ext == ".opus" || ext == ".wma") {
                return ext;
            }
        }
    }

    // Try URL
    QUrl qurl(url);
    QString path = qurl.path();
    int dotPos = path.lastIndexOf('.');
    if (dotPos > 0) {
        QString ext = path.mid(dotPos).toLower();
        if (ext == ".mp3" || ext == ".flac" || ext == ".wav" ||
            ext == ".ogg" || ext == ".m4a" || ext == ".aac" ||
            ext == ".opus" || ext == ".wma") {
            return ext;
        }
    }

    // Default to mp3
    return ".mp3";
}

#include "moc_tfmtrackmodel.cpp"
