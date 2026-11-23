/*
    This file is part of PhotographyLAB, a darktable-based photo editor.
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "labcontroller.h"
#include "engineworker.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRunnable>
#include <QSettings>

#include "plab_engine.h"

namespace
{
class ThumbResponse : public QQuickImageResponse, public QRunnable
{
public:
  ThumbResponse(int imgid, QSize size, bool ready)
    : m_imgid(imgid), m_size(size), m_ready(ready)
  {
    setAutoDelete(false);
  }
  QQuickTextureFactory *textureFactory() const override
  {
    return QQuickTextureFactory::textureFactoryForImage(m_image);
  }
  void run() override
  {
    if(m_ready && m_imgid > 0)
    {
      int w = 0, h = 0;
      uint8_t *buf = plab_thumbnail(m_imgid, m_size.width(), m_size.height(), &w, &h);
      // the thumbnail cache keeps RGBA byte order, unlike the export path
      if(buf) m_image = QImage(buf, w, h, w * 4, QImage::Format_RGBX8888, plab_free, buf);
    }
    emit finished();
  }

private:
  int m_imgid;
  QSize m_size;
  bool m_ready;
  QImage m_image;
};
} // namespace

ThumbnailProvider::ThumbnailProvider()
{
  // raw files decode on the CPU; two at a time keeps the editor responsive
  m_pool.setMaxThreadCount(2);
}

QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id,
                                                             const QSize &requested)
{
  // id is "<imgid>/<version>": the version only defeats QML's image cache
  const int imgid = id.section(QLatin1Char('/'), 0, 0).toInt();
  const QSize size = requested.isValid() && !requested.isEmpty() ? requested : QSize(400, 400);
  auto *r = new ThumbResponse(imgid, size, ready.load());
  m_pool.start(r);
  return r;
}

QImage PreviewProvider::requestImage(const QString &, QSize *size, const QSize &)
{
  QMutexLocker lock(&m_lock);
  if(size) *size = m_image.size();
  return m_image;
}

void PreviewProvider::set(const QImage &img)
{
  QMutexLocker lock(&m_lock);
  m_image = img;
}

LabController::LabController(PreviewProvider *provider, ThumbnailProvider *thumbs,
                             QObject *parent)
  : QObject(parent), m_provider(provider), m_adjust(defaultAdjust()), m_thumbs(thumbs)
{
  m_worker = new EngineWorker;
  m_worker->moveToThread(&m_thread);
  connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

  connect(this, &LabController::requestOpen, m_worker, &EngineWorker::open);
  connect(this, &LabController::requestRender, m_worker, &EngineWorker::render);
  connect(this, &LabController::requestOriginal, m_worker, &EngineWorker::renderOriginal);
  connect(this, &LabController::requestStyle, m_worker, &EngineWorker::applyStyle);
  connect(this, &LabController::requestImportStyles, m_worker, &EngineWorker::importStyles);
  connect(this, &LabController::requestExport, m_worker, &EngineWorker::exportImage);
  connect(this, &LabController::requestOpenId, m_worker, &EngineWorker::openId);
  connect(this, &LabController::requestImportFolder, m_worker, &EngineWorker::importFolder);
  connect(this, &LabController::requestFolders, m_worker, &EngineWorker::listFolders);
  connect(this, &LabController::requestImages, m_worker, &EngineWorker::listImages);
  connect(this, &LabController::requestRating, m_worker, &EngineWorker::setRating);
  connect(this, &LabController::requestLabel, m_worker, &EngineWorker::toggleLabel);
  connect(this, &LabController::requestAddLuts, m_worker, &EngineWorker::addLuts);
  connect(this, &LabController::requestStyleImages, m_worker, &EngineWorker::styleImages);
  connect(this, &LabController::requestPaste, m_worker, &EngineWorker::pasteEdit);
  connect(this, &LabController::requestExportImages, m_worker, &EngineWorker::exportImages);
  connect(this, &LabController::requestAiDenoise, m_worker, &EngineWorker::aiDenoise);
  connect(m_worker, &EngineWorker::aiStatus, this, [this](bool built, bool ready) {
    m_aiBuilt = built;
    m_aiReady = ready;
    emit aiChanged();
  });
  connect(m_worker, &EngineWorker::aiProgress, this, [this](const QString &stage, double f) {
    m_aiStage = f < 0.0 ? QString() : stage;
    m_aiProgress = qMax(f, 0.0);
    emit aiChanged();
    if(!stage.isEmpty()) setMessage(stage);
  });

  connect(m_worker, &EngineWorker::foldersListed, this, [this](const QVariantList &f) {
    m_folders = f;
    emit foldersChanged();
  });
  connect(m_worker, &EngineWorker::imagesListed, this, [this](const QVariantList &imgs, int gen) {
    if(gen != m_listGeneration) return; // an older filter's answer
    m_images = imgs;
    // keep only what is still shown selected
    QList<int> keep;
    for(const QVariant &v : imgs)
    {
      const int id = v.toMap().value(QStringLiteral("id")).toInt();
      if(m_selected.contains(id)) keep << id;
    }
    if(keep != m_selected)
    {
      m_selected = keep;
      emit selectionChanged();
    }
    emit imagesChanged();
  });
  connect(m_worker, &EngineWorker::imagesChanged, this, [this](const QList<int> &ids) {
    for(int id : ids)
    {
      const QString k = QString::number(id);
      m_thumbVersions.insert(k, m_thumbVersions.value(k).toInt() + 1);
    }
    emit thumbsChanged();
    if(m_mode == QLatin1String("library")) refreshImages();
  });
  connect(m_worker, &EngineWorker::exportProgress, this,
          [this](int done, int total, const QString &file) {
    m_exportDone = done;
    m_exportTotal = total;
    m_exportFile = file;
    emit exportChanged();
  });
  connect(m_worker, &EngineWorker::exportFinished, this,
          [this](int done, int total, const QString &folder, const QString &error) {
    setInFlight(false);
    m_exportDone = m_exportTotal = 0;
    m_exportFile.clear();
    m_lastExportFolder = QUrl::fromLocalFile(folder);
    emit exportChanged();
    if(!error.isEmpty())
      setMessage(tr("exported %1 of %2 photos: %3").arg(done).arg(total).arg(error));
    else if(done < total)
      setMessage(tr("export stopped after %1 of %2 photos").arg(done).arg(total));
    else
      setMessage(tr("exported %n photo(s) to %1", nullptr, done).arg(QDir(folder).dirName()));
  });
  connect(m_worker, &EngineWorker::batchDone, this, [this](const QString &m) {
    setInFlight(false);
    setMessage(m);
    refreshImages();
  });

  connect(m_worker, &EngineWorker::started, this, [this](bool ok, const QStringList &styles) {
    m_ready = ok;
    m_thumbs->ready = ok;
    m_styles = styles;
    if(ok)
    {
      QMetaObject::invokeMethod(m_worker, &EngineWorker::checkAi);
      emit requestFolders();
      refreshImages();
    }
    emit stylesChanged();
    emit readyChanged();
    setMessage(ok ? QString() : tr("the darktable engine could not start"));
  });

  connect(m_worker, &EngineWorker::opened, this,
          [this](int imgid, const QString &path, double asShot, const QString &style,
                 const QVariantMap &edit, const QString &error) {
    setInFlight(false);
    if(imgid < 0)
    {
      setMessage(error);
      return;
    }
    m_imgid = imgid;
    m_path = path;
    m_asShot = asShot;
    m_style = style;
    m_adjust = defaultAdjust();
    const QVariantMap saved = edit.value(QStringLiteral("adjust")).toMap();
    for(auto it = saved.begin(); it != saved.end(); ++it) m_adjust.insert(it.key(), it.value());
    m_luts = edit.value(QStringLiteral("luts")).toList();
    emit imageChanged();
    setMode(QStringLiteral("develop"));
    emit requestFolders();
    emit adjustChanged();
    emit lutsChanged();
    emit currentStyleChanged();
    setMessage(QString());
    scheduleRender();
  });

  connect(m_worker, &EngineWorker::previewReady, this, [this](const QImage &img, int gen) {
    m_provider->set(img);
    m_shown = gen;
    setInFlight(false);
    if(m_dirty) kick();
    emit previewChanged();
  });

  connect(m_worker, &EngineWorker::styleApplied, this, [this](const QString &, int) {
    setInFlight(false);
    scheduleRender();
  });

  connect(m_worker, &EngineWorker::stylesChanged, this, [this](const QStringList &styles) {
    m_styles = styles;
    emit stylesChanged();
  });

  connect(m_worker, &EngineWorker::exported, this, [this](const QString &path, const QString &error) {
    setInFlight(false);
    setMessage(error.isEmpty() ? tr("saved %1").arg(QFileInfo(path).fileName()) : error);
    if(m_dirty) kick();
  });

  connect(m_worker, &EngineWorker::failed, this, [this](const QString &m) {
    setMessage(m);
  });

  m_thread.start();
}

LabController::~LabController()
{
  QMetaObject::invokeMethod(m_worker, &EngineWorker::stop, Qt::BlockingQueuedConnection);
  m_thread.quit();
  m_thread.wait();
}

void LabController::start(const QStringList &coreArgs, const QString &stylesDir,
                          const QString &lutDir, const QString &dataDir)
{
  m_lutDir = lutDir;
  emit lutCollectionChanged();
  // the bundled styles go into the library once; after that they are the
  // user's to rename or delete
  QSettings settings(QDir(dataDir).filePath(QStringLiteral("photographylab.ini")),
                     QSettings::IniFormat);
  const bool first = !settings.value(QStringLiteral("stylesImported")).toBool();
  settings.setValue(QStringLiteral("stylesImported"), true);
  QMetaObject::invokeMethod(m_worker, [=, w = m_worker] {
    w->start(coreArgs, stylesDir, lutDir, first);
  });
}

QVariantMap LabController::defaultAdjust()
{
  return {
    { QStringLiteral("temperature"), 0.0 }, // 0: as shot
    { QStringLiteral("tint"), 0.0 },
    { QStringLiteral("exposure"), 0.0 },
    { QStringLiteral("contrast"), 0.0 },
    { QStringLiteral("highlights"), 0.0 },
    { QStringLiteral("shadows"), 0.0 },
    { QStringLiteral("blacks"), 0.0 },
    { QStringLiteral("clarity"), 0.0 },
    { QStringLiteral("dehaze"), 0.0 },
    { QStringLiteral("vibrance"), 0.0 },
    { QStringLiteral("saturation"), 0.0 },
    { QStringLiteral("noise"), 0.0 },
    { QStringLiteral("straighten"), 0.0 },
    { QStringLiteral("lens"), true },
  };
}

QVariantMap LabController::editState() const
{
  return { { QStringLiteral("adjust"), m_adjust }, { QStringLiteral("luts"), m_luts } };
}

QString LabController::imageName() const
{
  return QFileInfo(m_path).fileName();
}

QString LabController::previewSource() const
{
  return m_shown > 0 ? QStringLiteral("image://preview/%1").arg(m_shown) : QString();
}

void LabController::setMessage(const QString &m)
{
  if(m == m_message) return;
  m_message = m;
  emit messageChanged();
}

void LabController::setInFlight(bool on)
{
  if(on == m_inFlight) return;
  m_inFlight = on;
  emit busyChanged();
}

// one request at a time: slider drags arrive far faster than the pipe renders,
// so only the newest state is sent once the engine is free
void LabController::scheduleRender()
{
  if(m_imgid < 0) return;
  m_dirty = true;
  if(!m_inFlight) kick();
}

void LabController::kick()
{
  m_dirty = false;
  setInFlight(true);
  ++m_generation;
  if(m_original)
    emit requestOriginal(m_viewW, m_viewH, m_generation);
  else
    emit requestRender(editState(), m_viewW, m_viewH, m_generation);
}

void LabController::openFile(const QUrl &url)
{
  const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
  if(path.isEmpty()) return;
  setInFlight(true);
  setMessage(tr("opening %1").arg(QFileInfo(path).fileName()));
  emit requestOpen(path);
}

void LabController::setValue(const QString &key, const QVariant &value)
{
  if(m_adjust.value(key) == value) return;
  m_adjust.insert(key, value);
  emit adjustChanged();
  scheduleRender();
}

void LabController::resetAdjustments()
{
  m_adjust = defaultAdjust();
  emit adjustChanged();
  scheduleRender();
}

void LabController::applyStyle(const QString &name)
{
  if(m_imgid < 0) return;
  m_style = name;
  emit currentStyleChanged();
  // queued behind any render in flight, which the worker finishes first
  setInFlight(true);
  emit requestStyle(name);
}

void LabController::importStyles(const QUrl &url)
{
  emit requestImportStyles(url.toLocalFile());
}

void LabController::addLut(const QUrl &url)
{
  const QString file = copyLut(url);
  if(file.isEmpty()) return;
  m_luts.append(QVariantMap{ { QStringLiteral("file"), file },
                             { QStringLiteral("strength"), 100.0 } });
  emit lutsChanged();
  scheduleRender();
}

void LabController::removeLut(int index)
{
  if(index < 0 || index >= m_luts.size()) return;
  m_luts.removeAt(index);
  emit lutsChanged();
  scheduleRender();
}

void LabController::setLutStrength(int index, double strength)
{
  if(index < 0 || index >= m_luts.size()) return;
  QVariantMap l = m_luts[index].toMap();
  if(l.value(QStringLiteral("strength")).toDouble() == strength) return;
  l.insert(QStringLiteral("strength"), strength);
  m_luts[index] = l;
  emit lutsChanged();
  scheduleRender();
}

void LabController::exportTo(const QUrl &url, int quality)
{
  if(m_imgid < 0) return;
  setInFlight(true);
  setMessage(tr("exporting..."));
  emit requestExport(url.toLocalFile(), quality);
}

void LabController::setViewSize(int width, int height)
{
  // previews are rendered at about screen size, never past 2560 pixels
  width = qBound(256, width, 2560);
  height = qBound(256, height, 2560);
  if(qAbs(width - m_viewW) < 64 && qAbs(height - m_viewH) < 64) return;
  m_viewW = width;
  m_viewH = height;
  scheduleRender();
}

void LabController::showOriginal(bool on)
{
  if(on == m_original) return;
  m_original = on;
  scheduleRender();
}

// ---------------------------------------------------------------------------
// library
// ---------------------------------------------------------------------------

void LabController::setMode(const QString &mode)
{
  if(mode == m_mode) return;
  m_mode = mode;
  emit modeChanged();
  if(m_mode == QLatin1String("library")) refreshImages();
}

QVariantList LabController::selection() const
{
  QVariantList out;
  for(int id : m_selected) out << id;
  return out;
}

void LabController::refreshImages()
{
  if(!m_ready) return;
  emit requestImages(m_folder, m_minRating, m_rejected, ++m_listGeneration);
}

QList<int> LabController::selectedOrCurrent() const
{
  if(m_mode == QLatin1String("develop") && m_imgid >= 0) return { m_imgid };
  return m_selected;
}

void LabController::importFolder(const QUrl &url, bool recursive)
{
  const QString folder = url.toLocalFile();
  if(folder.isEmpty()) return;
  setInFlight(true);
  setMessage(tr("importing %1...").arg(QFileInfo(folder).fileName()));
  emit requestImportFolder(folder, recursive);
}

void LabController::showFolder(int filmId)
{
  m_folder = filmId;
  emit filterChanged();
  refreshImages();
}

void LabController::setMinRating(int stars)
{
  m_minRating = qBound(0, stars, 5);
  emit filterChanged();
  refreshImages();
}

void LabController::setShowRejected(bool on)
{
  m_rejected = on;
  emit filterChanged();
  refreshImages();
}

void LabController::select(int imgid, int how)
{
  if(how == 1)
  {
    if(m_selected.contains(imgid)) m_selected.removeAll(imgid);
    else m_selected << imgid;
    m_anchor = imgid;
  }
  else if(how == 2 && m_anchor >= 0)
  {
    int a = -1, b = -1;
    for(int i = 0; i < m_images.size(); i++)
    {
      const int id = m_images[i].toMap().value(QStringLiteral("id")).toInt();
      if(id == m_anchor) a = i;
      if(id == imgid) b = i;
    }
    if(a < 0 || b < 0) return select(imgid, 0);
    m_selected.clear();
    for(int i = qMin(a, b); i <= qMax(a, b); i++)
      m_selected << m_images[i].toMap().value(QStringLiteral("id")).toInt();
  }
  else
  {
    m_selected = { imgid };
    m_anchor = imgid;
  }
  emit selectionChanged();
}

void LabController::selectAll()
{
  m_selected.clear();
  for(const QVariant &v : std::as_const(m_images))
    m_selected << v.toMap().value(QStringLiteral("id")).toInt();
  emit selectionChanged();
}

void LabController::clearSelection()
{
  m_selected.clear();
  emit selectionChanged();
}

void LabController::rateSelected(int stars)
{
  const QList<int> ids = selectedOrCurrent();
  if(!ids.isEmpty()) emit requestRating(ids, qBound(0, stars, 5));
}

void LabController::rejectSelected()
{
  const QList<int> ids = selectedOrCurrent();
  if(!ids.isEmpty()) emit requestRating(ids, 6);
}

void LabController::toggleLabelSelected(int color)
{
  const QList<int> ids = selectedOrCurrent();
  if(!ids.isEmpty()) emit requestLabel(ids, color);
}

QString LabController::copyLut(const QUrl &url)
{
  const QString src = url.toLocalFile();
  const QFileInfo fi(src);
  if(!fi.isFile()) return {};
  // lut3d reads paths relative to one folder, so every LUT is kept there
  const QString dst = QDir(m_lutDir).filePath(fi.fileName());
  if(QFileInfo(dst).canonicalFilePath() != fi.canonicalFilePath())
  {
    QFile::remove(dst);
    QFile::copy(src, dst);
    emit lutCollectionChanged();
  }
  return fi.fileName();
}

static const QStringList kLutPatterns = { QStringLiteral("*.cube"), QStringLiteral("*.3dl"),
                                          QStringLiteral("*.png") };

QStringList LabController::lutCollection() const
{
  if(m_lutDir.isEmpty()) return {};
  return QDir(m_lutDir).entryList(kLutPatterns, QDir::Files, QDir::Name | QDir::IgnoreCase);
}

int LabController::importLuts(const QVariantList &urls)
{
  int n = 0;
  for(const QVariant &v : urls)
  {
    const QUrl url = v.toUrl();
    const QFileInfo fi(url.toLocalFile());
    if(fi.isDir())
    {
      const QStringList files = QDir(fi.filePath()).entryList(kLutPatterns, QDir::Files);
      for(const QString &f : files)
        if(!copyLut(QUrl::fromLocalFile(QDir(fi.filePath()).filePath(f))).isEmpty()) n++;
    }
    else if(!copyLut(url).isEmpty())
      n++;
  }
  setMessage(tr("%n LUT(s) in your collection now", nullptr, int(lutCollection().size())));
  return n;
}

void LabController::toggleLut(const QString &file)
{
  for(int i = 0; i < m_luts.size(); i++)
    if(m_luts[i].toMap().value(QStringLiteral("file")).toString() == file)
    {
      removeLut(i);
      return;
    }
  m_luts.append(QVariantMap{ { QStringLiteral("file"), file },
                             { QStringLiteral("strength"), 100.0 } });
  emit lutsChanged();
  scheduleRender();
}

void LabController::moveLut(int from, int to)
{
  if(from < 0 || from >= m_luts.size() || to < 0 || to >= m_luts.size() || from == to) return;
  m_luts.move(from, to);
  emit lutsChanged();
  scheduleRender();
}

void LabController::clearLuts()
{
  if(m_luts.isEmpty()) return;
  m_luts.clear();
  emit lutsChanged();
  scheduleRender();
}

void LabController::addLutsToSelected(const QStringList &files)
{
  if(files.isEmpty() || m_selected.isEmpty()) return;
  QVariantList luts;
  for(const QString &f : files)
    luts << QVariantMap{ { QStringLiteral("file"), f }, { QStringLiteral("strength"), 100.0 } };
  setInFlight(true);
  setMessage(tr("adding %1 LUT(s) to %2 photo(s)...").arg(files.size()).arg(m_selected.size()));
  emit requestAddLuts(m_selected, luts);
}

void LabController::addLutToSelected(const QUrl &url)
{
  const QString file = copyLut(url);
  if(file.isEmpty() || m_selected.isEmpty()) return;
  setInFlight(true);
  setMessage(tr("adding %1 to %n photo(s)...", nullptr, int(m_selected.size())).arg(file));
  emit requestAddLuts(m_selected, { QVariantMap{ { QStringLiteral("file"), file },
                                                 { QStringLiteral("strength"), 100.0 } } });
}

void LabController::styleSelected(const QString &name)
{
  if(m_selected.isEmpty()) return;
  setInFlight(true);
  setMessage(tr("styling %n photo(s)...", nullptr, int(m_selected.size())));
  emit requestStyleImages(m_selected, name);
}

void LabController::copyEdit()
{
  if(m_imgid < 0) return;
  m_copied = editState();
  m_copiedStyle = m_style;
  emit copiedEditChanged();
  setMessage(tr("edit copied"));
}

void LabController::pasteEditToSelected()
{
  if(m_copied.isEmpty() || m_selected.isEmpty()) return;
  setInFlight(true);
  setMessage(tr("pasting to %n photo(s)...", nullptr, int(m_selected.size())));
  emit requestPaste(m_selected, m_copied, m_copiedStyle);
}

void LabController::aiDenoise(double strength)
{
  if(m_imgid < 0 || aiBusy()) return;
  setInFlight(true);
  emit requestAiDenoise({ m_imgid }, strength, true);
}

void LabController::aiDenoiseSelected(double strength)
{
  if(m_selected.isEmpty() || aiBusy()) return;
  setInFlight(true);
  emit requestAiDenoise(m_selected, strength, false);
}

void LabController::exportSelected(const QUrl &folder, const QVariantMap &options)
{
  const QString dir = folder.isLocalFile() ? folder.toLocalFile() : folder.toString();
  if(m_selected.isEmpty() || dir.isEmpty() || exporting()) return;
  setInFlight(true);
  m_exportDone = 0;
  m_exportTotal = int(m_selected.size());
  emit exportChanged();
  setMessage(tr("exporting %n photo(s)...", nullptr, m_exportTotal));
  emit requestExportImages(m_selected, dir, options);
}

void LabController::cancelExport()
{
  // straight to the worker: its queue is busy with the export itself
  m_worker->cancelExport();
}

void LabController::openImage(int imgid)
{
  if(imgid == m_imgid)
  {
    setMode(QStringLiteral("develop"));
    return;
  }
  setInFlight(true);
  emit requestOpenId(imgid);
}

void LabController::openNeighbour(int step)
{
  for(int i = 0; i < m_images.size(); i++)
    if(m_images[i].toMap().value(QStringLiteral("id")).toInt() == m_imgid)
    {
      const int j = i + step;
      if(j >= 0 && j < m_images.size())
        openImage(m_images[j].toMap().value(QStringLiteral("id")).toInt());
      return;
    }
}
