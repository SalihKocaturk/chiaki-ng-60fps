// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Pre-launch window for frame generation settings. Runs before the main
// window (and its Vulkan device) exists, so changes never need a restart.

#ifndef CHIAKI_FRAMEGENLAUNCH_H
#define CHIAKI_FRAMEGENLAUNCH_H

#include <QObject>
#include <QString>

class Settings;
class QEventLoop;

class FrameGenLaunch : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(QString gpuName READ gpuName CONSTANT)
    Q_PROPERTY(QString language READ language CONSTANT)
    Q_PROPERTY(bool frameGenEnabled READ frameGenEnabled WRITE setFrameGenEnabled NOTIFY changed)
    Q_PROPERTY(int quality READ quality WRITE setQuality NOTIFY changed)
    Q_PROPERTY(int bufferMs READ bufferMs WRITE setBufferMs NOTIFY changed)

public:
    // Shows the window and blocks until Continue (or the window is closed).
    static void run(Settings *settings);

    // True if any Vulkan device has VK_NV_optical_flow. gpu_name gets that
    // device's name, else the first discrete (or first) device, else empty.
    static bool probeOpticalFlow(QString *gpu_name);

    bool supported() const { return is_supported; }
    QString gpuName() const { return gpu_name; }
    QString language() const { return lang; }
    bool frameGenEnabled() const { return enabled; }
    void setFrameGenEnabled(bool v);
    int quality() const { return qual; }
    void setQuality(int v);
    int bufferMs() const { return buffer_ms; }
    void setBufferMs(int v);

    Q_INVOKABLE void accept();

signals:
    void changed();

private:
    explicit FrameGenLaunch(QObject *parent = nullptr) : QObject(parent) {}

    bool is_supported = false;
    QString gpu_name;
    QString lang;
    bool enabled = true;
    int qual = 0;
    int buffer_ms = 8;
    QEventLoop *loop = nullptr;
};

#endif // CHIAKI_FRAMEGENLAUNCH_H
