#include "waveexport.h"
#include "engine.h"

#include <QAudioDecoder>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QtEndian>
#include <limits>
#include <stdexcept>

namespace ez {
namespace {

constexpr int sampleRate = 48000;
constexpr int sampleBytes = 2;
constexpr qint64 maxPcmBytes = std::numeric_limits<quint32>::max() - 36;

void require(bool ok, const QString &message)
{
    if (!ok)
        throw std::runtime_error(message.toStdString());
}

void checkCancel(const std::atomic_bool &cancel)
{
    require(!cancel.load(), QStringLiteral("已取消导出。"));
}

void readVerified(const WaveExportItem &item, QIODevice *copy, const std::atomic_bool &cancel)
{
    QFile file(checkedPath(item.source));
    require(file.open(QIODevice::ReadOnly) && file.size() > 44,
        QStringLiteral("无法读取成品 %1").arg(item.source));
    QCryptographicHash digest(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        checkCancel(cancel);
        const auto block = file.read(1024 * 1024);
        require(!block.isEmpty() && (!copy || copy->write(block) == block.size()),
            QStringLiteral("读取或写入 WAV 失败 %1").arg(item.source));
        digest.addData(block);
    }
    require(item.sha256.isEmpty() || digest.result().toHex() == item.sha256.toLatin1(),
        QStringLiteral("成品摘要不符，请重新生成所选台词。\n%1").arg(item.source));
}

QByteArray waveHeader(quint32 bytes)
{
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(bytes + 36);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << quint32(sampleRate)
        << quint32(sampleRate * sampleBytes) << quint16(sampleBytes) << quint16(sampleBytes * 8);
    stream.writeRawData("data", 4);
    stream << bytes;
    return header;
}

qint64 convertedPcmBytes(const QString &source)
{
    QFile file(checkedPath(source));
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    const auto riff = file.read(12);
    if (riff.size() != 12 || !riff.startsWith("RIFF") || riff.mid(8) != "WAVE")
        return 0;
    quint32 rate = 0;
    quint16 alignment = 0;
    quint16 encoding = 0;
    quint32 dataBytes = 0;
    quint32 decodedFrames = 0;
    while (file.pos() + 8 <= file.size()) {
        const auto chunk = file.read(8);
        if (chunk.size() != 8)
            return 0;
        const auto size = qFromLittleEndian<quint32>(chunk.constData() + 4);
        const auto end = file.pos() + size + (size & 1);
        if (end > file.size() + 1)
            return 0;
        if (chunk.startsWith("fmt ") && size >= 16) {
            const auto format = file.read(16);
            if (format.size() != 16)
                return 0;
            encoding = qFromLittleEndian<quint16>(format.constData());
            rate = qFromLittleEndian<quint32>(format.constData() + 4);
            alignment = qFromLittleEndian<quint16>(format.constData() + 12);
        } else if (chunk.startsWith("data")) {
            dataBytes = size;
        } else if (chunk.startsWith("fact") && size >= 4) {
            const auto fact = file.read(4);
            if (fact.size() != 4)
                return 0;
            decodedFrames = qFromLittleEndian<quint32>(fact.constData());
        }
        if (rate && dataBytes && ((encoding == 1 || encoding == 3 || encoding == 0xfffe) || decodedFrames))
            break;
        if (!file.seek(end))
            return 0;
    }
    if (decodedFrames == 0 && alignment && (encoding == 1 || encoding == 3 || encoding == 0xfffe))
        decodedFrames = dataBytes / alignment;
    return rate ? ((qint64(decodedFrames) * sampleRate + rate / 2) / rate) * sampleBytes : 0;
}

qint64 decodeWave(const WaveExportItem &item, QSaveFile &output, qint64 remaining, const std::atomic_bool &cancel)
{
    QAudioDecoder decoder;
    require(decoder.isSupported(), QStringLiteral("本机音频解码器不可用。"));
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    decoder.setAudioFormat(format);
    decoder.setSource(QUrl::fromLocalFile(checkedPath(item.source)));
    QEventLoop loop;
    QTimer watchdog;
    watchdog.setInterval(200);
    QElapsedTimer lastAudio;
    lastAudio.start();
    const auto expected = convertedPcmBytes(item.source);
    require(expected <= remaining, QStringLiteral("合并文件超出 RIFF 大小上限。"));
    qint64 bytes = 0;
    QByteArray lastSample;
    QString error;
    bool finished = false;
    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, [&] {
        const auto buffer = decoder.read();
        const auto count = expected > 0 ? qMin<qint64>(buffer.byteCount(), expected - bytes) : buffer.byteCount();
        const auto decoded = buffer.format();
        if (!buffer.isValid() || decoded.sampleRate() != sampleRate || decoded.channelCount() != 1
            || decoded.sampleFormat() != QAudioFormat::Int16 || count > remaining - bytes
            || output.write(buffer.constData<char>(), count) != count) {
            error = QStringLiteral("无法写入统一格式的 WAV，或文件超出 RIFF 大小上限。");
            loop.quit();
            return;
        }
        if (count >= sampleBytes)
            lastSample = QByteArray(buffer.constData<char>() + count - sampleBytes, sampleBytes);
        bytes += count;
        lastAudio.restart();
    });
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, [&] { finished = true; loop.quit(); });
    QObject::connect(&decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), &loop, [&] {
        error = decoder.errorString();
        loop.quit();
    });
    QObject::connect(&watchdog, &QTimer::timeout, &loop, [&] {
        if (cancel.load() || lastAudio.elapsed() > 30000) {
            error = cancel.load() ? QStringLiteral("已取消导出。") : QStringLiteral("读取音频超时。");
            loop.quit();
        }
    });
    decoder.start();
    if (!finished && error.isEmpty()) {
        watchdog.start();
        loop.exec();
    }
    decoder.stop();
    require(error.isEmpty() && finished && bytes > 0 && bytes % sampleBytes == 0,
        QStringLiteral("合并时无法解码 %1。%2").arg(item.source, error));
    if (expected > bytes) {
        // Some Qt FFmpeg backends discard the resampler's final filter tail.
        const auto missing = expected - bytes;
        require(missing <= sampleRate * sampleBytes / 50 && lastSample.size() == sampleBytes
            && output.write(lastSample.repeated(int(missing / sampleBytes))) == missing,
            QStringLiteral("解码后的音频长度异常 %1").arg(item.source));
        bytes = expected;
    }
    return bytes;
}

}

QString exportOrderedWaves(const QList<WaveExportItem> &items, const QString &directory,
                          const QSet<QString> &overwrite, const std::atomic_bool &cancel)
{
    try {
        const QDir target(checkedPath(directory));
        QTemporaryDir staging(target.filePath(".ez-export-XXXXXX"));
        require(staging.isValid(), QStringLiteral("无法创建导出暂存目录。"));
        QDir scratch(staging.path());
        QSet<QString> installed;
        QMap<QString, QString> backups;
        for (const auto &item : items) {
            if (item.filename != QFileInfo(item.filename).fileName())
                throw std::runtime_error("Invalid export filename");
            QSaveFile output(checkedPath(scratch.filePath(item.filename)));
            require(output.open(QIODevice::WriteOnly), QStringLiteral("无法准备 %1").arg(item.filename));
            readVerified(item, &output, cancel);
            require(output.commit(), QStringLiteral("无法保存 %1").arg(item.filename));
        }
        try {
            checkCancel(cancel);
            for (const auto &item : items) {
                const auto path = checkedPath(target.filePath(item.filename));
                if (QFileInfo::exists(path)) {
                    const auto backup = checkedPath(scratch.filePath(item.filename + ".previous"));
                    require(overwrite.contains(item.filename) && QFileInfo(path).isFile() && QFile::rename(path, backup),
                        QStringLiteral("导出位置有同名文件或无法替换 %1").arg(path));
                    backups.insert(path, backup);
                }
                require(QFile::rename(scratch.filePath(item.filename), path), QStringLiteral("无法发布 %1").arg(path));
                installed.insert(path);
            }
        } catch (...) {
            bool restored = true;
            for (const auto &path : installed)
                restored &= QFile::remove(path);
            for (auto item = backups.constBegin(); item != backups.constEnd(); ++item)
                restored &= QFile::rename(item.value(), item.key());
            if (!restored) {
                staging.setAutoRemove(false);
                throw std::runtime_error(QStringLiteral("恢复导出位置时有文件无法还原。原文件备份保存在 %1")
                    .arg(staging.path()).toStdString());
            }
            throw;
        }
        return {};
    } catch (const std::exception &error) {
        return QString::fromUtf8(error.what());
    }
}

QString exportMergedWave(const QList<WaveExportItem> &items, const QString &filename, int gapMs,
                         bool overwrite, const std::atomic_bool &cancel)
{
    try {
        const auto target = checkedPath(filename);
        QSaveFile output(target);
        const auto header = waveHeader(0);
        qint64 pcm = 0;
        require(!items.isEmpty() && gapMs >= 0 && gapMs <= 10000, QStringLiteral("导出项或停顿设置无效。"));
        require(output.open(QIODevice::WriteOnly) && output.write(header) == header.size(),
            QStringLiteral("无法创建 WAV 暂存文件。"));
        const QByteArray silence(gapMs * (sampleRate / 1000) * sampleBytes, '\0');
        for (qsizetype index = 0; index < items.size(); ++index) {
            readVerified(items.at(index), nullptr, cancel);
            if (index > 0) {
                require(silence.size() <= maxPcmBytes - pcm && output.write(silence) == silence.size(),
                    QStringLiteral("无法写入句间停顿。"));
                pcm += silence.size();
            }
            pcm += decodeWave(items.at(index), output, maxPcmBytes - pcm, cancel);
            readVerified(items.at(index), nullptr, cancel);
        }
        checkCancel(cancel);
        const auto completed = waveHeader(quint32(pcm));
        require(output.size() == header.size() + pcm && output.seek(0) && output.write(completed) == completed.size(),
            QStringLiteral("合并 WAV 的长度校验失败。"));
        require((overwrite || !QFileInfo::exists(target)) && output.commit(),
            QStringLiteral("成品位置已有同名文件，或无法保存合并 WAV。"));
        return {};
    } catch (const std::exception &error) {
        return QString::fromUtf8(error.what());
    }
}

}
