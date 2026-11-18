#include "gpulock.h"

#include <QDir>
#include <QFileInfo>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

GpuLock::GpuLock(const QString &path) : m_path(path) {}

GpuLock::~GpuLock() { unlock(); }

bool GpuLock::tryLock(QString *error) {
    if (isLocked())
        return true;
    if (m_fd < 0) {
        QDir().mkpath(QFileInfo(m_path).absolutePath());
        m_fd = ::open(m_path.toLocal8Bit().constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (m_fd < 0) {
            if (error) *error = QString("cannot open GPU lock %1: %2").arg(m_path, strerror(errno));
            return false;
        }
    }
    if (::flock(m_fd, LOCK_EX | LOCK_NB) == 0) {
        m_locked = true;
        return true;
    }
    if (errno != EWOULDBLOCK && error)
        *error = QString("flock %1: %2").arg(m_path, strerror(errno));
    return false;
}

void GpuLock::unlock() {
    if (m_fd >= 0) {
        if (m_locked)
            ::flock(m_fd, LOCK_UN);
        ::close(m_fd);
    }
    m_fd = -1;
    m_locked = false;
}

bool GpuLock::isBusy(const QString &path) {
    const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool busy = ::flock(fd, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    if (!busy)
        ::flock(fd, LOCK_UN);
    ::close(fd);
    return busy;
}
