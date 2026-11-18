#pragma once

#include <QString>

// Exclusive GPU lock shared with other apps on this machine (omni_upscaler
// and anything run as `flock $XDG_RUNTIME_DIR/omni-gpu.lock <command>`).
// It is flock(2) on the same file, so the shell's flock(1) and this class
// exclude each other. The kernel drops the lock if the process dies.
class GpuLock {
public:
    explicit GpuLock(const QString &path);
    ~GpuLock();
    GpuLock(const GpuLock &) = delete;
    GpuLock &operator=(const GpuLock &) = delete;

    bool tryLock(QString *error = nullptr);
    void unlock();
    bool isLocked() const { return m_fd >= 0 && m_locked; }
    QString path() const { return m_path; }
    // True when some process holds the lock right now (probes without keeping it).
    static bool isBusy(const QString &path);

private:
    QString m_path;
    int m_fd = -1;
    bool m_locked = false;
};
