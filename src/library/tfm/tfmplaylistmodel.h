#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

#include "library/baseexternalplaylistmodel.h"

class TrackCollectionManager;
class BaseTrackCache;

namespace tfm {
class TFMApiClient;
}

/// Playlist model for TFM (TelegramFileManager) channels.
/// Handles URL-based tracks that may need to be downloaded before loading.
class TFMPlaylistModel : public BaseExternalPlaylistModel {
    Q_OBJECT
  public:
    TFMPlaylistModel(QObject* parent,
                     TrackCollectionManager* pTrackCollectionManager,
                     const char* settingsNamespace,
                     const QString& playlistsTable,
                     const QString& playlistTracksTable,
                     QSharedPointer<BaseTrackCache> trackSource);
    ~TFMPlaylistModel() override;

    /// Get track, downloading if necessary
    TrackPointer getTrack(const QModelIndex& index) const override;

    /// Get the track location - may be a URL or local path
    QString getTrackLocation(const QModelIndex& index) const override;

  protected:
    /// Resolve location - handles both local paths and URLs
    QString resolveLocation(const QString& nativeLocation) const override;

  private:
    /// Download track synchronously from URL to local cache
    /// @param expectedSize Expected file size from API (0 if unknown)
    /// Returns local path on success, empty string on failure
    QString downloadTrackSync(const QString& url, const QString& destPath, qint64 expectedSize = 0) const;

    /// Get file extension from track name or URL
    QString getFileExtension(const QString& trackName, const QString& url) const;

    mutable QNetworkAccessManager m_downloadManager;
    QString m_cacheDir;
};
