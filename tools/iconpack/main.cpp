// Package the generated artwork; no drawing or background replacement.
#include <QBuffer>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QImage>
#include <QMap>
#include <QSaveFile>
#include <QTextStream>
#include <stdexcept>

static void write(const QString &path, const QByteArray &data) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
    throw std::runtime_error(("Cannot write " + path).toStdString());
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    if (app.arguments().size() != 3)
      throw std::runtime_error("Usage: vibeled_iconpack SOURCE.png OUTPUT_DIRECTORY");
    QImage source(app.arguments()[1]);
    if (source.isNull() || source.width() != source.height() || source.width() < 1024)
      throw std::runtime_error("Artwork must be square and at least 1024 pixels");
    if (!source.hasAlphaChannel() || source.pixelColor(0, 0).alpha() != 0)
      throw std::runtime_error("Artwork must preserve its generated transparent background");
    const QDir output(app.arguments()[2]);
    if (!QDir().mkpath(output.absolutePath()))
      throw std::runtime_error("Cannot create output directory");
    // Premultiplication keeps transparent edges clean when resampling.
    source = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QMap<int, QByteArray> pngs;
    for (int size : {16,20,24,32,40,48,64,128,256,512,1024}) {
      QImage image = source.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
      QBuffer buffer(&pngs[size]);
      buffer.open(QIODevice::WriteOnly);
      if (!image.save(&buffer, "PNG")) throw std::runtime_error("PNG encoding failed");
      write(output.filePath(QString("vibeled-%1.png").arg(size)), pngs[size]);
    }

    // ICO: directory followed by individually PNG-compressed RGBA images.
    const QList<int> sizes{16,20,24,32,40,48,64,128,256};
    QByteArray ico;
    QDataStream windows(&ico, QIODevice::WriteOnly);
    windows.setByteOrder(QDataStream::LittleEndian);
    windows << quint16(0) << quint16(1) << quint16(sizes.size());
    quint32 offset = 6 + 16 * sizes.size();
    for (int size : sizes) {
      windows << quint8(size == 256 ? 0 : size) << quint8(size == 256 ? 0 : size)
              << quint8(0) << quint8(0) << quint16(1) << quint16(32)
              << quint32(pngs[size].size()) << offset;
      offset += pngs[size].size();
    }
    for (int size : sizes) windows.writeRawData(pngs[size].constData(), pngs[size].size());
    write(output.filePath("vibeled.ico"), ico);

    // ICNS: PNG elements for standard and Retina sizes, in big-endian chunks.
    const QList<QPair<QByteArray, int>> entries{
        {"icp4",16},{"ic11",32},{"icp5",32},{"ic12",64},{"icp6",64},
        {"ic07",128},{"ic08",256},{"ic13",256},{"ic09",512},{"ic14",512},{"ic10",1024}};
    QByteArray chunks;
    QDataStream mac(&chunks, QIODevice::WriteOnly);
    mac.setByteOrder(QDataStream::BigEndian);
    for (const auto &entry : entries) {
      const auto &png = pngs[entry.second];
      mac.writeRawData(entry.first.constData(), 4);
      mac << quint32(png.size() + 8);
      mac.writeRawData(png.constData(), png.size());
    }
    QByteArray icns;
    QDataStream header(&icns, QIODevice::WriteOnly);
    header.setByteOrder(QDataStream::BigEndian);
    header.writeRawData("icns", 4);
    header << quint32(chunks.size() + 8);
    icns.append(chunks);
    write(output.filePath("vibeled.icns"), icns);
    QTextStream(stdout) << "Packaged transparent " << source.width() << "px artwork: "
                        << sizes.size() << " ICO sizes and " << entries.size() << " ICNS elements\n";
    return 0;
  } catch (const std::exception &error) {
    QTextStream(stderr) << error.what() << '\n';
    return 1;
  }
}
