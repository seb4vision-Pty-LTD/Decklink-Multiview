#include "StatusWindow.h"

#include <QFrame>
#include <QLabel>
#include <QProcess>
#include <QVBoxLayout>

#include <Windows.h>
#include <wbemidl.h>
#pragma comment(lib, "wbemuuid.lib")

std::optional<double> queryCpuTemperatureCelsius()
{
    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needsUninit = SUCCEEDED(initResult);

    std::optional<double> result;
    IWbemLocator* locator = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&locator))))
    {
        BSTR ns = SysAllocString(L"ROOT\\WMI");
        IWbemServices* services = nullptr;
        if (SUCCEEDED(locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services)))
        {
            CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

            BSTR wql = SysAllocString(L"WQL");
            BSTR query = SysAllocString(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
            IEnumWbemClassObject* enumerator = nullptr;
            if (SUCCEEDED(services->ExecQuery(wql, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator)) && enumerator)
            {
                IWbemClassObject* obj = nullptr;
                ULONG returned = 0;
                if (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK && returned > 0)
                {
                    VARIANT value;
                    VariantInit(&value);
                    if (SUCCEEDED(obj->Get(L"CurrentTemperature", 0, &value, nullptr, nullptr)))
                    {
                        // Value is reported in tenths of a Kelvin.
                        if (value.vt == VT_I4)
                        {
                            result = (static_cast<double>(value.lVal) / 10.0) - 273.15;
                        }
                        else if (value.vt == VT_UI4)
                        {
                            result = (static_cast<double>(value.ulVal) / 10.0) - 273.15;
                        }
                    }
                    VariantClear(&value);
                    obj->Release();
                }
                enumerator->Release();
            }
            SysFreeString(query);
            SysFreeString(wql);
            services->Release();
        }
        SysFreeString(ns);
        locator->Release();
    }

    if (needsUninit)
    {
        CoUninitialize();
    }

    return result;
}

std::optional<double> queryGpuTemperatureCelsius()
{
    QProcess process;
    process.start("nvidia-smi", { "--query-gpu=temperature.gpu", "--format=csv,noheader,nounits" });
    if (!process.waitForFinished(500) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        return std::nullopt;
    }

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    bool ok = false;
    const double value = output.split('\n').value(0).toDouble(&ok);
    return ok ? std::optional<double>(value) : std::nullopt;
}

StatusWindow::StatusWindow(QWidget* parent)
    : QWidget(parent, Qt::Dialog)
{
    setWindowTitle("System Status");
    setFixedWidth(260);

    // Qt::Dialog windows quit the app when closed by default; this is an auxiliary panel, not the app's main window.
    setAttribute(Qt::WA_QuitOnClose, false);

    auto* layout = new QVBoxLayout(this);
    clockLabel_ = new QLabel("Uptime: 00:00:00", this);
    cpuTempLabel_ = new QLabel("CPU Temp: N/A", this);
    gpuTempLabel_ = new QLabel("GPU Temp: N/A", this);

    layout->addWidget(clockLabel_);
    layout->addWidget(cpuTempLabel_);
    layout->addWidget(gpuTempLabel_);

    auto* separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    layout->addWidget(separator);

    feedsLayout_ = new QVBoxLayout();
    layout->addLayout(feedsLayout_);
    layout->addStretch();
}

void StatusWindow::setUptime(const QString& text) { clockLabel_->setText("Uptime: " + text); }
void StatusWindow::setCpuTemperature(const QString& text) { cpuTempLabel_->setText("CPU Temp: " + text); }
void StatusWindow::setGpuTemperature(const QString& text) { gpuTempLabel_->setText("GPU Temp: " + text); }

void StatusWindow::setFeedCount(int count)
{
    for (auto* label : feedLabels_)
    {
        feedsLayout_->removeWidget(label);
        delete label;
    }
    feedLabels_.clear();

    for (int i = 0; i < count; ++i)
    {
        auto* label = new QLabel(QString("Device %1: -- fps").arg(i), this);
        feedsLayout_->addWidget(label);
        feedLabels_.push_back(label);
    }
}

void StatusWindow::setFeedFps(int index, double fps)
{
    if (index >= 0 && index < static_cast<int>(feedLabels_.size()))
    {
        feedLabels_[index]->setText(QString("Device %1: %2 fps").arg(index).arg(fps, 0, 'f', 1));
    }
}
