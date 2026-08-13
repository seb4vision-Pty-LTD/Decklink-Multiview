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

// Compositor always renders onto this fixed canvas; the sink scales it to fit the actual window.
constexpr int kCanvasWidth = 1920;
constexpr int kCanvasHeight = 1080;

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

        teardownPipeline();
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
                switchLayout((index == 0) ? GridLayoutMode::Grid2x2 : GridLayoutMode::Grid3x3);
            });

        connect(fullscreenButton_, &QPushButton::clicked, this, [this]()
            {
                toggleFullscreen();
            });
    }

    void setupPipeline()
    {
        busPollTimer_ = new QTimer(this);
        connect(busPollTimer_, &QTimer::timeout, this, [this]()
            {
                processBusMessages();
            });
        busPollTimer_->start(50);

        buildPipeline(currentLayout_);
    }

    static int feedCountFor(GridLayoutMode mode)
    {
        return (mode == GridLayoutMode::Grid2x2) ? 4 : 8;
    }

    // Computes the on-canvas rectangle for every cell of the grid (9 cells for 3x3, only 8 are used).
    static std::vector<CellPlacement> computeGridPlacements(GridLayoutMode mode)
    {
        std::vector<CellPlacement> placements;

        if (mode == GridLayoutMode::Grid2x2)
        {
            const int cellW = kCanvasWidth / 2;
            const int cellH = kCanvasHeight / 2;

            placements = {
                {0, 0, cellW, cellH},
                {cellW, 0, kCanvasWidth - cellW, cellH},
                {0, cellH, cellW, kCanvasHeight - cellH},
                {cellW, cellH, kCanvasWidth - cellW, kCanvasHeight - cellH}
            };
        }
        else
        {
            const int cellW = kCanvasWidth / 3;
            const int cellH = kCanvasHeight / 3;

            placements.reserve(9);
            for (int row = 0; row < 3; ++row)
            {
                for (int col = 0; col < 3; ++col)
                {
                    const int x = col * cellW;
                    const int y = row * cellH;
                    const int w = (col == 2) ? (kCanvasWidth - x) : cellW;
                    const int h = (row == 2) ? (kCanvasHeight - y) : cellH;
                    placements.push_back({ x, y, w, h });
                }
            }
        }

        return placements;
    }

    void teardownPipeline()
    {
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

    // Builds and starts a fresh pipeline sized for the given layout; each source is pre-scaled to
    // its exact cell size before glupload, so the compositor never has to scale anything itself.
    void buildPipeline(GridLayoutMode mode)
    {
        const int feedCount = feedCountFor(mode);
        const std::vector<CellPlacement> placements = computeGridPlacements(mode);

        QString pipelineDescription =
            QString("glvideomixer name=mix background=1 ! "
                "video/x-raw(memory:GLMemory),width=%1,height=%2 ! "
                "glcolorconvert ! gldownload ! queue max-size-buffers=3 leaky=downstream ! "
                "videoconvert ! d3d11videosink name=vsink sync=false ")
                .arg(kCanvasWidth)
                .arg(kCanvasHeight);

        for (int device = 0; device < feedCount; ++device)
        {
            const CellPlacement& c = placements[device];
            pipelineDescription += QString(
                "decklinkvideosrc device-number=%1 mode=2160p50 ! "
                "video/x-raw,width=3840,height=2160,framerate=50/1 ! "
                "videoconvert ! videoscale ! video/x-raw,width=%2,height=%3,format=BGRA ! "
                "glupload ! glcolorconvert ! "
                "queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! mix.sink_%1 ")
                .arg(device)
                .arg(c.width)
                .arg(c.height);
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

        sourcePads_.reserve(feedCount);
        for (int device = 0; device < feedCount; ++device)
        {
            const QString padName = QString("sink_%1").arg(device);
            GstPad* mixerPad = gst_element_get_static_pad(compositor_, padName.toUtf8().constData());
            if (!mixerPad)
            {
                QMessageBox::critical(this, "GStreamer Error", "Could not get compositor pad: " + padName);
                return;
            }

            const CellPlacement& c = placements[device];
            g_object_set(mixerPad,
                "xpos", c.x,
                "ypos", c.y,
                "width", c.width,
                "height", c.height,
                "alpha", 1.0,
                nullptr);

            sourcePads_.push_back(mixerPad);
        }

        if (GST_IS_VIDEO_OVERLAY(sink_))
        {
            gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink_), static_cast<guintptr>(videoWidget_->winId()));
        }

        gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    }

    void switchLayout(GridLayoutMode mode)
    {
        if (mode == currentLayout_ && pipeline_)
        {
            return;
        }

        teardownPipeline();
        currentLayout_ = mode;
        buildPipeline(currentLayout_);
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
