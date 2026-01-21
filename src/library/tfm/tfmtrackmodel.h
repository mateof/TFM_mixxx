#pragma once

#include <QNetworkAccessManager>
#include <QObject>

#include "library/baseexternaltrackmodel.h"

class TrackCollectionManager;
class BaseTrackCache;

namespace tfm {
class TFMApiClient;
}

/// Track model for TFM (TelegramFileManager) tracks.
/// Handles URL-based tracks that may need to be downloaded/streamed.
class TFMTrackModel : public BaseExternalTrackModel {
    Q_OBJECT
  public:
    TFMTrackModel(QObject* parent,
                  TrackCollectionManager* pTrackCollectionManager,
                  QSharedPointer<BaseTrackCache> trackSource,
                  tfm::TFMApiClient* pApiClient);
    ~TFMTrackModel() override;

    /// Get track, downloading if necessary
    TrackPointer getTrack(const QModelIndex& index) const override;

    /// Get the track location - may be a URL or local path
    QString getTrackLocation(const QModelIndex& index) const override;

    /// Get capabilities - TFM tracks can be loaded to decks
    Capabilities getCapabilities() const override;

  protected:
    /// Resolve location - handles both local paths and URLs
    QString resolveLocation(const QString& nativeLocation) const override;

  private:
    /// Check if the track needs to be downloaded
    bool needsDownload(const QString& location) const;

    /// Get the local cache path for a track
    QString getCachePath(const QString& trackId) const;

    /// Download track synchronously from URL to local cache
    /// @param expectedSize Expected file size from API (0 if unknown)
    /// Returns local path on success, empty string on failure
    QString downloadTrackSync(const QString& url, const QString& destPath, qint64 expectedSize = 0) const;

    /// Get file extension from track name or URL
    QString getFileExtension(const QString& trackName, const QString& url) const;

    tfm::TFMApiClient* m_pApiClient;
    mutable QNetworkAccessManager m_downloadManager;
    QString m_cacheDir;
};
