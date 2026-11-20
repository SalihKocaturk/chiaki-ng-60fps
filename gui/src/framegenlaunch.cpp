// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "framegenlaunch.h"
#include "settings.h"

#include <libplacebo/vulkan.h>

#include <QEventLoop>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QtGlobal>

#include <cstring>
#include <vector>

bool FrameGenLaunch::probeOpticalFlow(QString *gpu_name)
{
    if (gpu_name)
        gpu_name->clear();

    // Temporary instance only for enumeration; destroyed before the main
    // window creates its own. A missing loader/driver just means "no".
    struct pl_vk_inst_params params = {};
    pl_vk_inst inst = pl_vk_inst_create(nullptr, &params);
    if (!inst)
        return false;

    auto enumerate_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        inst->get_proc_addr(inst->instance, "vkEnumeratePhysicalDevices"));
    auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        inst->get_proc_addr(inst->instance, "vkGetPhysicalDeviceProperties"));
    auto enumerate_extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        inst->get_proc_addr(inst->instance, "vkEnumerateDeviceExtensionProperties"));

    bool found = false;
    QString fallback_name;
    bool fallback_discrete = false;
    uint32_t count = 0;
    if (enumerate_devices && get_properties && enumerate_extensions &&
        enumerate_devices(inst->instance, &count, nullptr) == VK_SUCCESS && count > 0) {
        std::vector<VkPhysicalDevice> devices(count);
        if (enumerate_devices(inst->instance, &count, devices.data()) == VK_SUCCESS) {
            for (uint32_t i = 0; i < count && !found; i++) {
                VkPhysicalDeviceProperties props = {};
                get_properties(devices[i], &props);
                const QString name = QString::fromUtf8(props.deviceName);
                const bool discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
                if (fallback_name.isEmpty() || (discrete && !fallback_discrete)) {
                    fallback_name = name;
                    fallback_discrete = discrete;
                }

                uint32_t ext_count = 0;
                if (enumerate_extensions(devices[i], nullptr, &ext_count, nullptr) != VK_SUCCESS)
                    continue;
                std::vector<VkExtensionProperties> exts(ext_count);
                if (enumerate_extensions(devices[i], nullptr, &ext_count, exts.data()) != VK_SUCCESS)
                    continue;
                for (const auto &e : exts) {
                    if (std::strcmp(e.extensionName, VK_NV_OPTICAL_FLOW_EXTENSION_NAME) == 0) {
                        found = true;
                        if (gpu_name)
                            *gpu_name = name;
                        break;
                    }
                }
            }
        }
    }
    if (!found && gpu_name)
        *gpu_name = fallback_name;

    pl_vk_inst_destroy(&inst);
    return found;
}

void FrameGenLaunch::setFrameGenEnabled(bool v)
{
    if (v == enabled)
        return;
    enabled = v;
    emit changed();
}

void FrameGenLaunch::setQuality(int v)
{
    v = qBound(0, v, 2);
    if (v == qual)
        return;
    qual = v;
    emit changed();
}

void FrameGenLaunch::setBufferMs(int v)
{
    v = qBound(0, v, 50);
    if (v == buffer_ms)
        return;
    buffer_ms = v;
    emit changed();
}

void FrameGenLaunch::accept()
{
    if (loop)
        loop->quit();
}

void FrameGenLaunch::run(Settings *settings)
{
    FrameGenLaunch launch;
    launch.is_supported = probeOpticalFlow(&launch.gpu_name);
    // CHIAKI_LAUNCH_LOCALE: test-only override (e.g. tr_TR, ja_JP).
    const QString forced = qEnvironmentVariable("CHIAKI_LAUNCH_LOCALE");
    launch.lang = forced.isEmpty() ? QLocale::system().name() : forced;
    launch.enabled = settings->GetFrameGenEnabled();
    launch.qual = settings->GetFrameGenQuality();
    launch.buffer_ms = settings->GetFrameGenBufferMs();

    // Closing this window must not count as "last window closed" (which would
    // queue an app quit that fires once the main window's event loop starts).
    const bool quit_on_last = QGuiApplication::quitOnLastWindowClosed();
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QEventLoop event_loop;
    launch.loop = &event_loop;
    {
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("launch"), &launch);
        engine.load(QUrl(QStringLiteral("qrc:/LaunchWindow.qml")));
        if (engine.rootObjects().isEmpty()) {
            // QML failed to load: start normally with the stored settings.
            QGuiApplication::setQuitOnLastWindowClosed(quit_on_last);
            return;
        }
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (window)
            QObject::connect(window, &QWindow::visibleChanged, &launch, [&launch](bool visible) {
                if (!visible)
                    launch.accept(); // closing the window continues, never exits
            });
        event_loop.exec();
        if (window)
            window->hide();
    }
    launch.loop = nullptr;
    QGuiApplication::setQuitOnLastWindowClosed(quit_on_last);

    // Unsupported GPU: keep the stored on/off value (may move to an RTX machine).
    if (launch.is_supported)
        settings->SetFrameGenEnabled(launch.enabled);
    settings->SetFrameGenQuality(launch.qual);
    settings->SetFrameGenBufferMs(launch.buffer_ms);
}
