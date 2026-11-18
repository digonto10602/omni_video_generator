#pragma once

#include <QStringList>

namespace Cli {
// True when argv names a headless command (everything except `open`/no args).
bool isCommand(const QStringList &args);
int run(const QStringList &args);
QString usage();
}
