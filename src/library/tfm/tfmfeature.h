#pragma once

#include <QFuture>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QProgressDialog>
#include <atomic>

#include "library/baseexternallibraryfeature.h"
#include "util/parented_ptr.h"

class BaseExternalTrackModel;
class TFMPlaylistModel;
class WLibrarySidebar;
class BaseTrackCache;

// Forward declarations for TFM types
namespace tfm {
    class TFMApiClient;
    struct Channel;
    struct Track;
}

/// TFMFeature integrates TelegramFileManager music library into Mixxx.
/// It allows browsing channels, favorites, and local TFM folders,
/// and loading tracks from the TFM server.
class TFMFeature : public BaseExternalLibraryFeature {
    Q_OBJECT
  public:
    TFMFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~TFMFeature() override;

    /// Check if TFM feature is supported (always true since it's network-based)
    static bool isSupported();

    QVariant title() override;
    void bindSidebarWidget(WLibrarySidebar* pSidebarWidget) override;

    TreeItemModel* sidebarModel() const override;

    /// Get the configured TFM server URL
    QString getServerUrl() const;

    /// Set the TFM server URL
    void setServerUrl(const QString& url);

    /// Check if TFM is configured (has server URL)
    bool isConfigured() const;

  public slots:
    void activate() override;
    void activate(bool forceReload);
    void activateChild(const QModelIndex& index) override;
    void onRightClick(const QPoint& globalPos) override;

  signals:
    /// Emitted when channel list is loaded from server
    void channelsLoaded();

    /// Emitted when track list for a channel is loaded
    void tracksLoaded(const QString& channelId);

    /// Emitted when an error occurs during API communication
    void apiError(const QString& errorMessage);

  private slots:
    void slotChannelsLoaded(const QList<tfm::Channel>& channels);
    void slotTracksLoaded(const QString& channelId, const QList<tfm::Track>& tracks);
    void slotFolderContentsLoaded(const QString& channelId, const QString& folderId, const QList<tfm::Track>& items);
    void slotLocalTracksLoaded(const QString& folderPath, const QList<tfm::Track>& tracks);
    void slotApiError(const QString& error);
    void slotRefresh();
    void slotConfigure();

  private:
    std::unique_ptr<BaseSqlTableModel> createPlaylistModelForPlaylist(
            const QVariant& data) override;

    /// Build the sidebar tree with channels and folders
    TreeItem* buildSidebarTree();

    /// Clear the TFM table in database
    void clearTable(const QString& table_name);

    /// Load channels from the TFM server
    void loadChannels();

    /// Load tracks for a specific channel
    void loadChannelTracks(const QString& channelId);

    /// Insert tracks into the database
    void insertTracksIntoDatabase(const QString& channelId, const QList<tfm::Track>& tracks);

    /// Add folders to sidebar as expandable items under a channel
    void addFoldersToSidebar(const QString& channelId, const QList<tfm::Track>& folders);

    /// Add subfolders to sidebar under a folder item
    void addSubfoldersToSidebar(const QString& channelId, const QString& parentFolderId, const QList<tfm::Track>& subfolders);

    /// Add local folders to sidebar under Local TFM Folder
    void addLocalFoldersToSidebar(const QString& parentFolderId, const QList<tfm::Track>& folders);

    /// Find a TreeItem by its data string in the sidebar
    TreeItem* findTreeItemByData(TreeItem* parent, const QString& dataStr);

    /// Get QModelIndex for a TreeItem by traversing from root
    QModelIndex getModelIndexForTreeItem(TreeItem* item, TreeItem* rootItem);

    /// Ensure local playlist exists in database
    void ensureLocalPlaylistExists();

    /// Create the database tables for TFM
    void createDatabaseTables();

    /// Show/hide loading progress dialog
    void showLoadingDialog(const QString& message);
    void hideLoadingDialog();

    // Models
    BaseExternalTrackModel* m_pTFMTrackModel;
    parented_ptr<TFMPlaylistModel> m_pTFMPlaylistModel;
    parented_ptr<TreeItemModel> m_pSidebarModel;

    // Current channel being viewed
    QString m_currentChannelId;
    QString m_currentItemType;  // "channel" or "favorite" - tracks which section was clicked

    // API Client
    std::unique_ptr<tfm::TFMApiClient> m_pApiClient;
    QNetworkAccessManager* m_pNetworkManager;

    // State
    bool m_isActivated;
    std::atomic<bool> m_cancelLoading;

    // Database
    QSqlDatabase m_database;

    // Async loading
    QFutureWatcher<TreeItem*> m_futureWatcher;
    QFuture<TreeItem*> m_future;

    // Cache
    QSharedPointer<BaseTrackCache> m_trackSource;
    QPointer<WLibrarySidebar> m_pSidebarWidget;

    // Configuration keys
    static const QString kConfigGroup;
    static const QString kServerUrlKey;
    static const QString kLocalFolderKey;

    // Actions
    parented_ptr<QAction> m_pRefreshAction;
    parented_ptr<QAction> m_pConfigureAction;

    // Loading dialog
    QPointer<QProgressDialog> m_pLoadingDialog;
};
