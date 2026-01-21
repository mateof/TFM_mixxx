#include "library/tfm/tfmfeature.h"

#include <memory>
#include <vector>

#include <QCoreApplication>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QtConcurrent>
#include <QInputDialog>

#include "library/baseexternaltrackmodel.h"
#include "library/tfm/tfmplaylistmodel.h"
#include "library/basesqltablemodel.h"
#include "library/basetrackcache.h"
#include "library/library.h"
#include "library/queryutil.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "library/tfm/tfmapiclient.h"
#include "library/tfm/tfmtrackmodel.h"
#include "util/db/dbconnectionpooled.h"
#include "util/logger.h"
#include "widget/wlibrarysidebar.h"

namespace {
const mixxx::Logger kLogger("TFMFeature");

// Database table names
const QString kTFMTracksTable = QStringLiteral("tfm_tracks");
const QString kTFMPlaylistsTable = QStringLiteral("tfm_playlists");
const QString kTFMPlaylistTracksTable = QStringLiteral("tfm_playlist_tracks");

// Sidebar item data types
const QString kChannelType = QStringLiteral("channel");
const QString kFavoriteType = QStringLiteral("favorite");
const QString kLocalFolderType = QStringLiteral("local_folder");
const QString kFolderType = QStringLiteral("folder");  // Folder within a channel
const QString kRootChannels = QStringLiteral("root_channels");
const QString kRootFavorites = QStringLiteral("root_favorites");
const QString kRootLocal = QStringLiteral("root_local");

/// Parse artist and title from filename
/// Supports patterns like:
///   "01 - Artist - Title.ext"
///   "Artist - Title.ext"
///   "[Artist] Title.ext"
///   "(01) [Artist] - Title.ext"
struct TrackMetadata {
    QString artist;
    QString title;
};

TrackMetadata parseFilenameMetadata(const QString& filename) {
    TrackMetadata meta;

    // Remove extension
    QString name = filename;
    int dotPos = name.lastIndexOf('.');
    if (dotPos > 0) {
        name = name.left(dotPos);
    }

    // Try pattern: "NN - Artist - Title" or "Artist - Title"
    QStringList parts = name.split(" - ");
    if (parts.size() >= 2) {
        // Check if first part is a track number (starts with digits or is in parentheses)
        QString firstPart = parts.first().trimmed();
        bool startsWithNumber = !firstPart.isEmpty() &&
            (firstPart[0].isDigit() || firstPart.startsWith('(') || firstPart.startsWith('['));

        if (startsWithNumber && parts.size() >= 3) {
            // Pattern: "NN - Artist - Title"
            meta.artist = parts.at(1).trimmed();
            // Join remaining parts as title (in case title contains " - ")
            meta.title = QStringList(parts.mid(2)).join(" - ").trimmed();
        } else if (parts.size() >= 2) {
            // Pattern: "Artist - Title"
            meta.artist = parts.first().trimmed();
            meta.title = QStringList(parts.mid(1)).join(" - ").trimmed();
        }
    }

    // Try pattern: "[Artist] Title" if no hyphen pattern found
    if (meta.artist.isEmpty() && name.contains('[') && name.contains(']')) {
        int start = name.indexOf('[');
        int end = name.indexOf(']');
        if (start < end) {
            meta.artist = name.mid(start + 1, end - start - 1).trimmed();
            meta.title = name.mid(end + 1).trimmed();
            // Remove leading " - " or "-" from title
            if (meta.title.startsWith(" - ")) {
                meta.title = meta.title.mid(3).trimmed();
            } else if (meta.title.startsWith("-")) {
                meta.title = meta.title.mid(1).trimmed();
            }
        }
    }

    // Fallback: use entire name as title
    if (meta.title.isEmpty()) {
        meta.title = name;
    }

    // Clean up brackets from title if still present
    if (meta.title.startsWith("(") || meta.title.startsWith("[")) {
        // Remove leading track number like "(01)" or "[01]"
        QRegularExpression trackNumRegex(R"(^[\(\[]?\d+[\)\]]?\s*)");
        meta.title.remove(trackNumRegex);
    }

    return meta;
}

} // anonymous namespace

// Configuration keys
const QString TFMFeature::kConfigGroup = QStringLiteral("[TFM]");
const QString TFMFeature::kServerUrlKey = QStringLiteral("ServerUrl");
const QString TFMFeature::kLocalFolderKey = QStringLiteral("LocalFolder");

TFMFeature::TFMFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : BaseExternalLibraryFeature(pLibrary, pConfig, QStringLiteral("tfm")),
          m_pTFMTrackModel(nullptr),
          m_pTFMPlaylistModel(nullptr),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_pNetworkManager(new QNetworkAccessManager(this)),
          m_isActivated(false),
          m_cancelLoading(false) {

    // Initialize API client
    m_pApiClient = std::make_unique<tfm::TFMApiClient>(m_pNetworkManager, this);

    // Load configuration
    QString serverUrl = m_pConfig->getValue(
            ConfigKey(kConfigGroup, kServerUrlKey), QString());
    QString localFolder = m_pConfig->getValue(
            ConfigKey(kConfigGroup, kLocalFolderKey), QString());

    m_pApiClient->setServerUrl(serverUrl);
    m_pApiClient->setLocalFolder(localFolder);

    // Connect API signals
    connect(m_pApiClient.get(), &tfm::TFMApiClient::channelsLoaded,
            this, &TFMFeature::slotChannelsLoaded);
    connect(m_pApiClient.get(), &tfm::TFMApiClient::tracksLoaded,
            this, &TFMFeature::slotTracksLoaded);
    connect(m_pApiClient.get(), &tfm::TFMApiClient::folderContentsLoaded,
            this, &TFMFeature::slotFolderContentsLoaded);
    connect(m_pApiClient.get(), &tfm::TFMApiClient::localTracksLoaded,
            this, &TFMFeature::slotLocalTracksLoaded);
    connect(m_pApiClient.get(), &tfm::TFMApiClient::apiError,
            this, &TFMFeature::slotApiError);

    // Create context menu actions
    m_pRefreshAction = make_parented<QAction>(tr("Refresh"), this);
    connect(m_pRefreshAction, &QAction::triggered,
            this, &TFMFeature::slotRefresh);

    m_pConfigureAction = make_parented<QAction>(tr("Configure TFM Server..."), this);
    connect(m_pConfigureAction, &QAction::triggered,
            this, &TFMFeature::slotConfigure);

    // Initialize track source and model
    QString tableName = kTFMTracksTable;
    QString idColumn = QStringLiteral("id");
    QStringList columns = {
            QStringLiteral("id"),
            QStringLiteral("external_id"),
            QStringLiteral("channel_id"),
            QStringLiteral("artist"),
            QStringLiteral("title"),
            QStringLiteral("album"),
            QStringLiteral("genre"),
            QStringLiteral("duration"),
            QStringLiteral("file_url"),
            QStringLiteral("local_path"),
            QStringLiteral("location"),
            QStringLiteral("file_size"),
            QStringLiteral("cover_url"),
            QStringLiteral("bpm"),
            QStringLiteral("key"),
            QStringLiteral("datetime_added")};
    QStringList searchColumns = {
            QStringLiteral("artist"),
            QStringLiteral("title"),
            QStringLiteral("album"),
            QStringLiteral("genre")};

    m_trackSource = QSharedPointer<BaseTrackCache>::create(
            m_pTrackCollection,
            tableName,
            std::move(idColumn),
            std::move(columns),
            std::move(searchColumns),
            false);

    // Create TFMTrackModel for handling track loading with download support
    m_pTFMTrackModel = new TFMTrackModel(
            this,
            pLibrary->trackCollectionManager(),
            m_trackSource,
            m_pApiClient.get());

    m_pTFMPlaylistModel = make_parented<TFMPlaylistModel>(
            this,
            pLibrary->trackCollectionManager(),
            "mixxx.db.model.tfm.playlistmodel",
            kTFMPlaylistsTable,
            kTFMPlaylistTracksTable,
            m_trackSource);
}

TFMFeature::~TFMFeature() {
    m_cancelLoading = true;
    m_future.waitForFinished();
}

bool TFMFeature::isSupported() {
    // TFM is always supported since it's network-based
    return true;
}

QVariant TFMFeature::title() {
    return tr("TelegramFileManager");
}

void TFMFeature::bindSidebarWidget(WLibrarySidebar* pSidebarWidget) {
    BaseExternalLibraryFeature::bindSidebarWidget(pSidebarWidget);
    m_pSidebarWidget = pSidebarWidget;
}

TreeItemModel* TFMFeature::sidebarModel() const {
    return m_pSidebarModel.get();
}

QString TFMFeature::getServerUrl() const {
    return m_pApiClient->serverUrl();
}

void TFMFeature::setServerUrl(const QString& url) {
    m_pApiClient->setServerUrl(url);
    m_pConfig->setValue(ConfigKey(kConfigGroup, kServerUrlKey), url);
}

bool TFMFeature::isConfigured() const {
    return !m_pApiClient->serverUrl().isEmpty();
}

void TFMFeature::activate() {
    activate(false);
}

void TFMFeature::activate(bool forceReload) {
    kLogger.info() << "TFMFeature::activate" << (forceReload ? "(forced)" : "");

    if (!isConfigured()) {
        // Show configuration dialog
        slotConfigure();
        if (!isConfigured()) {
            return;
        }
    }

    if (m_isActivated && !forceReload) {
        emit showTrackModel(m_pTFMTrackModel);
        return;
    }

    // Create database tables on first activation
    if (!m_isActivated) {
        createDatabaseTables();
    }

    m_isActivated = true;
    m_cancelLoading = false;

    // Load channels from server
    emit featureIsLoading(this, true);
    showLoadingDialog(tr("Loading channels from TFM server..."));
    loadChannels();
}

void TFMFeature::activateChild(const QModelIndex& index) {
    qWarning() << "TFMFeature::activateChild called, index valid:" << index.isValid();

    if (!index.isValid()) {
        return;
    }

    TreeItem* item = static_cast<TreeItem*>(index.internalPointer());
    if (!item) {
        qWarning() << "TFMFeature::activateChild - item is null";
        return;
    }

    QVariant itemData = item->getData();
    QString dataStr = itemData.toString();

    qWarning() << "TFMFeature::activateChild - dataStr:" << dataStr;

    // Check what type of item was clicked
    if (dataStr.startsWith(kChannelType + ":")) {
        // Channel clicked
        QString channelId = dataStr.section(':', 1);
        m_currentItemType = kChannelType;
        emit featureIsLoading(this, true);
        showLoadingDialog(tr("Loading tracks from channel..."));
        loadChannelTracks(channelId);
    } else if (dataStr.startsWith(kFavoriteType + ":")) {
        // Favorite clicked
        QString channelId = dataStr.section(':', 1);
        m_currentItemType = kFavoriteType;
        emit featureIsLoading(this, true);
        showLoadingDialog(tr("Loading tracks from channel..."));
        loadChannelTracks(channelId);
    } else if (dataStr.startsWith(kFolderType + ":")) {
        // Folder within a channel - format is "folder:channelId:folderId"
        QStringList parts = dataStr.split(':');
        if (parts.size() >= 3) {
            QString channelId = parts.at(1);
            QString folderId = parts.at(2);
            qWarning() << "Loading folder" << folderId << "in channel" << channelId;
            emit featureIsLoading(this, true);
            showLoadingDialog(tr("Loading folder contents..."));
            m_pApiClient->fetchFolderContents(channelId, folderId);
        }
    } else if (dataStr == kRootLocal) {
        // Root local folder - fetch all local folders
        qWarning() << "Loading root local folder";
        emit featureIsLoading(this, true);
        showLoadingDialog(tr("Loading local folders..."));
        m_pApiClient->fetchLocalTracks("");  // Empty string for root
    } else if (dataStr.startsWith(kLocalFolderType + ":")) {
        // Local subfolder - data format is "local_folder:/path/to/folder"
        QString folderPath = dataStr.mid(QString(kLocalFolderType + ":").length());
        qWarning() << "Loading local folder path:" << folderPath;
        emit featureIsLoading(this, true);
        showLoadingDialog(tr("Loading folder contents..."));
        m_pApiClient->fetchLocalTracks(folderPath);
    }
}

void TFMFeature::onRightClick(const QPoint& globalPos) {
    QMenu menu;
    menu.addAction(m_pRefreshAction);
    menu.addSeparator();
    menu.addAction(m_pConfigureAction);
    menu.exec(globalPos);
}

std::unique_ptr<BaseSqlTableModel> TFMFeature::createPlaylistModelForPlaylist(
        const QVariant& /* data */) {
    // Not used for TFM - we use the track model directly
    return nullptr;
}

TreeItem* TFMFeature::buildSidebarTree() {
    auto pRootItem = TreeItem::newRoot(this);

    // Add "Channels" root
    TreeItem* pChannelsRoot = pRootItem->appendChild(
            tr("Channels"), kRootChannels);
    Q_UNUSED(pChannelsRoot);

    // Add "Favorites" root
    TreeItem* pFavoritesRoot = pRootItem->appendChild(
            tr("Favorites"), kRootFavorites);
    Q_UNUSED(pFavoritesRoot);

    // Add "Local Folder" root
    TreeItem* pLocalRoot = pRootItem->appendChild(
            tr("Local TFM Folder"), kRootLocal);
    Q_UNUSED(pLocalRoot);

    return pRootItem.release();
}

void TFMFeature::createDatabaseTables() {
    QSqlDatabase db = m_pTrackCollection->database();
    kLogger.info() << "Creating TFM database tables...";

    // Drop old tables to ensure clean schema (during development)
    QSqlQuery dropQuery(db);
    dropQuery.exec("DROP TABLE IF EXISTS " + kTFMPlaylistTracksTable);
    dropQuery.exec("DROP TABLE IF EXISTS " + kTFMTracksTable);
    dropQuery.exec("DROP TABLE IF EXISTS " + kTFMPlaylistsTable);
    kLogger.info() << "Dropped old TFM tables";

    // Create tracks table - use INTEGER id for Mixxx compatibility, external_id for TFM ObjectId
    QSqlQuery query(db);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + kTFMTracksTable + " ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "external_id TEXT UNIQUE, "
            "channel_id TEXT, "
            "artist TEXT, "
            "title TEXT, "
            "album TEXT, "
            "genre TEXT, "
            "duration INTEGER, "
            "file_url TEXT, "
            "local_path TEXT, "
            "file_size INTEGER, "
            "cover_url TEXT, "
            "bpm INTEGER, "
            "key TEXT, "
            "location TEXT, "
            "datetime_added TEXT"
            ")");
    if (!query.exec()) {
        kLogger.warning() << "Failed to create TFM tracks table:"
                          << query.lastError();
    } else {
        kLogger.info() << "Created tfm_tracks table";
    }

    // Create playlists (channels) table - use auto-increment id for BaseExternalPlaylistModel compatibility
    QSqlQuery playlistQuery(db);
    playlistQuery.prepare(
            "CREATE TABLE IF NOT EXISTS " + kTFMPlaylistsTable + " ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "channel_id TEXT UNIQUE, "
            "name TEXT, "
            "description TEXT, "
            "image_url TEXT, "
            "track_count INTEGER, "
            "is_favorite INTEGER DEFAULT 0"
            ")");
    if (!playlistQuery.exec()) {
        kLogger.warning() << "Failed to create TFM playlists table:"
                          << playlistQuery.lastError();
    } else {
        kLogger.info() << "Created tfm_playlists table";
    }

    // Create index on channel_id for lookups
    QSqlQuery channelIdxQuery(db);
    channelIdxQuery.exec(
            "CREATE INDEX IF NOT EXISTS idx_tfm_playlists_channel "
            "ON " + kTFMPlaylistsTable + " (channel_id)");

    // Create playlist_tracks linking table (required by BaseExternalPlaylistModel)
    // NOTE: Both playlist_id and track_id must be INTEGER to match the auto-increment IDs
    QSqlQuery linkQuery(db);
    linkQuery.prepare(
            "CREATE TABLE IF NOT EXISTS " + kTFMPlaylistTracksTable + " ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "playlist_id INTEGER, "
            "track_id INTEGER, "
            "position INTEGER, "
            "FOREIGN KEY(playlist_id) REFERENCES " + kTFMPlaylistsTable + "(id), "
            "FOREIGN KEY(track_id) REFERENCES " + kTFMTracksTable + "(id)"
            ")");
    if (!linkQuery.exec()) {
        kLogger.warning() << "Failed to create TFM playlist_tracks table:"
                          << linkQuery.lastError();
    } else {
        kLogger.info() << "Created tfm_playlist_tracks table";
    }

    // Create index for faster lookups
    QSqlQuery indexQuery(db);
    indexQuery.exec(
            "CREATE INDEX IF NOT EXISTS idx_tfm_playlist_tracks_playlist "
            "ON " + kTFMPlaylistTracksTable + " (playlist_id)");

    kLogger.info() << "TFM database tables created successfully";
}

void TFMFeature::clearTable(const QString& tableName) {
    QSqlDatabase db = m_pTrackCollection->database();
    QSqlQuery query(db);
    query.prepare("DELETE FROM " + tableName);
    if (!query.exec()) {
        kLogger.warning() << "Failed to clear table" << tableName
                          << ":" << query.lastError();
    }
}

void TFMFeature::loadChannels() {
    kLogger.info() << "Loading channels from TFM server";
    m_pApiClient->fetchChannels();
}

void TFMFeature::loadChannelTracks(const QString& channelId) {
    qWarning() << "TFMFeature::loadChannelTracks - channelId:" << channelId;
    m_pApiClient->fetchChannelTracks(channelId);
}

void TFMFeature::slotChannelsLoaded(const QList<tfm::Channel>& channels) {
    kLogger.info() << "Received" << channels.size() << "channels";

    // Update sidebar model
    if (!m_pSidebarModel) {
        m_pSidebarModel = make_parented<TreeItemModel>(this);
    }

    // Build sidebar tree with channels
    auto pRootItem = TreeItem::newRoot(this);

    // Add "Channels" section
    TreeItem* pChannelsRoot = pRootItem->appendChild(
            tr("Channels (%1)").arg(channels.size()), kRootChannels);
    for (const tfm::Channel& ch : channels) {
        QString itemData = kChannelType + ":" + QString::number(ch.id);
        QString label = QString("%1 (%2)").arg(ch.name).arg(ch.fileCount);
        pChannelsRoot->appendChild(label, itemData);
    }

    // Add "Favorites" section
    TreeItem* pFavoritesRoot = pRootItem->appendChild(
            tr("Favorites"), kRootFavorites);
    for (const tfm::Channel& ch : channels) {
        if (ch.isFavorite) {
            QString itemData = kFavoriteType + ":" + QString::number(ch.id);
            QString label = QString("%1 (%2)").arg(ch.name).arg(ch.fileCount);
            pFavoritesRoot->appendChild(label, itemData);
        }
    }

    // Add "Local Folder" section
    pRootItem->appendChild(tr("Local TFM Folder"), kRootLocal);

    // Update the model
    m_pSidebarModel->setRootItem(std::unique_ptr<TreeItem>(pRootItem.release()));

    // Also insert channels into database
    QSqlDatabase db = m_pTrackCollection->database();
    ScopedTransaction transaction(db);

    // Note: Don't clear the table - use ON CONFLICT to preserve auto-increment IDs

    for (const tfm::Channel& ch : channels) {
        QSqlQuery query(db);
        // Use INSERT...ON CONFLICT to preserve the auto-increment id
        query.prepare(
                "INSERT INTO " + kTFMPlaylistsTable +
                " (channel_id, name, description, image_url, track_count, is_favorite) "
                "VALUES (:channel_id, :name, :desc, :img, :count, :fav) "
                "ON CONFLICT(channel_id) DO UPDATE SET "
                "name = excluded.name, "
                "description = excluded.description, "
                "image_url = excluded.image_url, "
                "track_count = excluded.track_count, "
                "is_favorite = excluded.is_favorite");
        query.bindValue(":channel_id", QString::number(ch.id));
        query.bindValue(":name", ch.name);
        query.bindValue(":desc", ch.type);  // Use type as description
        query.bindValue(":img", ch.imageUrl);
        query.bindValue(":count", ch.fileCount);
        query.bindValue(":fav", ch.isFavorite ? 1 : 0);
        if (!query.exec()) {
            kLogger.warning() << "Failed to insert channel:" << query.lastError();
        }
    }

    transaction.commit();

    hideLoadingDialog();
    emit featureIsLoading(this, false);
    emit channelsLoaded();
}

void TFMFeature::slotTracksLoaded(const QString& channelId, const QList<tfm::Track>& tracks) {
    qWarning() << "TFMFeature::slotTracksLoaded - channelId:" << channelId << "tracks:" << tracks.size();

    // Separate audio files from folders
    QList<tfm::Track> audioTracks;
    QList<tfm::Track> folders;

    for (const tfm::Track& item : tracks) {
        // Check if it's a folder (either isFolder=true or isFile=false with category=Folder)
        bool itemIsFolder = item.isFolder || (!item.isFile && item.category == "Folder");

        if (itemIsFolder) {
            // This is a folder
            folders.append(item);
            qWarning() << "  Found folder:" << item.name << "id:" << item.id;
        } else if (item.category.toLower() == "audio" ||
                   item.name.endsWith(".mp3", Qt::CaseInsensitive) ||
                   item.name.endsWith(".flac", Qt::CaseInsensitive) ||
                   item.name.endsWith(".wav", Qt::CaseInsensitive) ||
                   item.name.endsWith(".ogg", Qt::CaseInsensitive) ||
                   item.name.endsWith(".m4a", Qt::CaseInsensitive)) {
            audioTracks.append(item);
            qWarning() << "  Found audio:" << item.name;
        } else {
            qWarning() << "  Skipping:" << item.name << "category:" << item.category << "isFile:" << item.isFile << "isFolder:" << item.isFolder;
        }
    }

    qWarning() << "Channel" << channelId << "has" << audioTracks.size() << "audio files and" << folders.size() << "folders";

    // If there are folders, add them to the sidebar as expandable items
    if (!folders.isEmpty()) {
        addFoldersToSidebar(channelId, folders);
    }

    // If we only have folders and no audio tracks, just update sidebar - user will select folder
    if (audioTracks.isEmpty() && !folders.isEmpty()) {
        qWarning() << "No audio files, folders added to sidebar for user selection";
        hideLoadingDialog();
        emit featureIsLoading(this, false);
        return;
    }

    // Insert audio tracks into database
    if (!audioTracks.isEmpty()) {
        insertTracksIntoDatabase(channelId, audioTracks);
    }

    // Store current channel
    m_currentChannelId = channelId;

    // Show tracks in the playlist model
    qWarning() << "Attempting to show tracks. m_pTFMPlaylistModel:" << (m_pTFMPlaylistModel ? "valid" : "null")
               << "audioTracks:" << audioTracks.size();

    if (m_pTFMPlaylistModel && !audioTracks.isEmpty()) {
        QSqlDatabase db = m_pTrackCollection->database();
        QSqlQuery query(db);
        // Get the integer ID for this channel
        query.prepare("SELECT id, name FROM " + kTFMPlaylistsTable + " WHERE channel_id = :channel_id");
        query.bindValue(":channel_id", channelId);

        int playlistId = -1;
        QString channelName;
        if (query.exec() && query.next()) {
            playlistId = query.value(0).toInt();
            channelName = query.value(1).toString();
            qWarning() << "Found playlist id:" << playlistId << "name:" << channelName;
        } else {
            qWarning() << "Channel query failed:" << query.lastError().text();
        }

        if (playlistId >= 0) {
            qWarning() << "Calling setPlaylistById with:" << playlistId;
            // Use BaseExternalPlaylistModel which properly filters by playlist
            m_pTFMPlaylistModel->setPlaylistById(playlistId);
            qWarning() << "Emitting showTrackModel for playlist";
            emit showTrackModel(m_pTFMPlaylistModel);
            qWarning() << "Showing tracks for channel" << channelId << "(" << channelName << ")";
        } else {
            qWarning() << "Could not find playlist for channel_id" << channelId;
        }
    } else if (audioTracks.isEmpty()) {
        qWarning() << "No audio tracks to show!";
    }

    hideLoadingDialog();
    emit featureIsLoading(this, false);
    emit tracksLoaded(channelId);
}

void TFMFeature::slotFolderContentsLoaded(const QString& channelId,
                                          const QString& folderId,
                                          const QList<tfm::Track>& items) {
    qWarning() << "TFMFeature::slotFolderContentsLoaded - channelId:" << channelId
               << "folderId:" << folderId << "items:" << items.size();

    // Separate audio files from folders
    QList<tfm::Track> audioTracks;
    QList<tfm::Track> folders;

    for (const tfm::Track& item : items) {
        // Check if it's a folder (either isFolder=true or isFile=false with category=Folder)
        bool itemIsFolder = item.isFolder || (!item.isFile && item.category == "Folder");

        if (itemIsFolder) {
            // This is a folder
            folders.append(item);
            qWarning() << "  Found folder:" << item.name << "id:" << item.id;
        } else if (item.category.toLower() == "audio" ||
                   item.name.endsWith(".mp3", Qt::CaseInsensitive) ||
                   item.name.endsWith(".flac", Qt::CaseInsensitive) ||
                   item.name.endsWith(".wav", Qt::CaseInsensitive) ||
                   item.name.endsWith(".ogg", Qt::CaseInsensitive) ||
                   item.name.endsWith(".m4a", Qt::CaseInsensitive)) {
            audioTracks.append(item);
            qWarning() << "  Found audio:" << item.name;
        } else {
            qWarning() << "  Skipping:" << item.name << "category:" << item.category << "isFile:" << item.isFile << "isFolder:" << item.isFolder;
        }
    }

    qWarning() << "Folder" << folderId << "contains" << audioTracks.size() << "audio files and" << folders.size() << "subfolders";

    // If there are subfolders, add them to the sidebar under the current folder item
    if (!folders.isEmpty()) {
        addSubfoldersToSidebar(channelId, folderId, folders);
    }

    // If we only have folders and no audio tracks, just update sidebar - user will select folder
    if (audioTracks.isEmpty() && !folders.isEmpty()) {
        qWarning() << "No audio files in folder, subfolders added to sidebar for user selection";
        hideLoadingDialog();
        emit featureIsLoading(this, false);
        return;
    }

    // Insert audio tracks into database
    if (!audioTracks.isEmpty()) {
        insertTracksIntoDatabase(channelId, audioTracks);
    }

    // Show the tracks
    qWarning() << "Attempting to show folder tracks. audioTracks:" << audioTracks.size();

    if (m_pTFMPlaylistModel && !audioTracks.isEmpty()) {
        QSqlDatabase db = m_pTrackCollection->database();
        QSqlQuery query(db);
        query.prepare("SELECT id, name FROM " + kTFMPlaylistsTable + " WHERE channel_id = :channel_id");
        query.bindValue(":channel_id", channelId);

        int playlistId = -1;
        QString channelName;
        if (query.exec() && query.next()) {
            playlistId = query.value(0).toInt();
            channelName = query.value(1).toString();
            qWarning() << "Found playlist id:" << playlistId << "name:" << channelName;
        }

        if (playlistId >= 0) {
            qWarning() << "Calling setPlaylistById with:" << playlistId;
            m_pTFMPlaylistModel->setPlaylistById(playlistId);
            qWarning() << "Emitting showTrackModel for playlist";
            emit showTrackModel(m_pTFMPlaylistModel);
        }
    } else if (audioTracks.isEmpty()) {
        qWarning() << "No audio tracks found in folder!";
    }

    hideLoadingDialog();
    emit featureIsLoading(this, false);
}

void TFMFeature::slotLocalTracksLoaded(const QString& folderPath, const QList<tfm::Track>& tracks) {
    qWarning() << "TFMFeature::slotLocalTracksLoaded - folderPath:" << folderPath << "tracks:" << tracks.size();

    // Separate audio files from folders
    QList<tfm::Track> audioTracks;
    QList<tfm::Track> folders;

    for (const tfm::Track& item : tracks) {
        bool itemIsFolder = item.isFolder || (!item.isFile && item.category == "Folder");

        if (itemIsFolder) {
            folders.append(item);
            qWarning() << "  Found folder:" << item.name << "path:" << item.path;
        } else if (item.category.toLower() == "audio" ||
                   item.name.endsWith(".mp3", Qt::CaseInsensitive) ||
                   item.name.endsWith(".flac", Qt::CaseInsensitive) ||
                   item.name.endsWith(".wav", Qt::CaseInsensitive) ||
                   item.name.endsWith(".ogg", Qt::CaseInsensitive) ||
                   item.name.endsWith(".m4a", Qt::CaseInsensitive)) {
            audioTracks.append(item);
            qWarning() << "  Found audio:" << item.name << "url:" << item.downloadUrl;
        } else {
            qWarning() << "  Skipping:" << item.name << "category:" << item.category;
        }
    }

    qWarning() << "Local folder" << folderPath << "has" << audioTracks.size() << "audio files and" << folders.size() << "folders";

    // Add folders to sidebar for navigation
    if (!folders.isEmpty()) {
        addLocalFoldersToSidebar(folderPath, folders);
    }

    // If we only have folders and no audio, just update sidebar
    if (audioTracks.isEmpty() && !folders.isEmpty()) {
        qWarning() << "No audio files in local folder, folders added to sidebar for navigation";
        hideLoadingDialog();
        emit featureIsLoading(this, false);
        return;
    }

    if (!audioTracks.isEmpty()) {
        // Local files don't belong to a channel, use special "local" channel
        QString localChannelId = "local";

        // Ensure local playlist exists in database
        ensureLocalPlaylistExists();

        insertTracksIntoDatabase(localChannelId, audioTracks);

        // Show the tracks using playlist model
        if (m_pTFMPlaylistModel) {
            QSqlDatabase db = m_pTrackCollection->database();
            QSqlQuery query(db);
            query.prepare("SELECT id FROM " + kTFMPlaylistsTable + " WHERE channel_id = 'local'");
            if (query.exec() && query.next()) {
                int playlistId = query.value(0).toInt();
                qWarning() << "Calling setPlaylistById with local playlist id:" << playlistId;
                m_pTFMPlaylistModel->setPlaylistById(playlistId);
                emit showTrackModel(m_pTFMPlaylistModel);
                qWarning() << "Showing" << audioTracks.size() << "local audio files";
            }
        }
    }

    if (!folders.isEmpty()) {
        qWarning() << "Local folders found - user should navigate manually (auto-nav disabled for local)";
        // Don't auto-navigate for local folders since there are many
        // The user should be able to browse the folder structure
    }

    hideLoadingDialog();
    emit featureIsLoading(this, false);
}

void TFMFeature::insertTracksIntoDatabase(const QString& channelId,
                                          const QList<tfm::Track>& tracks) {
    qWarning() << "insertTracksIntoDatabase: channelId=" << channelId << "tracks=" << tracks.size();

    QSqlDatabase db = m_pTrackCollection->database();
    if (!db.isOpen()) {
        kLogger.warning() << "Database is not open!";
        return;
    }

    ScopedTransaction transaction(db);

    // Get the integer playlist id for this channel
    int playlistId = -1;
    QSqlQuery idQuery(db);
    idQuery.prepare("SELECT id FROM " + kTFMPlaylistsTable + " WHERE channel_id = :channel_id");
    idQuery.bindValue(":channel_id", channelId);
    if (idQuery.exec() && idQuery.next()) {
        playlistId = idQuery.value(0).toInt();
        kLogger.info() << "Found playlistId:" << playlistId << "for channel" << channelId;
    } else {
        kLogger.warning() << "Query failed or no results:" << idQuery.lastError().text();
    }

    if (playlistId < 0) {
        kLogger.warning() << "Cannot find playlist id for channel" << channelId;
        return;
    }

    // Delete old tracks for this channel
    QSqlQuery deleteQuery(db);
    deleteQuery.prepare("DELETE FROM " + kTFMTracksTable + " WHERE channel_id = :channel");
    deleteQuery.bindValue(":channel", channelId);
    deleteQuery.exec();

    // Delete old playlist_tracks entries for this playlist
    QSqlQuery deleteLinkQuery(db);
    deleteLinkQuery.prepare("DELETE FROM " + kTFMPlaylistTracksTable + " WHERE playlist_id = :playlist_id");
    deleteLinkQuery.bindValue(":playlist_id", playlistId);
    deleteLinkQuery.exec();

    // Insert new tracks - map new Track fields to database schema
    int position = 0;
    for (const tfm::Track& tr : tracks) {
        // Only insert audio files
        if (tr.category.toLower() != "audio" &&
            !tr.name.endsWith(".mp3", Qt::CaseInsensitive) &&
            !tr.name.endsWith(".flac", Qt::CaseInsensitive) &&
            !tr.name.endsWith(".wav", Qt::CaseInsensitive) &&
            !tr.name.endsWith(".ogg", Qt::CaseInsensitive) &&
            !tr.name.endsWith(".m4a", Qt::CaseInsensitive)) {
            continue;
        }

        // Use streamUrl from API or construct it
        // For local files, use the path-based URL
        QString streamUrl;
        QString downloadUrl;

        if (channelId == "local") {
            // Local files ALWAYS use path-based URL: /api/mobile/stream/local/path=<encoded_path>
            // The API may return incorrect URLs, so always construct it ourselves
            QString localUrl = m_pApiClient->getLocalTrackUrl(tr.path);
            streamUrl = localUrl;
            downloadUrl = localUrl;
            qWarning() << "Local track URL:" << localUrl << "for path:" << tr.path;
        } else {
            // Channel files use id-based URLs
            streamUrl = tr.streamUrl.isEmpty()
                    ? m_pApiClient->getTrackStreamUrl(channelId, tr.id)
                    : tr.streamUrl;
            downloadUrl = tr.downloadUrl.isEmpty()
                    ? m_pApiClient->getTrackDownloadUrl(channelId, tr.id)
                    : tr.downloadUrl;
        }

        // Parse filename to extract artist and title
        TrackMetadata meta = parseFilenameMetadata(tr.name);

        // Insert new track (id will be auto-generated)
        // Don't check for existing - we already deleted all tracks for this channel
        QSqlQuery query(db);
        query.prepare(
                "INSERT INTO " + kTFMTracksTable +
                " (external_id, channel_id, artist, title, album, genre, "
                "duration, file_url, local_path, file_size, cover_url, bpm, key, location, "
                "datetime_added) "
                "VALUES (:ext_id, :channel, :artist, :title, :album, :genre, "
                ":dur, :url, :local, :size, :cover, :bpm, :key, :loc, "
                ":datetime_added)");
        query.bindValue(":ext_id", tr.id);  // MongoDB ObjectId stored in external_id
        query.bindValue(":channel", channelId);
        query.bindValue(":artist", meta.artist);  // Parsed from filename
        query.bindValue(":title", meta.title);    // Parsed from filename
        query.bindValue(":album", "");
        query.bindValue(":genre", "");  // Don't use category as genre
        query.bindValue(":dur", 0);  // Not available in API
        query.bindValue(":url", downloadUrl);  // Download URL for full file download
        query.bindValue(":local", tr.path);
        query.bindValue(":size", tr.size);
        query.bindValue(":cover", tr.thumbnailUrl);
        query.bindValue(":bpm", 0);
        query.bindValue(":key", "");
        query.bindValue(":loc", downloadUrl);  // Also use downloadUrl as location (primary for deck loading)
        // Store date as ISO 8601 string - use dateCreated from TFM API
        query.bindValue(":datetime_added", tr.dateCreated.isValid() ? tr.dateCreated.toString(Qt::ISODate) : QString());

        qWarning() << "Inserting track:" << tr.name << "file_url:" << downloadUrl << "local_path:" << tr.path;

        if (!query.exec()) {
            kLogger.warning() << "Failed to insert track:" << query.lastError() << "for" << tr.name;
            continue;
        }

        // Get the auto-generated ID
        QVariant lastId = query.lastInsertId();
        if (!lastId.isValid()) {
            kLogger.warning() << "lastInsertId invalid for" << tr.name;
            continue;
        }
        int trackId = lastId.toInt();

        qWarning() << "Inserted track" << position << "id:" << trackId << "title:" << meta.title;

        // Insert into playlist_tracks linking table (using integer IDs)
        QSqlQuery linkQuery(db);
        linkQuery.prepare(
                "INSERT INTO " + kTFMPlaylistTracksTable +
                " (playlist_id, track_id, position) "
                "VALUES (:playlist, :track, :pos)");
        linkQuery.bindValue(":playlist", playlistId);
        linkQuery.bindValue(":track", trackId);  // Now using integer track ID
        linkQuery.bindValue(":pos", position++);
        if (!linkQuery.exec()) {
            kLogger.warning() << "Failed to insert playlist_track link:" << linkQuery.lastError();
        }
    }

    transaction.commit();
    kLogger.info() << "Inserted" << position << "tracks for channel" << channelId;
}

TreeItem* TFMFeature::findTreeItemByData(TreeItem* parent, const QString& dataStr) {
    if (!parent) {
        return nullptr;
    }

    // Check if this item matches
    QString parentData = parent->getData().toString();
    if (parentData == dataStr) {
        qWarning() << "findTreeItemByData: Found match at" << parent->getLabel();
        return parent;
    }

    // Check children recursively
    for (int i = 0; i < parent->childRows(); ++i) {
        TreeItem* child = parent->child(i);
        if (child) {
            TreeItem* found = findTreeItemByData(child, dataStr);
            if (found) {
                return found;
            }
        }
    }

    return nullptr;
}

QModelIndex TFMFeature::getModelIndexForTreeItem(TreeItem* item, TreeItem* rootItem) {
    if (!item || !rootItem || item == rootItem) {
        return QModelIndex();
    }

    // Build path from root to item
    QList<TreeItem*> path;
    TreeItem* current = item;
    while (current && current != rootItem) {
        path.prepend(current);
        current = current->parent();
    }

    if (path.isEmpty()) {
        return QModelIndex();
    }

    // Build QModelIndex by traversing the path
    QModelIndex result;
    TreeItem* parentItem = rootItem;
    for (TreeItem* pathItem : path) {
        int row = -1;
        for (int i = 0; i < parentItem->childRows(); ++i) {
            if (parentItem->child(i) == pathItem) {
                row = i;
                break;
            }
        }
        if (row < 0) {
            qWarning() << "getModelIndexForTreeItem: Could not find row for" << pathItem->getLabel();
            return QModelIndex();
        }
        result = m_pSidebarModel->index(row, 0, result);
        parentItem = pathItem;
    }

    return result;
}

void TFMFeature::addFoldersToSidebar(const QString& channelId, const QList<tfm::Track>& folders) {
    if (!m_pSidebarModel) {
        kLogger.warning() << "Cannot add folders - sidebar model is null";
        return;
    }

    TreeItem* rootItem = m_pSidebarModel->getRootItem();
    if (!rootItem) {
        kLogger.warning() << "Cannot add folders - root item is null";
        return;
    }

    // Find the channel item in the sidebar based on which section was clicked
    // Use m_currentItemType to determine if we should look in channels or favorites
    QString itemData;
    if (m_currentItemType == kFavoriteType) {
        itemData = kFavoriteType + ":" + channelId;
    } else {
        itemData = kChannelType + ":" + channelId;
    }

    TreeItem* channelItem = findTreeItemByData(rootItem, itemData);

    // Fallback: try the other type if not found
    if (!channelItem) {
        QString fallbackData = (m_currentItemType == kFavoriteType)
            ? kChannelType + ":" + channelId
            : kFavoriteType + ":" + channelId;
        channelItem = findTreeItemByData(rootItem, fallbackData);
    }

    if (!channelItem) {
        kLogger.warning() << "Could not find channel item in sidebar for channelId:" << channelId;
        return;
    }

    qWarning() << "Found item for folders:" << channelItem->getLabel() << "data:" << channelItem->getData().toString();

    // Check if folders are already added (to avoid duplicates)
    if (channelItem->childRows() > 0) {
        qWarning() << "Channel already has child items, skipping folder addition";
        return;
    }

    // Build the QModelIndex by traversing from root to channelItem
    QModelIndex channelModelIndex = getModelIndexForTreeItem(channelItem, rootItem);

    if (!channelModelIndex.isValid()) {
        qWarning() << "Could not calculate valid QModelIndex for channel item";
        return;
    }

    qWarning() << "Channel QModelIndex valid:" << channelModelIndex.isValid()
               << "row:" << channelModelIndex.row();

    // Create TreeItems for each folder
    std::vector<std::unique_ptr<TreeItem>> folderItems;
    for (const tfm::Track& folder : folders) {
        QString folderData = kFolderType + ":" + channelId + ":" + folder.id;
        qWarning() << "Adding folder to sidebar:" << folder.name << "data:" << folderData;
        folderItems.push_back(std::make_unique<TreeItem>(folder.name, folderData));
    }

    // Use insertTreeItemRows which properly notifies the view
    m_pSidebarModel->insertTreeItemRows(std::move(folderItems), 0, channelModelIndex);

    // Trigger repaint to ensure the expand arrow is shown
    m_pSidebarModel->triggerRepaint(channelModelIndex);

    // Expand the parent item in the sidebar so the user can see the folders
    if (m_pSidebarWidget) {
        m_pSidebarWidget->expand(channelModelIndex);
    }

    qWarning() << "Added" << folders.size() << "folders to channel" << channelId << "in sidebar";
}

void TFMFeature::addSubfoldersToSidebar(const QString& channelId,
                                        const QString& parentFolderId,
                                        const QList<tfm::Track>& subfolders) {
    if (!m_pSidebarModel) {
        kLogger.warning() << "Cannot add subfolders - sidebar model is null";
        return;
    }

    TreeItem* rootItem = m_pSidebarModel->getRootItem();
    if (!rootItem) {
        kLogger.warning() << "Cannot add subfolders - root item is null";
        return;
    }

    // Find the parent folder item in the sidebar
    QString folderData = kFolderType + ":" + channelId + ":" + parentFolderId;
    TreeItem* folderItem = findTreeItemByData(rootItem, folderData);

    if (!folderItem) {
        kLogger.warning() << "Could not find folder item in sidebar for folderId:" << parentFolderId;
        return;
    }

    // Check if subfolders are already added (to avoid duplicates)
    if (folderItem->childRows() > 0) {
        qWarning() << "Folder already has child items, skipping subfolder addition";
        return;
    }

    // Get QModelIndex for the folder item
    QModelIndex folderModelIndex = getModelIndexForTreeItem(folderItem, rootItem);

    if (!folderModelIndex.isValid()) {
        qWarning() << "Could not calculate valid QModelIndex for folder item";
        return;
    }

    // Create TreeItems for each subfolder
    std::vector<std::unique_ptr<TreeItem>> subfolderItems;
    for (const tfm::Track& subfolder : subfolders) {
        QString subfolderData = kFolderType + ":" + channelId + ":" + subfolder.id;
        qWarning() << "Adding subfolder to sidebar:" << subfolder.name << "data:" << subfolderData;
        subfolderItems.push_back(std::make_unique<TreeItem>(subfolder.name, subfolderData));
    }

    // Use insertTreeItemRows which properly notifies the view
    m_pSidebarModel->insertTreeItemRows(std::move(subfolderItems), 0, folderModelIndex);

    // Trigger repaint to ensure the expand arrow is shown
    m_pSidebarModel->triggerRepaint(folderModelIndex);

    // Expand the parent folder in the sidebar so the user can see the subfolders
    if (m_pSidebarWidget) {
        m_pSidebarWidget->expand(folderModelIndex);
    }

    qWarning() << "Added" << subfolders.size() << "subfolders under folder" << parentFolderId << "in sidebar";
}

void TFMFeature::addLocalFoldersToSidebar(const QString& parentFolderPath, const QList<tfm::Track>& folders) {
    qWarning() << "addLocalFoldersToSidebar called - parentFolderPath:" << parentFolderPath << "folders:" << folders.size();

    if (!m_pSidebarModel) {
        kLogger.warning() << "Cannot add local folders - sidebar model is null";
        return;
    }

    TreeItem* rootItem = m_pSidebarModel->getRootItem();
    if (!rootItem) {
        kLogger.warning() << "Cannot add local folders - root item is null";
        return;
    }

    // Debug: print all children of root
    qWarning() << "Root item has" << rootItem->childRows() << "children:";
    for (int i = 0; i < rootItem->childRows(); ++i) {
        TreeItem* child = rootItem->child(i);
        if (child) {
            qWarning() << "  Child" << i << ":" << child->getLabel() << "data:" << child->getData().toString();
        }
    }

    TreeItem* targetItem = nullptr;
    QString targetData;

    if (parentFolderPath.isEmpty()) {
        // Root local folder - add under "Local TFM Folder" root item
        targetData = kRootLocal;
        qWarning() << "Looking for root local item with data:" << targetData;
        targetItem = findTreeItemByData(rootItem, kRootLocal);
    } else {
        // Subfolder - add under the parent local folder (using path)
        targetData = kLocalFolderType + ":" + parentFolderPath;
        qWarning() << "Looking for subfolder item with data:" << targetData;
        targetItem = findTreeItemByData(rootItem, targetData);
    }

    if (!targetItem) {
        kLogger.warning() << "Could not find target item for local folders. parentFolderPath:" << parentFolderPath << "targetData:" << targetData;
        return;
    }

    qWarning() << "Found target item:" << targetItem->getLabel() << "with" << targetItem->childRows() << "existing children";

    // Check if folders are already added (to avoid duplicates)
    if (targetItem->childRows() > 0) {
        qWarning() << "Target already has child items, skipping local folder addition";
        return;
    }

    // Get QModelIndex for the target item
    QModelIndex targetModelIndex = getModelIndexForTreeItem(targetItem, rootItem);

    if (!targetModelIndex.isValid()) {
        qWarning() << "Could not calculate valid QModelIndex for target item";
        return;
    }

    // Create TreeItems for each folder
    std::vector<std::unique_ptr<TreeItem>> folderItems;
    for (const tfm::Track& folder : folders) {
        // Use path for navigation (format: "local_folder:/path/to/folder")
        QString folderData = kLocalFolderType + ":" + folder.path;
        qWarning() << "Adding local folder to sidebar:" << folder.name << "path:" << folder.path << "data:" << folderData;
        folderItems.push_back(std::make_unique<TreeItem>(folder.name, folderData));
    }

    // Use insertTreeItemRows which properly notifies the view
    m_pSidebarModel->insertTreeItemRows(std::move(folderItems), 0, targetModelIndex);

    // Trigger repaint to ensure the expand arrow is shown
    m_pSidebarModel->triggerRepaint(targetModelIndex);

    // Expand the parent item in the sidebar so the user can see the folders
    if (m_pSidebarWidget) {
        m_pSidebarWidget->expand(targetModelIndex);
    }

    qWarning() << "Added" << folders.size() << "local folders to sidebar";
}

void TFMFeature::ensureLocalPlaylistExists() {
    QSqlDatabase db = m_pTrackCollection->database();

    // Check if local playlist already exists
    QSqlQuery checkQuery(db);
    checkQuery.prepare("SELECT id FROM " + kTFMPlaylistsTable + " WHERE channel_id = 'local'");
    if (checkQuery.exec() && checkQuery.next()) {
        // Already exists
        return;
    }

    // Create local playlist entry
    QSqlQuery insertQuery(db);
    insertQuery.prepare(
            "INSERT INTO " + kTFMPlaylistsTable +
            " (channel_id, name, description, track_count, is_favorite) "
            "VALUES ('local', 'Local TFM', 'Local TFM files', 0, 0)");
    if (!insertQuery.exec()) {
        kLogger.warning() << "Failed to create local playlist:" << insertQuery.lastError();
    } else {
        kLogger.info() << "Created local playlist entry";
    }
}

void TFMFeature::slotApiError(const QString& error) {
    kLogger.warning() << "TFM API error:" << error;
    hideLoadingDialog();
    emit featureIsLoading(this, false);
    emit apiError(error);

    // Show error to user
    if (m_pSidebarWidget) {
        QMessageBox::warning(m_pSidebarWidget,
                tr("TFM Error"),
                tr("Failed to communicate with TFM server:\n%1").arg(error));
    }
}

void TFMFeature::slotRefresh() {
    kLogger.info() << "Refreshing TFM library";
    activate(true);
}

void TFMFeature::showLoadingDialog(const QString& message) {
    // Hide any existing dialog first
    hideLoadingDialog();

    // Create progress dialog - use sidebar widget as parent if available
    QWidget* parent = m_pSidebarWidget ? m_pSidebarWidget->window() : nullptr;
    m_pLoadingDialog = new QProgressDialog(message, QString(), 0, 0, parent);
    m_pLoadingDialog->setWindowTitle(tr("TelegramFileManager"));
    m_pLoadingDialog->setWindowModality(Qt::WindowModal);
    m_pLoadingDialog->setMinimumDuration(300);  // Show after 300ms to avoid flicker for fast operations
    m_pLoadingDialog->setCancelButton(nullptr);  // No cancel button
    m_pLoadingDialog->setAutoClose(false);
    m_pLoadingDialog->setAutoReset(false);
    m_pLoadingDialog->show();

    // Process events to ensure dialog is shown
    QCoreApplication::processEvents();
}

void TFMFeature::hideLoadingDialog() {
    if (m_pLoadingDialog) {
        m_pLoadingDialog->close();
        m_pLoadingDialog->deleteLater();
        m_pLoadingDialog = nullptr;
    }
}

void TFMFeature::slotConfigure() {
    bool ok;
    QString currentUrl = m_pApiClient->serverUrl();
    QString url = QInputDialog::getText(
            m_pSidebarWidget,
            tr("Configure TFM Server"),
            tr("Enter the TFM server URL (e.g., http://localhost:5000):"),
            QLineEdit::Normal,
            currentUrl,
            &ok);

    if (ok && !url.isEmpty()) {
        setServerUrl(url);
        kLogger.info() << "TFM server URL configured:" << url;

        // Test connection and reload
        activate(true);
    }
}

#include "moc_tfmfeature.cpp"
