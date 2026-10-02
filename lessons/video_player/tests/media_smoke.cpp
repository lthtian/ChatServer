#include <QApplication>
#include <QDir>
#include <QSignalSpy>
#include <QtTest>

#include "media_window.h"

// 使用真实文件、Qt 窗口和系统默认音频设备，不模拟解码或设备消费。
class MediaSmoke : public QObject {
  Q_OBJECT
 private slots:
  void DecodeAndSeek() {
    MediaDecoder decoder;
    QSignalSpy opened(&decoder, &MediaDecoder::Opened);
    QSignalSpy errors(&decoder, &MediaDecoder::Failed);
    MediaBatch result;
    connect(
        &decoder, &MediaDecoder::BatchReady, this,
        [&result](quint64, MediaBatch batch) { result = std::move(batch); });
    decoder.Open(QDir(QCoreApplication::applicationDirPath())
                     .absoluteFilePath("../samples/sintel_trailer.mp4"),
                 1);
    QCOMPARE(errors.count(), 0);
    QCOMPARE(opened.count(), 1);
    QVERIFY(opened.first().at(2).toBool());
    decoder.SetOutputRate(48000);
    for (qint64 target : {qint64(0), qint64(13765432), qint64(50123456)}) {
      decoder.Seek(target, 2);
      QCOMPARE(errors.count(), 0);
      bool picture = false;
      bool sound = false;
      for (int packet = 0; packet < 600 && !(picture && sound); ++packet) {
        decoder.Read(2);
        QCOMPARE(errors.count(), 0);
        for (const auto& frame : result.video) {
          if (!picture) {
            QVERIFY(frame.pts_us >= target);
            QVERIFY(frame.pts_us - target < 50000);
            QCOMPARE(frame.image.size(), QSize(854, 480));
          }
          picture = true;
        }
        for (const auto& block : result.audio) {
          if (!sound) {
            QVERIFY(block.pts_us >= target);
            // ffprobe 确认该样本的首个解码音频帧为 0.021333 秒。
            const qint64 expected = target == 0 ? 21333 : target;
            QVERIFY2(std::abs(block.pts_us - expected) < 1000,
                     qPrintable(QString("target=%1 first_audio=%2")
                                    .arg(target)
                                    .arg(block.pts_us)));
            QCOMPARE(block.pcm.size() % 4, 0);
            QVERIFY(!block.pcm.isEmpty());
          }
          sound = true;
        }
      }
      QVERIFY(picture && sound);
    }
    bool finished = false;
    for (int packet = 0; packet < 1000 && !finished; ++packet) {
      decoder.Read(2);
      QCOMPARE(errors.count(), 0);
      finished = result.eof;
    }
    QVERIFY(finished);
  }
  void SilentPlaybackAndSeek() {
    MediaWindow window;
    window.show();
    window.OpenFile(QStringLiteral("D:/test_video.mp4"));
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == MediaWindow::State::Playing ||
                                 window.state() == MediaWindow::State::Error,
                             4000);
    QVERIFY2(window.state() == MediaWindow::State::Playing,
             qPrintable(window.ErrorText()));
    QTRY_VERIFY_WITH_TIMEOUT(window.PositionUs() > 250000, 2000);
    window.TogglePlayback();
    const qint64 paused = window.PositionUs();
    QTest::qWait(100);
    QCOMPARE(window.PositionUs(), paused);
    window.SeekTo(2500000);
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == MediaWindow::State::Paused ||
                                 window.state() == MediaWindow::State::Error,
                             3000);
    QVERIFY2(window.state() == MediaWindow::State::Paused,
             qPrintable(window.ErrorText()));
    QCOMPARE(window.PositionUs(), qint64(2500000));
    QVERIFY(!window.CurrentImage().isNull());
    window.TogglePlayback();
    auto* slider = window.findChild<ProgressSlider*>("mediaProgress");
    QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier,
                      QPoint(slider->width() * 93 / 100, slider->height() / 2));
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == MediaWindow::State::Ended, 4000);
    window.TogglePlayback();
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == MediaWindow::State::Playing,
                             3000);
    window.Stop();
    QCOMPARE(window.state(), MediaWindow::State::Stopped);
    QCOMPARE(window.PositionUs(), qint64(0));
  }
  void PlaybackControls() {
    if (QAudioDeviceInfo::availableDevices(QAudio::AudioOutput).isEmpty())
      QSKIP(
          "No physical audio output is enumerated; device playback remains "
          "unverified.");
    const auto device = QAudioDeviceInfo::defaultOutputDevice();
    qInfo() << "Audio device:" << device.deviceName()
            << "preferred:" << device.preferredFormat()
            << "rates:" << device.supportedSampleRates()
            << "channels:" << device.supportedChannelCounts()
            << "sizes:" << device.supportedSampleSizes();
    MediaWindow window;
    window.show();
    QSignalSpy frames(&window, &MediaWindow::FramePresented);
    const QString sample =
        QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath("../samples/sintel_trailer.mp4");
    window.OpenFile(sample);
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == MediaWindow::State::Playing ||
                                 window.state() == MediaWindow::State::Error,
                             10000);
    QVERIFY2(window.state() == MediaWindow::State::Playing,
             qPrintable(window.ErrorText()));
    QTRY_VERIFY_WITH_TIMEOUT(window.PositionUs() > 600000, 4000);
    QVERIFY(!window.CurrentImage().isNull());
    QVERIFY(frames.count() > 5);
    window.TogglePlayback();
    const qint64 paused = window.PositionUs();
    QTest::qWait(250);
    QCOMPARE(window.PositionUs(), paused);
    window.SeekTo(10000000);
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == MediaWindow::State::Paused ||
                                 window.state() == MediaWindow::State::Error,
                             5000);
    QVERIFY2(window.state() == MediaWindow::State::Paused,
             qPrintable(window.ErrorText()));
    QCOMPARE(window.PositionUs(), qint64(10000000));
    QVERIFY(!window.CurrentImage().isNull());
    QVERIFY(frames.last().at(0).toLongLong() >= 10000000);
    window.TogglePlayback();
    QTRY_VERIFY_WITH_TIMEOUT(window.PositionUs() > 10300000, 3000);
    // 点击真实滑槽定位到尾部，覆盖控件事件与播放中的 seek。
    auto* slider = window.findChild<ProgressSlider*>("mediaProgress");
    QVERIFY(slider);
    QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier,
                      QPoint(slider->width() * 97 / 100, slider->height() / 2));
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == MediaWindow::State::Ended ||
                                 window.state() == MediaWindow::State::Error,
                             6000);
    QVERIFY2(window.state() == MediaWindow::State::Ended,
             qPrintable(window.ErrorText()));
    QVERIFY(window.PositionUs() > 51000000);
    window.TogglePlayback();
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == MediaWindow::State::Playing,
                             4000);
    window.Stop();
    QCOMPARE(window.state(), MediaWindow::State::Stopped);
    QCOMPARE(window.PositionUs(), qint64(0));
    // 原来的无音轨文件继续使用单调时钟，也覆盖切换文件后的过期结果隔离。
    window.OpenFile(QStringLiteral("D:/test_video.mp4"));
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == MediaWindow::State::Playing ||
                                 window.state() == MediaWindow::State::Error,
                             4000);
    QVERIFY2(window.state() == MediaWindow::State::Playing,
             qPrintable(window.ErrorText()));
    QTRY_VERIFY_WITH_TIMEOUT(window.PositionUs() > 250000, 2000);
    window.grab().save(QDir(QCoreApplication::applicationDirPath())
                           .filePath("media-preview.png"));
  }
};
QTEST_MAIN(MediaSmoke)
#include "media_smoke.moc"
