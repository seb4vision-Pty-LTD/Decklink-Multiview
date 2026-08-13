#pragma once

#include <QString>
#include <QWidget>

#include <optional>
#include <vector>

class QLabel;
class QVBoxLayout;

// Best-effort sensor readouts: many systems don't expose these via WMI/nvidia-smi, in which case callers get nullopt.
std::optional<double> queryCpuTemperatureCelsius();
std::optional<double> queryGpuTemperatureCelsius();

// Floating, non-modal status panel; a plain dialog-style window so it can be moved behind the
// main window and never blocks interaction with it.
class StatusWindow : public QWidget
{
public:
    explicit StatusWindow(QWidget* parent = nullptr);

    void setUptime(const QString& text);
    void setCpuTemperature(const QString& text);
    void setGpuTemperature(const QString& text);

    void setFeedCount(int count);
    void setFeedFps(int index, double fps);

private:
    QLabel* clockLabel_ = nullptr;
    QLabel* cpuTempLabel_ = nullptr;
    QLabel* gpuTempLabel_ = nullptr;
    QVBoxLayout* feedsLayout_ = nullptr;
    std::vector<QLabel*> feedLabels_;
};
