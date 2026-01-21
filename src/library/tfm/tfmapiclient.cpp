#include "library/tfm/tfmapiclient.h"

#include <QFile>
#include <QNetworkRequest>
#include <QUrlQuery>

#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("TFMApiClient");
} // anonymous namespace

namespace tfm {

TFMApiClient::TFMApiClient(QNetworkAccessManager* networkManager, QObject* parent)
        : QObject(parent),
          m_pNetworkManager(networkManager) {
    connect(m_pNetworkManager, &QNetworkAccessManager::finished,
            this, &TFMApiClient::onReplyFinished);
}

TFMApiClient::~TFMApiClient() {
    cancelPendingRequests();
}

void TFMApiClient::setServerUrl(const QString& url) {
    m_serverUrl = url;
    // Ensure URL ends without trailing slash
    while (m_serverUrl.endsWith('/')) {
        m_serverUrl.chop(1);
    }
    kLogger.info() << "TFM server URL set to:" << m_serverUrl;
}

QString TFMApiClient::serverUrl() const {
    return m_serverUrl;
}

void TFMApiClient::setLocalFolder(const QString& path) {
    m_localFolder = path;
    kLogger.info() << "TFM local folder set to:" << m_localFolder;
}

QString TFMApiClient::localFolder() const {
    return m_localFolder;
}

QNetworkRequest TFMApiClient::createRequest(const QString& endpoint) const {
    QUrl url(m_serverUrl + endpoint);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    return request;
}

void TFMApiClient::checkConnection() {
    if (m_serverUrl.isEmpty()) {
        emit apiError(tr("TFM server URL is not configured"));
        return;
    }

    // Use channels endpoint to check connection
    fetchChannels();
}

void TFMApiClient::fetchChannels() {
    if (m_serverUrl.isEmpty()) {
        emit apiError(tr("TFM server URL is not configured"));
        return;
    }

    emit requestStarted();
    QNetworkRequest request = createRequest("/api/mobile/channels");
    QNetworkReply* reply = m_pNetworkManager->get(request);
    reply->setProperty("requestType", "fetchChannels");
    m_pendingReplies.append(reply);
}

void TFMApiClient::fetchChannelTracks(const QString& channelId, int offset, int limit) {
    Q_UNUSED(offset);
    qWarning() << "TFMApiClient::fetchChannelTracks - channelId:" << channelId;

    if (m_serverUrl.isEmpty()) {
        qWarning() << "TFMApiClient::fetchChannelTracks - server URL is empty!";
        emit apiError(tr("TFM server URL is not configured"));
        return;
    }

    // Clear any previous accumulated tracks for this channel
    QString requestKey = "channel:" + channelId;
    m_paginatedRequests.remove(requestKey);

    // Start fetching from page 1
    fetchChannelTracksPage(channelId, 1, limit);
}

void TFMApiClient::fetchChannelTracksPage(const QString& channelId, int page, int pageSize) {
    emit requestStarted();
    QString endpoint = QString("/api/mobile/channels/%1/files?Page=%2&PageSize=%3")
            .arg(channelId)
            .arg(page)
            .arg(pageSize);
    QString fullUrl = m_serverUrl + endpoint;
    qWarning() << "TFMApiClient::fetchChannelTracksPage - requesting:" << fullUrl;
    QNetworkRequest request = createRequest(endpoint);
    QNetworkReply* reply = m_pNetworkManager->get(request);
    reply->setProperty("requestType", "fetchChannelTracks");
    reply->setProperty("channelId", channelId);
    reply->setProperty("page", page);
    m_pendingReplies.append(reply);
}

void TFMApiClient::fetchFolderContents(const QString& channelId, const QString& folderId, int offset, int limit) {
    Q_UNUSED(offset);
    qWarning() << "TFMApiClient::fetchFolderContents - channelId:" << channelId << "folderId:" << folderId;

    if (m_serverUrl.isEmpty()) {
        qWarning() << "TFMApiClient::fetchFolderContents - server URL is empty!";
        emit apiError(tr("TFM server URL is not configured"));
        return;
    }

    // Clear any previous accumulated items for this folder
    QString requestKey = "folder:" + channelId + ":" + folderId;
    m_paginatedRequests.remove(requestKey);

    // Start fetching from page 1
    fetchFolderContentsPage(channelId, folderId, 1, limit);
}

void TFMApiClient::fetchFolderContentsPage(const QString& channelId, const QString& folderId, int page, int pageSize) {
    emit requestStarted();
    QString endpoint = QString("/api/mobile/channels/%1/files?folderId=%2&Page=%3&PageSize=%4")
            .arg(channelId)
            .arg(folderId)
            .arg(page)
            .arg(pageSize);
    QString fullUrl = m_serverUrl + endpoint;
    qWarning() << "TFMApiClient::fetchFolderContentsPage - requesting:" << fullUrl;
    QNetworkRequest request = createRequest(endpoint);
    QNetworkReply* reply = m_pNetworkManager->get(request);
    reply->setProperty("requestType", "fetchFolderContents");
    reply->setProperty("channelId", channelId);
    reply->setProperty("folderId", folderId);
    reply->setProperty("page", page);
    m_pendingReplies.append(reply);
}

void TFMApiClient::fetchFavorites() {
    if (m_serverUrl.isEmpty()) {
        emit apiError(tr("TFM server URL is not configured"));
        return;
    }

    emit requestStarted();
    QNetworkRequest request = createRequest("/api/mobile/channels/favorites");
    QNetworkReply* reply = m_pNetworkManager->get(request);
    reply->setProperty("requestType", "fetchFavorites");
    m_pendingReplies.append(reply);
}

void TFMApiClient::fetchLocalFolders() {
    if (m_serverUrl.isEmpty()) {
        emit apiError(tr("TFM server URL is not configured"));
        return;
    }

    emit requestStarted();
    QNetworkRequest request = createRequest("/api/mobile/files/local");
    QNetworkReply* reply = m_pNetworkManager->get(request);
    reply->setProperty("requestType", "fetchLocalFolders");
    m_pendingReplies.append(reply);
}

void TFMApiClient::fetchLocalTracks(const QString& folderPath) {
    qWarning() << "TFMApiClient::fetchLocalTracks - folderPath:" << folderPath;

    if (m_serverUrl.isEmpty()) {
        qWarning() << "TFMApiClient::fetchLocalTracks - server URL is empty!";
        emit apiError(tr("TFM server URL is not configured"));
        return;
    }

    emit requestStarted();
    QString endpoint;
    if (folderPath.isEmpty()) {
        // Root local folder - get audio folders
        endpoint = "/api/mobile/files/local?filter=audio_folders&page=1&pageSize=100&sortBy=name&sortDescending=false";
    } else {
        // Subfolder - use Path parameter (URL encoded)
        QByteArray encodedPath = QUrl::toPercentEncoding(folderPath);
        endpoint = QString("/api/mobile/files/local?Path=%1&filter=audio_folders&page=1&pageSize=100&sortBy=name&sortDescending=false")
                .arg(QString::fromUtf8(encodedPath));
    }
    QString fullUrl = m_serverUrl + endpoint;
    qWarning() << "TFMApiClient::fetchLocalTracks - requesting:" << fullUrl;
    QNetworkRequest request = createRequest(endpoint);
    QNetworkReply* reply = m_pNetworkManager->get(request);
    reply->setProperty("requestType", "fetchLocalTracks");
    reply->setProperty("folderPath", folderPath);
    m_pendingReplies.append(reply);
}

void TFMApiClient::searchTracks(const QString& query, int offset, int limit) {
    if (m_serverUrl.isEmpty()) {
        emit apiError(tr("TFM server URL is not configured"));
        return;
    }

    emit requestStarted();
    // Search is done via the files endpoint with SearchText parameter
    QString endpoint = QString("/api/mobile/channels/0/files?SearchText=%1&Page=%2&PageSize=%3")
            .arg(query)
            .arg(offset / limit + 1)
            .arg(limit);
    QNetworkRequest request = createRequest(endpoint);
    QNetworkReply* reply = m_pNetworkManager->get(request);
    reply->setProperty("requestType", "searchTracks");
    m_pendingReplies.append(reply);
}

QString TFMApiClient::getTrackDownloadUrl(const QString& channelId, const QString& fileId) const {
    return m_serverUrl + "/api/mobile/stream/download/" + channelId + "/" + fileId;
}

QString TFMApiClient::getTrackStreamUrl(const QString& channelId, const QString& fileId) const {
    // Use /api/mobile/stream/tfm/ endpoint for streaming
    return m_serverUrl + "/api/mobile/stream/tfm/" + channelId + "/" + fileId;
}

QString TFMApiClient::getLocalTrackUrl(const QString& filePath) const {
    // URL format for local files: /api/mobile/stream/local?path=<double_encoded_path>
    // The path needs to be double-encoded (first encode, then encode the result again)
    QByteArray firstEncode = QUrl::toPercentEncoding(filePath);
    QByteArray doubleEncode = QUrl::toPercentEncoding(QString::fromUtf8(firstEncode));
    return m_serverUrl + "/api/mobile/stream/local?path=" + QString::fromUtf8(doubleEncode);
}

void TFMApiClient::downloadTrack(const QString& trackId, const QString& destPath) {
    Q_UNUSED(trackId);
    Q_UNUSED(destPath);
    // Not implemented - tracks are streamed directly
    emit apiError(tr("Download not implemented - use streaming instead"));
}

void TFMApiClient::cancelPendingRequests() {
    for (QNetworkReply* reply : m_pendingReplies) {
        if (reply && !reply->isFinished()) {
            reply->abort();
        }
    }
    m_pendingReplies.clear();
}

void TFMApiClient::onReplyFinished(QNetworkReply* reply) {
    qWarning() << "TFMApiClient::onReplyFinished called";
    m_pendingReplies.removeAll(reply);
    emit requestFinished();

    if (reply->error() != QNetworkReply::NoError) {
        QString errorMsg = QString("Network error: %1").arg(reply->errorString());
        qWarning() << "TFMApiClient - Network error:" << errorMsg;
        emit apiError(errorMsg);
        reply->deleteLater();
        return;
    }

    QString requestType = reply->property("requestType").toString();
    qWarning() << "TFMApiClient::onReplyFinished - requestType:" << requestType;
    handleResponse(reply, requestType);
    reply->deleteLater();
}

void TFMApiClient::handleResponse(QNetworkReply* reply, const QString& requestType) {
    QByteArray data = reply->readAll();
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        QString errorMsg = QString("JSON parse error: %1").arg(parseError.errorString());
        kLogger.warning() << errorMsg;
        emit apiError(errorMsg);
        return;
    }

    // All responses are wrapped in ApiResponse: { success, data, error, message, pagination }
    QJsonObject responseObj = doc.object();
    bool success = responseObj.value("success").toBool();

    if (!success) {
        QString errorMsg = responseObj.value("error").toString();
        if (errorMsg.isEmpty()) {
            errorMsg = responseObj.value("message").toString();
        }
        if (errorMsg.isEmpty()) {
            errorMsg = "Unknown API error";
        }
        kLogger.warning() << "API error:" << errorMsg;
        emit apiError(errorMsg);
        return;
    }

    if (requestType == "fetchChannels") {
        QList<Channel> channels;
        QJsonArray arr = responseObj.value("data").toArray();
        for (const QJsonValue& val : arr) {
            channels.append(parseChannel(val.toObject()));
        }
        kLogger.info() << "Loaded" << channels.size() << "channels from TFM";
        emit channelsLoaded(channels);

    } else if (requestType == "fetchChannelTracks") {
        QString channelId = reply->property("channelId").toString();
        int currentPage = reply->property("page").toInt();
        qWarning() << "Processing fetchChannelTracks for channel:" << channelId << "page:" << currentPage;

        // Parse tracks from response
        QList<Track> tracks;
        QJsonValue dataVal = responseObj.value("data");

        QJsonArray arr;
        if (dataVal.isArray()) {
            arr = dataVal.toArray();
        } else if (dataVal.isObject()) {
            QJsonObject dataObj = dataVal.toObject();
            if (dataObj.contains("items")) {
                arr = dataObj.value("items").toArray();
            }
        }

        for (const QJsonValue& val : arr) {
            Track tr = parseTrack(val.toObject());
            tr.channelId = channelId;
            tracks.append(tr);
        }

        // Parse pagination info
        PaginationInfo pagination = parsePagination(responseObj);
        qWarning() << "Page" << pagination.page << "of" << pagination.totalPages
                   << "- hasNext:" << pagination.hasNext << "totalItems:" << pagination.totalItems;

        // Accumulate tracks
        QString requestKey = "channel:" + channelId;
        if (!m_paginatedRequests.contains(requestKey)) {
            m_paginatedRequests[requestKey] = PaginatedRequest();
            m_paginatedRequests[requestKey].channelId = channelId;
        }
        m_paginatedRequests[requestKey].accumulatedTracks.append(tracks);
        m_paginatedRequests[requestKey].currentPage = pagination.page;
        m_paginatedRequests[requestKey].totalPages = pagination.totalPages;

        if (pagination.hasNext) {
            // Fetch next page
            qWarning() << "Fetching next page" << (pagination.page + 1) << "for channel" << channelId;
            fetchChannelTracksPage(channelId, pagination.page + 1, pagination.pageSize);
        } else {
            // All pages loaded, emit signal with all tracks
            QList<Track> allTracks = m_paginatedRequests[requestKey].accumulatedTracks;
            m_paginatedRequests.remove(requestKey);
            qWarning() << "All pages loaded. Emitting tracksLoaded with" << allTracks.size() << "total tracks for channel" << channelId;
            emit tracksLoaded(channelId, allTracks);
        }

    } else if (requestType == "fetchFolderContents") {
        QString channelId = reply->property("channelId").toString();
        QString folderId = reply->property("folderId").toString();
        int currentPage = reply->property("page").toInt();
        qWarning() << "Processing fetchFolderContents for channel:" << channelId << "folder:" << folderId << "page:" << currentPage;

        // Parse items from response
        QList<Track> items;
        QJsonValue dataVal = responseObj.value("data");

        QJsonArray arr;
        if (dataVal.isArray()) {
            arr = dataVal.toArray();
        } else if (dataVal.isObject()) {
            QJsonObject dataObj = dataVal.toObject();
            if (dataObj.contains("items")) {
                arr = dataObj.value("items").toArray();
            }
        }

        for (const QJsonValue& val : arr) {
            Track tr = parseTrack(val.toObject());
            tr.channelId = channelId;
            items.append(tr);
        }

        // Parse pagination info
        PaginationInfo pagination = parsePagination(responseObj);
        qWarning() << "Folder page" << pagination.page << "of" << pagination.totalPages
                   << "- hasNext:" << pagination.hasNext;

        // Accumulate items
        QString requestKey = "folder:" + channelId + ":" + folderId;
        if (!m_paginatedRequests.contains(requestKey)) {
            m_paginatedRequests[requestKey] = PaginatedRequest();
            m_paginatedRequests[requestKey].channelId = channelId;
            m_paginatedRequests[requestKey].folderId = folderId;
        }
        m_paginatedRequests[requestKey].accumulatedTracks.append(items);
        m_paginatedRequests[requestKey].currentPage = pagination.page;
        m_paginatedRequests[requestKey].totalPages = pagination.totalPages;

        if (pagination.hasNext) {
            // Fetch next page
            qWarning() << "Fetching next folder page" << (pagination.page + 1);
            fetchFolderContentsPage(channelId, folderId, pagination.page + 1, pagination.pageSize);
        } else {
            // All pages loaded, emit signal with all items
            QList<Track> allItems = m_paginatedRequests[requestKey].accumulatedTracks;
            m_paginatedRequests.remove(requestKey);
            qWarning() << "All folder pages loaded. Emitting folderContentsLoaded with" << allItems.size() << "total items";
            emit folderContentsLoaded(channelId, folderId, allItems);
        }

    } else if (requestType == "fetchFavorites") {
        QList<Channel> favorites;
        QJsonArray arr = responseObj.value("data").toArray();
        for (const QJsonValue& val : arr) {
            Channel ch = parseChannel(val.toObject());
            ch.isFavorite = true;
            favorites.append(ch);
        }
        emit favoritesLoaded(favorites);

    } else if (requestType == "fetchLocalFolders" || requestType == "fetchLocalTracks") {
        // FolderContentsDto response - data is an object with items array
        qWarning() << "Processing" << requestType;

        QJsonValue dataVal = responseObj.value("data");
        QJsonArray itemsArr;

        if (dataVal.isObject()) {
            QJsonObject dataObj = dataVal.toObject();
            itemsArr = dataObj.value("items").toArray();
            qWarning() << "Found items array with" << itemsArr.size() << "items";
        } else {
            qWarning() << "Unexpected data format for local files";
        }

        if (requestType == "fetchLocalFolders") {
            QList<Folder> folders;
            for (const QJsonValue& val : itemsArr) {
                QJsonObject item = val.toObject();
                if (item.value("isFolder").toBool()) {
                    folders.append(parseFolder(item));
                }
            }
            emit localFoldersLoaded(folders);
        } else {
            QString folderPath = reply->property("folderPath").toString();
            QList<Track> tracks;
            for (const QJsonValue& val : itemsArr) {
                Track tr = parseTrack(val.toObject());
                tracks.append(tr);
                qWarning() << "  Local item:" << tr.name << "isFolder:" << tr.isFolder << "path:" << tr.path << "category:" << tr.category;
            }
            qWarning() << "Emitting localTracksLoaded with" << tracks.size() << "items for path:" << folderPath;
            emit localTracksLoaded(folderPath, tracks);
        }

    } else if (requestType == "searchTracks") {
        QList<Track> tracks;
        QJsonArray arr = responseObj.value("data").toArray();
        for (const QJsonValue& val : arr) {
            tracks.append(parseTrack(val.toObject()));
        }
        emit searchResultsReady(tracks);
    }
}

Channel TFMApiClient::parseChannel(const QJsonObject& json) {
    Channel ch;
    ch.id = json.value("id").toVariant().toLongLong();
    ch.name = json.value("name").toString();
    ch.imageUrl = json.value("imageUrl").toString();
    ch.isOwner = json.value("isOwner").toBool();
    ch.canPost = json.value("canPost").toBool();
    ch.isFavorite = json.value("isFavorite").toBool();
    ch.type = json.value("type").toString();
    ch.fileCount = json.value("fileCount").toInt();
    return ch;
}

Track TFMApiClient::parseTrack(const QJsonObject& json) {
    Track tr;
    tr.id = json.value("id").toString();
    tr.name = json.value("name").toString();
    tr.path = json.value("path").toString();
    tr.parentId = json.value("parentId").toString();
    tr.size = json.value("size").toVariant().toLongLong();
    tr.type = json.value("type").toString();
    tr.category = json.value("category").toString();
    tr.isFile = json.value("isFile").toBool();
    tr.isFolder = json.value("isFolder").toBool();  // For local API
    tr.hasChildren = json.value("hasChildren").toBool();
    tr.streamUrl = json.value("streamUrl").toString();
    tr.downloadUrl = json.value("downloadUrl").toString();
    tr.thumbnailUrl = json.value("thumbnailUrl").toString();
    // Parse dates - format is ISO 8601: "2024-04-26T09:00:29Z"
    tr.dateCreated = QDateTime::fromString(json.value("dateCreated").toString(), Qt::ISODate);
    tr.dateModified = QDateTime::fromString(json.value("dateModified").toString(), Qt::ISODate);
    return tr;
}

Folder TFMApiClient::parseFolder(const QJsonObject& json) {
    Folder f;
    f.id = json.value("id").toString();
    f.name = json.value("name").toString();
    f.path = json.value("path").toString();
    f.parentId = json.value("parentId").toString();
    f.isFolder = json.value("isFolder").toBool();
    f.hasChildren = json.value("hasChildren").toBool();
    return f;
}

TFMApiClient::PaginationInfo TFMApiClient::parsePagination(const QJsonObject& responseObj) {
    PaginationInfo info;

    QJsonValue paginationVal = responseObj.value("pagination");
    if (paginationVal.isObject()) {
        QJsonObject pagination = paginationVal.toObject();
        info.page = pagination.value("page").toInt(1);
        info.pageSize = pagination.value("pageSize").toInt(100);
        info.totalItems = pagination.value("totalItems").toInt(0);
        info.totalPages = pagination.value("totalPages").toInt(1);
        info.hasNext = pagination.value("hasNext").toBool(false);
        info.hasPrevious = pagination.value("hasPrevious").toBool(false);
    }

    return info;
}

} // namespace tfm

#include "moc_tfmapiclient.cpp"
