#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include <algorithm>
#include <vector>

enum class GridLayoutMode
{
    Grid2x2,
    Grid3x3
};

class MultiviewWindow : public QMainWindow
{
public:
    MultiviewWindow()
    {
        setWindowTitle("DeckLink Multiview");
        resize(1920, 1080);

        setupUi();
        setupPipeline();
    }

    ~MultiviewWindow() override
    {
        if (busPollTimer_)
        {
            busPollTimer_->stop();
        }

        if (compositor_)
        {
            for (auto* pad : sourcePads_)
            {
                if (pad)
                {
                    gst_element_release_request_pad(compositor_, pad);
                    gst_object_unref(pad);
                }
            }
            sourcePads_.clear();

            gst_object_unref(compositor_);
            compositor_ = nullptr;
        }

        if (sink_)
        {
            gst_object_unref(sink_);
            sink_ = nullptr;
        }

        if (pipeline_)
        {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QMainWindow::resizeEvent(event);
        // applyLayout(currentLayout_);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_F11)
        {
            toggleFullscreen();
            return;
        }

        if (event->key() == Qt::Key_Escape && isFullScreen())
        {
            showNormal();
            fullscreenButton_->setText("Fullscreen (F11)");
            return;
        }

        QMainWindow::keyPressEvent(event);
    }

private:
    struct CellPlacement
    {
        int x;
        int y;
        int width;
        int height;
    };

    void setupUi()
    {
        auto* central = new QWidget(this);
        auto* rootLayout = new QVBoxLayout(central);

        auto* topBar = new QHBoxLayout();
        layoutSelector_ = new QComboBox(this);
        layoutSelector_->addItem("2x2");
        layoutSelector_->addItem("3x3");

        fullscreenButton_ = new QPushButton("Fullscreen (F11)", this);

        topBar->addWidget(layoutSelector_);
        topBar->addWidget(fullscreenButton_);
        topBar->addStretch();

        videoWidget_ = new QWidget(this);
        videoWidget_->setAttribute(Qt::WA_NativeWindow, true);
        videoWidget_->setMinimumSize(1280, 720);
        videoWidget_->setStyleSheet("background-color: black;");

        rootLayout->addLayout(topBar);
        rootLayout->addWidget(videoWidget_, 1);

        setCentralWidget(central);

        connect(layoutSelector_, &QComboBox::currentIndexChanged, this, [this](int index)
            {
                currentLayout_ = (index == 0) ? GridLayoutMode::Grid2x2 : GridLayoutMode::Grid3x3;
                applyLayout(currentLayout_);
            });

        connect(fullscreenButton_, &QPushButton::clicked, this, [this]()
            {
                toggleFullscreen();
            });
    }

    void setupPipeline()
    {
        QString pipelineDescription =
            "glvideomixer name=mix background=1 ! "
            "glcolorconvert ! gldownload ! queue max-size-buffers=3 leaky=downstream ! videoconvert ! d3d11videosink name=vsink sync=false ";

        for (int device = 0; device < 4; ++device)
        {
            pipelineDescription += QString(
                "decklinkvideosrc device-number=%1 mode=2160p50 ! "
                "video/x-raw,width=3840,height=2160,framerate=50/1 ! "
                "videoconvert ! videoscale ! video/x-raw,format=BGRA ! "
                "glupload ! glcolorconvert ! "
                "queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! mix.sink_%1 ")
                .arg(device);
        }

        GError* parseError = nullptr;
        pipeline_ = gst_parse_launch(pipelineDescription.toUtf8().constData(), &parseError);

        if (!pipeline_ || parseError)
        {
            const QString errorText = parseError ? QString::fromUtf8(parseError->message) : "Unknown parser error.";
            if (parseError)
            {
                g_error_free(parseError);
            }
            QMessageBox::critical(this, "GStreamer Error", "Failed to create pipeline from string: " + errorText);
            return;
        }

        compositor_ = gst_bin_get_by_name(GST_BIN(pipeline_), "mix");
        sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "vsink");

        if (!compositor_ || !sink_)
        {
            QMessageBox::critical(this, "GStreamer Error", "Pipeline is missing required named elements (mix/vsink).");
            return;
        }

        sourcePads_.reserve(4);
        for (int device = 0; device < 4; ++device)
        {
            const QString padName = QString("sink_%1").arg(device);
            GstPad* mixerPad = gst_element_get_static_pad(compositor_, padName.toUtf8().constData());
            if (!mixerPad)
            {
                QMessageBox::critical(this, "GStreamer Error", "Could not get compositor pad: " + padName);
                return;
            }
            sourcePads_.push_back(mixerPad);
        }

        if (GST_IS_VIDEO_OVERLAY(sink_))
        {
            gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink_), static_cast<guintptr>(videoWidget_->winId()));
        }

        busPollTimer_ = new QTimer(this);
        connect(busPollTimer_, &QTimer::timeout, this, [this]()
            {
                processBusMessages();
            });
        busPollTimer_->start(50);

        applyLayout(currentLayout_);
        gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    }

    void applyLayout(GridLayoutMode mode)
    {
        if (sourcePads_.empty())
        {
            return;
        }

        const int width = std::max(1, videoWidget_->width());
        const int height = std::max(1, videoWidget_->height());

        std::vector<CellPlacement> placements;
        placements.reserve(9);

        if (mode == GridLayoutMode::Grid2x2)
        {
            const int cellW = width / 2;
            const int cellH = height / 2;

            placements = {
                {0, 0, cellW, cellH},
                {cellW, 0, width - cellW, cellH},
                {0, cellH, cellW, height - cellH},
                {cellW, cellH, width - cellW, height - cellH}
            };
        }
        else
        {
            const int cellW = width / 3;
            const int cellH = height / 3;

            for (int row = 0; row < 3; ++row)
            {
                for (int col = 0; col < 3; ++col)
                {
                    const int x = col * cellW;
                    const int y = row * cellH;
                    const int w = (col == 2) ? (width - x) : cellW;
                    const int h = (row == 2) ? (height - y) : cellH;
                    placements.push_back({ x, y, w, h });
                }
            }
        }

        for (size_t i = 0; i < sourcePads_.size(); ++i)
        {
            GstPad* pad = sourcePads_[i];
            if (!pad)
            {
                continue;
            }

            if (mode == GridLayoutMode::Grid2x2)
            {
                if (i < 4)
                {
                    const auto& c = placements[i];
                    g_object_set(pad,
                        "xpos", c.x,
                        "ypos", c.y,
                        "width", c.width,
                        "height", c.height,
                        "alpha", 1.0,
                        nullptr);
                }
                else
                {
                    g_object_set(pad, "alpha", 0.0, nullptr);
                }
            }
            else
            {
                const auto& c = placements[i];
                g_object_set(pad,
                    "xpos", c.x,
                    "ypos", c.y,
                    "width", c.width,
                    "height", c.height,
                    "alpha", 1.0,
                    nullptr);
            }
        }
    }

    void processBusMessages()
    {
        if (!pipeline_)
        {
            return;
        }

        GstBus* bus = gst_element_get_bus(pipeline_);
        if (!bus)
        {
            return;
        }

        while (auto* msg = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS)))
        {
            switch (GST_MESSAGE_TYPE(msg))
            {
            case GST_MESSAGE_ERROR:
            {
                GError* err = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                QMessageBox::critical(this, "GStreamer Error", err ? err->message : "Unknown GStreamer error.");
                if (err)
                {
                    g_error_free(err);
                }
                if (dbg)
                {
                    g_free(dbg);
                }
                break;
            }
            case GST_MESSAGE_WARNING:
            {
                GError* warn = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_warning(msg, &warn, &dbg);
                if (warn)
                {
                    qWarning("GStreamer warning: %s", warn->message);
                    g_error_free(warn);
                }
                if (dbg)
                {
                    g_free(dbg);
                }
                break;
            }
            case GST_MESSAGE_EOS:
                gst_element_seek_simple(pipeline_, GST_FORMAT_TIME, static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
                break;
            default:
                break;
            }

            gst_message_unref(msg);
        }

        gst_object_unref(bus);
    }

    void toggleFullscreen()
    {
        if (isFullScreen())
        {
            showNormal();
            fullscreenButton_->setText("Fullscreen (F11)");
        }
        else
        {
            showFullScreen();
            fullscreenButton_->setText("Exit Fullscreen (Esc)");
        }
    }

private:
    QWidget* videoWidget_ = nullptr;
    QComboBox* layoutSelector_ = nullptr;
    QPushButton* fullscreenButton_ = nullptr;

    GstElement* pipeline_ = nullptr;
    GstElement* compositor_ = nullptr;
    GstElement* sink_ = nullptr;
    QTimer* busPollTimer_ = nullptr;

    std::vector<GstPad*> sourcePads_;
    GridLayoutMode currentLayout_ = GridLayoutMode::Grid2x2;
};

int main(int argc, char* argv[])
{
    gst_init(&argc, &argv);

    QApplication app(argc, argv);
    MultiviewWindow window;
    window.show();

    return app.exec();
}
