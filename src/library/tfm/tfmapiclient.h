#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>

namespace tfm {

/// Represents a TFM channel (Telegram channel with music)
struct Channel {
    qint64 id;
    QString name;
    QString imageUrl;
    bool isOwner;
    bool canPost;
    bool isFavorite;
    QString type;
    int fileCount;
};

/// Represents a file/track from TFM (ChannelFileDto)
struct Track {
    QString id;           // file id
    QString channelId;    // channel id as string
    QString name;         // file name
    QString path;
    QString parentId;
    qint64 size;
    QString type;         // file extension
    QString category;     // audio, video, etc.
    bool isFile;          // true if it's a file (from channel API)
    bool isFolder;        // true if it's a folder (from local API)
    bool hasChildren;
    QString streamUrl;
    QString downloadUrl;
    QString thumbnailUrl;
    QDateTime dateCreated;   // when the file was added
    QDateTime dateModified;  // when the file was last modified
};

/// Represents a folder in TFM
struct Folder {
    QString id;
    QString name;
    QString path;
    QString parentId;
    bool isFolder;
    bool hasChildren;
};

/// API Client for TelegramFileManager server
class TFMApiClient : public QObject {
    Q_OBJECT

  public:
    explicit TFMApiClient(QNetworkAccessManager* networkManager, QObject* parent = nullptr);
    ~TFMApiClient() override;

    /// Set the base URL for the TFM server
    void setServerUrl(const QString& url);
    QString serverUrl() const;

    /// Set the local TFM folder path
    void setLocalFolder(const QString& path);
    QString localFolder() const;

    /// Check if the server is configured and reachable
    void checkConnection();

    /// Fetch all channels from the server
    void fetchChannels();

    /// Fetch tracks for a specific channel
    void fetchChannelTracks(const QString& channelId, int offset = 0, int limit = 100);

    /// Fetch contents of a folder within a channel
    void fetchFolderContents(const QString& channelId, const QString& folderId, int offset = 0, int limit = 100);

    /// Fetch favorite channels
    void fetchFavorites();

    /// Fetch local folder structure
    void fetchLocalFolders();

    /// Fetch tracks in a local folder
    void fetchLocalTracks(const QString& folderId);

    /// Search tracks across all channels
    void searchTracks(const QString& query, int offset = 0, int limit = 50);

    /// Get the download URL for a track
    QString getTrackDownloadUrl(const QString& channelId, const QString& fileId) const;

    /// Get the streaming URL for a track
    QString getTrackStreamUrl(const QString& channelId, const QString& fileId) const;

    /// Get the URL for a local file (using path)
    QString getLocalTrackUrl(const QString& filePath) const;

    /// Download a track to local cache
    void downloadTrack(const QString& trackId, const QString& destPath);

    /// Cancel any pending requests
    void cancelPendingRequests();

  signals:
    /// Emitted when connection check completes
    void connectionChecked(bool success, const QString& serverVersion);

    /// Emitted when channels are loaded
    void channelsLoaded(const QList<Channel>& channels);

    /// Emitted when tracks are loaded for a channel
    void tracksLoaded(const QString& channelId, const QList<Track>& tracks);

    /// Emitted when folder contents are loaded (includes both files and subfolders)
    void folderContentsLoaded(const QString& channelId, const QString& folderId, const QList<Track>& items);

    /// Emitted when favorites are loaded
    void favoritesLoaded(const QList<Channel>& favorites);

    /// Emitted when local folders are loaded
    void localFoldersLoaded(const QList<Folder>& folders);

    /// Emitted when local tracks are loaded
    void localTracksLoaded(const QString& folderId, const QList<Track>& tracks);

    /// Emitted when search results are ready
    void searchResultsReady(const QList<Track>& tracks);

    /// Emitted when a track download completes
    void trackDownloaded(const QString& trackId, const QString& localPath);

    /// Emitted on any API error
    void apiError(const QString& errorMessage);

    /// Emitted when a request starts (for progress indication)
    void requestStarted();

    /// Emitted when a request finishes
    void requestFinished();

  private slots:
    void onReplyFinished(QNetworkReply* reply);

  private:
    /// Create a network request with proper headers
    QNetworkRequest createRequest(const QString& endpoint) const;

    /// Parse channel from JSON
    static Channel parseChannel(const QJsonObject& json);

    /// Parse track from JSON
    static Track parseTrack(const QJsonObject& json);

    /// Parse folder from JSON
    static Folder parseFolder(const QJsonObject& json);

    /// Handle API response
    void handleResponse(QNetworkReply* reply, const QString& requestType);

    /// Parse pagination info from response
    struct PaginationInfo {
        int page = 1;
        int pageSize = 100;
        int totalItems = 0;
        int totalPages = 1;
        bool hasNext = false;
        bool hasPrevious = false;
    };
    PaginationInfo parsePagination(const QJsonObject& responseObj);

    /// Fetch next page of channel tracks
    void fetchChannelTracksPage(const QString& channelId, int page, int pageSize);

    /// Fetch next page of folder contents
    void fetchFolderContentsPage(const QString& channelId, const QString& folderId, int page, int pageSize);

    QNetworkAccessManager* m_pNetworkManager;
    QString m_serverUrl;
    QString m_localFolder;
    QList<QNetworkReply*> m_pendingReplies;

    // Pagination state - accumulate tracks across pages
    struct PaginatedRequest {
        QString channelId;
        QString folderId;  // Empty for channel root
        QList<Track> accumulatedTracks;
        int currentPage = 1;
        int pageSize = 100;
        int totalPages = 1;
    };
    QMap<QString, PaginatedRequest> m_paginatedRequests;  // Key: "channel:id" or "folder:channelId:folderId"
};

} // namespace tfm
