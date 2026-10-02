#include <QApplication>

#include "media_window.h"

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  MediaWindow window;
  window.show();
  if (app.arguments().size() > 1) window.OpenFile(app.arguments()[1]);
  return app.exec();
}
