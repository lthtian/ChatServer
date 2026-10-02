
#include <QElapsedTimer>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>

#include "player_window.h"

class PlaybackTest : public QObject {
  Q_OBJECT

 private slots:
  void CompleteSample() {
    PlayerWindow window;
    QSignalSpy frames(&window, &PlayerWindow::FramePresented);
    QImage first;
    connect(&window, &PlayerWindow::FramePresented, &window, [&] {
      if (first.isNull()) first = window.CurrentImage();
    });
    window.show();
    window.OpenFile(qEnvironmentVariable("PLAYER_SAMPLE"));
    QTRY_VERIFY_WITH_TIMEOUT(!frames.empty(), 5000);
    QElapsedTimer elapsed;
    elapsed.start();
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == PlayerWindow::State::Ended,
                             10000);
    QCOMPARE(frames.size(), 40);
    for (int i = 0; i < frames.size(); ++i) {
      QCOMPARE(frames[i][0].toLongLong(), qint64(i) * 125000);
    }
    QVERIFY(elapsed.elapsed() >= 4700);
    QCOMPARE(window.positionUs(), qint64(5000000));
    QImage expected(qEnvironmentVariable("PLAYER_REFERENCE"));
    QVERIFY(!expected.isNull());
    QCOMPARE(first.convertToFormat(QImage::Format_RGB888),
             expected.convertToFormat(QImage::Format_RGB888));
  }

  void PauseResumeAndRepaint() {
    PlayerWindow window;
    int heartbeats = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, &window, [&] { ++heartbeats; });
    heartbeat.start(10);
    QSignalSpy frames(&window, &PlayerWindow::FramePresented);
    window.show();
    window.OpenFile(qEnvironmentVariable("PLAYER_SAMPLE"));
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= 3, 5000);
    auto* button = window.findChild<QPushButton*>("playButton");
    QVERIFY(button);
    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(window.state(), PlayerWindow::State::Paused);
    const int count = frames.size();
    const qint64 position = window.positionUs();
    const int before = heartbeats;
    window.resize(880, 590);
    QTest::qWait(350);
    QCOMPARE(frames.size(), count);
    QCOMPARE(window.positionUs(), position);
    QVERIFY(heartbeats - before >= 10);
    QVERIFY(window.grab().save(qEnvironmentVariable("PLAYER_SCREENSHOT")));
    QTest::mouseClick(button, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() > count, 2000);
    QVERIFY(window.positionUs() - position < 300000);
  }

  void VariableFrameIntervals() {
    PlayerWindow window;
    QList<qint64> arrival;
    QElapsedTimer elapsed;
    QSignalSpy frames(&window, &PlayerWindow::FramePresented);
    connect(&window, &PlayerWindow::FramePresented, &window, [&] {
      if (!elapsed.isValid()) elapsed.start();
      arrival.append(elapsed.elapsed());
    });
    window.OpenFile(qEnvironmentVariable("PLAYER_VFR"));
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == PlayerWindow::State::Ended,
                             5000);
    QCOMPARE(frames.size(), 3);
    QCOMPARE(frames[0][0].toLongLong(), qint64(0));
    QCOMPARE(frames[1][0].toLongLong(), qint64(250000));
    QCOMPARE(frames[2][0].toLongLong(), qint64(875000));
    QVERIFY(arrival[1] >= 240);
    QVERIFY(arrival[2] >= 865);
  }

  void SwitchReplayAndAspectRatio() {
    PlayerWindow window;
    window.show();
    window.OpenFile(qEnvironmentVariable("PLAYER_SAMPLE"));
    window.OpenFile(qEnvironmentVariable("PLAYER_SHORT"));
    QSignalSpy frames(&window, &PlayerWindow::FramePresented);
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == PlayerWindow::State::Ended,
                             5000);
    QCOMPARE(frames.size(), 6);
    QCOMPARE(window.CurrentImage().size(), QSize(80, 40));
    auto* view = window.findChild<QWidget*>("videoView");
    QVERIFY(view);
    view->setFixedSize(200, 200);
    const QImage painted = view->grab().toImage();
    QCOMPARE(painted.pixelColor(100, 10), QColor(Qt::black));
    QVERIFY(painted.pixelColor(100, 100).red() > 240);
    QVERIFY(painted.pixelColor(100, 100).green() < 10);
    auto* button = window.findChild<QPushButton*>("playButton");
    QTest::mouseClick(button, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() == 12, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == PlayerWindow::State::Ended,
                             3000);
  }

  void ErrorsAndRecovery() {
    PlayerWindow window;
    window.OpenFile(qEnvironmentVariable("PLAYER_MISSING"));
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == PlayerWindow::State::Error,
                             3000);
    QVERIFY(!window.errorText().isEmpty());
    window.OpenFile(qEnvironmentVariable("PLAYER_AUDIO"));
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == PlayerWindow::State::Error,
                             3000);
    QVERIFY(window.errorText().contains(QString::fromUtf8("没有视频流")));
    window.OpenFile(qEnvironmentVariable("PLAYER_SHORT"));
    QTRY_VERIFY_WITH_TIMEOUT(window.state() == PlayerWindow::State::Ended,
                             5000);
    QVERIFY(window.errorText().isEmpty());
  }

  void CloseDuringOpening() {
    QElapsedTimer elapsed;
    elapsed.start();
    auto* window = new PlayerWindow;
    window->OpenFile(qEnvironmentVariable("PLAYER_SAMPLE"));
    delete window;
    QVERIFY(elapsed.elapsed() < 3000);
  }
};

QTEST_MAIN(PlaybackTest)
#include "playback_test.moc"
