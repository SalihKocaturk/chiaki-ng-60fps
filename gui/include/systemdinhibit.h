#pragma once

#include <QObject>

class SystemdInhibit : public QObject
{
    Q_OBJECT

public:
    SystemdInhibit(const QString &who, const QString &why, const QString &what, const QString &mode, QObject *parent = nullptr);
    ~SystemdInhibit() override;

    void inhibit();
    void simulateUserActivity();
    void release();

signals:
    void sleep();
    void resume();

private slots:
    void login1PrepareForSleep(bool start);

private:
    QString who;
    QString why;
    QString what;
    QString mode;
    int fd = -1;
#ifdef Q_OS_WIN
    // Windows power request (PowerCreateRequest): keeps the system and display
    // awake during a stream, since gamepad input doesn't reset the idle timer.
    void *power_request = nullptr;
#endif
};
